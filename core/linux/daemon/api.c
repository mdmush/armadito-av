/***

Copyright (C) 2015, 2016 Teclib'

This file is part of Armadito core.

Armadito core is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Armadito core is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License
along with Armadito core.  If not, see <http://www.gnu.org/licenses/>.

***/

#include <assert.h>
#include <glib.h>
#include <json.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <libarmadito.h>

#include "api.h"
#include "debug.h"

#define HASH_ONE(H, C) (H) ^= ((H) << 5) + ((H) >> 2) + (C)
#define HASH_INIT_VAL 0

/* #define HASH_ONE(H, C) (H) = (((H) << 5) + (H)) + (C) */
/* #define HASH_INIT_VAL 5381 */

static void hash_init(int64_t *hash)
{
	*hash = HASH_INIT_VAL;
}

static void hash_buff(const char *buff, size_t len, int64_t *hash)
{
	for ( ; len--; buff++)
		HASH_ONE(*hash, *buff);
}

static void hash_str(const char *str, int64_t *hash)
{
	for ( ; *str; str++)
		HASH_ONE(*hash, *str);
}

int register_api_cb(struct api_handler *a, struct MHD_Connection *connection, struct json_object *in, struct json_object **out, void *user_data)
{
	int64_t token;
	const char *user_agent;
	char here;
	time_t now;

	hash_init(&token);
	time(&now);
	hash_buff((const char *)&now, sizeof(time_t), &token);
	user_agent = api_get_user_agent(connection);
	hash_str(user_agent, &token);
	hash_buff(&here, sizeof(char *), &token);

	if (token < 0)
		token = -token;

	a6o_log(ARMADITO_LOG_SERVICE, ARMADITO_LOG_LEVEL_DEBUG, "token %lld", token);

	*out = json_object_new_object();
	json_object_object_add(*out, "token", json_object_new_int64(token));

	api_handler_add_client(a, token);

	return 0;
}

int unregister_api_cb(struct api_handler *a, struct MHD_Connection *connection, struct json_object *in, struct json_object **out, void *user_data)
{
	int64_t token;

	api_get_token(connection, &token);

	api_handler_remove_client(a, token);

	return 0;
}

int ping_api_cb(struct api_handler *a, struct MHD_Connection *connection, struct json_object *in, struct json_object **out, void *user_data)
{
	*out = json_object_new_object();
	json_object_object_add(*out, "status", json_object_new_string("ok"));

	return 0;
}

static struct json_object *report_json(struct a6o_report *report)
{
	struct json_object *j_report;

	j_report = json_object_new_object();

	json_object_object_add(j_report, "progress", json_object_new_int(report->progress));
	json_object_object_add(j_report, "malware_count", json_object_new_int(report->malware_count));
	json_object_object_add(j_report, "suspicious_count", json_object_new_int(report->suspicious_count));
	json_object_object_add(j_report, "scanned_count", json_object_new_int(report->scanned_count));

	if (report->path != NULL)
		json_object_object_add(j_report, "path", json_object_new_string(report->path));

	json_object_object_add(j_report, "scan_status", json_object_new_string(a6o_file_status_pretty_str(report->status)));
	json_object_object_add(j_report, "scan_action", json_object_new_string(a6o_action_pretty_str(report->action)));

	if (report->mod_name != NULL)
		json_object_object_add(j_report, "mod_name", json_object_new_string(report->mod_name));

	if (report->mod_report != NULL)
		json_object_object_add(j_report, "mod_report", json_object_new_string(report->mod_report));

	return j_report;
}

struct scan_data {
	const char *path;
	struct api_client *client;
	time_t last_send_time;
	int last_send_progress;
	struct a6o_on_demand *on_demand;
};

#ifdef linux
static time_t get_milliseconds(void)
{
	struct timeval now;

	if (gettimeofday(&now, NULL) < 0) {
		a6o_log(ARMADITO_LOG_MODULE, ARMADITO_LOG_LEVEL_ERROR, "error getting time IHM (%s)", strerror(errno));
		return 0;
	}

	return now.tv_sec * 1000 + now.tv_usec / 1000;
}
#endif

#ifdef _WIN32
static time_t get_milliseconds( ) {

	time_t ms = 0;
	struct _timeb tb;

	_ftime64_s(&tb);
	ms = tb.time * 1000 + tb.millitm;

	return ms;
}
#endif

#define SEND_PERIOD 200  /* milliseconds */

static void scan_callback(struct a6o_report *report, void *callback_data)
{
	struct json_object *j_report;
	struct scan_data *scan_data = (struct scan_data *)callback_data;
	time_t now = get_milliseconds();

	if (report->status == ARMADITO_CLEAN
		&& report->progress != 100
		&& scan_data->last_send_progress != REPORT_PROGRESS_UNKNOWN
		&& scan_data->last_send_progress == report->progress
		&& scan_data->last_send_time != 0
		&& (now - scan_data->last_send_time) < SEND_PERIOD)
		return;

	j_report = report_json(report);

#ifdef DEBUG
	jobj_debug(j_report, "scan JSON report");
#endif

	api_client_push_event(scan_data->client, j_report);

	scan_data->last_send_time = now;
	scan_data->last_send_progress = report->progress;
}

static gpointer scan_api_thread(gpointer data)
{
	struct scan_data *scan_data = (struct scan_data *)data;

	a6o_on_demand_run(scan_data->on_demand);

	a6o_on_demand_free(scan_data->on_demand);

	free(scan_data);

	return NULL;
}

int scan_api_cb(struct api_handler *a, struct MHD_Connection *connection, struct json_object *in, struct json_object **out, void *user_data)
{
	struct json_object *j_path;
	const char *path;
	struct scan_data *scan_data;
	int64_t token;
	struct api_client *client;
	struct armadito *armadito = (struct armadito *)user_data;

	jobj_debug(in, "scan JSON input");

	api_get_token(connection, &token);
	client = api_handler_get_client(a, token);

	/* check if 'in' object contains key "path" with a string value */
	if (!json_object_object_get_ex(in, "path", &j_path)
		|| !json_object_is_type(j_path, json_type_string))
		return 1;

	scan_data = malloc(sizeof(struct scan_data));
	scan_data->path = strdup(path);
	scan_data->client = client;
	scan_data->last_send_time = 0L;
	scan_data->last_send_progress = REPORT_PROGRESS_UNKNOWN;

	scan_data->on_demand = a6o_on_demand_new(armadito, 42, json_object_get_string(j_path), ARMADITO_SCAN_RECURSE | ARMADITO_SCAN_THREADED);

	a6o_scan_add_callback(a6o_on_demand_get_scan(scan_data->on_demand), scan_callback, scan_data);

	g_thread_new("scan thread", scan_api_thread, scan_data);

	return 0;
}

int poll_api_cb(struct api_handler *a, struct MHD_Connection *connection, struct json_object *in, struct json_object **out, void *user_data)
{
	int64_t token;
	struct api_client *client;

	api_get_token(connection, &token);

	client = api_handler_get_client(a, token);

	if (client != NULL) {
		api_client_pop_event(client, out);

#ifdef DEBUG
		jobj_debug(*out, "JSON event");
#endif

		return 0;
	}

	return 1;
}

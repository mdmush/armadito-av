#include <libumwsu/module.h>
#include <libumwsu/scan.h>
#include "alert.h"
#include "conf.h"
#include "dir.h"
#include "modulep.h"
#include "protocol.h"
#include "quarantine.h"
#include "statusp.h"
#include "umwsup.h"
#include "unixsock.h"

#include <assert.h>
#include <glib.h>
#include <magic.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>

struct callback_entry {
  umwsu_scan_callback_t callback;
  void *callback_data;
};

struct local_scan {
  GThreadPool *thread_pool;  
  GPrivate *private_magic_key;
};

struct remote_scan {
  int sock;
  struct protocol_handler *handler;
};

struct umwsu_scan {
  struct umwsu *umwsu;
  const char *path;
  enum umwsu_scan_flags flags;
  GArray *callbacks;
  int is_remote;

  union {
    struct local_scan local;
    struct remote_scan remote;
  };

  GThreadPool *thread_pool;  
  GPrivate *private_magic_key;
};

static enum umwsu_status local_scan_start(struct umwsu_scan *scan);
static enum umwsu_status remote_scan_start(struct umwsu_scan *scan);

struct umwsu_scan *umwsu_scan_new(struct umwsu *umwsu, const char *path, enum umwsu_scan_flags flags)
{
  struct umwsu_scan *scan = (struct umwsu_scan *)malloc(sizeof(struct umwsu_scan));

  scan->umwsu = umwsu;
  scan->path = (const char *)strdup(path);
  scan->flags = flags;
  scan->callbacks = g_array_new(FALSE, FALSE, sizeof(struct callback_entry));
  scan->is_remote = umwsu_is_remote(umwsu);

  return scan;
}

void umwsu_scan_add_callback(struct umwsu_scan *scan, umwsu_scan_callback_t callback, void *callback_data)
{
  struct callback_entry entry;

  entry.callback = callback;
  entry.callback_data = callback_data;

  g_array_append_val(scan->callbacks, entry);
}

static void umwsu_scan_call_callbacks(struct umwsu_scan *scan, struct umwsu_report *report)
{
  int i;

  for(i = 0; i < scan->callbacks->len; i++) {
    struct callback_entry *entry = &g_array_index(scan->callbacks, struct callback_entry, i);
    umwsu_scan_callback_t callback = entry->callback;

    (*callback)(report, entry->callback_data);
  }
}

enum umwsu_status umwsu_scan_start(struct umwsu_scan *scan)
{
  if (scan->is_remote)
    return remote_scan_start(scan);

  return local_scan_start(scan);
}

void umwsu_scan_finish(struct umwsu_scan *scan)
{
  if (scan->local.thread_pool != NULL)
    g_thread_pool_free(scan->local.thread_pool, FALSE, TRUE);
}

void umwsu_scan_free(struct umwsu_scan *scan)
{
  free((char *)scan->path);

  g_array_free(scan->callbacks, TRUE);

  free(scan);
}



static enum umwsu_status local_scan_apply_modules(const char *path, const char *mime_type, GPtrArray *mod_array,  struct umwsu_report *report)
{
  enum umwsu_status current_status = UMWSU_UNDECIDED;
  int i;

  for (i = 0; i < mod_array->len; i++) {
    struct umwsu_module *mod = (struct umwsu_module *)g_ptr_array_index(mod_array, i);
    enum umwsu_status mod_status;
    char *mod_report = NULL;

#if 0
    if (umwsu_get_verbose(u) >= 2)
      printf("UMWSU: module %s: scanning %s\n", mod->name, path);
#endif

    mod_status = (*mod->scan)(path, mime_type, mod->data, &mod_report);

#if 0
    printf("UMWSU: module %s: scanning %s -> %s\n", mod->name, path, umwsu_status_str(mod_status));
#endif

    if (umwsu_status_cmp(current_status, mod_status) < 0) {
      current_status = mod_status;
      umwsu_report_change(report, mod_status, (char *)mod->name, mod_report);
    } else if (mod_report != NULL)
      free(mod_report);

#if 0
    printf("UMWSU: current status %s\n", umwsu_status_str(current_status));
#endif

    if (current_status == UMWSU_WHITE_LISTED || current_status == UMWSU_MALWARE)
      break;
  }

  return current_status;
}

static enum umwsu_status local_scan_file(struct umwsu_scan *scan, magic_t magic, const char *path)
{
  enum umwsu_status status;
  struct umwsu_report report;
  GPtrArray *modules;
  char *mime_type;

  umwsu_report_init(&report, path);

  modules = umwsu_get_applicable_modules(scan->umwsu, magic, path, &mime_type);

  if (modules == NULL)
    report.status = UMWSU_UNKNOWN_FILE_TYPE;
  else
    status = local_scan_apply_modules(path, mime_type, modules, &report);

  if (umwsu_get_verbose(scan->umwsu) >= 3)
    printf("%s: %s\n", path, umwsu_status_str(status));

  umwsu_scan_call_callbacks(scan, &report);

  umwsu_report_destroy(&report);

  free(mime_type);

  return status;
}

/* Unfortunately, libmagic is not thread-safe. */
/* We create a new magic_t for each thread, and keep it  */
/* in thread's private data, so that it is created only once. */
static void magic_destroy_notify(gpointer data)
{
  magic_close((magic_t)data);
}

static magic_t get_private_magic(struct umwsu_scan *scan)
{
  magic_t m = (magic_t)g_private_get(scan->local.private_magic_key);

  if (m == NULL) {
    m = magic_open(MAGIC_MIME_TYPE);
    magic_load(m, NULL);

    g_private_set(scan->local.private_magic_key, (gpointer)m);
  }

  return m;
}

static void local_scan_entry_thread(gpointer data, gpointer user_data)
{
  struct umwsu_scan *scan = (struct umwsu_scan *)user_data;
  char *path = (char *)data;

  local_scan_file(scan, get_private_magic(scan), path);

  free(path);
}

static void local_scan_entry_threaded(const char *full_path, const struct dirent *dir_entry, void *data)
{
  struct umwsu_scan *scan = (struct umwsu_scan *)data;

  if (dir_entry->d_type == DT_DIR)
    return;

  g_thread_pool_push(scan->local.thread_pool, (gpointer)strdup(full_path), NULL);
}

static void local_scan_entry_non_threaded(const char *full_path, const struct dirent *dir_entry, void *data)
{
  struct umwsu_scan *scan = (struct umwsu_scan *)data;

  if (dir_entry->d_type == DT_DIR)
    return;

  local_scan_file(scan, NULL, full_path);
}

static int get_max_threads(void)
{
  return 8;
}

enum umwsu_status local_scan_start(struct umwsu_scan *scan)
{
  struct stat sb;

  umwsu_scan_add_callback(scan, alert_callback, NULL);
  umwsu_scan_add_callback(scan, quarantine_callback, NULL);

  if (scan->flags & UMWSU_SCAN_THREADED) {
    scan->local.thread_pool = g_thread_pool_new(local_scan_entry_thread, scan, get_max_threads(), FALSE, NULL);
    scan->local.private_magic_key = g_private_new(magic_destroy_notify);
  } else {
    scan->local.thread_pool = NULL;
    scan->local.private_magic_key = NULL;
  }

  if (stat(scan->path, &sb) == -1) {
    perror("stat");
    /* exit(EXIT_FAILURE); */
  }

  if (S_ISREG(sb.st_mode))
    return local_scan_file(scan, NULL, scan->path);

  if (S_ISDIR(sb.st_mode)) {
    int threaded = scan->flags  & UMWSU_SCAN_THREADED;
    int recurse = scan->flags  & UMWSU_SCAN_RECURSE;

    dir_map(scan->path, recurse, (threaded) ? local_scan_entry_threaded : local_scan_entry_non_threaded, scan);

    return UMWSU_CLEAN;
  }

  return UMWSU_EINVAL;
}

static void remote_scan_cb_scan_start(struct protocol_handler *h, void *data)
{
}

static void remote_scan_cb_scan_file(struct protocol_handler *h, void *data)
{
  struct umwsu_scan *scan = (struct umwsu_scan *)data;
  char *path = protocol_handler_header_value(h, "Path");
  char *status = protocol_handler_header_value(h, "Status");
  char *x_status = protocol_handler_header_value(h, "X-Status");
  char *action = protocol_handler_header_value(h, "Action");
  struct umwsu_report report;

  umwsu_report_init(&report, path);

  report.status = (enum umwsu_status)atoi(status);
  report.action = (enum umwsu_action)atoi(action);
  report.mod_name = "unknown";
  report.mod_report = x_status;

  umwsu_scan_call_callbacks(scan, &report);

  umwsu_report_destroy(&report);
}

static void remote_scan_cb_scan_end(struct protocol_handler *h, void *data)
{
}

static enum umwsu_status remote_scan_start(struct umwsu_scan *scan)
{
  char *sock_path;
  
  sock_path = conf_get(scan->umwsu, "remote", "socket-path");
  assert(sock_path != NULL);

  scan->remote.sock = client_socket_create(sock_path, 10);
  scan->remote.handler = protocol_handler_new(scan->remote.sock, scan->remote.sock);

  protocol_handler_add_callback(scan->remote.handler, "SCAN_START", remote_scan_cb_scan_start, scan);
  protocol_handler_add_callback(scan->remote.handler, "SCAN_FILE", remote_scan_cb_scan_file, scan);
  protocol_handler_add_callback(scan->remote.handler, "SCAN_END", remote_scan_cb_scan_end, scan);

  protocol_handler_output_message(scan->remote.handler, "SCAN",
				  "Path", scan->path,
				  NULL);
}

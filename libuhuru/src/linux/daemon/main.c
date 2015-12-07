#include <libuhuru/core.h>

#include "utils/getopt.h"
#include "log.h"
#include "server.h"
#include "daemonize.h"
#include "monitor.h"
#include "pollset.h"
#include "tcpsock.h"
#include "unixsock.h"

#include <glib.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#define DEFAULT_LOG_LEVEL "critical"

struct uhuru_daemon_options {
  int no_daemon;
  enum {
    TCP_SOCKET,
    UNIX_SOCKET,
  } socket_type;
  unsigned short port_number;
  const char *unix_path;
  const char *s_log_level;
  const char *pid_file;
};

struct opt daemon_opt_defs[] = {
  { .long_form = "help", .short_form = 'h', .need_arg = 0, .is_set = 0, .value = NULL},
  { .long_form = "no-daemon", .short_form = 'n', .need_arg = 0, .is_set = 0, .value = NULL},
  { .long_form = "log-level", .short_form = 'l', .need_arg = 1, .is_set = 0, .value = NULL},
  { .long_form = "tcp", .short_form = 't', .need_arg = 0, .is_set = 0, .value = NULL},
  { .long_form = "port", .short_form = 'p', .need_arg = 1, .is_set = 0, .value = NULL},
  { .long_form = "unix", .short_form = 'u', .need_arg = 0, .is_set = 0, .value = NULL},
  { .long_form = "path", .short_form = 'a', .need_arg = 1, .is_set = 0, .value = NULL},
  { .long_form = "pidfile", .short_form = 'i', .need_arg = 1, .is_set = 0, .value = NULL},
  { .long_form = NULL, .short_form = '\0', .need_arg = 0, .is_set = 0, .value = NULL},
};

static void usage(void)
{
  fprintf(stderr, "usage: uhuru-daemon [options]\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "Uhuru antivirus scanner daemon\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "Options:\n");
  fprintf(stderr, "  --help  -h                     print help and quit\n");
  fprintf(stderr, "  --no-daemon -n                 do not fork and go to background\n");
  fprintf(stderr, "  --log-level=LEVEL | -l LEVEL   set log level\n");
  fprintf(stderr, "                                 Log level can be: critical, warning, message, info, debug\n");
  fprintf(stderr, "                                 Default is : %s\n", DEFAULT_LOG_LEVEL);
  fprintf(stderr, "  --tcp -t | --unix -u           use TCP (--tcp) or unix (--unix) socket (default is unix)\n");
  fprintf(stderr, "  --port=PORT | -p PORT          TCP port number\n");
  fprintf(stderr, "  --path=PATH | -a PATH          unix socket path\n");
  fprintf(stderr, "  --pidfile=PATH | -i PATH       create PID file at specified location\n");
  fprintf(stderr, "\n");

  exit(1);
}

static int check_log_level(const char *s_log_level)
{
  if (!strcmp(s_log_level,"error")
      || !strcmp(s_log_level,"critical")
      || !strcmp(s_log_level,"warning")
      || !strcmp(s_log_level,"message")
      || !strcmp(s_log_level,"info")
      || !strcmp(s_log_level,"debug"))
    return 0;

  return 1;
}

static void parse_options(int argc, const char **argv, struct uhuru_daemon_options *opts)
{
  int r = opt_parse(daemon_opt_defs, argc, argv);
  const char *s_port;

 // printf( "ARGV[0] : %s \n", argv[0] );
 // printf( "ARGV[1] : %s \n", argv[1] );

  if (r < 0|| r > argc)
    usage();

  if (opt_is_set(daemon_opt_defs, "help"))
      usage();

  if (opt_is_set(daemon_opt_defs, "tcp") && opt_is_set(daemon_opt_defs, "unix"))
    usage();

  opts->pid_file = NULL;

  opts->no_daemon = opt_is_set(daemon_opt_defs, "no-daemon");

  opts->s_log_level = opt_value(daemon_opt_defs, "log-level", DEFAULT_LOG_LEVEL);
  if (check_log_level(opts->s_log_level))
    usage();

  opts->socket_type = UNIX_SOCKET;
  if (opt_is_set(daemon_opt_defs, "tcp"))
    opts->socket_type = TCP_SOCKET;

  s_port = opt_value(daemon_opt_defs, "port", "14444");
  opts->port_number = (unsigned short)atoi(s_port);

  opts->unix_path = opt_value(daemon_opt_defs, "path", "@/tmp/.uhuru/daemon");

  if (opt_is_set(daemon_opt_defs, "pidfile"))
    opts->pid_file = opt_value(daemon_opt_defs, "pidfile", LOCALSTATEDIR "/run/uhuru-scand.pid");
}

static int main_loop(struct server *server, struct access_monitor *monitor)
{
  struct poll_set *ps = poll_set_new();

  poll_set_add_fd(ps, server_get_poll_fd(server), server_cb, server);

  if (monitor != NULL)
    poll_set_add_fd(ps, access_monitor_get_poll_fd(monitor), access_monitor_cb, monitor);

  if (monitor != NULL)
    access_monitor_activate(monitor);

  /* FIXME: must use configuration */
  if (monitor != NULL)
    access_monitor_enable_permission(monitor, 1);

  return poll_set_loop(ps);
}

int main(int argc, const char **argv)
{
  struct uhuru_daemon_options opts;
  int server_sock;
  struct uhuru *uhuru;
  struct server *server;
  struct access_monitor *monitor;
  uhuru_error *error = NULL;

#ifdef HAVE_GTHREAD_INIT
  g_thread_init(NULL);
#endif

  parse_options(argc, argv, &opts);

  if (!opts.no_daemon)
    daemonize();

  if (opts.pid_file != NULL) {
    int r = create_pid_file(opts.pid_file);

    /* a priori, writing to stderr is meaningless, since stderr would have probably been closed by daemonize() */
    if (r)
      exit(r);
  }

  if (opts.socket_type == TCP_SOCKET)
    server_sock = tcp_server_listen(opts.port_number, "127.0.0.1");
  else
    server_sock = unix_server_listen(opts.unix_path);

  if (server_sock < 0) {
    fprintf(stderr, "cannot open server socket (errno %d)\n", errno);
    return 1;
  }

  log_init(opts.s_log_level, !opts.no_daemon);

  uhuru_log(UHURU_LOG_SERVICE, UHURU_LOG_LEVEL_NONE, "starting %s%s", argv[0], opts.no_daemon ? "" : " in daemon mode");

  uhuru = uhuru_open(&error);
  if (uhuru == NULL) {
    uhuru_error_print(error, stderr);
    exit(1);
  }

  uhuru_log(UHURU_LOG_SERVICE, UHURU_LOG_LEVEL_INFO, "uhuru init done");

  server = server_new(uhuru, server_sock);

  monitor = access_monitor_new(uhuru);
  /* FIXME: must use configuration */
  if (monitor != NULL)
    access_monitor_add(monitor, "/home");

  main_loop(server, monitor);

  return 0;
}

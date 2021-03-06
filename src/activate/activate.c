/* SPDX-License-Identifier: LGPL-2.1+ */
/***
  This file is part of systemd.

  Copyright 2013 Zbigniew Jędrzejewski-Szmek
***/

#include <getopt.h>
#include <sys/epoll.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "sd-daemon.h"

#include "alloc-util.h"
#include "escape.h"
#include "fd-util.h"
#include "log.h"
#include "macro.h"
#include "process-util.h"
#include "signal-util.h"
#include "socket-util.h"
#include "string-util.h"
#include "strv.h"

static char** arg_listen = NULL;
static bool arg_accept = false;
static int arg_socket_type = SOCK_STREAM;
static char** arg_args = NULL;
static char** arg_setenv = NULL;
static char **arg_fdnames = NULL;
static bool arg_inetd = false;

static int add_epoll(int epoll_fd, int fd) {
        struct epoll_event ev = {
                .events = EPOLLIN,
                .data.fd = fd,
        };

        assert(epoll_fd >= 0);
        assert(fd >= 0);

        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0)
                return log_error_errno(errno, "Failed to add event on epoll fd:%d for fd:%d: %m", epoll_fd, fd);

        return 0;
}

static int open_sockets(int *epoll_fd, bool accept) {
        char **address;
        int n, fd, r;
        int count = 0;

        n = sd_listen_fds(true);
        if (n < 0)
                return log_error_errno(n, "Failed to read listening file descriptors from environment: %m");
        if (n > 0) {
                log_info("Received %i descriptors via the environment.", n);

                for (fd = SD_LISTEN_FDS_START; fd < SD_LISTEN_FDS_START + n; fd++) {
                        r = fd_cloexec(fd, arg_accept);
                        if (r < 0)
                                return r;

                        count++;
                }
        }

        /* Close logging and all other descriptors */
        if (arg_listen) {
                int except[3 + n];

                for (fd = 0; fd < SD_LISTEN_FDS_START + n; fd++)
                        except[fd] = fd;

                log_close();
                close_all_fds(except, 3 + n);
        }

        /** Note: we leak some fd's on error here. I doesn't matter
         *  much, since the program will exit immediately anyway, but
         *  would be a pain to fix.
         */

        STRV_FOREACH(address, arg_listen) {
                fd = make_socket_fd(LOG_DEBUG, *address, arg_socket_type, (arg_accept*SOCK_CLOEXEC));
                if (fd < 0) {
                        log_open();
                        return log_error_errno(fd, "Failed to open '%s': %m", *address);
                }

                assert(fd == SD_LISTEN_FDS_START + count);
                count++;
        }

        if (arg_listen)
                log_open();

        *epoll_fd = epoll_create1(EPOLL_CLOEXEC);
        if (*epoll_fd < 0)
                return log_error_errno(errno, "Failed to create epoll object: %m");

        for (fd = SD_LISTEN_FDS_START; fd < SD_LISTEN_FDS_START + count; fd++) {
                _cleanup_free_ char *name = NULL;

                getsockname_pretty(fd, &name);
                log_info("Listening on %s as %i.", strna(name), fd);

                r = add_epoll(*epoll_fd, fd);
                if (r < 0)
                        return r;
        }

        return count;
}

static int exec_process(const char* name, char **argv, char **env, int start_fd, int n_fds) {

        _cleanup_strv_free_ char **envp = NULL;
        _cleanup_free_ char *joined = NULL;
        unsigned n_env = 0, length;
        const char *tocopy;
        char **s;
        int r;

        if (arg_inetd && n_fds != 1) {
                log_error("--inetd only supported for single file descriptors.");
                return -EINVAL;
        }

        length = strv_length(arg_setenv);

        /* PATH, TERM, HOME, USER, LISTEN_FDS, LISTEN_PID, LISTEN_FDNAMES, NULL */
        envp = new0(char *, length + 8);
        if (!envp)
                return log_oom();

        STRV_FOREACH(s, arg_setenv) {

                if (strchr(*s, '=')) {
                        char *k;

                        k = strdup(*s);
                        if (!k)
                                return log_oom();

                        envp[n_env++] = k;
                } else {
                        _cleanup_free_ char *p;
                        const char *n;

                        p = strappend(*s, "=");
                        if (!p)
                                return log_oom();

                        n = strv_find_prefix(env, p);
                        if (!n)
                                continue;

                        envp[n_env] = strdup(n);
                        if (!envp[n_env])
                                return log_oom();

                        n_env++;
                }
        }

        FOREACH_STRING(tocopy, "TERM=", "PATH=", "USER=", "HOME=") {
                const char *n;

                n = strv_find_prefix(env, tocopy);
                if (!n)
                        continue;

                envp[n_env] = strdup(n);
                if (!envp[n_env])
                        return log_oom();

                n_env++;
        }

        if (arg_inetd) {
                assert(n_fds == 1);

                r = rearrange_stdio(start_fd, start_fd, STDERR_FILENO); /* invalidates start_fd on success + error */
                if (r < 0)
                        return log_error_errno(r, "Failed to move fd to stdin+stdout: %m");

        } else {
                if (start_fd != SD_LISTEN_FDS_START) {
                        assert(n_fds == 1);

                        if (dup2(start_fd, SD_LISTEN_FDS_START) < 0)
                                return log_error_errno(errno, "Failed to dup connection: %m");

                        safe_close(start_fd);
                        start_fd = SD_LISTEN_FDS_START;
                }

                if (asprintf((char**)(envp + n_env++), "LISTEN_FDS=%i", n_fds) < 0)
                        return log_oom();

                if (asprintf((char**)(envp + n_env++), "LISTEN_PID=" PID_FMT, getpid_cached()) < 0)
                        return log_oom();

                if (arg_fdnames) {
                        _cleanup_free_ char *names = NULL;
                        size_t len;
                        char *e;
                        int i;

                        len = strv_length(arg_fdnames);
                        if (len == 1)
                                for (i = 1; i < n_fds; i++) {
                                        r = strv_extend(&arg_fdnames, arg_fdnames[0]);
                                        if (r < 0)
                                                return log_error_errno(r, "Failed to extend strv: %m");
                                }
                        else if (len != (unsigned) n_fds)
                                log_warning("The number of fd names is different than number of fds: %zu vs %d",
                                            len, n_fds);

                        names = strv_join(arg_fdnames, ":");
                        if (!names)
                                return log_oom();

                        e = strappend("LISTEN_FDNAMES=", names);
                        if (!e)
                                return log_oom();

                        envp[n_env++] = e;
                }
        }

        joined = strv_join(argv, " ");
        if (!joined)
                return log_oom();

        log_info("Execing %s (%s)", name, joined);
        execvpe(name, argv, envp);

        return log_error_errno(errno, "Failed to execp %s (%s): %m", name, joined);
}

static int fork_and_exec_process(const char* child, char** argv, char **env, int fd) {
        _cleanup_free_ char *joined = NULL;
        pid_t child_pid;
        int r;

        joined = strv_join(argv, " ");
        if (!joined)
                return log_oom();

        r = safe_fork("(activate)", FORK_RESET_SIGNALS|FORK_DEATHSIG|FORK_LOG, &child_pid);
        if (r < 0)
                return r;
        if (r == 0) {
                /* In the child */
                exec_process(child, argv, env, fd, 1);
                _exit(EXIT_FAILURE);
        }

        log_info("Spawned %s (%s) as PID " PID_FMT ".", child, joined, child_pid);
        return 0;
}

static int do_accept(const char* name, char **argv, char **envp, int fd) {
        _cleanup_free_ char *local = NULL, *peer = NULL;
        _cleanup_close_ int fd_accepted = -1;

        fd_accepted = accept4(fd, NULL, NULL, 0);
        if (fd_accepted < 0)
                return log_error_errno(errno, "Failed to accept connection on fd:%d: %m", fd);

        getsockname_pretty(fd_accepted, &local);
        getpeername_pretty(fd_accepted, true, &peer);
        log_info("Connection from %s to %s", strna(peer), strna(local));

        return fork_and_exec_process(name, argv, envp, fd_accepted);
}

/* SIGCHLD handler. */
static void sigchld_hdl(int sig) {
        PROTECT_ERRNO;

        for (;;) {
                siginfo_t si;
                int r;

                si.si_pid = 0;
                r = waitid(P_ALL, 0, &si, WEXITED|WNOHANG);
                if (r < 0) {
                        if (errno != ECHILD)
                                log_error_errno(errno, "Failed to reap children: %m");
                        return;
                }
                if (si.si_pid == 0)
                        return;

                log_info("Child %d died with code %d", si.si_pid, si.si_status);
        }
}

static int install_chld_handler(void) {
        static const struct sigaction act = {
                .sa_flags = SA_NOCLDSTOP|SA_RESTART,
                .sa_handler = sigchld_hdl,
        };

        if (sigaction(SIGCHLD, &act, 0) < 0)
                return log_error_errno(errno, "Failed to install SIGCHLD handler: %m");

        return 0;
}

static void help(void) {
        printf("%s [OPTIONS...]\n\n"
               "Listen on sockets and launch child on connection.\n\n"
               "Options:\n"
               "  -h --help                  Show this help and exit\n"
               "     --version               Print version string and exit\n"
               "  -l --listen=ADDR           Listen for raw connections at ADDR\n"
               "  -d --datagram              Listen on datagram instead of stream socket\n"
               "     --seqpacket             Listen on SOCK_SEQPACKET instead of stream socket\n"
               "  -a --accept                Spawn separate child for each connection\n"
               "  -E --setenv=NAME[=VALUE]   Pass an environment variable to children\n"
               "     --fdname=NAME[:NAME...] Specify names for file descriptors\n"
               "     --inetd                 Enable inetd file descriptor passing protocol\n"
               "\n"
               "Note: file descriptors from sd_listen_fds() will be passed through.\n"
               , program_invocation_short_name);
}

static int parse_argv(int argc, char *argv[]) {
        enum {
                ARG_VERSION = 0x100,
                ARG_FDNAME,
                ARG_SEQPACKET,
                ARG_INETD,
        };

        static const struct option options[] = {
                { "help",        no_argument,       NULL, 'h'           },
                { "version",     no_argument,       NULL, ARG_VERSION   },
                { "datagram",    no_argument,       NULL, 'd'           },
                { "seqpacket",   no_argument,       NULL, ARG_SEQPACKET },
                { "listen",      required_argument, NULL, 'l'           },
                { "accept",      no_argument,       NULL, 'a'           },
                { "setenv",      required_argument, NULL, 'E'           },
                { "environment", required_argument, NULL, 'E'           }, /* legacy alias */
                { "fdname",      required_argument, NULL, ARG_FDNAME    },
                { "inetd",       no_argument,       NULL, ARG_INETD     },
                {}
        };

        int c, r;

        assert(argc >= 0);
        assert(argv);

        while ((c = getopt_long(argc, argv, "+hl:aE:d", options, NULL)) >= 0)
                switch(c) {
                case 'h':
                        help();
                        return 0;

                case ARG_VERSION:
                        return version();

                case 'l':
                        r = strv_extend(&arg_listen, optarg);
                        if (r < 0)
                                return log_oom();

                        break;

                case 'd':
                        if (arg_socket_type == SOCK_SEQPACKET) {
                                log_error("--datagram may not be combined with --seqpacket.");
                                return -EINVAL;
                        }

                        arg_socket_type = SOCK_DGRAM;
                        break;

                case ARG_SEQPACKET:
                        if (arg_socket_type == SOCK_DGRAM) {
                                log_error("--seqpacket may not be combined with --datagram.");
                                return -EINVAL;
                        }

                        arg_socket_type = SOCK_SEQPACKET;
                        break;

                case 'a':
                        arg_accept = true;
                        break;

                case 'E':
                        r = strv_extend(&arg_setenv, optarg);
                        if (r < 0)
                                return log_oom();

                        break;

                case ARG_FDNAME: {
                        _cleanup_strv_free_ char **names;
                        char **s;

                        names = strv_split(optarg, ":");
                        if (!names)
                                return log_oom();

                        STRV_FOREACH(s, names)
                                if (!fdname_is_valid(*s)) {
                                        _cleanup_free_ char *esc;

                                        esc = cescape(*s);
                                        log_warning("File descriptor name \"%s\" is not valid.", esc);
                                }

                        /* Empty optargs means one empty name */
                        r = strv_extend_strv(&arg_fdnames,
                                             strv_isempty(names) ? STRV_MAKE("") : names,
                                             false);
                        if (r < 0)
                                return log_error_errno(r, "strv_extend_strv: %m");
                        break;
                }

                case ARG_INETD:
                        arg_inetd = true;
                        break;

                case '?':
                        return -EINVAL;

                default:
                        assert_not_reached("Unhandled option");
                }

        if (optind == argc) {
                log_error("%s: command to execute is missing.",
                          program_invocation_short_name);
                return -EINVAL;
        }

        if (arg_socket_type == SOCK_DGRAM && arg_accept) {
                log_error("Datagram sockets do not accept connections. "
                          "The --datagram and --accept options may not be combined.");
                return -EINVAL;
        }

        arg_args = argv + optind;

        return 1 /* work to do */;
}

int main(int argc, char **argv, char **envp) {
        int r, n;
        int epoll_fd = -1;

        log_parse_environment();
        log_open();

        r = parse_argv(argc, argv);
        if (r <= 0)
                return r == 0 ? EXIT_SUCCESS : EXIT_FAILURE;

        r = install_chld_handler();
        if (r < 0)
                return EXIT_FAILURE;

        n = open_sockets(&epoll_fd, arg_accept);
        if (n < 0)
                return EXIT_FAILURE;
        if (n == 0) {
                log_error("No sockets to listen on specified or passed in.");
                return EXIT_FAILURE;
        }

        for (;;) {
                struct epoll_event event;

                if (epoll_wait(epoll_fd, &event, 1, -1) < 0) {
                        if (errno == EINTR)
                                continue;

                        log_error_errno(errno, "epoll_wait() failed: %m");
                        return EXIT_FAILURE;
                }

                log_info("Communication attempt on fd %i.", event.data.fd);
                if (arg_accept) {
                        r = do_accept(argv[optind], argv + optind, envp, event.data.fd);
                        if (r < 0)
                                return EXIT_FAILURE;
                } else
                        break;
        }

        exec_process(argv[optind], argv + optind, envp, SD_LISTEN_FDS_START, n);

        return EXIT_SUCCESS;
}

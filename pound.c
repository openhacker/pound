/*
 * Pound - the reverse-proxy load-balancer
 * Copyright (C) 2002-2010 Apsis GmbH
 *
 * This file is part of Pound.
 *
 * Pound is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 * 
 * Pound is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Contact information:
 * Apsis GmbH
 * P.O.Box
 * 8707 Uetikon am See
 * Switzerland
 * EMail: roseg@apsis.ch
 */

#include <dlfcn.h>
#include    "pound.h"

/* common variables */
char        *user,              /* user to run as */
            *group,             /* group to run as */
            *root_jail,         /* directory to chroot to */
            *pid_name,          /* file to record pid in */
            *ctrl_name;         /* control socket name */

int         alive_to,           /* check interval for resurrection */
            anonymise,          /* anonymise client address */
            daemonize,          /* run as daemon */
            log_facility,       /* log facility to use */
            print_log,          /* print log messages to stdout/stderr */
            grace,              /* grace period before shutdown */
            control_sock;       /* control socket */

SERVICE     *services;          /* global services (if any) */

LISTENER    *listeners;         /* all available listeners */

PLUGIN      *plugins;


regex_t HEADER,             /* Allowed header */
        CONN_UPGRD,         /* upgrade in connection header */
        CHUNK_HEAD,         /* chunk header line */
        RESP_SKIP,          /* responses for which we skip response */
        RESP_IGN,           /* responses for which we ignore content */
        LOCATION,           /* the host we are redirected to */
        AUTHORIZATION;      /* the Authorisation header */

static int  shut_down = 0;

#ifndef  SOL_TCP
/* for systems without the definition */
int     SOL_TCP;
#endif

/* worker pid */
static  pid_t               son = 0;

/*
 * OpenSSL thread support stuff
 */
#if OPENSSL_VERSION_NUMBER >= 0x10100000L
#define l_init()
#else
static pthread_mutex_t  *l_array;

static void
l_init(void)
{
    int i, n_locks;

    n_locks = CRYPTO_num_locks();
    if((l_array = (pthread_mutex_t *)calloc(n_locks, sizeof(pthread_mutex_t))) == NULL) {
        logmsg(LOG_ERR, "lock init: out of memory - aborted...");
        exit(1);
    }
    for(i = 0; i < n_locks; i++)
        /* pthread_mutex_init() always returns 0 */
        pthread_mutex_init(&l_array[i], NULL);
    return;
}

static void
l_lock(const int mode, const int n, /* unused */ const char *file, /* unused */ int line)
{
    int ret_val;

    if(mode & CRYPTO_LOCK) {
        if(ret_val = pthread_mutex_lock(&l_array[n]))
            logmsg(LOG_ERR, "l_lock lock(): %s", strerror(ret_val));
    } else {
        if(ret_val = pthread_mutex_unlock(&l_array[n]))
            logmsg(LOG_ERR, "l_lock unlock(): %s", strerror(ret_val));
    }
    return;
}

static unsigned long
l_id(void)
{
    return (unsigned long)pthread_self();
}
#endif

/*
 * work queue stuff
 */
static thr_arg          *first = NULL, *last = NULL;
static pthread_cond_t   arg_cond;
static pthread_mutex_t  arg_mut;
int                     numthreads;

static void
init_thr_arg(void)
{
    pthread_cond_init(&arg_cond, NULL);
    pthread_mutex_init(&arg_mut, NULL);
    return;
}

/*
 * add a request to the queue
 */
int
put_thr_arg(thr_arg *arg)
{
    thr_arg *res;

    if((res = malloc(sizeof(thr_arg))) == NULL) {
        logmsg(LOG_WARNING, "thr_arg malloc");
        return -1;
    }
    memcpy(res, arg, sizeof(thr_arg));
    res->next = NULL;
    (void)pthread_mutex_lock(&arg_mut);
    if(last == NULL)
        first = last = res;
    else {
        last->next = res;
        last = last->next;
    }
    (void)pthread_mutex_unlock(&arg_mut);
    pthread_cond_signal(&arg_cond);
    return 0;
}

/*
 * get a request from the queue
 */
thr_arg *
get_thr_arg(void)
{
    thr_arg *res;

    (void)pthread_mutex_lock(&arg_mut);
    if(first == NULL)
        (void)pthread_cond_wait(&arg_cond, &arg_mut);
    if((res = first) != NULL)
        if((first = first->next) == NULL)
            last = NULL;
    (void)pthread_mutex_unlock(&arg_mut);
    if(first != NULL)
        pthread_cond_signal(&arg_cond);
    return res;
}

/*
 * get the current queue length
 */
int
get_thr_qlen(void)
{
    int     res;
    thr_arg *tap;

    (void)pthread_mutex_lock(&arg_mut);
    for(res = 0, tap = first; tap != NULL; tap = tap->next, res++)
        ;
    (void)pthread_mutex_unlock(&arg_mut);
    return res;
}

/*
 * handle SIGTERM/SIGQUIT - exit
 */
static RETSIGTYPE
h_term(const int sig)
{
    logmsg(LOG_NOTICE, "received signal %d - exiting...", sig);
    if(son > 0)
        kill(son, sig);
    if(ctrl_name != NULL)
        (void)unlink(ctrl_name);
    exit(0);
}

/*
 * handle SIGHUP/SIGINT - exit after grace period
 */
static RETSIGTYPE
h_shut(const int sig)
{
    int         status;
    LISTENER    *lstn;

    logmsg(LOG_NOTICE, "received signal %d - shutting down...", sig);
    if(son > 0) {
        for(lstn = listeners; lstn; lstn = lstn->next)
            close(lstn->sock);
        kill(son, sig);
        (void)wait(&status);
        if(ctrl_name != NULL)
            (void)unlink(ctrl_name);
        exit(0);
    } else
        shut_down = 1;
}

static void setup_plugins(void)
{
	PLUGIN *this_plugin;
	
	for(this_plugin = plugins; this_plugin; this_plugin = this_plugin->next) {
		void *lib_handle;

		lib_handle = dlopen(this_plugin->so_name, RTLD_LAZY);
		if(!lib_handle) {
			logmsg(LOG_ERR, "Cannot dlopen %s", this_plugin->so_name);
			exit(1);
		}
		this_plugin->dlopen = lib_handle;
		this_plugin->startup = dlsym(lib_handle, "plugin_startup");
		if(!this_plugin->startup) {
			logmsg(LOG_ERR, "Cannot find startup in %s", this_plugin->so_name);
			exit(1);
		}
		this_plugin->shutdown = dlsym(lib_handle, "plugin_shutdown");
		if(!this_plugin->shutdown) {
			logmsg(LOG_ERR, "cannot find shutdown in %s", this_plugin->so_name);
			exit(1);
		}

		(*this_plugin->startup)();	/* ml -- do it now??  */

	}
}


static void find_backend_function(SERVICE *this_service, const char *so_name)
{
	PLUGIN *this_plugin;

	for(this_plugin = plugins; this_plugin; this_plugin = this_plugin->next) {
		if(!strcmp(so_name, this_plugin->so_name)) {
			fprintf(stderr, "found plugin %s\n", so_name);
			this_service->lookup_backend = dlsym(this_plugin->dlopen,
						 this_service->lookup_backend_function_name);
			if(!this_service->lookup_backend) {
				logmsg(LOG_ERR, "cannot find %s inside %s",
					this_service->lookup_backend_function_name,
					so_name);
				exit(1);
			}
			return;
		}
	}
	logmsg(LOG_ERR, "cannot match plugin %s", so_name);
	exit(1);
}

/* this is a wag -- find global, then specific to a LISTENER */
static void setup_services(void)
{
	LISTENER *this_listener;
	SERVICE *this_service;

	/* global services */
	for(this_service = services; this_service; this_service = this_service->next) {
		if(this_service->lookup_backend_so)
			find_backend_function(this_service, this_service->lookup_backend_so);
	}
	
	/* specific services */
	for(this_listener = listeners; this_listener; this_listener = this_listener->next) {
		for(this_service = this_listener->services; this_service; this_service = this_service->next) {
			if(this_service->lookup_backend_so)
				find_backend_function(this_service, this_service->lookup_backend_so);
		}
	}
			
}



static void shutdown_plugins(void)
{
	PLUGIN *this_plugin;

	/* ml error handling? */
	for(this_plugin = plugins; this_plugin; this_plugin = this_plugin->next) {
		(*this_plugin->shutdown)();
		dlclose(this_plugin->dlopen);
		/* ml -- zero/free storage? */
	}
}



/*
 * Pound: the reverse-proxy/load-balancer
 *
 * Arguments:
 *  -f config_file      configuration file - exclusive of other flags
 */

int
main(const int argc, char **argv)
{
    int                 n_listeners, i, clnt_length, clnt;
    struct pollfd       *polls;
    LISTENER            *lstn;
    pthread_t           thr;
    pthread_attr_t      attr;
    uid_t               user_id;
    gid_t               group_id;
    FILE                *fpid;
    struct sockaddr_storage clnt_addr;
    char                tmp[MAXBUF];
#ifndef SOL_TCP
    struct protoent     *pe;
#endif

    print_log = 0;
    (void)umask(077);
    control_sock = -1;
    log_facility = -1;
    logmsg(LOG_NOTICE, "starting...");

    signal(SIGHUP, h_shut);
    signal(SIGINT, h_shut);
    signal(SIGTERM, h_term);
    signal(SIGQUIT, h_term);
    signal(SIGPIPE, SIG_IGN);

    srandom(getpid());

    /* SSL stuff */
    SSL_load_error_strings();
    SSL_library_init();
    OpenSSL_add_all_algorithms();
    l_init();
    init_thr_arg();
    CRYPTO_set_id_callback(l_id);
    CRYPTO_set_locking_callback(l_lock);
    init_timer();

    /* Disable SSL Compression for OpenSSL pre-1.0.  1.0 is handled with an option in config.c */
#if OPENSSL_VERSION_NUMBER >= 0x00907000L
#ifndef SSL_OP_NO_COMPRESSION
    {
      int i,n;
      STACK_OF(SSL_COMP) *ssl_comp_methods;

      ssl_comp_methods = SSL_COMP_get_compression_methods();
      n = sk_SSL_COMP_num(ssl_comp_methods);

      for(i=n-1; i>=0; i--) {
        sk_SSL_COMP_delete(ssl_comp_methods, i);
      }
    }
#endif
#endif

    /* prepare regular expressions */
    if(regcomp(&HEADER, "^([a-z0-9!#$%&'*+.^_`|~-]+):[ \t]*(.*)[ \t]*$", REG_ICASE | REG_NEWLINE | REG_EXTENDED)
    || regcomp(&CONN_UPGRD, "(^|[ \t,])upgrade([ \t,]|$)", REG_ICASE | REG_NEWLINE | REG_EXTENDED)
    || regcomp(&CHUNK_HEAD, "^([0-9a-f]+).*$", REG_ICASE | REG_NEWLINE | REG_EXTENDED)
    || regcomp(&RESP_SKIP, "^HTTP/1.1 100.*$", REG_ICASE | REG_NEWLINE | REG_EXTENDED)
    || regcomp(&RESP_IGN, "^HTTP/1.[01] (10[1-9]|1[1-9][0-9]|204|30[456]).*$", REG_ICASE | REG_NEWLINE | REG_EXTENDED)
    || regcomp(&LOCATION, "(http|https)://([^/]+)(.*)", REG_ICASE | REG_NEWLINE | REG_EXTENDED)
    || regcomp(&AUTHORIZATION, "Authorization:[ \t]*Basic[ \t]*\"?([^ \t]*)\"?[ \t]*", REG_ICASE | REG_NEWLINE | REG_EXTENDED)
    ) {
        logmsg(LOG_ERR, "bad essential Regex - aborted");
        exit(1);
    }

#ifndef SOL_TCP
    /* for systems without the definition */
    if((pe = getprotobyname("tcp")) == NULL) {
        logmsg(LOG_ERR, "missing TCP protocol");
        exit(1);
    }
    SOL_TCP = pe->p_proto;
#endif

    /* read config */
    config_parse(argc, argv);

    
    if(log_facility != -1)
        openlog("pound", LOG_CONS | LOG_NDELAY, LOG_DAEMON);

    setup_plugins();
    setup_services();
    if(ctrl_name != NULL) {
        struct sockaddr_un  ctrl;

        memset(&ctrl, 0, sizeof(ctrl));
        ctrl.sun_family = AF_UNIX;
        strncpy(ctrl.sun_path, ctrl_name, sizeof(ctrl.sun_path) - 1);
        (void)unlink(ctrl.sun_path);
        if((control_sock = socket(PF_UNIX, SOCK_STREAM, 0)) < 0) {
            logmsg(LOG_ERR, "Control \"%s\" create: %s", ctrl.sun_path, strerror(errno));
            exit(1);
        }
        if(bind(control_sock, (struct sockaddr *)&ctrl, (socklen_t)sizeof(ctrl)) < 0) {
            logmsg(LOG_ERR, "Control \"%s\" bind: %s", ctrl.sun_path, strerror(errno));
            exit(1);
        }
        listen(control_sock, 512);
    }

    /* open listeners */
    for(lstn = listeners, n_listeners = 0; lstn; lstn = lstn->next, n_listeners++) {
        int opt;

        /* prepare the socket */
        if((lstn->sock = socket(lstn->addr.ai_family == AF_INET? PF_INET: PF_INET6, SOCK_STREAM, 0)) < 0) {
            addr2str(tmp, MAXBUF - 1, &lstn->addr, 0);
            logmsg(LOG_ERR, "HTTP socket %s create: %s - aborted", tmp, strerror(errno));
            exit(1);
        }
        opt = 1;
        setsockopt(lstn->sock, SOL_SOCKET, SO_REUSEADDR, (void *)&opt, sizeof(opt));
        if(bind(lstn->sock, lstn->addr.ai_addr, (socklen_t)lstn->addr.ai_addrlen) < 0) {
            addr2str(tmp, MAXBUF - 1, &lstn->addr, 0);
            logmsg(LOG_ERR, "HTTP socket bind %s: %s - aborted", tmp, strerror(errno));
            exit(1);
        }
        listen(lstn->sock, 512);
    }

    /* alloc the poll structures */
    if((polls = (struct pollfd *)calloc(n_listeners, sizeof(struct pollfd))) == NULL) {
        logmsg(LOG_ERR, "Out of memory for poll - aborted");
        exit(1);
    }
    for(lstn = listeners, i = 0; lstn; lstn = lstn->next, i++)
        polls[i].fd = lstn->sock;

    /* set uid if necessary */
    if(user) {
        struct passwd   *pw;

        if((pw = getpwnam(user)) == NULL) {
            logmsg(LOG_ERR, "no such user %s - aborted", user);
            exit(1);
        }
        user_id = pw->pw_uid;
    }

    /* set gid if necessary */
    if(group) {
        struct group    *gr;

        if((gr = getgrnam(group)) == NULL) {
            logmsg(LOG_ERR, "no such group %s - aborted", group);
            exit(1);
        }
        group_id = gr->gr_gid;
    }

    /* Turn off verbose messages (if necessary) */
    print_log = 0;

    if(daemonize) {
        /* daemonize - make ourselves a subprocess. */
        switch (fork()) {
            case 0:
                if(log_facility != -1) {
                    close(0);
                    close(1);
                    close(2);
                }
                break;
            case -1:
                logmsg(LOG_ERR, "fork: %s - aborted", strerror(errno));
                exit(1);
            default:
                exit(0);
        }
#ifdef  HAVE_SETSID
        (void) setsid();
#endif
    }

    /* record pid in file */
    if((fpid = fopen(pid_name, "wt")) != NULL) {
        fprintf(fpid, "%d\n", getpid());
        fclose(fpid);
    } else
        logmsg(LOG_NOTICE, "Create \"%s\": %s", pid_name, strerror(errno));

    /* chroot if necessary */
    if(root_jail) {
        if(chroot(root_jail)) {
            logmsg(LOG_ERR, "chroot: %s - aborted", strerror(errno));
            exit(1);
        }
        if(chdir("/")) {
            logmsg(LOG_ERR, "chroot/chdir: %s - aborted", strerror(errno));
            exit(1);
        }
    }

    if(group)
        if(setgid(group_id) || setegid(group_id)) {
            logmsg(LOG_ERR, "setgid: %s - aborted", strerror(errno));
            exit(1);
        }
    if(user)
        if(setuid(user_id) || seteuid(user_id)) {
            logmsg(LOG_ERR, "setuid: %s - aborted", strerror(errno));
            exit(1);
        }

    /* split off into monitor and working process if necessary */
    for(;;) {
#if SUPERVISOR
        if((son = fork()) > 0) {
            int status;

            (void)wait(&status);
            if(WIFEXITED(status))
                logmsg(LOG_ERR, "MONITOR: worker exited normally %d, restarting...", WEXITSTATUS(status));
            else if(WIFSIGNALED(status))
                logmsg(LOG_ERR, "MONITOR: worker exited on signal %d, restarting...", WTERMSIG(status));
            else
                logmsg(LOG_ERR, "MONITOR: worker exited (stopped?) %d, restarting...", status);
        } else if (son == 0) {
#endif

            /* thread stuff */
            pthread_attr_init(&attr);
            pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

#ifdef  NEED_STACK
            /* set new stack size - necessary for OpenBSD/FreeBSD and Linux NPTL */
            if(pthread_attr_setstacksize(&attr, 1 << 18)) {
                logmsg(LOG_ERR, "can't set stack size - aborted");
                exit(1);
            }
#endif
            /* start timer */
            if(pthread_create(&thr, &attr, thr_timer, NULL)) {
                logmsg(LOG_ERR, "create thr_resurect: %s - aborted", strerror(errno));
                exit(1);
            }

            /* start the controlling thread (if needed) */
            if(control_sock >= 0 && pthread_create(&thr, &attr, thr_control, NULL)) {
                logmsg(LOG_ERR, "create thr_control: %s - aborted", strerror(errno));
                exit(1);
            }

            /* pause to make sure the service threads were started */
            sleep(1);

            /* create the worker threads */
            for(i = 0; i < numthreads; i++)
                if(pthread_create(&thr, &attr, thr_http, NULL)) {
                    logmsg(LOG_ERR, "create thr_http: %s - aborted", strerror(errno));
                    exit(1);
                }

            /* pause to make sure at least some of the worker threads were started */
            sleep(1);

            /* and start working */
            for(;;) {
                if(shut_down) {
                    logmsg(LOG_NOTICE, "shutting down...");
                    for(lstn = listeners; lstn; lstn = lstn->next)
                        close(lstn->sock);
                    if(grace > 0) {
                        sleep(grace);
                        logmsg(LOG_NOTICE, "grace period expired - exiting...");
                    }
                    if(ctrl_name != NULL)
                        (void)unlink(ctrl_name);
		    shutdown_plugins();
                    exit(0);
                }
                for(lstn = listeners, i = 0; i < n_listeners; lstn = lstn->next, i++) {
                    polls[i].events = POLLIN | POLLPRI;
                    polls[i].revents = 0;
                }
                if(poll(polls, n_listeners, -1) < 0) {
                    logmsg(LOG_WARNING, "poll: %s", strerror(errno));
                } else {
                    for(lstn = listeners, i = 0; lstn; lstn = lstn->next, i++) {
                        if(polls[i].revents & (POLLIN | POLLPRI)) {
                            memset(&clnt_addr, 0, sizeof(clnt_addr));
                            clnt_length = sizeof(clnt_addr);
                            if((clnt = accept(lstn->sock, (struct sockaddr *)&clnt_addr,
                                (socklen_t *)&clnt_length)) < 0) {
                                logmsg(LOG_WARNING, "HTTP accept: %s", strerror(errno));
                            } else if(((struct sockaddr_in *)&clnt_addr)->sin_family == AF_INET
                                   || ((struct sockaddr_in *)&clnt_addr)->sin_family == AF_INET6) {
                                thr_arg arg;

                                if(lstn->disabled) {
                                    /*
                                    addr2str(tmp, MAXBUF - 1, &clnt_addr, 1);
                                    logmsg(LOG_WARNING, "HTTP disabled listener from %s", tmp);
                                    */
                                    close(clnt);
                                }
                                arg.sock = clnt;
                                arg.lstn = lstn;
                                if((arg.from_host.ai_addr = (struct sockaddr *)malloc(clnt_length)) == NULL) {
                                    logmsg(LOG_WARNING, "HTTP arg address: malloc");
                                    close(clnt);
                                    continue;
                                }
                                memcpy(arg.from_host.ai_addr, &clnt_addr, clnt_length);
                                arg.from_host.ai_addrlen = clnt_length;
                                if(((struct sockaddr_in *)&clnt_addr)->sin_family == AF_INET)
                                    arg.from_host.ai_family = AF_INET;
                                else
                                    arg.from_host.ai_family = AF_INET6;
                                if(put_thr_arg(&arg))
                                    close(clnt);
                            } else {
                                /* may happen on FreeBSD, I am told */
                                logmsg(LOG_WARNING, "HTTP connection prematurely closed by peer");
                                close(clnt);
                            }
                        }
                    }
                }
            }
#if SUPERVISOR
        } else {
            /* failed to spawn son */
            logmsg(LOG_ERR, "Can't fork worker (%s) - aborted", strerror(errno));
            exit(1);
        }
#endif
    }
}

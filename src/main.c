/*
 *   Copyright 1996, 1997, 1998, 1999, 2000 Michiel Boland.
 *
 *   Redistribution and use in source and binary forms, with or
 *   without modification, are permitted provided that the following
 *   conditions are met:
 *
 *   1. Redistributions of source code must retain the above
 *      copyright notice, this list of conditions and the following
 *      disclaimer.
 *
 *   2. Redistributions in binary form must reproduce the above
 *      copyright notice, this list of conditions and the following
 *      disclaimer in the documentation and/or other materials
 *      provided with the distribution.
 *
 *   3. The name of the author may not be used to endorse or promote
 *      products derived from this software without specific prior
 *      written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY
 *   EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 *   THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 *   PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR
 *   BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 *   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
 *   TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 *   ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *   LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 *   IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 *   THE POSSIBILITY OF SUCH DAMAGE.
 */

/* Once Around */

#include "mathopd.h"

const char server_version[] = "Mathopd/1.4b5";

volatile int gotsigterm;
volatile int gotsighup;
volatile int gotsigusr1;
volatile int gotsigusr2;
volatile int gotsigchld;
volatile int gotsigquit;
int numchildren;
time_t startuptime;
int debug;
int fcm;
int stayroot;
int my_pid;

static char *progname;
static int forked;

static const char devnull[] = "/dev/null";

static int mysignal(int sig, void(*f)(int))
{
	struct sigaction act;

	act.sa_handler = f;
	sigemptyset(&act.sa_mask);
	act.sa_flags = 0;
	return sigaction(sig, &act, 0);
}

static void die(const char *t, const char *fmt, ...)
{
	va_list ap;

	if (fmt) {
		fprintf(stderr, "%s: ", progname);
		va_start(ap, fmt);
		vfprintf(stderr, fmt, ap);
		fprintf(stderr, "\n");
		va_end(ap);
	}
	if (t)
		perror(t);
	exit(1);
}

static void startup_server(struct server *s)
{
	int onoff, rv;
	struct sockaddr_in sa;

	s->fd = socket(AF_INET, SOCK_STREAM, 0);
	if (s->fd == -1)
		die("socket", 0);
	onoff = 1;
	rv = setsockopt(s->fd, SOL_SOCKET, SO_REUSEADDR, (char *) &onoff, sizeof onoff);
	if (rv == -1)
		die("setsockopt", "cannot set re-use flag");
	fcntl(s->fd, F_SETFD, FD_CLOEXEC);
	fcntl(s->fd, F_SETFL, O_NONBLOCK);
	memset((char *) &sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	sa.sin_addr = s->addr;
	sa.sin_port = htons(s->port);
	rv = bind(s->fd, (struct sockaddr *) &sa, sizeof sa);
	if (rv == -1)
		die("bind", "cannot start up server at %s port %d", s->s_name ? s->s_name : "0", s->port);
	rv = listen(s->fd, 128);
	if (rv == -1)
		die("listen", 0);
}

static void sigterm(int sig)
{
	gotsigterm = 1;
}

static void sighup(int sig)
{
	gotsighup = 1;
}

static void sigusr1(int sig)
{
	gotsigusr1 = 1;
}

static void sigusr2(int sig)
{
	gotsigusr2 = 1;
}

static void sigchld(int sig)
{
	gotsigchld = 1;
}

static void sigquit(int sig)
{
	gotsigquit = 1;
}

int main(int argc, char *argv[])
{
	int c, i, n, am_daemon, version, pid_fd, null_fd;
	struct server *s;
	char buf[10];
	struct rlimit rl;
	struct passwd *pwd;
	const char *message;
	const char *config_filename;

	progname = argv[0];
	am_daemon = 1;
	version = 0;
	config_filename = 0;
	while ((c = getopt(argc, argv, "ndvf:")) != EOF) {
		switch(c) {
		case 'n':
			am_daemon = 0;
			break;
		case 'd':
			debug = 1;
			break;
		case 'v':
			version = 1;
			break;
		case 'f':
			if (config_filename == 0)
				config_filename = optarg;
			else
				die(0, "You may not specify more than one configuration file.");
			break;
		default:
			die(0, "usage: %s [ -ndv ] [ -f configuration_file ]", progname);
			break;
		}
	}
	if (version) {
		fprintf(stderr, "%s\n", server_version);
		return 0;
	}
	if (getrlimit(RLIMIT_NOFILE, &rl) == -1)
		die("getrlimit", 0);
	n = rl.rlim_cur = rl.rlim_max;
	setrlimit(RLIMIT_NOFILE, &rl);
	for (i = 3; i < n; i++)
		close(i);
	null_fd = open(devnull, O_RDWR);
	if (null_fd == -1)
		die("open", "Cannot open %s", devnull);
	while (null_fd < 3)
		null_fd = dup(null_fd);
	message = config(config_filename);
	if (message)
		die(0, "%s", message);
	s = servers;
	while (s) {
		startup_server(s);
		s = s->next;
	}
	if (rootdir) {
		if (chroot(rootdir) == -1)
			die("chroot", 0);
		if (chdir("/") == -1)
			die("chdir", 0);
	}
	setuid(geteuid());
	if (geteuid() == 0) {
		if (user_name == 0)
			die(0, "No user specified.");
		pwd = getpwnam(user_name);
		if (pwd == 0 || pwd->pw_uid == 0)
			die(0, "Invalid user name.");
		if (initgroups(user_name, pwd->pw_gid) == -1)
			die("initgroups", 0);
		if (setgid(pwd->pw_gid) == -1)
			die("setgid", 0);
		if (stayroot) {
			if (seteuid(pwd->pw_uid) == -1)
				die("seteuid", 0);
		} else {
			if (setuid(pwd->pw_uid) == -1)
				die("setuid", 0);
		}
	}
	if (getrlimit(RLIMIT_CORE, &rl) == -1)
		die("getrlimit", 0);
	if (coredir) {
		rl.rlim_cur = rl.rlim_max;
		if (chdir(coredir) == -1)
			die("chdir", 0);
	} else {
		rl.rlim_cur = 0;
		chdir("/");
	}
	setrlimit(RLIMIT_CORE, &rl);
	umask(fcm);
	if (pid_filename) {
		pid_fd = open(pid_filename, O_WRONLY | O_CREAT, DEFAULT_FILEMODE);
		if (pid_fd == -1)
			die("open", "Cannot open PID file");
	} else
		pid_fd = -1;
	dup2(null_fd, 0);
	dup2(null_fd, 1);
	dup2(null_fd, 2);
	close(null_fd);
	if (am_daemon) {
		if (fork())
			_exit(0);
		setsid();
		if (fork())
			_exit(0);
	}
	mysignal(SIGCHLD, sigchld);
	mysignal(SIGHUP, sighup);
	mysignal(SIGTERM, sigterm);
	mysignal(SIGINT, sigterm);
	mysignal(SIGQUIT, sigquit);
	mysignal(SIGUSR1, sigusr1);
	mysignal(SIGUSR2, sigusr2);
	mysignal(SIGPIPE, SIG_IGN);
	my_pid = getpid();
	if (pid_fd != -1) {
		ftruncate(pid_fd, 0);
		sprintf(buf, "%d\n", my_pid);
		write(pid_fd, buf, strlen(buf));
		close(pid_fd);
	}
	gotsighup = 1;
	gotsigterm = 0;
	gotsigusr1 = 0;
	gotsigusr2 = 0;
	gotsigchld = 0;
	gotsigquit = 0;
	time(&startuptime);
	time(&current_time);
	base64initialize();
	httpd_main();
	return 0;
}

int fork_request(struct request *r, int (*f)(struct request *))
{
	int fd, efd, rv;
	pid_t pid;
	char *child_filename;

	if (forked)
		_exit(1);
	pid = fork();
	switch (pid) {
	case 0:
		my_pid = getpid();
		forked = 1;
		mysignal(SIGPIPE, SIG_DFL);
		fd = r->cn->fd;
		child_filename = r->c->child_filename;
		if (child_filename == 0)
			efd = fd;
		else {
			efd = open(child_filename, O_WRONLY | O_CREAT | O_APPEND, DEFAULT_FILEMODE);
			if (efd == -1) {
				log_d("cannot open child log %s", child_filename);
				lerror("open");
				efd = fd;
			}
		}
		rv = dup2(fd, 0);
		rv = dup2(fd, 1);
		rv = dup2(efd, 2);
		rv = fcntl(0, F_SETFL, 0);
		rv = fcntl(1, F_SETFL, 0);
		if (efd == fd) {
			rv = fcntl(2, F_SETFL, 0);
		}
		rv = close(fd);
		if (efd != fd) {
			rv = close(efd);
		}
		rv = f(r);
		_exit(rv);
		break;
	case -1:
		lerror("fork");
		return 503;
	default:
		log_d("child process %d created", pid);
		r->status_line = "---";
	}
	return -1;
}

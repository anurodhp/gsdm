/*
 * gsdm -- the GNUstep display manager daemon.
 *
 * Runs as root (from launchd, init or a service manager), in the
 * foreground. For one local display it loops:
 *
 *   1. write a fresh MIT-MAGIC-COOKIE-1 server authorization file and start
 *      the X server with "-auth <file>", waiting for the server's SIGUSR1
 *      "ready" signal (an X server that inherits SIGUSR1 as SIG_IGN sends
 *      it to its parent when it is ready -- the handshake xdm uses,
 *      xdm/server.c StartServerOnce);
 *   2. start the greeter (LoginWindow.app, through the configured greeter
 *      command) with a socketpair on its fd 3;
 *   3. answer the greeter's requests: LOGIN (the password is checked
 *      against the account database with crypt(3), as xdm's non-PAM
 *      greeter/verify.c Verify does), RESTART and SHUTDOWN (run the
 *      system's reboot / halt command);
 *   4. after a successful login, start the user's session the way xdm's
 *      session.c StartClient does (setgid, setlogin, initgroups, setuid,
 *      the user environment, the user's Xauthority, chdir HOME, exec the
 *      session script), wait for it to end, take the session's cookie back
 *      out of the user's Xauthority (xdm RemoveUserAuthorization) and
 *      terminate the X server (xdm's terminateServer); then go back to 1.
 *
 * SIGTERM, SIGINT and SIGHUP stop the greeter or session and the X server,
 * and gsdm exits 0.
 *
 * Copyright (c) 2026 Anurodh Pokharel. MIT license, see LICENSE.
 */
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef __linux__
#include <shadow.h>
#endif

#include <X11/Xauth.h>

#ifndef GSDM_CONFIG_FILE
#define GSDM_CONFIG_FILE "/etc/gsdm/gsdm.conf"
#endif

/* crypt(3): declared by <unistd.h> on BSD and Darwin, by <crypt.h> on glibc. */
extern char *crypt(const char *, const char *);

/* ------------------------------------------------------------------ */
/* configuration                                                       */

struct config {
	char *display;          /* ":0" */
	char *server;           /* X server command; gsdm appends -auth <file> */
	int server_timeout;     /* seconds to wait for the server's SIGUSR1 */
	char *auth_dir;         /* server authorization file (xdm authDir) */
	char *user_auth_dir;    /* fallback user Xauthority dir (xdm userAuthDir) */
	char *greeter;          /* greeter command */
	char *greeter_home;     /* HOME for the greeter (its GNUstep defaults) */
	char *session;          /* session command, run as the user */
	char *user_path;        /* PATH for users (xdm userPath) */
	char *system_path;      /* PATH for root (xdm systemPath) */
	char *reboot;           /* Restart */
	char *halt;             /* Shut Down */
	char *disable_file;     /* if this file exists, exit 0 at once */
	char *require_bootarg;  /* Darwin: exit 0 unless kern.bootargs has <name>=<nonzero> */
	int allow_root_login;   /* xdm allowRootLogin */
	int allow_null_passwd;  /* xdm allowNullPasswd */
	int min_uid;            /* the greeter lists accounts with uid >= this */
	char *background_image; /* greeter background image (empty: the blue gradient) */
	char *title;            /* greeter title (empty: the system name) */
};

static struct config cfg;

static void
config_defaults(void)
{
	cfg.display = strdup(":0");
	cfg.server = strdup("/usr/bin/X :0 -nolisten tcp");
	cfg.server_timeout = 120;
	cfg.auth_dir = strdup("/var/lib/gsdm");
	cfg.user_auth_dir = strdup("/tmp");
	cfg.greeter = strdup("/etc/gsdm/Xgreeter");
	cfg.greeter_home = strdup("/var/lib/gsdm/greeter");
	cfg.session = strdup("/etc/gsdm/Xsession");
	cfg.user_path = strdup("/usr/local/bin:/usr/bin:/bin");
	cfg.system_path = strdup("/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin");
	cfg.reboot = strdup("/sbin/reboot");
	cfg.halt = strdup("/sbin/halt");
	cfg.disable_file = strdup("");
	cfg.require_bootarg = strdup("");
	cfg.allow_root_login = 0;
	cfg.allow_null_passwd = 0;
	cfg.min_uid = 500;
	cfg.background_image = strdup("");
	cfg.title = strdup("");
}

static void logmsg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Copies a name from the greeter for the log: control characters (a newline
 * would forge a log line) become '?', and the cut at `max` bytes never lands
 * inside a UTF-8 sequence. */
static const char *
logsafe(const char *s, char *out, size_t max)
{
	size_t n = 0;

	for (; *s != '\0' && n + 1 < max; s++) {
		unsigned char c = (unsigned char)*s;

		out[n++] = (c < 0x20 || c == 0x7f) ? '?' : (char)c;
	}
	if (((unsigned char)*s & 0xc0) == 0x80) {   /* cut inside a sequence: drop it whole */
		while (n > 0 && ((unsigned char)out[n - 1] & 0xc0) == 0x80)
			n--;
		if (n > 0)
			n--;                      /* its lead byte */
	}
	out[n] = '\0';
	return out;
}

static void
logmsg(const char *fmt, ...)
{
	char ts[32];
	time_t now = time(NULL);
	struct tm tm;
	va_list ap;

	localtime_r(&now, &tm);
	strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
	fprintf(stderr, "%s gsdm[%d]: ", ts, (int)getpid());
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	fflush(stderr);
}

static char *
trim(char *s)
{
	char *e;

	while (*s == ' ' || *s == '\t')
		s++;
	e = s + strlen(s);
	while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r'))
		*--e = '\0';
	return s;
}

static int
parse_bool(const char *v)
{
	return strcmp(v, "yes") == 0 || strcmp(v, "true") == 0 || strcmp(v, "1") == 0;
}

/* Reads "key = value" lines; a line starting with '#' is a comment. */
static int
config_load(const char *path, int must_exist)
{
	struct { const char *key; char **s; int *i; int is_bool; } keys[] = {
		{ "display", &cfg.display, NULL, 0 },
		{ "server", &cfg.server, NULL, 0 },
		{ "server_timeout", NULL, &cfg.server_timeout, 0 },
		{ "auth_dir", &cfg.auth_dir, NULL, 0 },
		{ "user_auth_dir", &cfg.user_auth_dir, NULL, 0 },
		{ "greeter", &cfg.greeter, NULL, 0 },
		{ "greeter_home", &cfg.greeter_home, NULL, 0 },
		{ "session", &cfg.session, NULL, 0 },
		{ "user_path", &cfg.user_path, NULL, 0 },
		{ "system_path", &cfg.system_path, NULL, 0 },
		{ "reboot", &cfg.reboot, NULL, 0 },
		{ "halt", &cfg.halt, NULL, 0 },
		{ "disable_file", &cfg.disable_file, NULL, 0 },
		{ "require_bootarg", &cfg.require_bootarg, NULL, 0 },
		{ "allow_root_login", NULL, &cfg.allow_root_login, 1 },
		{ "allow_null_passwd", NULL, &cfg.allow_null_passwd, 1 },
		{ "min_uid", NULL, &cfg.min_uid, 0 },
		{ "background_image", &cfg.background_image, NULL, 0 },
		{ "title", &cfg.title, NULL, 0 },
	};
	char line[2048];
	FILE *f;
	int lineno = 0;
	size_t k;

	f = fopen(path, "r");
	if (f == NULL) {
		if (must_exist || errno != ENOENT) {
			logmsg("cannot read %s: %s", path, strerror(errno));
			return -1;
		}
		return 0;
	}
	while (fgets(line, sizeof(line), f) != NULL) {
		char *key, *val, *eq;

		lineno++;
		key = trim(line);
		if (*key == '\0' || *key == '#')
			continue;
		eq = strchr(key, '=');
		if (eq == NULL) {
			logmsg("%s:%d: no '=', ignored", path, lineno);
			continue;
		}
		*eq = '\0';
		val = trim(eq + 1);
		key = trim(key);
		for (k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
			if (strcmp(key, keys[k].key) != 0)
				continue;
			if (keys[k].s != NULL) {
				free(*keys[k].s);
				*keys[k].s = strdup(val);
			} else if (keys[k].is_bool) {
				*keys[k].i = parse_bool(val);
			} else {
				*keys[k].i = atoi(val);
			}
			break;
		}
		if (k == sizeof(keys) / sizeof(keys[0]))
			logmsg("%s:%d: unknown key \"%s\", ignored", path, lineno, key);
	}
	fclose(f);
	return 0;
}

/* Splits a command on blanks, no quoting (as xdm's parseArgs). */
static char **
split_args(const char *cmd, int extra)
{
	char *copy = strdup(cmd), *p, *save = NULL;
	char **argv;
	int n = 0;

	argv = calloc(strlen(cmd) / 2 + 2 + (size_t)extra, sizeof(char *));
	for (p = strtok_r(copy, " \t", &save); p != NULL; p = strtok_r(NULL, " \t", &save))
		argv[n++] = p;
	argv[n] = NULL;
	return argv;
}

static char *
env_entry(const char *k, const char *v)
{
	size_t l = strlen(k) + strlen(v) + 2;
	char *e = malloc(l);

	snprintf(e, l, "%s=%s", k, v);
	return e;
}

/* ------------------------------------------------------------------ */
/* signals and children                                                */

static volatile sig_atomic_t got_quit;
static volatile sig_atomic_t got_usr1;

static void
on_quit(int sig)
{
	(void)sig;
	got_quit = 1;
}

static void
on_usr1(int sig)
{
	(void)sig;
	got_usr1 = 1;
}

static void
set_handler(int sig, void (*fn)(int))
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = fn;
	sigemptyset(&sa.sa_mask);
	sigaction(sig, &sa, NULL);
}

/* In a freshly forked child: default signals, own session, no stray fds. */
static void
child_setup(int keep_fd)
{
	sigset_t none;
	int fd, maxfd;

	set_handler(SIGTERM, SIG_DFL);
	set_handler(SIGINT, SIG_DFL);
	set_handler(SIGHUP, SIG_DFL);
	set_handler(SIGUSR1, SIG_DFL);
	set_handler(SIGPIPE, SIG_DFL);
	sigemptyset(&none);
	sigprocmask(SIG_SETMASK, &none, NULL);
	setsid();
	maxfd = (int)sysconf(_SC_OPEN_MAX);
	if (maxfd < 0 || maxfd > 8192)
		maxfd = 8192;
	for (fd = 3; fd < maxfd; fd++)
		if (fd != keep_fd)
			close(fd);
	fd = open("/dev/null", O_RDONLY);
	if (fd > 0) {
		dup2(fd, 0);
		close(fd);
	}
}

/* mkdir -p, the last component with mode. */
static void
make_dirs(const char *path, mode_t mode)
{
	char buf[PATH_MAX], *p;

	snprintf(buf, sizeof(buf), "%s", path);
	for (p = buf + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			(void)mkdir(buf, 0755);
			*p = '/';
		}
	}
	(void)mkdir(buf, mode);
}

static void
msleep(int ms)
{
	poll(NULL, 0, ms);
}

/* Waits up to timeout_ms (-1: forever) for pid; returns 1 once it is gone. */
static int
wait_for(pid_t pid, int timeout_ms, int *status)
{
	int waited = 0, st = 0;

	if (pid <= 0)
		return 1;
	for (;;) {
		pid_t r = waitpid(pid, &st, WNOHANG);

		if (r == pid || (r < 0 && errno == ECHILD)) {
			if (status != NULL)
				*status = (r == pid) ? st : 0;
			return 1;
		}
		if (timeout_ms >= 0 && waited >= timeout_ms)
			return 0;
		msleep(50);
		waited += 50;
	}
}

/* SIGTERM the process group pid leads, then SIGKILL after grace_ms. */
static void
stop_group(pid_t pid, int grace_ms)
{
	if (pid <= 0)
		return;
	kill(-pid, SIGTERM);
	kill(pid, SIGTERM);
	if (wait_for(pid, grace_ms, NULL))
		return;
	logmsg("pid %d did not exit after SIGTERM, sending SIGKILL", (int)pid);
	kill(-pid, SIGKILL);
	kill(pid, SIGKILL);
	wait_for(pid, -1, NULL);
}

/* ------------------------------------------------------------------ */
/* gates                                                               */

static int
bootarg_present(const char *name)
{
#ifdef __APPLE__
	/* kern.bootargs is the kernel's PE_boot_args() string. */
	char buf[1024], *p, *save = NULL;
	size_t len = sizeof(buf) - 1, nlen = strlen(name);

	if (sysctlbyname("kern.bootargs", buf, &len, NULL, 0) != 0) {
		logmsg("sysctl kern.bootargs: %s", strerror(errno));
		return 0;
	}
	buf[len < sizeof(buf) ? len : sizeof(buf) - 1] = '\0';
	for (p = strtok_r(buf, " \t", &save); p != NULL; p = strtok_r(NULL, " \t", &save))
		if (strncmp(p, name, nlen) == 0 && p[nlen] == '=' && atoi(p + nlen + 1) != 0)
			return 1;
	return 0;
#else
	(void)name;
	return 1;
#endif
}

/* ------------------------------------------------------------------ */
/* X authorization                                                     */

static unsigned char cookie[16];
static char server_auth_file[PATH_MAX];

static void
new_cookie(void)
{
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
	arc4random_buf(cookie, sizeof(cookie));
#else
	int fd = open("/dev/urandom", O_RDONLY);

	if (fd < 0 || read(fd, cookie, sizeof(cookie)) != (ssize_t)sizeof(cookie)) {
		logmsg("cannot read /dev/urandom: %s", strerror(errno));
		exit(1);
	}
	close(fd);
#endif
}

/* The display number: ":0" -> "0", "host:1.0" -> "1". */
static const char *
display_number(void)
{
	static char num[16];
	const char *c = strrchr(cfg.display, ':');
	size_t n;

	c = c ? c + 1 : cfg.display;
	n = strcspn(c, ".");
	if (n >= sizeof(num))
		n = sizeof(num) - 1;
	memcpy(num, c, n);
	num[n] = '\0';
	return num;
}

static void
fill_auth(Xauth *a, unsigned short family, const char *addr, const char *number)
{
	static char name[] = "MIT-MAGIC-COOKIE-1";

	memset(a, 0, sizeof(*a));
	a->family = family;
	a->address = (char *)addr;
	a->address_length = (unsigned short)strlen(addr);
	a->number = (char *)number;
	a->number_length = (unsigned short)strlen(number);
	a->name = name;
	a->name_length = (unsigned short)strlen(name);
	a->data = (char *)cookie;
	a->data_length = sizeof(cookie);
}

static void
local_host(char *host, size_t size)
{
	if (gethostname(host, size) != 0)
		snprintf(host, size, "localhost");
	host[size - 1] = '\0';
}

/*
 * The server's file: one FamilyWild entry, as xdm's MitGetAuth
 * (xdm/mitauth.c) makes it and SaveServerAuthorizations writes it. The
 * greeter connects with the same file, as xdm's Xsetup does (session.c
 * systemEnv: XAUTHORITY=d->authFile).
 */
static int
write_server_auth(void)
{
	Xauth a;
	FILE *f;
	int fd;
	mode_t mask;

	snprintf(server_auth_file, sizeof(server_auth_file), "%s/auth-%s", cfg.auth_dir, display_number());
	make_dirs(cfg.auth_dir, 0700);
	mask = umask(077);
	unlink(server_auth_file);
	fd = open(server_auth_file, O_WRONLY | O_CREAT | O_EXCL, 0600);
	umask(mask);
	if (fd < 0 || (f = fdopen(fd, "w")) == NULL) {
		logmsg("cannot create %s: %s", server_auth_file, strerror(errno));
		if (fd >= 0)
			close(fd);
		return -1;
	}
	fill_auth(&a, FamilyWild, "", "");
	if (!XauWriteAuth(f, &a) || fflush(f) != 0) {
		logmsg("cannot write %s", server_auth_file);
		fclose(f);
		return -1;
	}
	fclose(f);
	return 0;
}

static int
same_entry(const Xauth *e, const Xauth *a)
{
	return e->family == a->family &&
	    e->address_length == a->address_length &&
	    memcmp(e->address, a->address, a->address_length) == 0 &&
	    e->number_length == a->number_length &&
	    memcmp(e->number, a->number, a->number_length) == 0 &&
	    e->name_length == a->name_length &&
	    memcmp(e->name, a->name, a->name_length) == 0;
}

/*
 * Rewrites <path> with the session's entry first (when add) followed by
 * every old entry except one for the same family/address/display/protocol
 * -- what xdm's SetUserAuthorization and RemoveUserAuthorization do
 * (auth.c: XauLockAuth, write "<file>-n", link it over the old name). The
 * entry is xdm's DefineLocal one: FamilyLocal, this host's name, the
 * display number. The caller already runs as the user.
 */
static int
update_auth_file(const char *path, int add)
{
	char newname[PATH_MAX], host[256];
	Xauth ours, *e;
	FILE *old, *nw;
	int fd;
	mode_t mask;

	local_host(host, sizeof(host));
	fill_auth(&ours, FamilyLocal, host, display_number());
	if (XauLockAuth(path, 1, 2, 10) != LOCK_SUCCESS)
		return -1;
	snprintf(newname, sizeof(newname), "%s-n", path);
	mask = umask(077);
	unlink(newname);
	fd = open(newname, O_WRONLY | O_CREAT | O_EXCL, 0600);
	umask(mask);
	if (fd < 0 || (nw = fdopen(fd, "w")) == NULL) {
		if (fd >= 0)
			close(fd);
		XauUnlockAuth(path);
		return -1;
	}
	if (add)
		XauWriteAuth(nw, &ours);
	old = fopen(path, "r");
	if (old != NULL) {
		while ((e = XauReadAuth(old)) != NULL) {
			if (!same_entry(e, &ours))
				XauWriteAuth(nw, e);
			XauDisposeAuth(e);
		}
		fclose(old);
	}
	if (fclose(nw) != 0) {
		unlink(newname);
		XauUnlockAuth(path);
		return -1;
	}
	unlink(path);
	if (link(newname, path) == 0)
		unlink(newname);
	else
		rename(newname, path);
	XauUnlockAuth(path);
	return 0;
}

static void
user_auth_path(char *path, size_t size, const char *home)
{
	snprintf(path, size, "%s/.Xauthority", strcmp(home, "/") == 0 ? "" : home);
}

/* xdm's fallback: a private file in userAuthDir, removed when the session ends. */
static void
fallback_auth_path(char *path, size_t size, uid_t uid)
{
	snprintf(path, size, "%s/.Xauth-gsdm-%d", cfg.user_auth_dir, (int)uid);
}

/* Writes the session's cookie; returns the file the session should use. */
static char *
write_user_auth(const char *home, uid_t uid)
{
	static char path[PATH_MAX];
	char host[256];
	Xauth ours;
	FILE *f;
	int fd;

	user_auth_path(path, sizeof(path), home);
	if (update_auth_file(path, 1) == 0)
		return path;
	fallback_auth_path(path, sizeof(path), uid);
	unlink(path);
	fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
	if (fd < 0 || (f = fdopen(fd, "w")) == NULL) {
		fprintf(stderr, "gsdm: cannot write an Xauthority for the session: %s\n", strerror(errno));
		return NULL;
	}
	local_host(host, sizeof(host));
	fill_auth(&ours, FamilyLocal, host, display_number());
	XauWriteAuth(f, &ours);
	fclose(f);
	return path;
}

/* ------------------------------------------------------------------ */
/* X server                                                            */

static pid_t server_pid = -1;

/*
 * A lock left by a server that is gone (hard reset) would stop the next one.
 * Whether the old server is alive is decided by connecting to its socket:
 * the pid in the lock may have been reused by an unrelated process.
 */
static int
x_socket_alive(const char *sock)
{
	struct sockaddr_un sa;
	int fd, ok;

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return 0;
	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", sock);
	ok = connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0;
	close(fd);
	return ok;
}

static void
clear_stale_lock(void)
{
	char lock[64], sock[64], buf[16];
	ssize_t n;
	int fd;
	pid_t pid;

	snprintf(lock, sizeof(lock), "/tmp/.X%s-lock", display_number());
	snprintf(sock, sizeof(sock), "/tmp/.X11-unix/X%s", display_number());
	fd = open(lock, O_RDONLY);
	if (fd < 0)
		return;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	buf[n > 0 ? n : 0] = '\0';
	pid = (pid_t)atoi(buf);
	if (x_socket_alive(sock))
		return;         /* a live server owns it */
	logmsg("removing stale %s (pid %d is not serving) and %s", lock, (int)pid, sock);
	unlink(lock);
	unlink(sock);
}

static int
start_server(void)
{
	char **argv = split_args(cfg.server, 2);
	int n, waited = 0;

	for (n = 0; argv[n] != NULL; n++)
		;
	if (n == 0) {
		logmsg("no server command configured");
		return -1;
	}
	argv[n++] = "-auth";
	argv[n++] = server_auth_file;
	argv[n] = NULL;
	clear_stale_lock();
	got_usr1 = 0;
	server_pid = fork();
	if (server_pid < 0) {
		logmsg("fork: %s", strerror(errno));
		return -1;
	}
	if (server_pid == 0) {
		child_setup(-1);
		/* SIG_IGN SIGUSR1 tells the server to signal its parent when ready. */
		set_handler(SIGUSR1, SIG_IGN);
		execv(argv[0], argv);
		fprintf(stderr, "gsdm: exec %s: %s\n", argv[0], strerror(errno));
		_exit(1);
	}
	logmsg("started X server: %s -auth %s (pid %d)", cfg.server, server_auth_file, (int)server_pid);
	while (!got_usr1) {
		int st;

		if (waitpid(server_pid, &st, WNOHANG) == server_pid) {
			logmsg("X server exited during startup (status 0x%x)", st);
			server_pid = -1;
			return -1;
		}
		if (got_quit)
			return -1;
		if (waited >= cfg.server_timeout * 1000) {
			/* xdm serverPause: a live server that never signalled is used anyway. */
			logmsg("no ready signal from the X server after %d s, continuing", cfg.server_timeout);
			break;
		}
		msleep(100);
		waited += 100;
	}
	logmsg("X server ready");
	return 0;
}

static void
stop_server(void)
{
	if (server_pid > 0) {
		logmsg("terminating X server (pid %d)", (int)server_pid);
		stop_group(server_pid, 10000);
	}
	server_pid = -1;
}

static int
server_alive(void)
{
	int st;

	if (server_pid <= 0)
		return 0;
	if (waitpid(server_pid, &st, WNOHANG) == server_pid) {
		logmsg("X server exited (status 0x%x)", st);
		server_pid = -1;
		return 0;
	}
	return 1;
}

/* ------------------------------------------------------------------ */
/* authentication: xdm greeter/verify.c Verify, the non-PAM branch     */

struct user {
	char name[256];
	char home[PATH_MAX];
	char shell[PATH_MAX];
	uid_t uid;
	gid_t gid;
};

enum verdict { V_OK = 0, V_UNKNOWN = 1, V_DENIED = 2 };

/* A login shell: executable, not false/nologin, and in /etc/shells if there is one. */
static int
real_shell(const char *sh)
{
	const char *base;
	char line[1024];
	FILE *f;
	int listed = 0;

	if (sh == NULL || *sh == '\0' || access(sh, X_OK) != 0)
		return 0;
	base = strrchr(sh, '/');
	base = base ? base + 1 : sh;
	if (strcmp(base, "false") == 0 || strcmp(base, "nologin") == 0)
		return 0;
	f = fopen("/etc/shells", "r");
	if (f == NULL)
		return 1;
	while (!listed && fgets(line, sizeof(line), f) != NULL) {
		line[strcspn(line, " \t\r\n")] = '\0';
		if (line[0] != '#' && strcmp(line, sh) == 0)
			listed = 1;
	}
	fclose(f);
	return listed;
}

/* The checks the greeter's list implies but cannot enforce: "Other..." takes any name. */
static int
account_allowed(const struct passwd *p)
{
	if (p->pw_uid == 0)
		return cfg.allow_root_login;
	if ((int)p->pw_uid < cfg.min_uid || p->pw_name[0] == '_' || !real_shell(p->pw_shell))
		return 0;
	if (access("/etc/nologin", F_OK) == 0)
		return 0;
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
	if (p->pw_expire != 0 && p->pw_expire < time(NULL))
		return 0;
#endif
	return 1;
}

/*
 * Runs in a child: a long-lived gsdm must not keep the account database's
 * state (an open pwd.db, caches) between logins, or an account added or a
 * password changed since gsdm started would not be seen. Writes the
 * account to wfd on success; the exit status is the verdict.
 */
static void
verify_in_child(const char *name, const char *password, int wfd)
{
	struct passwd *p = getpwnam(name);
	const char *hash;
	char *crypted;
	struct user u;
	int ok;

	if (p == NULL) {
		(void)crypt(password, "xx");    /* take as long as a real account */
		_exit(V_UNKNOWN);
	}
	hash = p->pw_passwd ? p->pw_passwd : "";
#ifdef __linux__
	{
		struct spwd *sp = getspnam(name);

		if (sp != NULL && sp->sp_pwdp != NULL)
			hash = sp->sp_pwdp;
	}
#endif
	crypted = crypt(password, hash);
	ok = crypted != NULL && strcmp(crypted, hash) == 0;
	if (!ok && cfg.allow_null_passwd && *hash == '\0' && *password == '\0')
		ok = 1;
	if (!ok || !account_allowed(p))
		_exit(V_DENIED);
	memset(&u, 0, sizeof(u));
	snprintf(u.name, sizeof(u.name), "%s", p->pw_name);
	snprintf(u.home, sizeof(u.home), "%s", p->pw_dir && *p->pw_dir ? p->pw_dir : "/");
	snprintf(u.shell, sizeof(u.shell), "%s", p->pw_shell && *p->pw_shell ? p->pw_shell : "/bin/sh");
	u.uid = p->pw_uid;
	u.gid = p->pw_gid;
	if (write(wfd, &u, sizeof(u)) != (ssize_t)sizeof(u))
		_exit(V_DENIED);
	_exit(V_OK);
}

static enum verdict
verify_password(const char *name, const char *password, struct user *u)
{
	int fds[2], st = 0;
	size_t got = 0;
	pid_t pid;

	if (*name == '\0')
		return V_UNKNOWN;
	if (pipe(fds) != 0)
		return V_DENIED;
	pid = fork();
	if (pid < 0) {
		close(fds[0]);
		close(fds[1]);
		return V_DENIED;
	}
	if (pid == 0) {
		close(fds[0]);
		child_setup(fds[1]);
		verify_in_child(name, password, fds[1]);
	}
	close(fds[1]);
	while (got < sizeof(*u)) {
		ssize_t n = read(fds[0], (char *)u + got, sizeof(*u) - got);

		if (n <= 0)
			break;
		got += (size_t)n;
	}
	close(fds[0]);
	wait_for(pid, -1, &st);
	if (WIFEXITED(st) && WEXITSTATUS(st) == V_OK && got == sizeof(*u))
		return V_OK;
	memset(u, 0, sizeof(*u));
	return WIFEXITED(st) && WEXITSTATUS(st) == V_UNKNOWN ? V_UNKNOWN : V_DENIED;
}

/* ------------------------------------------------------------------ */
/* session: xdm session.c StartClient and SessionExit                  */

static pid_t
start_session(struct user *u)
{
	pid_t pid = fork();

	if (pid < 0) {
		logmsg("fork: %s", strerror(errno));
		return -1;
	}
	if (pid == 0) {
		char **argv, *env[10], *xauth;
		int n = 0;

		child_setup(-1);
		/* StartClient without HAVE_SETUSERCONTEXT: setgid, setlogin,
		 * initgroups, setuid -- the order login(1) uses as well. */
		if (setgid(u->gid) != 0) {
			fprintf(stderr, "gsdm: setgid %d: %s\n", (int)u->gid, strerror(errno));
			_exit(1);
		}
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
		if (setlogin(u->name) != 0) {
			fprintf(stderr, "gsdm: setlogin %s: %s\n", u->name, strerror(errno));
			_exit(1);
		}
#endif
		if (initgroups(u->name, (int)u->gid) != 0) {
			fprintf(stderr, "gsdm: initgroups %s: %s\n", u->name, strerror(errno));
			_exit(1);
		}
		if (setuid(u->uid) != 0) {
			fprintf(stderr, "gsdm: setuid %d: %s\n", (int)u->uid, strerror(errno));
			_exit(1);
		}
		/* After setuid, as xdm's SetUserAuthorization runs. */
		xauth = write_user_auth(u->home, u->uid);
		if (xauth == NULL)
			_exit(1);
		if (chdir(u->home) != 0) {
			fprintf(stderr, "gsdm: cannot chdir to %s, using /\n", u->home);
			(void)chdir("/");
			snprintf(u->home, sizeof(u->home), "/");
		}
		/* xdm verify.c userEnv: DISPLAY, HOME, LOGNAME, USER, PATH, SHELL.
		 * XAUTHORITY is always exported here (xdm only exports it for
		 * its temp-file fallback), so a session script that moves HOME
		 * still finds the cookie. */
		env[n++] = env_entry("DISPLAY", cfg.display);
		env[n++] = env_entry("HOME", u->home);
		env[n++] = env_entry("LOGNAME", u->name);
		env[n++] = env_entry("USER", u->name);
		env[n++] = env_entry("PATH", u->uid == 0 ? cfg.system_path : cfg.user_path);
		env[n++] = env_entry("SHELL", u->shell);
		if (xauth != NULL)
			env[n++] = env_entry("XAUTHORITY", xauth);
		env[n] = NULL;
		argv = split_args(cfg.session, 0);
		/* Not the daemon's log: the session script redirects its own output. */
		{
			int nul = open("/dev/null", O_WRONLY);

			if (nul >= 0) {
				dup2(nul, 1);
				dup2(nul, 2);
				if (nul > 2)
					close(nul);
			}
		}
		umask(022);
		execve(argv[0], argv, env);
		/* stderr is /dev/null now; the failure is only in the exit status. */
		_exit(1);
	}
	return pid;
}

/* SessionExit: take the session's cookie out of the user's file, as the user. */
static void
remove_user_auth(const struct user *u)
{
	pid_t pid = fork();

	if (pid == 0) {
		char path[PATH_MAX];

		child_setup(-1);
		if (setgid(u->gid) != 0 || initgroups(u->name, (int)u->gid) != 0 ||
		    setuid(u->uid) != 0)
			_exit(1);
		user_auth_path(path, sizeof(path), u->home);
		if (access(path, F_OK) == 0)
			update_auth_file(path, 0);
		fallback_auth_path(path, sizeof(path), u->uid);
		unlink(path);
		_exit(0);
	}
	if (pid > 0 && !wait_for(pid, 10000, NULL)) {
		kill(pid, SIGKILL);
		wait_for(pid, -1, NULL);
	}
}

/*
 * A session asks for the machine to restart or shut down by ending with one of these exit statuses (the session script
 * does it for a desktop that leaves the request in the user's home; Naples' Hexley menu is one). It is the Restart and
 * Shut Down buttons of the login window, which anyone at the screen can press, so it gives the user nothing new.
 */
enum { SESSION_RESTART = 10, SESSION_SHUTDOWN = 11 };

/* Waits for the session; stops it if the X server or gsdm goes away. Returns the session's exit status, or -1. */
static int
wait_session(pid_t pid)
{
	for (;;) {
		int st;

		if (waitpid(pid, &st, WNOHANG) == pid) {
			logmsg("session ended (status 0x%x)", st);
			/* What it left running must not outlive the login. */
			kill(-pid, SIGHUP);
			msleep(300);
			kill(-pid, SIGKILL);
			return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
		}
		if (got_quit || !server_alive()) {
			logmsg("stopping session (pid %d)", (int)pid);
			kill(-pid, SIGHUP);
			stop_group(pid, 5000);
			return -1;
		}
		msleep(250);
	}
}

/* ------------------------------------------------------------------ */
/* greeter                                                             */

static pid_t greeter_pid = -1;
static pid_t greeter_pgid = -1;         /* its process group (setsid) */
static int greeter_fd = -1;

static int
start_greeter(void)
{
	int sv[2];

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
		logmsg("socketpair: %s", strerror(errno));
		return -1;
	}
	make_dirs(cfg.greeter_home, 0700);
	greeter_pid = fork();
	if (greeter_pid < 0) {
		logmsg("fork: %s", strerror(errno));
		close(sv[0]);
		close(sv[1]);
		return -1;
	}
	if (greeter_pid == 0) {
		char **argv = split_args(cfg.greeter, 0);
		char *env[16], minuid[16];
		int n = 0;

		close(sv[0]);
		if (sv[1] != 3) {
			dup2(sv[1], 3);
			close(sv[1]);
		}
		child_setup(3);
		snprintf(minuid, sizeof(minuid), "%d", cfg.min_uid);
		env[n++] = env_entry("DISPLAY", cfg.display);
		env[n++] = env_entry("XAUTHORITY", server_auth_file);
		env[n++] = env_entry("HOME", cfg.greeter_home);
		env[n++] = env_entry("USER", "root");
		env[n++] = env_entry("LOGNAME", "root");
		env[n++] = env_entry("SHELL", "/bin/sh");
		env[n++] = env_entry("PATH", cfg.system_path);
		env[n++] = env_entry("GSDM_FD", "3");
		env[n++] = env_entry("GSDM_MIN_UID", minuid);
		env[n++] = env_entry("GSDM_BACKGROUND_IMAGE", cfg.background_image);
		env[n++] = env_entry("GSDM_TITLE", cfg.title);
		env[n] = NULL;
		execve(argv[0], argv, env);
		fprintf(stderr, "gsdm: exec greeter %s: %s\n", argv[0], strerror(errno));
		_exit(1);
	}
	close(sv[1]);
	greeter_fd = sv[0];
	fcntl(greeter_fd, F_SETFD, FD_CLOEXEC);
	greeter_pgid = greeter_pid;
	logmsg("started greeter %s (pid %d)", cfg.greeter, (int)greeter_pid);
	return 0;
}

/* force: kill it now; otherwise give a greeter that said goodbye time to exit. */
static void
close_greeter(int force)
{
	if (greeter_fd >= 0) {
		close(greeter_fd);
		greeter_fd = -1;
	}
	if (greeter_pid > 0 && (force || !wait_for(greeter_pid, 5000, NULL)))
		stop_group(greeter_pid, 3000);
	/* Helpers the greeter left in its process group. */
	if (greeter_pgid > 0)
		kill(-greeter_pgid, SIGTERM);
	greeter_pid = -1;
	greeter_pgid = -1;
}

static void
reply(const char *s)
{
	if (greeter_fd >= 0)
		(void)write(greeter_fd, s, strlen(s));
}

/* Restart / Shut Down: hand over to the system's command, which asks init. */
static void
run_power_command(const char *what, const char *cmd)
{
	char **argv = split_args(cmd, 0);

	logmsg("%s requested: running %s", what, cmd);
	close_greeter(1);
	stop_server();
	execv(argv[0], argv);
	logmsg("exec %s: %s", argv[0], strerror(errno));
}

enum outcome { GREETER_LOGIN, GREETER_DIED, GREETER_RESTART_SERVER, GREETER_QUIT };

/*
 * Serves one greeter until a login succeeds or something ends it.
 * Requests are NUL-terminated fields: "LOGIN\0<user>\0<password>\0",
 * "RESTART\0", "SHUTDOWN\0". Replies to LOGIN are "OK\n" or "FAIL\n".
 */
static enum outcome
serve_greeter(struct user *u)
{
	char buf[2048];
	size_t have = 0;
	int skipping = 0;        /* dropping the rest of an oversized request */

	for (;;) {
		struct pollfd pfd;
		ssize_t got;
		int st;

		if (got_quit)
			return GREETER_QUIT;
		if (!server_alive())
			return GREETER_RESTART_SERVER;
		if (waitpid(greeter_pid, &st, WNOHANG) == greeter_pid) {
			logmsg("greeter exited (status 0x%x)", st);
			greeter_pid = -1;
			return GREETER_DIED;
		}
		pfd.fd = greeter_fd;
		pfd.events = POLLIN;
		pfd.revents = 0;
		if (poll(&pfd, 1, 500) <= 0)
			continue;
		if (have >= sizeof(buf)) {
			logmsg("greeter request too long, ignored");
			memset(buf, 0, sizeof(buf));
			have = 0;
			skipping = 1;
			reply("FAIL\n");
		}
		got = read(greeter_fd, buf + have, sizeof(buf) - have);
		if (got <= 0) {
			/* It closed its end: collect it. */
			if (wait_for(greeter_pid, 5000, &st))
				logmsg("greeter closed the connection and exited (status 0x%x)", st);
			else
				logmsg("greeter closed the connection");
			greeter_pid = -1;
			memset(buf, 0, sizeof(buf));
			return GREETER_DIED;
		}
		if (skipping) {
			char *z = memchr(buf + have, '\0', (size_t)got);
			size_t drop;

			if (z == NULL) {
				memset(buf + have, 0, (size_t)got);
				continue;
			}
			drop = (size_t)(z + 1 - (buf + have));
			memmove(buf + have, buf + have + drop, (size_t)got - drop);
			memset(buf + have + got - drop, 0, drop);
			got -= (ssize_t)drop;
			skipping = 0;
		}
		have += (size_t)got;
		for (;;) {
			char *f[3];
			size_t i, nf = 0, end = 0, need;

			for (i = 0; i < have && nf < 3; i++) {
				if (buf[i] == '\0') {
					f[nf++] = buf + end;
					end = i + 1;
				}
			}
			if (nf == 0)
				break;
			need = strcmp(f[0], "LOGIN") == 0 ? 3 : 1;
			if (nf < need)
				break;
			end = (size_t)(f[need - 1] - buf) + strlen(f[need - 1]) + 1;
			if (need == 3) {
				enum verdict v = verify_password(f[1], f[2], u);

				memset(f[2], 0, strlen(f[2]));
				if (v == V_OK) {
					logmsg("login: %s", u->name);
					reply("OK\n");
					memset(buf, 0, sizeof(buf));
					return GREETER_LOGIN;
				}
				/* Only a name that is an account is logged: anything
				 * else may be a password typed into the name box. */
				if (v == V_DENIED)
				{
					char safe[80];

					logmsg("failed login for \"%s\"", logsafe(f[1], safe, sizeof(safe)));
				}
				else
					logmsg("failed login for an unknown name");
				msleep(1000);   /* slow down guessing */
				reply("FAIL\n");
			} else if (strcmp(f[0], "RESTART") == 0) {
				run_power_command("restart", cfg.reboot);
				return GREETER_RESTART_SERVER;  /* exec failed */
			} else if (strcmp(f[0], "SHUTDOWN") == 0) {
				run_power_command("shut down", cfg.halt);
				return GREETER_RESTART_SERVER;
			} else {
				logmsg("unknown greeter request \"%s\"", f[0]);
			}
			memmove(buf, buf + end, have - end);
			memset(buf + have - end, 0, end);
			have -= end;
		}
	}
}

/* ------------------------------------------------------------------ */

/* Too many failures in a minute: stop, and let the service manager decide. */
static int
too_many_failures(void)
{
	static time_t window_start;
	static int failures;
	time_t now = time(NULL);

	if (now - window_start >= 60) {
		window_start = now;
		failures = 0;
	}
	return ++failures >= 5;
}

int
main(int argc, char **argv)
{
	const char *config_file = GSDM_CONFIG_FILE;
	int must_exist = 0, i;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-config") == 0 && i + 1 < argc) {
			config_file = argv[++i];
			must_exist = 1;
		} else if (strcmp(argv[i], "-nodaemon") == 0) {
			/* gsdm always runs in the foreground */
		} else {
			fprintf(stderr, "usage: gsdm [-config file]\n");
			return 2;
		}
	}
	config_defaults();
	if (config_load(config_file, must_exist) != 0)
		return 1;
	if (getuid() != 0) {
		logmsg("must be run as root");
		return 1;
	}
	if (*cfg.disable_file && access(cfg.disable_file, F_OK) == 0) {
		logmsg("%s exists, not starting", cfg.disable_file);
		return 0;
	}
	if (*cfg.require_bootarg && !bootarg_present(cfg.require_bootarg)) {
		logmsg("no %s=<nonzero> in the kernel boot-args, not starting", cfg.require_bootarg);
		return 0;
	}
	set_handler(SIGTERM, on_quit);
	set_handler(SIGINT, on_quit);
	set_handler(SIGHUP, on_quit);
	set_handler(SIGUSR1, on_usr1);
	set_handler(SIGPIPE, SIG_IGN);
	logmsg("starting on display %s", cfg.display);

	while (!got_quit) {
		new_cookie();
		if (write_server_auth() != 0)
			return 1;
		if (start_server() != 0) {
			stop_server();
			if (got_quit)
				break;
			if (too_many_failures()) {
				logmsg("the X server keeps failing, giving up");
				return 1;
			}
			sleep(5);
			continue;
		}
		for (;;) {
			struct user u;
			enum outcome o;
			int req = -1;

			memset(&u, 0, sizeof(u));
			if (start_greeter() != 0) {
				sleep(5);
				break;
			}
			o = serve_greeter(&u);
			if (o == GREETER_LOGIN) {
				pid_t s;

				close_greeter(0);
				s = start_session(&u);
				if (s > 0) {
					logmsg("session for %s started (pid %d)", u.name, (int)s);
					req = wait_session(s);
					remove_user_auth(&u);
					if (req == SESSION_RESTART) run_power_command("restart (asked by the session)", cfg.reboot);
					else if (req == SESSION_SHUTDOWN) run_power_command("shut down (asked by the session)", cfg.halt);
				}
				break;          /* xdm terminateServer: a fresh server per login */
			}
			close_greeter(1);
			if (o == GREETER_RESTART_SERVER) {
				if (too_many_failures()) {
					logmsg("the X server keeps dying, giving up");
					stop_server();
					return 1;
				}
				sleep(1);
			}
			if (o != GREETER_DIED)
				break;
			if (too_many_failures()) {
				logmsg("the greeter keeps failing, giving up");
				stop_server();
				return 1;
			}
			sleep(1);
		}
		stop_server();
	}
	close_greeter(1);
	stop_server();
	logmsg("exiting");
	return 0;
}

/* See LICENSE file for copyright and license details. */
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/syscall.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <shadow.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "config.h"
#include "passwd.h"
#include "text.h"
#include "util.h"

static int
open_tty(void)
{
	int fd;
	const char *tty;

	/* SynapseOS /dev/tty is a global console, while SSH uses a PTY. */
	if (isatty(STDIN_FILENO))return dup(STDIN_FILENO);
	if (isatty(STDERR_FILENO))return dup(STDERR_FILENO);
	fd = open("/dev/tty", O_RDWR);
	if (fd >= 0)
		return fd;

	tty = getenv("SHELL_TTY");
	if (tty && tty[0]) {
		fd = open(tty, O_RDWR);
		if (fd >= 0)
			return fd;
	}

	fd = open("/dev/console", O_RDWR);
	if (fd >= 0)
		return fd;

	fd = open("/dev/tty0", O_RDWR);
	return fd;
}

static char *
tty_getpass(const char *prompt)
{
	static char buf[BUFSIZ];
	struct termios old, new;
	int ttyfd;
	ssize_t len;

	ttyfd = open_tty();
	if (ttyfd < 0) {
		weprintf("cannot open any tty device:");
		return NULL;
	}

	fputs(prompt, stderr);
	fflush(stderr);

	if (tcgetattr(ttyfd, &old) < 0) {
		weprintf("tcgetattr:");
		close(ttyfd);
		return NULL;
	}
	new = old;
	new.c_lflag &= ~(ECHO | ECHONL);
	if (tcsetattr(ttyfd, TCSAFLUSH, &new) < 0) {
		weprintf("tcsetattr:");
		close(ttyfd);
		return NULL;
	}

	len = read(ttyfd, buf, sizeof(buf) - 1);
	tcsetattr(ttyfd, TCSANOW, &old);
	fputc('\n', stderr);
	fflush(stderr);
	close(ttyfd);

	if (len < 0) {
		weprintf("read:");
		return NULL;
	}
	buf[len] = '\0';
	if (len > 0 && buf[len - 1] == '\n')
		buf[len - 1] = '\0';

	return buf;
}

static void
flush_tty(void)
{
	int fd = open_tty();
	if (fd >= 0) {
		tcflush(fd, TCIFLUSH);
		close(fd);
	}
}

static FILE *
spw_get_file(const char *user)
{
	FILE *fp = NULL;
	char file[PATH_MAX];
	int r;

	r = snprintf(file, sizeof(file), "/etc/tcb/%s/shadow", user);
	if (r < 0 || (size_t)r >= sizeof(file))
		eprintf("snprintf:");
	fp = fopen(file, "r+");
	if (!fp)
		fp = fopen("/etc/shadow", "r+");
	return fp;
}

static int
spw_write_file(FILE *fp, const struct spwd *spw, char *pwhash)
{
	struct spwd *spwent;
	int r = -1, w = 0;
	FILE *tfp = NULL;

	tfp = tmpfile();
	if (!tfp) {
		weprintf("tmpfile:");
		goto cleanup;
	}
	while ((spwent = fgetspent(fp))) {
		if (strcmp(spwent->sp_namp, spw->sp_namp) == 0) {
			spwent->sp_pwdp = pwhash;
			w++;
		}
		errno = 0;
		if (putspent(spwent, tfp) == -1) {
			weprintf("putspent:");
			goto cleanup;
		}
	}
	if (!w) {
		weprintf("shadow: no matching entry to write to\n");
		goto cleanup;
	}
	fflush(tfp);

	if (fseek(fp, 0, SEEK_SET) == -1 || fseek(tfp, 0, SEEK_SET) == -1) {
		weprintf("fseek:");
		goto cleanup;
	}

	concat(tfp, "tmpfile", fp, "shadow");
	ftruncate(fileno(fp), ftell(tfp));

	r = 0;
cleanup:
	if (tfp)
		fclose(tfp);
	return r;
}

static int
pw_write_file(FILE *fp, const struct passwd *pw, char *pwhash)
{
	struct passwd *pwent;
	int r = -1, w = 0;
	FILE *tfp = NULL;

	tfp = tmpfile();
	if (!tfp) {
		weprintf("tmpfile:");
		goto cleanup;
	}
	while ((pwent = fgetpwent(fp))) {
		if (strcmp(pwent->pw_name, pw->pw_name) == 0) {
			pwent->pw_passwd = pwhash;
			w++;
		}
		errno = 0;
		if (putpwent(pwent, tfp) == -1) {
			weprintf("putpwent:");
			goto cleanup;
		}
	}
	if (!w) {
		weprintf("passwd: no matching entry to write to\n");
		goto cleanup;
	}
	fflush(tfp);

	if (fseek(fp, 0, SEEK_SET) == -1 || fseek(tfp, 0, SEEK_SET) == -1) {
		weprintf("fseek:");
		goto cleanup;
	}

	concat(tfp, "tmpfile", fp, "passwd");
	ftruncate(fileno(fp), ftell(tfp));

	r = 0;
cleanup:
	if (tfp)
		fclose(tfp);
	return r;
}

static void
gensalt(char *s)
{
	static const char b64[] = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
	uint8_t buf[12];
	uint32_t n;
	int i;

	if (syscall(SYS_getrandom, buf, sizeof(buf), 0) < 0)
		eprintf("getrandom:");
	for (i = 0; i < 12; i += 3) {
		n = buf[i] << 16 | buf[i+1] << 8 | buf[i+2];
		*s++ = b64[n%64]; n /= 64;
		*s++ = b64[n%64]; n /= 64;
		*s++ = b64[n%64]; n /= 64;
		*s++ = b64[n];
	}
	*s++ = '\0';
}

static void
usage(void)
{
	eprintf("usage: %s [username [password]]\n", argv0);
}

int
main(int argc, char *argv[])
{
	char *cryptpass = NULL;
	char *inpass, *p, salt[sizeof(PW_CIPHER) + 16] = PW_CIPHER;
	struct passwd *pw;
	struct spwd *spw = NULL;
	FILE *fp = NULL;
	int r = -1, status = 1;
	const char *target_user = NULL;
	const char *direct_password = NULL;

	ARGBEGIN {
	default:
		usage();
	} ARGEND;

	pw_init();
	umask(077);

	if (argc >= 1)
		target_user = argv[0];
	if (argc >= 2)
		direct_password = argv[1];
	if (argc > 2)
		usage();

	errno = 0;
	if (!target_user)
		pw = getpwuid(getuid());
	else
		pw = getpwnam(target_user);
	if (!pw) {
		if (errno)
			eprintf("getpwnam: %s:", target_user ? target_user : "");
		else
			eprintf("who are you?\n");
	}

	if (pw->pw_passwd[0] == 'x' && pw->pw_passwd[1] == '\0') {
		errno = 0;
		spw = getspnam(pw->pw_name);
		if (!spw) {
			if (errno)
				eprintf("getspnam: %s:", pw->pw_name);
			else
				eprintf("who are you?\n");
		}
	}

	if (direct_password) {
		if (getuid() != 0)
			eprintf("only root can set password non-interactively\n");
		inpass = (char *)direct_password;
	} else {
		flush_tty();

		if (getuid() != 0) {
			char *prevhash = NULL;
			char *cryptpass_old = NULL;

			if (pw->pw_passwd[0] == '!' ||
			    pw->pw_passwd[0] == '*')
				eprintf("denied\n");
			if (pw->pw_passwd[0] == '\0')
				goto newpass_interactive;
			if (pw->pw_passwd[0] == 'x' &&
			    pw->pw_passwd[1] == '\0')
				prevhash = spw->sp_pwdp;
			else
				prevhash = pw->pw_passwd;

			printf("Changing password for %s\n", pw->pw_name);
			inpass = tty_getpass("Old password: ");
			if (!inpass)
				eprintf("getpass:");
			if (inpass[0] == '\0')
				eprintf("no password supplied\n");
			p = crypt(inpass, prevhash);
			if (!p)
				eprintf("crypt:");
			cryptpass_old = estrdup(p);
			if (strcmp(cryptpass_old, prevhash) != 0)
				eprintf("incorrect password\n");
			free(cryptpass_old);
		}

newpass_interactive:
		inpass = tty_getpass("Enter new password: ");
		if (!inpass)
			eprintf("getpass:");
		if (inpass[0] == '\0')
			eprintf("no password supplied\n");

		gensalt(salt + strlen(salt));
		p = crypt(inpass, salt);
		if (!p)
			eprintf("crypt:");
		cryptpass = estrdup(p);

		flush_tty();

		inpass = tty_getpass("Retype new password: ");
		if (!inpass)
			eprintf("getpass:");
		if (inpass[0] == '\0')
			eprintf("no password supplied\n");
		p = crypt(inpass, salt);
		if (!p)
			eprintf("crypt:");
		if (strcmp(cryptpass, p) != 0)
			eprintf("passwords don't match\n");
	}

	if (direct_password) {
		gensalt(salt + strlen(salt));
		p = crypt(direct_password, salt);
		if (!p)
			eprintf("crypt:");
		cryptpass = estrdup(p);
	}

	fp = spw_get_file(pw->pw_name);
	if (fp) {
		r = spw_write_file(fp, spw, cryptpass);
	} else {
		fp = fopen("/etc/passwd", "r+");
		if (fp)
			r = pw_write_file(fp, pw, cryptpass);
		else
			weprintf("fopen:");
	}
	if (!r)
		status = 0;

	if (fp)
		fclose(fp);
	free(cryptpass);

	return status;
}
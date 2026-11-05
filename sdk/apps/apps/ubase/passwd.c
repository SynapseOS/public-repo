// passwd.c: Смена пароля с сохранением файла учётных записей
// Автор изменений SynapseOS: Елчинян Арен Артурович, 2026
// Архитектура: x86_64, ABI: Linux ELF64 musl
// Подсистема: учётные записи
// Первое изменение: дата исходной адаптации не установлена
// Последнее изменение: 03.11.2026
// Доступ: общедоступный
/* See LICENSE file for copyright and license details. */
#include <sys/ioctl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/syscall.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <signal.h>
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

static volatile sig_atomic_t password_signal;

// Прервать ввод, чтобы обычный путь вернул режим терминала
static void password_interrupted(int number) {
    password_signal = number;
}

// Прочитать строку без эха и вернуть режим терминала перед обработкой сигнала
static char *tty_getpass(const char *prompt) {
    static char buf[BUFSIZ];
    struct termios old, hidden;
    const int signals[] = {SIGINT, SIGTERM, SIGHUP, SIGQUIT};
    struct sigaction previous[4], action;
    unsigned installed = 0;
    int changed = 0, valid = 0;
    size_t length = 0;
    explicit_bzero(buf, sizeof(buf));
    int ttyfd = open_tty();
    if (ttyfd < 0 || tcgetattr(ttyfd, &old) != 0) {
        if (ttyfd >= 0) {
            close(ttyfd);
        }
        return NULL;
    }

    memset(&action, 0, sizeof(action));
    action.sa_handler = password_interrupted;
    sigemptyset(&action.sa_mask);
    password_signal = 0;
    for (; installed < 4; installed++) {
        if (sigaction(signals[installed], &action, &previous[installed]) != 0) {
            goto done;
        }
    }

    hidden = old;
    hidden.c_lflag = (hidden.c_lflag | ICANON) & ~(ECHO | ECHONL);
    if (tcsetattr(ttyfd, TCSAFLUSH, &hidden) != 0) {
        goto done;
    }
    changed = 1;
    fputs(prompt, stderr);
    fflush(stderr);

    while (password_signal == 0) {
        char byte;
        ssize_t count = read(ttyfd, &byte, 1);
        if (count < 0 && errno == EINTR && password_signal == 0) {
            continue;
        }
        if (count != 1) {
            errno = count == 0 ? EIO : errno;
            break;
        }
        if (byte == '\n' || byte == '\r') {
            valid = 1;
            break;
        }
        if (length >= sizeof(buf) - 1) {
            errno = EOVERFLOW;
            break;
        }
        buf[length++] = byte;
    }
    buf[length] = '\0';

done:
    {
        int error = errno;
        if (changed && tcsetattr(ttyfd, TCSAFLUSH, &old) != 0) {
            error = errno;
            valid = 0;
        }
        close(ttyfd);
        for (unsigned i = 0; i < installed; i++) {
            sigaction(signals[i], &previous[i], NULL);
        }
        fputc('\n', stderr);
        fflush(stderr);
        if (!valid || password_signal != 0) {
            explicit_bzero(buf, sizeof(buf));
        }
        errno = error;
    }
    if (password_signal != 0) {
        raise(password_signal);
        return NULL;
    }
    return valid ? buf : NULL;
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

// Заблокировать всю замену учётных данных на постоянном файле
static int lock_password_files(void) {
    int fd = open("/etc/.pwd.lock", O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        return -1;
    }

    struct stat info;
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != 0 ||
        info.st_nlink != 1 || (info.st_mode & 0777) != 0600) {
        close(fd);
        errno = EINVAL;
        return -1;
    }

    while (flock(fd, LOCK_EX) != 0) {
        if (errno != EINTR) {
            int error = errno;
            close(fd);
            errno = error;
            return -1;
        }
    }

    return fd;
}

// Открыть только обычный файл root, не переходя по символьной ссылке
static FILE *open_account_file(const char *path) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        return NULL;
    }

    struct stat info;
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != 0 || info.st_nlink != 1) {
        close(fd);
        errno = EINVAL;
        return NULL;
    }

    FILE *file = fdopen(fd, "r");
    if (file == NULL) {
        int error = errno;
        close(fd);
        errno = error;
    }

    return file;
}

// Найти используемый shadow, не подменяя ошибку доступа другим файлом
static FILE *spw_get_file(const char *user, char path[PATH_MAX]) {
    int length = snprintf(path, PATH_MAX, "/etc/tcb/%s/shadow", user);
    if (length < 0 || length >= PATH_MAX) {
        errno = ENAMETOOLONG;
        return NULL;
    }

    FILE *file = open_account_file(path);
    if (file != NULL || (errno != ENOENT && errno != ENOTDIR)) {
        return file;
    }

    strcpy(path, "/etc/shadow");
    return open_account_file(path);
}

// Сохранить новый файл целиком, затем заменить старое имя и синхронизировать каталог
static int write_password_file(FILE *source, const char *path, const char *user, const char *hash) {
    char directory[PATH_MAX], temporary[PATH_MAX];
    const char *name = strrchr(path, '/');
    if (name == NULL || name == path || (size_t)(name - path) >= sizeof(directory)) {
        errno = EINVAL;
        return -1;
    }

    size_t directory_length = (size_t)(name - path);
    memcpy(directory, path, directory_length);
    directory[directory_length] = '\0';
    int length = snprintf(temporary, sizeof(temporary), "%s/.passwd-XXXXXX", directory);
    if (length < 0 || (size_t)length >= sizeof(temporary)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    int parent = open(directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (parent < 0) {
        return -1;
    }

    struct stat info;
    if (fstat(parent, &info) != 0 || info.st_uid != 0 || (info.st_mode & 0022) != 0) {
        close(parent);
        errno = EINVAL;
        return -1;
    }

    int fd = mkstemp(temporary);
    if (fd < 0) {
        close(parent);
        return -1;
    }

    int result = -1;
    FILE *target = NULL;
    char *line = NULL;
    size_t capacity = 0;
    if (fchmod(fd, strcmp(path, "/etc/passwd") == 0 ? 0644 : 0600) != 0 ||
        fstat(fd, &info) != 0 || info.st_uid != 0 || !S_ISREG(info.st_mode) || info.st_nlink != 1) {
        goto done;
    }

    target = fdopen(fd, "w");
    if (target == NULL) {
        goto done;
    }
    fd = -1;
    size_t user_length = strlen(user);
    int found = 0;
    ssize_t size;
    while ((size = getline(&line, &capacity, source)) >= 0) {
        if (size > 16384) {
            errno = EOVERFLOW;
            goto done;
        }

        if (strncmp(line, user, user_length) == 0 && line[user_length] == ':') {
            char *rest = strchr(line + user_length + 1, ':');
            if (rest == NULL || found != 0) {
                errno = EINVAL;
                goto done;
            }
            found = 1;
            if (fprintf(target, "%s:%s%s", user, hash, rest) < 0) {
                goto done;
            }
        } else if (fputs(line, target) == EOF) {
            goto done;
        }
    }

    if (ferror(source) || found != 1 || fflush(target) != 0 || fsync(fileno(target)) != 0) {
        if (found != 1) {
            errno = EINVAL;
        }
        goto done;
    }

    if (fclose(target) != 0) {
        target = NULL;
        goto done;
    }
    target = NULL;
    if (rename(temporary, path) != 0 || fsync(parent) != 0) {
        goto done;
    }
    result = 0;

done:
    {
        int error = errno;
        free(line);
        if (target != NULL) {
            fclose(target);
        }
        if (fd >= 0) {
            close(fd);
        }
        if (result != 0) {
            unlink(temporary);
        }
        close(parent);
        errno = error;
    }
    return result;
}

// Получить полную случайную соль для имеющегося SHA-512 crypt
static int
gensalt(char *s)
{
	static const char b64[] = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
	uint8_t buf[12];
	uint32_t n;
	int i;

    size_t done = 0;
    while (done < sizeof(buf)) {
        ssize_t count = syscall(SYS_getrandom, buf + done, sizeof(buf) - done, 0);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            if (count == 0) {
                errno = EIO;
            }
            explicit_bzero(buf, sizeof(buf));
            return -1;
        }
        done += (size_t)count;
    }
	for (i = 0; i < 12; i += 3) {
		n = buf[i] << 16 | buf[i+1] << 8 | buf[i+2];
		*s++ = b64[n%64]; n /= 64;
		*s++ = b64[n%64]; n /= 64;
		*s++ = b64[n%64]; n /= 64;
		*s++ = b64[n];
	}
	*s++ = '\0';
    explicit_bzero(buf, sizeof(buf));
    return 0;
}

// Показать поддерживаемый способ запуска
static void
usage(void)
{
	eprintf("usage: %s [username [password]]\n", argv0);
}

// Сменить пароль после проверки аккаунта и полного подтверждения ввода
int main(int argc, char *argv[]) {
    char *cryptpass = NULL, *inpass = NULL, *encoded;
    char salt[sizeof(PW_CIPHER) + 16] = PW_CIPHER;
    char account_path[PATH_MAX];
    struct passwd *account;
    struct spwd *shadow = NULL;
    FILE *file = NULL;
    int lock_fd = -1, status = 1;
    const char *target_user = NULL, *direct_password = NULL;

    ARGBEGIN {
    default:
        usage();
        goto cleanup;
    } ARGEND;
    if (argc > 2) {
        usage();
        goto cleanup;
    }
    if (argc >= 1) {
        target_user = argv[0];
    }
    if (argc == 2) {
        direct_password = argv[1];
    }

    pw_init();
    umask(077);
    lock_fd = lock_password_files();
    if (lock_fd < 0) {
        weprintf("lock password files:");
        goto cleanup;
    }

    account = target_user != NULL ? getpwnam(target_user) : getpwuid(getuid());
    if (account == NULL || account->pw_name == NULL || account->pw_passwd == NULL) {
        weprintf("account is unavailable\n");
        goto cleanup;
    }
    if (strcmp(account->pw_passwd, "x") == 0) {
        shadow = getspnam(account->pw_name);
        if (shadow == NULL || shadow->sp_pwdp == NULL) {
            weprintf("shadow account is unavailable\n");
            goto cleanup;
        }
    }

    if (direct_password != NULL) {
        if (getuid() != 0) {
            weprintf("only root can set password non-interactively\n");
            goto cleanup;
        }
    } else {
        flush_tty();
        if (getuid() != 0) {
            const char *prior = shadow != NULL ? shadow->sp_pwdp : account->pw_passwd;
            if (prior[0] == '!' || prior[0] == '*') {
                weprintf("denied\n");
                goto cleanup;
            }
            if (prior[0] != '\0') {
                inpass = tty_getpass("Old password: ");
                if (inpass == NULL || inpass[0] == '\0') {
                    weprintf("no password supplied\n");
                    goto cleanup;
                }
                encoded = crypt(inpass, prior);
                explicit_bzero(inpass, strlen(inpass));
                if (encoded == NULL || encoded[0] == '*' || strcmp(encoded, prior) != 0) {
                    weprintf("incorrect password\n");
                    goto cleanup;
                }
            }
        }
    }

    if (direct_password != NULL) {
        inpass = (char *)direct_password;
    } else {
        inpass = tty_getpass("Enter new password: ");
    }
    if (inpass == NULL || inpass[0] == '\0') {
        weprintf("no password supplied\n");
        goto cleanup;
    }
    if (gensalt(salt + strlen(salt)) != 0) {
        weprintf("getrandom:");
        goto cleanup;
    }
    encoded = crypt(inpass, salt);
    explicit_bzero(inpass, strlen(inpass));
    if (encoded == NULL || encoded[0] == '*') {
        weprintf("crypt failed\n");
        goto cleanup;
    }
    cryptpass = strdup(encoded);
    if (cryptpass == NULL) {
        weprintf("copy password hash:");
        goto cleanup;
    }

    if (direct_password == NULL) {
        flush_tty();
        inpass = tty_getpass("Retype new password: ");
        if (inpass == NULL || inpass[0] == '\0') {
            weprintf("no password supplied\n");
            goto cleanup;
        }
        encoded = crypt(inpass, salt);
        explicit_bzero(inpass, strlen(inpass));
        if (encoded == NULL || encoded[0] == '*' || strcmp(cryptpass, encoded) != 0) {
            weprintf("passwords do not match\n");
            goto cleanup;
        }
    }

    file = spw_get_file(account->pw_name, account_path);
    if (file == NULL && (errno == ENOENT || errno == ENOTDIR)) {
        strcpy(account_path, "/etc/passwd");
        file = open_account_file(account_path);
    }
    if (file == NULL || write_password_file(file, account_path, account->pw_name, cryptpass) != 0) {
        weprintf("update password file:");
        goto cleanup;
    }
    status = 0;

cleanup:
    if (inpass != NULL) {
        explicit_bzero(inpass, strlen(inpass));
    }
    if (file != NULL && fclose(file) != 0) {
        status = 1;
    }
    if (cryptpass != NULL) {
        explicit_bzero(cryptpass, strlen(cryptpass));
        free(cryptpass);
    }
    if (lock_fd >= 0) {
        close(lock_fd);
    }
    explicit_bzero(salt, sizeof(salt));
    return status;
}

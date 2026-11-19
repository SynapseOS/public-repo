/*
    sh.c: Командная оболочка и разбор команд
    Автор изменений: Елчинян Арен Артурович 2026
    Архитектура: x86_64, ABI: Linux (ELF64)

    Описание: Запуск программ, перенаправления и условные списки команд
    Подсистема: пользовательская командная оболочка
    Первое изменение: исходная дата не установлена
    Последнее изменение: 14.11.2026
    Доступ: общедоступный
*/

#include <signal.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <errno.h>
#include <setjmp.h>
#include <glob.h>
#include <limits.h>
#include <sys/stat.h>

enum { EXEC = 1, REDIR, PIPE, LIST, BACK, AND, OR, SUBSHELL };

#define MAXARGS 64

struct cmd {
    int type;
};

struct execcmd {
    int   type;
    char *argv[MAXARGS + 1];
    char *eargv[MAXARGS + 1];
    int expanded;
};

struct redircmd {
    int         type;
    struct cmd *cmd;
    char       *file;
    char       *efile;
    int         mode;
    int         fd;
    int         duplicate;
};

struct pipecmd {
    int         type;
    struct cmd *left;
    struct cmd *right;
};

struct listcmd {
    int         type;
    struct cmd *left;
    struct cmd *right;
};

struct backcmd {
    int         type;
    struct cmd *cmd;
};

static void        panic(const char *msg);
static int         fork1(void);
static _Noreturn void runcmd(struct cmd *cmd);
static int         getcmd(char *buf, size_t nbuf);
static struct cmd *parsecmd(char *s);

static struct cmd *execcmd(void);
static struct cmd *redircmd(struct cmd *sub, char *file, char *efile,
                            int mode, int fd, int duplicate);
static struct cmd *pipecmd(struct cmd *left, struct cmd *right);
static struct cmd *listcmd(struct cmd *left, struct cmd *right);
static struct cmd *backcmd(struct cmd *sub);

static struct cmd *parseline(char **ps, char *es);
static struct cmd *parsepipe(char **ps, char *es);
static struct cmd *parseandor(char **ps, char *es);
static struct cmd *parseexec(char **ps, char *es);
static struct cmd **parseredirs(struct cmd **tail, char **ps, char *es);
static int fd_number(const char *start, const char *end);
static struct cmd *parseblock(char **ps, char *es);
static struct cmd *nulterminate(struct cmd *cmd);
static int         gettoken(char **ps, char *es, char **q, char **eq);
static int         peek(char **ps, char *es, const char *toks);

static int shell_fd = STDIN_FILENO;
static jmp_buf builtin_syntax;
static int builtin_parsing;
static int builtin_cd(char *line);
static void unquote(char *word, char *end);
static int exec_builtin(struct execcmd *cmd);
static void expand_exec(struct execcmd *cmd);
static size_t expand_word(char *word, char *end, char **result, size_t capacity);

// Дождаться нужного дочернего процесса и преобразовать сигнал в статус оболочки
static int wait_command(pid_t child) {
    int status;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited < 0) {
        return 1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

// Выполнить левую часть условного списка и получить её статус без потери изменений cd
static int run_wait(struct cmd *cmd) {
    if (cmd->type == AND || cmd->type == OR) {
        struct listcmd *list = (struct listcmd *)cmd;
        int status = run_wait(list->left);
        if ((cmd->type == AND && status == 0) || (cmd->type == OR && status != 0)) {
            return run_wait(list->right);
        }
        return status;
    }
    if (cmd->type == EXEC) {
        expand_exec((struct execcmd *)cmd);
        int status = exec_builtin((struct execcmd *)cmd);
        if (status >= 0) {
            return status;
        }
    }

    pid_t child = fork1();
    if (child == 0) {
        runcmd(cmd);
    }
    return wait_command(child);
}

// Запустить разобранное дерево команд в текущем дочернем процессе
static _Noreturn void runcmd(struct cmd *cmd) {
    for (;;) {
        if (cmd == NULL) {
            exit(1);
        }

        switch (cmd->type) {
        case EXEC: {
            struct execcmd *exec = (struct execcmd *)cmd;
            expand_exec(exec);
            if (exec->argv[0] == NULL) {
                exit(0);
            }
            int builtin = exec_builtin(exec);
            if (builtin >= 0) {
                exit(builtin);
            }
            if (strcmp(exec->argv[0], "exec") == 0 && exec->argv[1] != NULL) {
                execvp(exec->argv[1], exec->argv + 1);
            } else {
                execvp(exec->argv[0], exec->argv);
            }
            int error = errno;
            fprintf(stderr, "exec %s failed: %s\n", exec->argv[0], strerror(error));
            exit(error == ENOENT ? 127 : 126);
        }

        case REDIR: {
            struct redircmd *redirect = (struct redircmd *)cmd;
            expand_word(redirect->file, redirect->efile, &redirect->file, 1);
            if (redirect->duplicate) {
                const char *word = redirect->file;
                if (strcmp(word, "-") == 0) {
                    if (close(redirect->fd) < 0 && errno != EBADF) {
                        perror("sh: close");
                        exit(1);
                    }
                } else {
                    int source = fd_number(word, word + strlen(word));
                    if (source < 0) {
                        fprintf(stderr, "sh: invalid file descriptor: %s\n", word);
                        exit(1);
                    }
                    if (dup2(source, redirect->fd) < 0) {
                        perror("sh: dup2");
                        exit(1);
                    }
                }
                cmd = redirect->cmd;
                break;
            }

            int fd = open(redirect->file, redirect->mode, 0644);
            if (fd < 0) {
                fprintf(stderr, "open %s failed: %s\n", redirect->file, strerror(errno));
                exit(1);
            }
            if (fd != redirect->fd) {
                if (dup2(fd, redirect->fd) < 0) {
                    fprintf(stderr, "dup2: %s\n", strerror(errno));
                    exit(1);
                }
                close(fd);
            }
            cmd = redirect->cmd;
            break;
        }

        case AND:
        case OR:
            exit(run_wait(cmd));

        case LIST: {
            struct listcmd *list = (struct listcmd *)cmd;
            run_wait(list->left);
            cmd = list->right;
            break;
        }

        case PIPE: {
            struct pipecmd *pipeline = (struct pipecmd *)cmd;
            int p[2];
            if (pipe(p) < 0) {
                panic("pipe");
            }
            pid_t left = fork1();
            if (left == 0) {
                if (dup2(p[1], STDOUT_FILENO) < 0) {
                    exit(1);
                }
                close(p[0]);
                close(p[1]);
                cmd = pipeline->left;
                break;
            }
            pid_t right = fork1();
            if (right == 0) {
                if (dup2(p[0], STDIN_FILENO) < 0) {
                    exit(1);
                }
                close(p[0]);
                close(p[1]);
                cmd = pipeline->right;
                break;
            }
            close(p[0]);
            close(p[1]);
            wait_command(left);
            exit(wait_command(right));
        }

        case SUBSHELL: {
            struct backcmd *group = (struct backcmd *)cmd;
            cmd = group->cmd;
            break;
        }

        case BACK: {
            struct backcmd *background = (struct backcmd *)cmd;
            if (fork1() == 0) {
                cmd = background->cmd;
                break;
            }
            exit(0);
        }

        default:
            panic("runcmd: unknown command type");
        }
    }
}

#define HISTORY_MAX 100
#define HISTORY_LINE 1024
static char history[HISTORY_MAX][HISTORY_LINE];
static size_t history_count;
static char history_path[4096];
static int shell_interactive;

static void history_add(const char *line) {
    if (!*line || (history_count && !strcmp(history[history_count-1], line))) return;
    if (history_count == HISTORY_MAX) {
        memmove(history, history+1, sizeof(history)-sizeof(history[0]));
        history_count--;
    }
    snprintf(history[history_count++], HISTORY_LINE, "%s", line);
}

static void history_load(void) {
    const char *path = getenv("HISTFILE");
    const char *home = getenv("HOME");
    if (path) snprintf(history_path, sizeof(history_path), "%s", path);
    else snprintf(history_path, sizeof(history_path), "%s/.sh_history", home ? home : "/");
    if (!*history_path) return;
    FILE *file = fopen(history_path, "r");
    if (!file) return;
    char line[HISTORY_LINE];
    while (fgets(line, sizeof(line), file)) {
        size_t len = strlen(line);
        if (len && line[len-1] == '\n') line[--len] = 0;
        else if (!feof(file)) {
            int c; while ((c=fgetc(file)) != '\n' && c != EOF) { }
        }
        history_add(line);
    }
    fclose(file);
}

static void history_save(const char *line) {
    size_t old_count = history_count;
    int duplicate = old_count && !strcmp(history[old_count-1], line);
    history_add(line);
    if (duplicate || !*history_path) return;
    int fd = open(history_path, O_WRONLY|O_CREAT, 0600);
    if (fd < 0) return;
    off_t size = lseek(fd, 0, SEEK_END);
    if (size > 128*1024) {
        close(fd);
        fd = open(history_path, O_WRONLY|O_TRUNC, 0600);
        if (fd < 0) return;
        for (size_t i=0; i<history_count; i++) dprintf(fd, "%s\n", history[i]);
    } else if (size >= 0) dprintf(fd, "%s\n", line);
    close(fd);
}

// Считать символы UTF-8 для стирания строки без лишних пробелов
static size_t text_columns(const char *text, size_t length) {
    size_t count = 0;
    for (size_t i = 0; i < length; i++) {
        if (((unsigned char)text[i] & 0xc0) != 0x80) {
            count++;
        }
    }
    return count;
}

// Стереть указанное количество видимых символов
static void erase_line(size_t length) {
    while (length > 0) {
        write(shell_fd, "\b \b", 3);
        length--;
    }
}

// Показать приглашение повторно после списка дополнений
static void show_prompt(void) {
    char *cwd = getcwd(NULL, 0);
    if (cwd != NULL) {
        dprintf(shell_fd, "\033[1;32m%s\033[0m \033[1;34m$\033[0m ", cwd);
        free(cwd);
    } else {
        dprintf(shell_fd, "\033[1;34m$\033[0m ");
    }
}

// Дополнить последнее слово командами PATH или именами файлов, сохраняя экранирование
static void complete_line(char *buf, size_t *position, size_t capacity) {
    size_t start = 0;
    char quote = 0;
    int escaped = 0;
    int command = 1;
    for (size_t i = 0; i < *position; i++) {
        char c = buf[i];
        if (escaped) {
            escaped = 0;
        } else if (c == '\\' && quote != '\'') {
            escaped = 1;
        } else if (quote != 0) {
            if (c == quote) {
                quote = 0;
            }
        } else if (c == '\'' || c == '"') {
            quote = c;
        } else if (strchr(" \t;|&()<>", c) != NULL) {
            if (strchr(";|&(", c) != NULL) {
                command = 1;
            } else if (i > start || c == '<' || c == '>') {
                command = 0;
            }
            start = i + 1;
        }
    }

    size_t length = *position - start;
    char *word = strndup(buf + start, length);
    char *pattern = malloc(length * 2 + 2);
    if (word == NULL || pattern == NULL) {
        free(word);
        free(pattern);
        return;
    }
    // Частично набранные кавычки допустимы при дополнении слова
    unquote(word, word + length);
    size_t used = 0;
    for (size_t i = 0; word[i] != 0; i++) {
        if (strchr("*?[]\\", word[i]) != NULL) {
            pattern[used++] = '\\';
        }
        pattern[used++] = word[i];
    }
    pattern[used++] = '*';
    pattern[used] = 0;

    char *matches[256];
    size_t count = 0;
    int overflow = 0;
    char *paths = command && strchr(word, '/') == NULL ? strdup(getenv("PATH") ? getenv("PATH") : "/bin") : NULL;
    if (paths != NULL) {
        static const char *builtins[] = {"cd", "exit", "history", "exec"};
        for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++) {
            if (strncmp(builtins[i], word, strlen(word)) == 0) {
                matches[count] = strdup(builtins[i]);
                if (matches[count] == NULL) {
                    overflow = 1;
                    break;
                }
                count++;
            }
        }
    }
    char *path = paths;
    int more = 1;
    while (more) {
        char *next = NULL;
        char *query = pattern;
        if (path != NULL) {
            next = strchr(path, ':');
            if (next != NULL) {
                *next++ = 0;
            }
            size_t size = strlen(path) + strlen(pattern) + 3;
            query = malloc(size);
            if (query == NULL) {
                overflow = 1;
                break;
            }
            snprintf(query, size, "%s/%s", *path ? path : ".", pattern);
        }
        glob_t found = {0};
        int status = glob(query, GLOB_MARK, NULL, &found);
        if (status == 0) {
            for (size_t i = 0; i < found.gl_pathc; i++) {
                char *candidate = found.gl_pathv[i];
                size_t n = strlen(candidate);
                if (path != NULL) {
                    if (n == 0 || candidate[n - 1] == '/' || access(candidate, X_OK) != 0) {
                        continue;
                    }
                    candidate = strrchr(candidate, '/') + 1;
                }
                int duplicate = 0;
                for (size_t j = 0; j < count; j++) {
                    if (strcmp(matches[j], candidate) == 0) {
                        duplicate = 1;
                    }
                }
                if (!duplicate) {
                    if (count == sizeof(matches) / sizeof(matches[0])) {
                        overflow = 1;
                        break;
                    }
                    matches[count] = strdup(candidate);
                    if (matches[count] == NULL) {
                        overflow = 1;
                        break;
                    }
                    count++;
                }
            }
        } else if (status != GLOB_NOMATCH) {
            overflow = 1;
        }
        globfree(&found);
        if (query != pattern) {
            free(query);
        }
        more = next != NULL;
        path = next;
    }
    if (count > 0 && !overflow) {
        size_t common = strlen(matches[0]);
        for (size_t i = 1; i < count; i++) {
            size_t j = 0;
            while (j < common && matches[0][j] == matches[i][j]) {
                j++;
            }
            common = j;
        }
        // Общий префикс не должен заканчиваться посередине символа UTF-8
        while (common > 0 && ((unsigned char)matches[0][common] & 0xc0) == 0x80) {
            common--;
        }
        if (common > strlen(word) || count == 1) {
            size_t needed = start + common * 2 + 2;
            if (needed <= capacity) {
                erase_line(text_columns(buf + start, *position - start));
                *position = start;
                for (size_t i = 0; i < common; i++) {
                    unsigned char c = (unsigned char)matches[0][i];
                    if (c < 128 && strchr(" \t\n\\'\"*?[]$`;|&()<>", c) != NULL) {
                        buf[(*position)++] = '\\';
                    }
                    buf[(*position)++] = (char)c;
                }
                if (count == 1 && common > 0 && matches[0][common - 1] != '/') {
                    buf[(*position)++] = ' ';
                }
                buf[*position] = 0;
                write(shell_fd, buf + start, *position - start);
            }
        } else {
            write(shell_fd, "\n", 1);
            for (size_t i = 0; i < count; i++) {
                dprintf(shell_fd, "%s%s", matches[i], (i + 1) % 4 == 0 ? "\n" : "  ");
            }
            write(shell_fd, "\n", 1);
            show_prompt();
            write(shell_fd, buf, *position);
        }
    } else {
        write(shell_fd, "\a", 1);
    }
    for (size_t i = 0; i < count; i++) {
        free(matches[i]);
    }
    free(paths);
    free(pattern);
    free(word);
}

static int getcmd(char *buf, size_t nbuf) {
    if (!nbuf) return -1;
    if (shell_interactive) {
        show_prompt();
    }
    struct termios saved, editing;
    int edit = shell_interactive && tcgetattr(shell_fd, &saved) == 0;
    if (edit) {
        editing = saved;
        editing.c_lflag &= ~(ICANON|ECHO);
        editing.c_iflag &= ~IXON;
        editing.c_cc[VMIN] = 1;
        editing.c_cc[VTIME] = 0;
        if (tcsetattr(shell_fd, TCSANOW, &editing) < 0) edit = 0;
    }
    size_t pos = 0, selected = history_count;
    char draft[HISTORY_LINE] = {0};
    int result = 0, escape = 0;
    buf[0] = 0;
    for (;;) {
        unsigned char c;
        ssize_t n = read(shell_fd, &c, 1);
        if (n < 0 && errno == EINTR) {
            pos = 0;
            if (edit) write(shell_fd, "\n", 1);
            break;
        }
        if (n <= 0) { if (!pos) result = -1; break; }
        if (edit && c == 27) { escape = 1; continue; }
        if (edit && escape == 1 && c == '[') { escape = 2; continue; }
        if (edit && (escape || c == 16 || c == 14)) {
            int up = c == 16 || (escape == 2 && c == 'A');
            int down = c == 14 || (escape == 2 && c == 'B');
            escape = 0;
            if (!up && !down) continue;
            if (selected == history_count) snprintf(draft, sizeof(draft), "%s", buf);
            if (up && selected) selected--;
            else if (down && selected < history_count) selected++;
            else continue;
            erase_line(text_columns(buf, pos));
            snprintf(buf, nbuf, "%s", selected < history_count ? history[selected] : draft);
            pos = strlen(buf);
            write(shell_fd, buf, pos);
            continue;
        }
        if (c == '\n' || c == '\r') { if (edit) write(shell_fd, "\n", 1); break; }
        if (edit && c == 4) { if (!pos) result = -1; if (!pos) break; continue; }
        if (edit && (c == 127 || c == 8)) {
            if (pos > 0) {
                erase_line(1);
                do {
                    pos--;
                } while (pos > 0 && ((unsigned char)buf[pos] & 0xc0) == 0x80);
                buf[pos] = 0;
            }
            continue;
        }
        if (edit && c == 21) { erase_line(text_columns(buf, pos)); pos = 0; buf[0] = 0; continue; }
        if (edit && c == '\t') {
            complete_line(buf, &pos, nbuf);
            continue;
        }
        if (edit && c < 32) continue;
        if (pos < nbuf-1) {
            buf[pos++] = c; buf[pos] = 0;
            if (edit) write(shell_fd, &c, 1);
        }
    }
    buf[pos] = 0;
    if (edit) tcsetattr(shell_fd, TCSANOW, &saved);
    return result;
}

int main(int argc, char **argv) {
    int login = argv[0] && argv[0][0] == '-';
    int arg = 1;
    while (arg < argc && (!strcmp(argv[arg], "-l") || !strcmp(argv[arg], "--login"))) {
        login = 1;
        arg++;
    }
    if (login) {
        unsetenv("SHELL_TTY");
    }
    if (arg + 1 < argc && strcmp(argv[arg], "-c") == 0) {
        char *command = strdup(argv[arg + 1]);
        if (!command) panic("out of memory");
        runcmd(parsecmd(command));
    }
    if (arg < argc && strcmp(argv[arg], "--") == 0) {
        arg++;
    }
    if (arg < argc) {
        int script = open(argv[arg], O_RDONLY);
        if (script < 0) {
            fprintf(stderr, "sh: %s: %s\n", argv[arg], strerror(errno));
            return 1;
        }
        if (dup2(script, STDIN_FILENO) < 0) { close(script); return 1; }
        if (script != STDIN_FILENO) close(script);
        unsetenv("SHELL_TTY");
    }
    static char buf[1024];
    int fd;

    const char *dev_path = getenv("SHELL_TTY");

    if (dev_path && *dev_path) {
        fd = open(dev_path, O_RDWR);
        if (fd >= 0) {
            shell_fd = fd;
            dup2(fd, STDIN_FILENO);
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            if (fd > STDERR_FILENO) {
                close(fd);
                shell_fd = STDIN_FILENO;
            }
        } else {
            fprintf(stderr, "SHELL_TTY=%s: %s\n", dev_path, strerror(errno));
            fprintf(stderr, "Falling back to default tty\n");
        }
    }

    while ((fd = open("/dev/null", O_RDWR)) >= 0) {
        if (fd >= 3) {
            close(fd);
            break;
        }
    }

    int interactive = isatty(shell_fd);
    shell_interactive = interactive;
    if (interactive) history_load();
    if (interactive) {
        setpgid(0, 0);
        ioctl(shell_fd, TIOCSCTTY, 0);
        tcsetpgrp(shell_fd, getpgrp());
        signal(SIGINT, SIG_IGN);
    }
    while (getcmd(buf, sizeof(buf)) >= 0) {
        if (!buf[0] || buf[strspn(buf, " \t")] == '#') continue;
        if (interactive) history_save(buf);
        if (strcmp(buf, "history") == 0) {
            for (size_t i=0; i<history_count; i++) printf("%zu  %s\n", i+1, history[i]);
            continue;
        }
        if (builtin_cd(buf)) continue;

        if (strcmp(buf, "exit") == 0)
            break;

        pid_t child = fork1();
        if (child == 0) {
            if (interactive) {
                setpgid(0, 0);
                tcsetpgrp(shell_fd, getpgrp());
                signal(SIGINT, SIG_DFL);
            }
            struct cmd *parsed = parsecmd(buf);
            runcmd(parsed);
        }
        if (interactive) {
            setpgid(child, child);
            tcsetpgrp(shell_fd, child);
        }
        while (waitpid(child, NULL, 0) < 0 && errno == EINTR) { }
        if (interactive) tcsetpgrp(shell_fd, getpgrp());
    }

    return 0;
}

static void panic(const char *msg) {
    fprintf(stderr, "sh: %s\n", msg);
    if (builtin_parsing) longjmp(builtin_syntax, 1);
    exit(1);
}

static int fork1(void) {
    pid_t pid = fork();
    if (pid == -1)
        panic("fork");
    return (int)pid;
}

static struct cmd *execcmd(void) {
    struct execcmd *cmd = calloc(1, sizeof(*cmd));
    if (cmd == NULL) panic("out of memory");
    cmd->type = EXEC;
    return (struct cmd *)cmd;
}

static struct cmd *redircmd(struct cmd *sub, char *file, char *efile, int mode, int fd, int duplicate) {
    struct redircmd *cmd = calloc(1, sizeof(*cmd));
    if (cmd == NULL) panic("out of memory");
    cmd->type = REDIR;
    cmd->cmd  = sub;
    cmd->file = file;
    cmd->efile = efile;
    cmd->mode = mode;
    cmd->fd   = fd;
    cmd->duplicate = duplicate;
    return (struct cmd *)cmd;
}

static struct cmd *pipecmd(struct cmd *left, struct cmd *right) {
    struct pipecmd *cmd = calloc(1, sizeof(*cmd));
    if (cmd == NULL) panic("out of memory");
    cmd->type  = PIPE;
    cmd->left  = left;
    cmd->right = right;
    return (struct cmd *)cmd;
}

static struct cmd *listcmd(struct cmd *left, struct cmd *right) {
    struct listcmd *cmd = calloc(1, sizeof(*cmd));
    if (cmd == NULL) panic("out of memory");
    cmd->type  = LIST;
    cmd->left  = left;
    cmd->right = right;
    return (struct cmd *)cmd;
}

static struct cmd *backcmd(struct cmd *sub) {
    struct backcmd *cmd = calloc(1, sizeof(*cmd));
    if (cmd == NULL) panic("out of memory");
    cmd->type = BACK;
    cmd->cmd  = sub;
    return (struct cmd *)cmd;
}

static const char WHITESPACE[] = " \t\r\n\v";
static const char SYMBOLS[]    = "<|>&;()";

static int gettoken(char **ps, char *es, char **q, char **eq) {
    char *s = *ps;
    int   ret;

    while (s < es && strchr(WHITESPACE, *s))
        s++;

    if (q)  *q = s;
    ret = *s;

    switch (*s) {
    case '\0':
        break;
    case '|':
    case '&': {
        char symbol = *s++;
        if (s < es && *s == symbol) {
            s++;
            ret = symbol == '&' ? 'A' : 'O';
        }
        break;
    }
    case '(': case ')': case ';': case '<':
        s++;
        break;
    case '>':
        s++;
        if (*s == '>') {
            ret = '+';
            s++;
        }
        break;
    default:
        ret = 'a';
        while (s < es && !strchr(WHITESPACE, *s) && !strchr(SYMBOLS, *s)) {
            if (*s == '\\') {
                if (++s == es) panic("syntax: trailing escape");
                s++;
            } else if (*s == '\'' || *s == '"') {
                char quote = *s++;
                while (s < es && *s != quote) {
                    if (quote == '"' && *s == '\\') {
                        if (++s == es) panic("syntax: trailing escape");
                    }
                    s++;
                }
                if (s == es) panic("syntax: unterminated quote");
                s++;
            } else s++;
        }
        break;
    }

    if (eq) *eq = s;

    while (s < es && strchr(WHITESPACE, *s))
        s++;

    *ps = s;
    return ret;
}

static int peek(char **ps, char *es, const char *toks) {
    char *s = *ps;
    while (s < es && strchr(WHITESPACE, *s))
        s++;
    *ps = s;
    return (*s != '\0') && (strchr(toks, *s) != NULL);
}

struct cmd *parsecmd(char *s) {
    char *es = s + strlen(s);
    struct cmd *cmd = parseline(&s, es);

    peek(&s, es, "");
    if (s != es) {
        fprintf(stderr, "leftovers: %s\n", s);
        panic("syntax error");
    }

    nulterminate(cmd);
    return cmd;
}

static struct cmd *parseline(char **ps, char *es) {
    struct cmd *cmd = parseandor(ps, es);

    while (peek(ps, es, "&")) {
        gettoken(ps, es, NULL, NULL);
        cmd = backcmd(cmd);
    }

    if (peek(ps, es, ";")) {
        gettoken(ps, es, NULL, NULL);
        cmd = listcmd(cmd, parseline(ps, es));
    }

    return cmd;
}

// && и || имеют одинаковый приоритет и вычисляются слева направо
static struct cmd *parseandor(char **ps, char *es) {
    if (peek(ps, es, "&|")) {
        panic("missing command before operator");
    }

    struct cmd *cmd = parsepipe(ps, es);
    while (peek(ps, es, "&|") && *ps + 1 < es && (*ps)[0] == (*ps)[1]) {
        int token = gettoken(ps, es, NULL, NULL);
        peek(ps, es, "");
        if (*ps == es || peek(ps, es, "&|;)")) {
            panic("missing command after logical operator");
        }
        struct cmd *right = parsepipe(ps, es);
        struct listcmd *joined = (struct listcmd *)listcmd(cmd, right);
        joined->type = token == 'A' ? AND : OR;
        cmd = (struct cmd *)joined;
    }
    return cmd;
}

static struct cmd *parsepipe(char **ps, char *es) {
    struct cmd *cmd = parseexec(ps, es);

    if (peek(ps, es, "|") && (*ps + 1 == es || (*ps)[1] != '|')) {
        gettoken(ps, es, NULL, NULL);
        cmd = pipecmd(cmd, parsepipe(ps, es));
    }

    return cmd;
}

static int fd_number(const char *start, const char *end) {
    int fd = 0;
    if (start == end) {
        return -1;
    }
    for (; start < end; start++) {
        int digit = *start - '0';
        if (digit < 0 || digit > 9 || fd > (INT_MAX - digit) / 10) {
            return -1;
        }
        fd = fd * 10 + digit;
    }
    return fd;
}

// Оставлять обычный числовой аргумент, если оператор отделён пробелом
static struct cmd **parseredirs(struct cmd **tail, char **ps, char *es) {
    peek(ps, es, "");
    for (;;) {
        char *start = *ps;
        char *op = start + strspn(start, "0123456789");
        if (*op != '<' && *op != '>') {
            return tail;
        }
        int fd = op == start ? (*op == '<' ? STDIN_FILENO : STDOUT_FILENO) : fd_number(start, op);
        if (fd < 0) {
            panic("invalid redirection descriptor");
        }
        int duplicate = op + 1 < es && op[1] == '&';
        *ps = op;
        int tok = gettoken(ps, es, NULL, NULL);
        if (duplicate) {
            (*ps)++;
        }

        char *q, *eq;
        if (gettoken(ps, es, &q, &eq) != 'a') {
            panic(duplicate ? "missing descriptor for redirection" : "missing file for redirection");
        }
        int mode = tok == '<' ? O_RDONLY : O_WRONLY | O_CREAT | (tok == '+' ? O_APPEND : O_TRUNC);
        // Дерево выполняется снаружи внутрь, поэтому добавлять в порядке записи
        *tail = redircmd(*tail, q, eq, mode, fd, duplicate);
        tail = &((struct redircmd *)*tail)->cmd;
    }
}

static struct cmd *parseblock(char **ps, char *es) {
    if (!peek(ps, es, "("))
        panic("parseblock");
    gettoken(ps, es, NULL, NULL);

    struct cmd *cmd = parseline(ps, es);

    if (!peek(ps, es, ")"))
        panic("syntax: missing ')'");
    gettoken(ps, es, NULL, NULL);

    struct cmd *group = backcmd(cmd);
    group->type = SUBSHELL;
    parseredirs(&group, ps, es);
    return group;
}

static struct cmd *parseexec(char **ps, char *es) {
    if (peek(ps, es, "("))
        return parseblock(ps, es);

    struct cmd  *ret = execcmd();
    struct execcmd *cmd = (struct execcmd *)ret;
    int argc = 0;

    struct cmd **tail = parseredirs(&ret, ps, es);

    while (!peek(ps, es, "|)&;")) {
        char *q, *eq;
        int tok = gettoken(ps, es, &q, &eq);
        if (tok == 0)
            break;
        if (tok != 'a')
            panic("syntax error in command");

        if (argc >= MAXARGS)
            panic("too many arguments");

        cmd->argv[argc]  = q;
        cmd->eargv[argc] = eq;
        argc++;

        tail = parseredirs(tail, ps, es);
    }

    cmd->argv[argc]  = NULL;
    cmd->eargv[argc] = NULL;
    return ret;
}

/* Quote removal never expands literal $ or backticks. In double quotes,
 * backslash only quotes $, `, ", backslash and newline (POSIX word rules). */
static void unquote(char *word, char *end) {
    char *out = word;
    char quote = 0;
    for (char *in = word; in < end; in++) {
        char c = *in;
        if (quote == '\'') {
            if (c == quote) quote = 0;
            else *out++ = c;
        } else if (c == '\\') {
            char next = in[1];
            if (!quote || next == '$' || next == '`' || next == '"' || next == '\\' || next == '\n') {
                in++;
                if (next != '\n') *out++ = next;
            } else *out++ = c;
        } else if (c == '\'' || c == '"') {
            if (!quote) quote = c;
            else if (quote == c) quote = 0;
            else *out++ = c;
        } else *out++ = c;
    }
    *out = '\0';
}

// Раскрыть маску в одном слове, защищая символы внутри кавычек и после обратной косой черты
static size_t expand_word(char *word, char *end, char **result, size_t capacity) {
    size_t length = (size_t)(end - word);
    char *pattern = malloc(length * 2 + 1);
    if (pattern == NULL) {
        panic("out of memory");
    }
    char quote = 0;
    size_t used = 0;
    int wildcard = 0;
    for (char *in = word; in < end; in++) {
        char c = *in;
        int literal = quote != 0;
        if (c == '\\' && quote != '\'') {
            char next = in + 1 < end ? in[1] : 0;
            if (!quote || next == '$' || next == '`' || next == '"' || next == '\\' || next == '\n') {
                in++;
                c = next;
                literal = 1;
                if (c == '\n' || c == 0) {
                    continue;
                }
            }
        } else if (c == '\'' || c == '"') {
            if (quote == 0) {
                quote = c;
                continue;
            }
            if (quote == c) {
                quote = 0;
                continue;
            }
        }
        if ((literal && strchr("*?[]", c) != NULL) || c == '\\') {
            pattern[used++] = '\\';
        } else if (!literal && strchr("*?[", c) != NULL) {
            wildcard = 1;
        }
        pattern[used++] = c;
    }
    pattern[used] = 0;
    glob_t found = {0};
    int status = wildcard ? glob(pattern, 0, NULL, &found) : GLOB_NOMATCH;
    free(pattern);
    size_t count = status == 0 ? found.gl_pathc : 1;
    if ((status != 0 && status != GLOB_NOMATCH) || count > capacity) {
        globfree(&found);
        panic(count > capacity ? "too many wildcard matches" : "cannot expand wildcard");
    }
    if (status == 0) {
        for (size_t i = 0; i < count; i++) {
            result[i] = strdup(found.gl_pathv[i]);
            if (result[i] == NULL) {
                for (size_t j = 0; j < i; j++) {
                    free(result[j]);
                }
                globfree(&found);
                panic("out of memory");
            }
        }
    } else {
        unquote(word, end);
        result[0] = strdup(word);
        if (result[0] == NULL) {
            globfree(&found);
            panic("out of memory");
        }
    }
    globfree(&found);
    return count;
}

// Раскрывать маски при выполнении команды, после предшествующих cd и перенаправлений
static void expand_exec(struct execcmd *cmd) {
    if (cmd->expanded) {
        return;
    }
    char *expanded[MAXARGS + 1];
    size_t count = 0;
    for (int i = 0; cmd->argv[i] != NULL; i++) {
        count += expand_word(cmd->argv[i], cmd->eargv[i], expanded + count, MAXARGS - count);
    }
    expanded[count] = NULL;
    memcpy(cmd->argv, expanded, (count + 1) * sizeof(expanded[0]));
    cmd->expanded = 1;
}

static int exec_builtin(struct execcmd *cmd) {
    if (!cmd->argv[0] || strcmp(cmd->argv[0], "cd")) return -1;
    if (cmd->argv[1] && cmd->argv[2]) { fprintf(stderr, "sh: cd: too many arguments\n"); return 1; }
    const char *dir = cmd->argv[1] ? cmd->argv[1] : getenv("HOME");
    if (!dir) dir = "/";
    if (chdir(dir) < 0) { fprintf(stderr, "cannot cd to %s: %s\n", dir, strerror(errno)); return 1; }
    return 0;
}

/* cd runs in the parent, but uses the same token/quote rules as exec argv.
 * A malformed builtin must report a syntax error without killing the shell. */
static int builtin_cd(char *line) {
    char *start = line + strspn(line, WHITESPACE);
    if (strncmp(start, "cd", 2) || (start[2] && !strchr(WHITESPACE, start[2]))) return 0;
    builtin_parsing = 1;
    if (setjmp(builtin_syntax)) { builtin_parsing = 0; return 1; }
    char *end = line + strlen(line), *cursor = start + 2, *word = NULL, *word_end = NULL;
    int token = gettoken(&cursor, end, &word, &word_end);
    if (token && token != 'a') panic("syntax: invalid cd argument");
    if (gettoken(&cursor, end, NULL, NULL)) panic("cd: too many arguments");
    char *expanded = NULL;
    if (token) {
        expand_word(word, word_end, &expanded, 1);
    }
    const char *dir = token ? expanded : getenv("HOME");
    if (!dir) dir = "/";
    if (chdir(dir) < 0) fprintf(stderr, "cannot cd to %s: %s\n", dir, strerror(errno));
    free(expanded);
    builtin_parsing = 0;
    return 1;
}

static struct cmd *nulterminate(struct cmd *cmd) {
    if (cmd == NULL)
        return NULL;

    switch (cmd->type) {
    case EXEC: {
        struct execcmd *ecmd = (struct execcmd *)cmd;
        for (int i = 0; ecmd->argv[i] != NULL; i++) {
            size_t length = (size_t)(ecmd->eargv[i] - ecmd->argv[i]);
            ecmd->argv[i] = strndup(ecmd->argv[i], length);
            if (ecmd->argv[i] == NULL) {
                panic("out of memory");
            }
            ecmd->eargv[i] = ecmd->argv[i] + length;
        }
        break;
    }
    case REDIR: {
        struct redircmd *rcmd = (struct redircmd *)cmd;
        nulterminate(rcmd->cmd);
        size_t length = (size_t)(rcmd->efile - rcmd->file);
        rcmd->file = strndup(rcmd->file, length);
        if (rcmd->file == NULL) {
            panic("out of memory");
        }
        rcmd->efile = rcmd->file + length;
        break;
    }
    case PIPE: {
        struct pipecmd *pcmd = (struct pipecmd *)cmd;
        nulterminate(pcmd->left);
        nulterminate(pcmd->right);
        break;
    }
    case LIST:
    case AND:
    case OR: {
        struct listcmd *lcmd = (struct listcmd *)cmd;
        nulterminate(lcmd->left);
        nulterminate(lcmd->right);
        break;
    }
    case SUBSHELL:
    case BACK: {
        struct backcmd *bcmd = (struct backcmd *)cmd;
        nulterminate(bcmd->cmd);
        break;
    }
    }
    return cmd;
}

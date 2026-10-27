// help.c: Просмотр справки
// Автор: Елчинян Арен Артурович, 2026
// Архитектура: x86_64, ABI: Linux (ELF64)
//
// Подсистема: Просмотр справки в пользовательском пространстве
// Первое изменение в истории Git: 11.10.2026
// Последнее изменение: 14.10.2026
// Доступ: общедоступный

#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef HELP_DIRECTORY
#define HELP_DIRECTORY "/usr/share/help"
#endif

static int color;

// Сохраняем UTF-8, отбрасывая управляющие последовательности из страницы
static void emit(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 32 && c != 127) {
            putchar(c);
        } else if (c == '\t') {
            fputs("    ", stdout);
        }
    }
}

static void style(const char *escape) {
    if (color) {
        fputs(escape, stdout);
    }
}

static void inline_text(const char *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        if (s[i] == '\\' && i + 1 < n && strchr("\\`*_[]()#!", s[i + 1])) {
            emit(s + i + 1, 1);
            i += 2;
            continue;
        }
        if (s[i] == '`') {
            size_t end = i + 1;
            while (end < n && s[end] != '`') {
                end++;
            }
            if (end < n) {
                style("\033[36m");
                emit(s + i + 1, end - i - 1);
                style("\033[0m");
                i = end + 1;
                continue;
            }
        }
        if (s[i] == '[' || (s[i] == '!' && i + 1 < n && s[i + 1] == '[')) {
            size_t start = i + (s[i] == '!'), end = start + 1;
            while (end < n && s[end] != ']') {
                end++;
            }
            if (end + 1 < n && s[end + 1] == '(') {
                size_t urlend = end + 2;
                while (urlend < n && s[urlend] != ')') {
                    urlend++;
                }
                if (urlend < n) {
                    inline_text(s + start + 1, end - start - 1);
                    fputs(" (", stdout);
                    emit(s + end + 2, urlend - end - 2);
                    putchar(')');
                    i = urlend + 1;
                    continue;
                }
            }
        }
        if (s[i] == '*' || s[i] == '_') {
            size_t width = i + 1 < n && s[i + 1] == s[i] ? 2 : 1;
            // Подчёркивания внутри имён вроде root_password выводятся буквально
            if (s[i] == '_' && i && isalnum((unsigned char)s[i - 1])) {
                emit(s + i++, 1);
                continue;
            }

            size_t end = i + width;
            while (end + width <= n && memcmp(s + i, s + end, width)) {
                end++;
            }
            if (end + width <= n && end > i + width) {
                style(width == 2 ? "\033[1m" : "\033[3m");
                inline_text(s + i + width, end - i - width);
                style("\033[0m");
                i = end + width;
                continue;
            }
        }
        emit(s + i++, 1);
    }
}

static size_t fence_width(const char *s, size_t n, char marker) {
    size_t count = 0;
    while (count < n && s[count] == marker) {
        count++;
    }
    return count;
}

static int render(const char *path) {
    FILE *file = fopen(path, "r");
    if (!file) {
        fprintf(stderr, "help: %s: %s\n", path, strerror(errno));
        return 1;
    }

    char *line = NULL, fence = 0;
    size_t capacity = 0, fence_len = 0;
    ssize_t len;
    while ((len = getline(&line, &capacity, file)) >= 0) {
        size_t n = (size_t)len;
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
            n--;
        }

        size_t indent = 0;
        while (indent < n && line[indent] == ' ') {
            indent++;
        }

        char *text = line + indent;
        size_t count = n - indent;
        if (fence) {
            size_t close = fence_width(text, count, fence);
            size_t tail = close;
            while (tail < count && isspace((unsigned char)text[tail])) {
                tail++;
            }
            if (indent <= 3 && close >= fence_len && tail == count) {
                fence = 0;
                putchar('\n');
                continue;
            }
            fputs("    ", stdout);
            emit(line, n);
            putchar('\n');
            continue;
        }

        size_t width = count ? fence_width(text, count, text[0]) : 0;
        if (indent <= 3 && count && (text[0] == '`' || text[0] == '~') && width >= 3) {
            fence = text[0];
            fence_len = width;
            continue;
        }
        if (indent >= 4) {
            emit(line, n);
            putchar('\n');
            continue;
        }
        width = fence_width(text, count, '#');
        if (width && width <= 6 && (width == count || text[width] == ' ')) {
            size_t start = width;
            while (start < count && text[start] == ' ') {
                start++;
            }

            size_t end = count;
            while (end > start && text[end - 1] == ' ') {
                end--;
            }

            size_t hashes = end;
            while (hashes > start && text[hashes - 1] == '#') {
                hashes--;
            }
            if (hashes < end && hashes > start && text[hashes - 1] == ' ') {
                end = hashes - 1;
                while (end > start && text[end - 1] == ' ') {
                    end--;
                }
            }
            style("\033[1m");
            inline_text(text + start, end - start);
            style("\033[0m");
        } else if (count > 1 && strchr("*-+", text[0]) && text[1] == ' ') {
            emit(line, indent);
            fputs("- ", stdout);
            inline_text(text + 2, count - 2);
        } else {
            inline_text(line, n);
        }
        putchar('\n');
        if (ferror(stdout)) {
            break;
        }
    }

    int error = ferror(file) || ferror(stdout);
    if (error) {
        fprintf(stderr, "help: read/write error\n");
    }
    free(line);
    fclose(file);
    return error ? 1 : 0;
}

static int valid_topic(const char *topic) {
    if (!*topic || !strcmp(topic, ".") || !strcmp(topic, "..")) {
        return 0;
    }

    for (const unsigned char *p = (const unsigned char *)topic; *p; p++) {
        if (!isalnum(*p) && *p != '-' && *p != '_' && *p != '.') {
            return 0;
        }
    }
    return 1;
}

static int page_filter(const struct dirent *entry) {
    size_t n = strlen(entry->d_name);
    return n > 3 && !strcmp(entry->d_name + n - 3, ".md");
}

static int list_topics(void) {
    struct dirent **entries;
    puts("help TOPIC | help -f FILE | help --plain TOPIC");
    int n = scandir(HELP_DIRECTORY, &entries, page_filter, alphasort);
    if (n < 0) {
        if (errno == ENOENT) {
            puts("No help pages installed.");
            return 0;
        }
        fprintf(stderr, "help: %s: %s\n", HELP_DIRECTORY, strerror(errno));
        return 1;
    }
    if (!n) {
        puts("No help pages installed.");
    }

    for (int i = 0; i < n; i++) {
        size_t len = strlen(entries[i]->d_name) - 3;
        printf("  %.*s\n", (int)len, entries[i]->d_name);
        free(entries[i]);
    }
    free(entries);
    return ferror(stdout) ? 1 : 0;
}

int main(int argc, char **argv) {
    const char *topic = NULL, *file = NULL;
    color = isatty(STDOUT_FILENO) && !getenv("NO_COLOR");
    const char *term = getenv("TERM");
    if (term && !strcmp(term, "dumb")) {
        color = 0;
    }

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--plain")) {
            color = 0;
        } else if (!strcmp(argv[i], "-f") && i + 1 < argc && !file) {
            file = argv[++i];
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            puts("help [--plain] [TOPIC] | help [--plain] -f FILE");
            return 0;
        } else if (argv[i][0] == '-' || topic) {
            goto usage;
        } else {
            topic = argv[i];
        }
    }
    if (file && topic) {
        goto usage;
    }
    if (file) {
        return render(file);
    }
    if (!topic) {
        return list_topics();
    }
    if (!valid_topic(topic)) {
        fputs("help: invalid topic\n", stderr);
        return 1;
    }

    char path[1024];
    int n = snprintf(path, sizeof(path), "%s/%s.md", HELP_DIRECTORY, topic);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        fputs("help: topic too long\n", stderr);
        return 1;
    }
    return render(path);
usage:
    fputs("usage: help [--plain] [TOPIC] | help [--plain] -f FILE\n", stderr);
    return 1;
}

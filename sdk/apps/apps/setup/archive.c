// archive.c: Чтение файлов пакетного архива
// Автор: Елчинян Арен Артурович, 2026
// Проверка заголовков tar и извлечение содержимого пакета
// Подсистема: пакетный менеджер setup
// Создан: 11.10.2026
// Последнее изменение: 14.10.2026

#include "setup.h"

static uint64_t octal(const char *p, size_t n) {
	uint64_t v = 0;
	size_t i = 0;

	while (i < n && (p[i] == ' ' || p[i] == 0)) {
		i++;
	}

	for (; i < n && p[i] && p[i] != ' '; i++) {
		if (p[i] < '0' || p[i] > '7' || v > (UINT64_MAX >> 3)) {
			fail("invalid tar number");
		}

		v = v * 8 + p[i] - '0';
	}

	for (; i < n; i++) {
		if (p[i] && p[i] != ' ') {
			fail("invalid tar number padding");
		}
	}

	return v;
}

static int zeros(const unsigned char *p, size_t n) {
	for (size_t i = 0; i < n; i++) {
		if (p[i]) {
			return 0;
		}
	}

	return 1;
}

void extract(struct package *p, const char *archive, const char *stage) {
	size_t len;
	unsigned char *data = (void *)read_bytes(archive, PACKAGE_LIMIT, &len);

	if (len != p->size || len % 512) {
		fail("archive length mismatch");
	}

	int seen[MAXFILES] = { 0 }, manifest = 0, ended = 0;
	size_t offset = 0;
	mkdirs(stage);

	while (offset + 512 <= len) {
		unsigned char *h = data + offset;

		if (zeros(h, 512)) {
			if (offset + 1024 > len || !zeros(h, len - offset)) {
				fail("invalid tar end marker");
			}

			ended = 1;
			break;
		}

		uint64_t sum = 0;

		for (int i = 0; i < 512; i++) {
			sum += (i >= 148 && i < 156) ? 32 : h[i];
		}

		if (sum != octal((void *)h + 148, 8)) {
			fail("tar checksum mismatch");
		}

		if (memcmp(h + 257, "ustar", 5) || h[262] != 0) {
			fail("only USTAR packages are accepted");
		}

		uint64_t size = octal((void *)h + 124, 12), mode = octal((void *)h + 100, 8);

		if (size > PACKAGE_LIMIT || size > len - offset - 512 || mode & ~0777 ||
		    octal((void *)h + 108, 8) || octal((void *)h + 116, 8)) {
			fail("invalid tar metadata");
		}

		char name[512], part[101], prefix[156];

		if (!memchr(h, 0, 100) || !memchr(h + 345, 0, 155)) {
			fail("unterminated tar path");
		}

		memcpy(part, h, 100);
		part[100] = 0;
		memcpy(prefix, h + 345, 155);
		prefix[155] = 0;

		if (*prefix) {
			snprintf(name, sizeof name, "%s/%s", prefix, part);
		} else {
			snprintf(name, sizeof name, "%s", part);
		}

		int type = h[156] == '5' ? 1 : 0;

		if (h[156] != 0 && h[156] != '0' && h[156] != '5') {
			fail("links/devices/PAX records are forbidden");
		}

		if (type && *name && name[strlen(name) - 1] == '/') {
			name[strlen(name) - 1] = 0;
		}

		if (!safe_path(name)) {
			fail("unsafe tar path");
		}

		const void *body = h + 512;

		if (!strcmp(name, "manifest.json")) {
			if (offset || manifest || type || size > META_LIMIT) {
				fail("manifest must be the first regular tar member");
			}

			struct json *j = parse_json(body, size);
			struct package *m = parse_package(j, 0);

			if (!package_same(p, m)) {
				fail("manifest does not match signed index");
			}

			free(m->manifest);
			free(m->files);
			free(m);
			free(j);
			manifest = 1;
		} else {
			if (!manifest || strncmp(name, "payload/", 8)) {
				fail("unexpected tar member");
			}

			int found = -1;

			for (int i = 0; i < p->nfile; i++) {
				if (!strcmp(name + 8, p->files[i].path)) {
					found = i;
				}
			}

			if (found < 0 || seen[found]++) {
				fail("undeclared or duplicate tar member");
			}

			struct file *f = p->files + found;

			if (f->type != type || f->size != size || f->mode != mode) {
				fail("payload metadata mismatch");
			}

			char digest[65];
			sha_bytes(body, size, digest);

			if (strcmp(digest, f->hash)) {
				fail("payload SHA256 mismatch");
			}

			if (!type) {
				char target[4096];
				snprintf(target, sizeof target, "%s/f%d", stage, found);
				write_bytes(target, body, size, mode);
			}
		}

		size_t padded = (size + 511) & ~(size_t)511;

		if (padded > len - offset - 512) {
			fail("truncated tar padding");
		}

		if (!zeros((void *)((const unsigned char *)body + size), padded - size)) {
			fail("nonzero tar padding");
		}

		offset += 512 + padded;
	}

	if (!ended || !manifest) {
		fail("incomplete archive");
	}

	for (int i = 0; i < p->nfile; i++) {
		if (!seen[i]) {
			fail("missing payload member %s", p->files[i].path);
		}
	}

	free(data);
}

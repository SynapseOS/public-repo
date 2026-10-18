// resolver.c: Выбор версий и зависимостей пакетов
// Автор: Елчинян Арен Артурович, 2026
// Построение плана установки с учётом конфликтов и установленных версий
// Подсистема: пакетный менеджер setup
// Создан: 11.10.2026
// Последнее изменение: 14.10.2026

#include "setup.h"

static void version(const char *s, uint32_t v[4]) {
	const char *p = s;

	for (int i = 0; i < 4; i++) {
		if (*p < '0' || *p > '9') {
			fail("invalid version %s (use A.B.C-rN)", s);
		}

		errno = 0;
		char *end;
		unsigned long long n = strtoull(p, &end, 10);

		if (errno || n > UINT32_MAX) {
			fail("version overflow");
		}

		v[i] = n;
		p = end;

		if (i < 2) {
			if (*p++ != '.') {
				fail("invalid version %s", s);
			}
		} else if (i == 2) {
			if (p[0] != '-' || p[1] != 'r') {
				fail("invalid version %s", s);
			}

			p += 2;
		} else if (*p) {
			fail("invalid version %s", s);
		}
	}
}

int version_cmp(const char *a, const char *b) {
	uint32_t x[4], y[4];
	version(a, x);
	version(b, y);

	for (int i = 0; i < 4; i++) {
		if (x[i] != y[i]) {
			return x[i] > y[i] ? 1 : -1;
		}
	}

	return 0;
}

int satisfies(const char *v, const char *constraint) {
	const char *p = constraint;

	while (*p) {
		while (*p == ' ') {
			p++;
		}

		int op = 0;

		if (*p == '=') {
			op = 1;
			p++;
		} else if (*p == '>' || *p == '<') {
			op = *p == '>' ? 2 : 4;
			p++;

			if (*p == '=') {
				op++;
				p++;
			}
		} else {
			fail("invalid dependency constraint %s", constraint);
		}

		while (*p == ' ') {
			p++;
		}

		const char *e = strchr(p, ',');
		size_t n = e ? (size_t)(e - p) : strlen(p);

		while (n && p[n - 1] == ' ') {
			n--;
		}

		char ver[64];

		if (!n || n >= sizeof ver) {
			fail("invalid dependency version");
		}

		memcpy(ver, p, n);
		ver[n] = 0;
		int c = version_cmp(v, ver);

		if ((op == 1 && c) || (op == 2 && c <= 0) || (op == 3 && c < 0) || (op == 4 && c >= 0) ||
		    (op == 5 && c > 0)) {
			return 0;
		}

		if (!e) {
			break;
		}

		p = e + 1;

		if (!*p) {
			fail("empty dependency constraint");
		}
	}

	return 1;
}

static void dependency(const char *s, char name[64], const char **constraint) {
	const char *e = strchr(s, ' ');
	size_t n = e ? (size_t)(e - s) : strlen(s);

	if (n >= 64 || !n) {
		fail("invalid dependency");
	}

	memcpy(name, s, n);
	name[n] = 0;

	if (!valid_name(name)) {
		fail("invalid dependency name");
	}

	*constraint = e ? e + 1 : "";
}

static struct package *oldpkg(const char *name) {
	for (int i = 0; i < ninstalled; i++) {
		if (!strcmp(installed[i]->name, name)) {
			return installed[i];
		}
	}

	return NULL;
}

static int protected(const char *name) {
	return !strcmp(name, "kernel") || !strcmp(name, "musl") || !strcmp(name, "init") ||
	       !strcmp(name, "setup") || !strcmp(name, "base-system");
}

static struct {
	char name[64], constraint[192];
	int explicit, upgrade;
} requests[MAXPKG];

static int nrequest, steps;
static struct package *selected[MAXPKG];
static int nselected;

static int lookup(const char *name) {
	for (int i = 0; i < nselected; i++) {
		if (!strcmp(selected[i]->name, name)) {
			return i;
		}
	}

	return -1;
}

static int admissible(struct package *p) {
	if (p->installed) {
		return 1;
	}

	struct repo *r = repos + p->repo;

	if (strcmp(p->arch, "x86_64") || strcmp(p->abi, ABI)) {
		return 0;
	}

	if (protected(p->name) && !r->official) {
		return 0;
	}

	if (*r->prefix && strncmp(p->name, r->prefix, strlen(r->prefix))) {
		return 0;
	}

	struct package *old = oldpkg(p->name);

	if (old && strcmp(old->url, r->id)) {
		return 0;
	}

	if (!strcmp(p->kind, "kernel") && (strcmp(p->name, "kernel") || !r->official)) {
		return 0;
	}

	return 1;
}

static int constraints(struct package *p) {
	for (int i = 0; i < nrequest; i++) {
		if (!strcmp(requests[i].name, p->name) && !satisfies(p->version, requests[i].constraint)) {
			return 0;
		}
	}

	for (int i = 0; i < nselected; i++) {
		for (int d = 0; d < selected[i]->ndep; d++) {
			char name[64];
			const char *c;
			dependency(selected[i]->dep[d], name, &c);

			if (!strcmp(name, p->name) && !satisfies(p->version, c)) {
				return 0;
			}
		}
	}

	struct package *old = oldpkg(p->name);

	if (old && version_cmp(p->version, old->version) < 0) {
		int explicit = 0;

		for (int i = 0; i < nrequest; i++) {
			if (!strcmp(requests[i].name, p->name) && requests[i].explicit &&
			    requests[i].constraint[0] == '=') {
				explicit = 1;
			}
		}

		if (!explicit) {
			return 0;
		}
	}

	return 1;
}

static int conflicts(void) {
	for (int i = 0; i < nselected; i++) {
		for (int d = 0; d < selected[i]->nconf; d++) {
			if (lookup(selected[i]->conflict[d]) >= 0) {
				return 1;
			}
		}
	}

	return 0;
}

static int cycle_visit(int n, int *colors) {
	if (colors[n] == 1) {
		return 1;
	}

	if (colors[n] == 2) {
		return 0;
	}

	colors[n] = 1;

	for (int d = 0; d < selected[n]->ndep; d++) {
		char name[64];
		const char *c;
		dependency(selected[n]->dep[d], name, &c);
		int k = lookup(name);

		if (k >= 0 && cycle_visit(k, colors)) {
			return 1;
		}
	}

	colors[n] = 2;
	return 0;
}

static int search(void) {
	if (++steps > 20000) {
		fail("dependency search limit exceeded");
	}

	for (int i = 0; i < nselected; i++) {
		if (!constraints(selected[i])) {
			return 0;
		}
	}

	if (conflicts( )) {
		return 0;
	}

	char missing[64] = "";

	for (int i = 0; i < nrequest; i++) {
		if (lookup(requests[i].name) < 0) {
			strcpy(missing, requests[i].name);
			break;
		}
	}

	if (!*missing) {
		for (int i = 0; i < nselected && !*missing; i++) {
			for (int d = 0; d < selected[i]->ndep; d++) {
				char name[64];
				const char *c;
				dependency(selected[i]->dep[d], name, &c);

				if (lookup(name) < 0) {
					strcpy(missing, name);
					break;
				}
			}
		}
	}

	if (!*missing) {
		int colors[MAXPKG] = { 0 };

		for (int i = 0; i < nselected; i++) {
			if (cycle_visit(i, colors)) {
				return 0;
			}
		}

		return 1;
	}

	if (nselected == MAXPKG) {
		fail("too many packages in plan");
	}

	struct package *candidates[MAXPKG + 1];
	int n = 0;
	struct package *old = oldpkg(missing);

	if (old) {
		candidates[n++] = old;
	}

	int priority = -1;

	for (int i = 0; i < navailable; i++) {
		struct package *p = available[i];

		if (strcmp(p->name, missing) || !admissible(p)) {
			continue;
		}

		int pr = repos[p->repo].priority;

		if (pr > priority) {
			priority = pr;
		}
	}

	for (int i = 0; i < navailable; i++) {
		struct package *p = available[i];

		if (!strcmp(p->name, missing) && admissible(p) && repos[p->repo].priority == priority) {
			candidates[n++] = p;
		}
	}

	for (int i = 0; i < n; i++) {
		for (int k = i + 1; k < n; k++) {
			if (version_cmp(candidates[k]->version, candidates[i]->version) > 0) {
				struct package *tmp = candidates[i];
				candidates[i] = candidates[k];
				candidates[k] = tmp;
			} // Не меняем незапрошенный установленный пакет, если его версия
			// удовлетворяет зависимостям
		}
	}

	int upgrade = 0;

	for (int i = 0; i < nrequest; i++) {
		if (!strcmp(requests[i].name, missing) && requests[i].upgrade) {
			upgrade = 1;
		}
	}

	if (old && !upgrade) {
		for (int i = 0; i < n; i++) {
			if (candidates[i] == old) {
				memmove(candidates + 1, candidates, i * sizeof *candidates);
				candidates[0] = old;
				break;
			}
		}
	}

	for (int i = 0; i < n; i++) {
		struct package *p = candidates[i];

		if (!constraints(p)) {
			continue;
		}

		int ambiguous = 0;

		if (!p->installed) {
			for (int k = i + 1; k < n; k++) {
				if (!candidates[k]->installed && candidates[k]->repo != p->repo &&
				    version_cmp(candidates[k]->version, p->version) == 0) {
					ambiguous = 1;
				}
			}
		}

		if (ambiguous) {
			fail("ambiguous repository for %s; adjust priorities", missing);
		}

		selected[nselected++] = p;

		if (search( )) {
			return 1;
		}

		nselected--;
	}

	return 0;
}

void solve(int argc, char **argv, int upgrade) {
	for (int i = 0; i < argc; i++) {
		if (nrequest == MAXPKG) {
			fail("too many requested packages");
		}

		char s[256];

		if (strlen(argv[i]) >= sizeof s) {
			fail("request too long");
		}

		strcpy(s, argv[i]);
		char *equal = strchr(s, '=');

		if (equal) {
			*equal++ = 0;
			snprintf(requests[nrequest].constraint, sizeof requests[nrequest].constraint, "= %s",
			         equal);
			version_cmp(equal, equal);
		}

		if (!valid_name(s)) {
			fail("invalid package request");
		}

		strcpy(requests[nrequest].name, s);
		requests[nrequest].explicit = 1;
		requests[nrequest].upgrade = 1;
		nrequest++;
	}

	for (int i = 0; i < ninstalled; i++) {
		int found = 0;

		for (int k = 0; k < nrequest; k++) {
			if (!strcmp(requests[k].name, installed[i]->name)) {
				found = 1;
			}
		}

		if (!found) {
			if (nrequest == MAXPKG) {
				fail("too many installed packages");
			}

			strcpy(requests[nrequest].name, installed[i]->name);
			requests[nrequest].upgrade = upgrade && !argc;
			nrequest++;
		}
	}

	if (!search( )) {
		fail("no dependency solution (missing package, incompatible constraints, "
		     "source, ABI, "
		     "conflict or cycle)");
	}

	nchosen = nselected;

	for (int i = 0; i < nselected; i++) {
		chosen[i] = selected[i];
		struct package *old = oldpkg(chosen[i]->name);
		chosen[i]->explicit = old ? old->explicit : 0;

		for (int k = 0; k < nrequest; k++) {
			if (!strcmp(requests[k].name, chosen[i]->name) && requests[k].explicit) {
				chosen[i]->explicit = 1;
			}
		}
	}
}

void plan_remove(int argc, char **argv) {
	for (int a = 0; a < argc; a++) {
		if (protected(argv[a])) {
			fail("cannot remove protected package %s", argv[a]);
		}

		if (!oldpkg(argv[a])) {
			fail("package is not installed: %s", argv[a]);
		}
	}

	for (int i = 0; i < ninstalled; i++) {
		int remove = 0;

		for (int a = 0; a < argc; a++) {
			if (!strcmp(argv[a], installed[i]->name)) {
				remove = 1;
			}
		}

		if (!remove) {
			chosen[nchosen++] = installed[i];
		}
	}

	for (int i = 0; i < nchosen; i++) {
		for (int d = 0; d < chosen[i]->ndep; d++) {
			char name[64];
			const char *c;
			dependency(chosen[i]->dep[d], name, &c);
			int found = 0;

			for (int k = 0; k < nchosen; k++) {
				if (!strcmp(chosen[k]->name, name) && satisfies(chosen[k]->version, c)) {
					found = 1;
				}
			}

			if (!found) {
				fail("%s requires %s", chosen[i]->name, chosen[i]->dep[d]);
			}
		}
	}
}

void verify(int argc, char **argv) {
	int errors = 0;

	for (int a = 0; a < argc; a++) {
		if (!oldpkg(argv[a])) {
			fail("package is not installed: %s", argv[a]);
		}
	}

	for (int i = 0; i < ninstalled; i++) {
		struct package *p = installed[i];
		int include = !argc;

		for (int a = 0; a < argc; a++) {
			if (!strcmp(argv[a], p->name)) {
				include = 1;
			}
		}

		if (!include) {
			continue;
		}

		for (int k = 0; k < p->nfile; k++) {
			struct file *f = p->files + k;
			char full[4096], h[65];
			path(full, f->path);
			struct stat st;

			if (lstat(full, &st) ||
			    !((f->type == 1 && S_ISDIR(st.st_mode)) || (f->type == 0 && S_ISREG(st.st_mode)))) {
				fprintf(stderr, "Missing/invalid: %s\n", f->path);
				errors++;
				continue;
			}

			if (!f->type) {
				sha_file(full, h);

				if (strcmp(h, f->hash)) {
					fprintf(stderr, "Modified%s: %s\n", f->config ? " config" : "", f->path);
					errors++;
				}
			}
		}

		printf("%s %s checked\n", p->name, p->version);
	}

	if (errors) {
		fail("%d installed file(s) differ", errors);
	}
}

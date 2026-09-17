// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* Library reads no environment (docs/design/specs/
 * 2026-09-17-tunables-api-and-shared-refusal-codes.md §1). Walks src/ and
 * transport/src/ (relative to the repo root, where `make test` runs this from)
 * and refuses any `getenv(` outside the one sanctioned exception:
 * transport/src/reac_conf.c, which IS the daemon's layered-lookup implementation
 * (layer 2 of reac_conf_lookup's own precedence) rather than a knob site. Every
 * knob site itself now reads a struct the daemon sets (reac_tunables.h).
 *
 * A directory scan that silently finds nothing looks identical to "no
 * violations" (false-signals #2/"a search that finds nothing may be a broken
 * search") — so this asserts the scan actually walked a plausible number of
 * files before trusting an empty violation list. */
#include <dirent.h>
#include <stdio.h>
#include <string.h>

static const char *EXEMPT[] = {
	"transport/src/reac_conf.c",   /* the sanctioned env-reading layer itself */
};

static int is_exempt(const char *path)
{
	for (size_t i = 0; i < sizeof EXEMPT / sizeof EXEMPT[0]; i++)
		if (strcmp(path, EXEMPT[i]) == 0)
			return 1;
	return 0;
}

static int scan_dir(const char *dir, int *files_seen, int *violations)
{
	DIR *d = opendir(dir);
	if (!d) {
		fprintf(stderr, "test_no_getenv_conformance: could not open %s\n", dir);
		return 0;
	}
	struct dirent *e;
	while ((e = readdir(d))) {
		size_t n = strlen(e->d_name);
		if (n < 3 || strcmp(e->d_name + n - 2, ".c") != 0)
			continue;
		char path[512];
		snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
		(*files_seen)++;
		FILE *f = fopen(path, "r");
		if (!f) {
			fprintf(stderr, "test_no_getenv_conformance: could not open %s\n", path);
			continue;
		}
		char line[2048];
		int lineno = 0;
		while (fgets(line, sizeof line, f)) {
			lineno++;
			if (strstr(line, "getenv(") && !is_exempt(path)) {
				fprintf(stderr, "test_no_getenv_conformance: %s:%d reads the "
				        "environment directly — libreac takes knobs through "
				        "reac_tunables.h, never getenv\n", path, lineno);
				(*violations)++;
			}
		}
		fclose(f);
	}
	closedir(d);
	return 1;
}

int main(void)
{
	int files_seen = 0, violations = 0;
	int ok = 1;
	ok &= scan_dir("src", &files_seen, &violations);
	ok &= scan_dir("transport/src", &files_seen, &violations);

	/* expectFound(): a scan of both directories that saw fewer than 15 .c files
	 * did not really run — the repo carries far more than that today (~30 in
	 * src/, ~20 in transport/src/) — so a low count means the CWD was wrong,
	 * not that the tree shrank. */
	if (!ok || files_seen < 15) {
		fprintf(stderr, "test_no_getenv_conformance: FAIL — scanned only %d .c "
		        "files (expected the run cwd to be the repo root); this is a "
		        "broken scan, not a clean one\n", files_seen);
		return 1;
	}

	if (violations) {
		fprintf(stderr, "test_no_getenv_conformance: FAIL — %d getenv( site(s) "
		        "outside the sanctioned exception, across %d files scanned\n",
		        violations, files_seen);
		return 1;
	}

	printf("test_no_getenv_conformance: PASS — %d .c files scanned, 0 "
	       "unsanctioned getenv(\n", files_seen);
	return 0;
}

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_code — the ONE token list every refusal, failure and notable status line
 * in libreac and its consumers carries (<reac/reac_code.h>).
 *
 * The list is an X-macro so the enum and the token table cannot drift apart. This
 * pins what the X-macro alone does not:
 *   1. every token is its enumerator's name without "RC_", so the enum and the
 *      printed token cannot be renamed apart;
 *   2. no two enumerators share a token (a grep for one would match two events);
 *   3. reac-pw's own tokens are declared here, so its copy can become an include;
 *   4. RC_NONE has no token and reads "?", never a plausible one. */
#include <reac/reac_code.h>

#include <stdio.h>
#include <string.h>

static int fails;
#define CHK(cond) do { \
	if (!(cond)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

struct entry {
	enum reac_code code;
	const char *name;
	const char *token;
};

static const struct entry entries[] = {
#define X(name, token) { name, #name, token },
	REAC_CODE_LIST(X)
#undef X
};
#define N_ENTRIES (sizeof entries / sizeof entries[0])

static int declared(const char *token)
{
	for (size_t i = 0; i < N_ENTRIES; i++)
		if (!strcmp(entries[i].token, token))
			return 1;
	return 0;
}

int main(void)
{
	/* the control: a list that expanded to nothing would pass every loop below */
	if (N_ENTRIES < 2) {
		fprintf(stderr, "NOT A RESULT: REAC_CODE_LIST expanded to %zu entries\n", N_ENTRIES);
		return 2;
	}

	for (size_t i = 0; i < N_ENTRIES; i++) {
		const struct entry *e = &entries[i];
		/* 1. RC_<TOKEN> */
		CHK(strncmp(e->name, "RC_", 3) == 0 && strcmp(e->name + 3, e->token) == 0);
		CHK(e->token[0] == 'E' || e->token[0] == 'S');
		CHK(e->token[1] == '_');
		/* the enumerator and the table agree */
		CHK(strcmp(reac_code_token(e->code), e->token) == 0);
		CHK(e->code == (enum reac_code)(i + 1));
		/* 2. unique */
		for (size_t j = i + 1; j < N_ENTRIES; j++)
			CHK(strcmp(e->token, entries[j].token) != 0);
	}

	/* 3. reac-pw's own (its src/reac_code.h), declared once, here */
	static const char *const reac_pw_tokens[] = {
		"E_SIZING", "E_ROOT_REFUSED", "E_SEGMENT_HELD", "E_ENROLL_REFUSED",
		"E_LINK_BUDGET", "E_ORPHAN_PAIR", "E_ROSTER_REMOVE", "E_ROSTER_NODE",
		"S_SEGMENT_HEARD", "S_BUDGET_YIELDED", "S_SEGMENT_UP", "S_SEGMENT_DROPPED",
		"S_KNOB_SET", "S_KNOB_SUMMARY", "E_UNKNOWN_KNOB", "S_NO_OVERRIDES",
	};
	for (size_t i = 0; i < sizeof reac_pw_tokens / sizeof reac_pw_tokens[0]; i++)
		if (!declared(reac_pw_tokens[i])) {
			fprintf(stderr, "FAIL: reac-pw's token %s is not declared in REAC_CODE_LIST\n",
			        reac_pw_tokens[i]);
			fails++;
		}

	/* 4. */
	CHK(strcmp(reac_code_token(RC_NONE), "?") == 0);

	if (fails) {
		printf("%d reac_code check(s) failed\n", fails);
		return 1;
	}
	printf("OK: reac_code — %zu tokens, each its enumerator's name and unique, reac-pw's "
	       "included\n", N_ENTRIES);
	return 0;
}

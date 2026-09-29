// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_cfg — the ONE declaration of the reac.cfg.* / reac.rate.* / reac.role
 * vocabulary, and the consumer headers that name it (libreac review 2026-09-25, M7).
 *
 * tests/conformance-cfg-declared-once.sh proves the SHAPE (read, never restated).
 * This proves the VALUES line up where a shape test cannot see them:
 *   1. the role flag's encoding is enum reac_role's, not a second 0/1;
 *   2. the closed rate list is the REAC_MODE_* descriptors' rates;
 *   3. the refusal tables are indexed right — the idle answer is the same "none"
 *      for rate and role, which is the drift this file exists to stop (reac-pw's
 *      own names are its aliases of these, pinned in that repo);
 *   4. the refusal sentinel is the one libreac's own arbitration publishes. */
#include <reac/reac.h>
#include <reac/reac_cfg.h>
#include <reac/reac_role.h>
#include <reac/reac_arbitration.h>

#include <stdio.h>
#include <string.h>

static int fails;
#define CHK(cond) do { \
	if (!(cond)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

_Static_assert(REAC_CFG_ROLE_MASTER == REAC_ROLE_MASTER, "role flag 0 is enum reac_role's master");
_Static_assert(REAC_CFG_ROLE_SLAVE == REAC_ROLE_SLAVE, "role flag 1 is enum reac_role's slave");

int main(void)
{
	/* 2. the closed list IS the descriptors' rates */
	CHK(REAC_MODE_44K1.sample_rate == REAC_CFG_RATE_44100);
	CHK(REAC_MODE_48K.sample_rate == REAC_CFG_RATE_48000);
	CHK(REAC_MODE_96K.sample_rate == REAC_CFG_RATE_96000);
	CHK(reac_rate_snap(3675.0) == REAC_CFG_RATE_44100);
	CHK(reac_rate_snap(4000.0) == REAC_CFG_RATE_48000);
	CHK(reac_rate_snap(8000.0) == REAC_CFG_RATE_96000);
	CHK(reac_rate_snap(3837.0) == REAC_CFG_RATE_44100);   /* the midpoints held */
	CHK(reac_rate_snap(3838.0) == REAC_CFG_RATE_48000);
	CHK(reac_rate_snap(5999.0) == REAC_CFG_RATE_48000);
	CHK(reac_rate_snap(6000.0) == REAC_CFG_RATE_96000);

	/* 3. the refusal tables and the rate bits */
	CHK((REAC_RATE_ALL_BITS & (REAC_RATE_BIT_44100 | REAC_RATE_BIT_48000 |
	                           REAC_RATE_BIT_96000)) == REAC_RATE_ALL_BITS);
	CHK(REAC_RATE_BIT_44100 != REAC_RATE_BIT_48000 && REAC_RATE_BIT_48000 != REAC_RATE_BIT_96000);

	static const char *const rate_codes[] = REAC_RATE_REFUSE_CODES_INIT;
	static const char *const role_codes[] = REAC_ROLE_REFUSE_CODES_INIT;
	CHK(sizeof rate_codes / sizeof rate_codes[0] == REAC_RATE_REFUSE_MALFORMED + 1);
	CHK(sizeof role_codes / sizeof role_codes[0] == REAC_ROLE_REFUSE_MALFORMED + 1);
	CHK(strcmp(rate_codes[REAC_RATE_REFUSE_NONE], "none") == 0);
	CHK(strcmp(role_codes[REAC_ROLE_REFUSE_NONE], "none") == 0);
	CHK(strcmp(rate_codes[REAC_RATE_REFUSE_NOT_CLOSED], "not_closed") == 0);
	CHK(strcmp(rate_codes[REAC_RATE_REFUSE_ROLE_SLAVE], "role_slave") == 0);
	CHK(strcmp(role_codes[REAC_ROLE_REFUSE_MALFORMED], "malformed") == 0);
	for (size_t i = 0; i < sizeof rate_codes / sizeof rate_codes[0]; i++)
		CHK(rate_codes[i] != NULL);   /* no hole in the enum-indexed table */

	/* 4. one refusal sentinel across the library */
	CHK(strcmp(reac_rival_refusal(REAC_RIVAL_NONE), REAC_CFG_REFUSED_NONE) == 0);

	if (fails) {
		printf("%d reac_cfg check(s) failed\n", fails);
		return 1;
	}
	printf("OK: reac_cfg — one declaration: the role flag is enum reac_role's, the rate "
	       "list is the descriptors', the refusal tables resolve to it, "
	       "and \"none\" is the one idle refusal\n");
	return 0;
}

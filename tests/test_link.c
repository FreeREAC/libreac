// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* test_link — the control plane against the two captures that were GRANTED.
 *
 * These are not shape assertions. Both files are a real box enrolling with a real box, taken
 * on 2026-09-09 with reac-pw stopped, and each ends in a grant; what this asserts is that
 * what reac_link builds for the same step is the same bytes that were granted. A rule that
 * is not in a capture cannot be added without one, and a rule that is cannot be broken
 * without this going red.
 */
#include <reac/reac_link.h>
#include <stdio.h>
#include <string.h>

#define CHK(x) do { if (!(x)) { \
	fprintf(stderr, "FAIL: %s (line %d)\n", #x, __LINE__); return 1; } } while (0)

int main(void)
{
	static const uint8_t M[6] = { 0x00, 0x40, 0xab, 0xc4, 0x80, 0x41 };  /* the master  */
	static const uint8_t S[6] = { 0x00, 0x40, 0xab, 0xc4, 0xdc, 0x9c };  /* the joiner  */
	uint8_t f[2048];
	char got[128];

	/* THE BURST, step for step, against the S-0808's own frames at t=16.1162/16.1164 and
	 * the heartbeat it put on the very next frame at 16.1165 — two milliseconds before the
	 * S-1608 granted it. */
	static const char *WANT[3] = {
		"cdea04030014000200fe0ff0410a0000121201000600010078f70000000000000000",
		"cdea04030013000200fe0ef0410a0000121203020001007af7000000000000000002",
		"cdea01030001810000000000000000000000000000000000000000000000000000",
	};
	for (int step = 0; step < 3; step++) {
		int n = reac_link_slave_burst(step, f, M, S, 0, 8, NULL, 0);
		CHK(n == 340);
		int hex = (int)strlen(WANT[step]) / 2;
		for (int i = 0; i < hex; i++)
			sprintf(got + i * 2, "%02x", f[16 + i]);
		CHK(strcmp(got, WANT[step]) == 0);
	}
	/* AND THERE IS NO STEP 3. The join is two records and a heartbeat; the 0000 head_mark
	 * belongs to the master's grant (see the header). */
	CHK(reac_link_slave_burst(3, f, M, S, 0, 8, NULL, 0) == 0);

	/* THE DECLARATION IS OUR OWN INVENTORY, and there are two captured. */
	CHK(reac_ctrl_build_config_announce_box_master(f, M, S, 0, 8) == 340);
	for (int i = 0; i < 34; i++) sprintf(got + i * 2, "%02x", f[16 + i]);
	CHK(strcmp(got, "cdea010300108000000001010101"
	                "0202030303030303000000000000000000000052") == 0);

	/* THE DESCRIPTOR'S THREE STATES, and the transition that decides each. */
	CHK(reac_link_desc_for(FSM_FLOOD_ANNOUNCE, 0) == REAC_LINK_DESC_NONE);
	CHK(reac_link_desc_for(FSM_COLDCONNECT, 1) == REAC_LINK_DESC_REQUESTING);
	CHK(reac_link_desc_for(FSM_ESTABLISHED, 1) == REAC_LINK_DESC_ESTABLISHED);
	memset(f, 0xff, sizeof f);
	reac_link_fill_descriptor(f, REAC_LINK_DESC_REQUESTING);
	for (int i = 0; i < 16; i++) {
		CHK(f[REAC_CTRL_BLOCK_OFF + i * 2] == 0x00);
		CHK(f[REAC_CTRL_BLOCK_OFF + i * 2 + 1] == 0x52);
	}
	reac_link_fill_descriptor(f, REAC_LINK_DESC_NONE);
	for (int i = 0; i < 32; i++) CHK(f[REAC_CTRL_BLOCK_OFF + i] == 0x00);

	printf("OK: reac_link — the join burst, the declaration and the descriptor are the bytes"
	       " two real boxes were granted for\n");
	return 0;
}

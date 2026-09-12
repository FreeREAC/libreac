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
#include <reac/reac_macaddr.h>
#include <reac/reac_fsm.h>
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

	/* THE LIBRARY CARRIES THE MAC PACKING ITS OWN reac_link_state.c CALLS. Declaring
	 * reac_mac48_unpack in a header libreac ships, and leaving the definition behind in
	 * the daemon, built a libreac.so with an undefined symbol: every reac-pw test target
	 * that linked it failed `ld returned 1` in the RPM's %build, for a function whose
	 * caller is INSIDE this library. A round trip here is what refuses that shape. */
	{
		const uint8_t in[6] = { 0x00, 0x40, 0xab, 0x12, 0x34, 0x56 };
		uint8_t out[6];
		CHK(reac_mac48_pack(in) == 0x0040ab123456ull);
		reac_mac48_unpack(reac_mac48_pack(in), out);
		CHK(memcmp(in, out, 6) == 0);
		reac_mac48_unpack(0, out);
		for (int i = 0; i < 6; i++) CHK(out[i] == 0);
	}

	/* THE COURTSHIP THAT IS NEVER GRANTED MUST GET OFF THE WIRE — the opposite of
	 * everything above, and the one the M-200 capture of 2026-09-11 measured.
	 * An ungranted slave that courts forever feeds the desk's box-session liveness,
	 * so the desk never declares the session over and a real box booting beside us
	 * never gets courted (box-boot-with-slave-analysis.md §3). Drive the pure FSM
	 * with a master that announces and never grants, for longer than one full
	 * budget+backoff cycle, and require: COLDCONNECT ends within its budget, then
	 * a silence longer than the desk's measured 7.148 s session hold, then a
	 * re-flood. FPS is the 44.1 k wire rate of that capture (44100/12 = 3675) —
	 * both bounds are wall-clock facts about the master and scale with it. */
	{
		enum { FPS = 3675, RUN_S = 25 };
		static const uint8_t MM[6] = { 0x00, 0x40, 0xab, 0xc9, 0xcc, 0x03 };  /* the M-200 */
		struct reac_fsm fsm;
		struct reac_ctrl_parsed ann;
		memset(&ann, 0, sizeof ann);
		ann.kind = REAC_CTRL_MASTER_ANNOUNCE;    /* cfea 1/s in the capture; never a grant */
		memcpy(ann.src, MM, 6);

		reac_fsm_init(&fsm);
		fsm.heartbeat_period = FPS;              /* the slave sets this from the rate */
		reac_fsm_step(&fsm, FSM_EV_PHY_UP, NULL);

		int cold_entered = -1, cold_left = -1, first_gap = -1, gap = 0, longest = 0;
		int reflood_after_gap = -1;
		for (int i = 0; i < FPS * RUN_S; i++) {
			struct reac_fsm_out s2 = reac_fsm_step(&fsm, FSM_EV_RX, &ann);
			if (cold_entered < 0 && s2.state == FSM_COLDCONNECT)
				cold_entered = i;
			if (cold_entered >= 0 && cold_left < 0 && s2.state != FSM_COLDCONNECT)
				cold_left = i;
			if (s2.action == FSM_ACT_STOP) {
				gap++;
				if (gap > longest) longest = gap;
			} else {
				if (gap > 0 && first_gap < 0) {
					first_gap = gap;
					reflood_after_gap = (s2.action == FSM_ACT_FLOOD_BCAST);
				}
				gap = 0;
			}
		}
		/* (a) it LEAVES the cold-connect, and within the 4 s budget. */
		CHK(cold_entered >= 0);
		CHK(cold_left > cold_entered);
		CHK(cold_left - cold_entered <= REAC_FSM_COLDCONNECT_BUDGET_S * FPS);
		/* (b) then it emits NOTHING for the whole backoff — longer than the desk's
		 *     7.148 s hold, which is the entire point of the number. */
		CHK(first_gap >= REAC_FSM_BACKOFF_S * FPS);
		CHK(REAC_FSM_BACKOFF_S * FPS > 7.148 * FPS);
		/* (c) and then it courts again: the gap ends in a fresh broadcast flood. */
		CHK(reflood_after_gap == 1);
		/* The whole ungranted run is a duty cycle, not a carrier: over 25 s it is
		 * off the wire for at least one full backoff. */
		CHK(longest >= REAC_FSM_BACKOFF_S * FPS);
	}

	printf("OK: reac_link — the join burst, the declaration and the descriptor are the bytes"
	       " two real boxes were granted for, and an ungranted courtship gets off the wire\n");
	return 0;
}

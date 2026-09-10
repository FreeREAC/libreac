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
#include <reac/reac.h>          /* REAC_MAX_CHANNELS, REAC_SAMPLES_PER_PKT */
#include <reac/reac_encode.h>   /* reac_downstream_build — the desk-side carrier */
#include <reac/reac_macaddr.h>
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

	/* ---- THE HEAD-AMP RECORD IS ROLE-BLIND (operator ruling, 2026-09-10) ----
	 *
	 * "libreac should allow preamp control in any mode (m, s or SP)" and "there is no
	 * change in the protocol once we exchange frames, it is exactly the same". This is
	 * the byte proof of both, and it needed no new builder: `reac_ctrl_stamp_headamp`
	 * writes frame[16] through frame[49] and nothing else — the REAC type word plus the
	 * 32-byte control block — and `spec/reac.ksy` gives that window the SAME absolute
	 * offsets in every 0x8819 frame ("control, size: 34, i.e. frame[16:50]", ahead of an
	 * `audio` region whose LENGTH is the only thing a width changes). So the 1492 B
	 * downstream a desk broadcasts and the 628 B upstream a box returns carry a head-amp
	 * SET in the same bytes, and which end of the wire holds the clock never enters it.
	 *
	 * THE GOLDEN IS THE RIG'S OWN. These 34 bytes are what reac-pw put on enp131s0 at the
	 * S-1608 on 2026-09-09 (ha-write-s1608.pcap, four distinct cdea 04 03 blocks: ch 0x20
	 * = the box's input 1 at the S-1608 head-amp base, param 00/01/02 = phantom/pad/SENS,
	 * the DT1 record checksum, f7, and the block checksum 0x02). Asserting against the
	 * capture rather than against our own second call is what makes this a conformance
	 * test and not a tautology. */
	{
		static const struct { uint8_t ch, param, value; const char *want; } HA[] = {
			{ 0x20, 0, 0, "cdea04030013000200fe0ef0410a0000121201012000005ef7"
			              "000000000000000002" },
			{ 0x20, 0, 1, "cdea04030013000200fe0ef0410a0000121201012000015df7"
			              "000000000000000002" },
			{ 0x20, 1, 0, "cdea04030013000200fe0ef0410a0000121201012001005df7"
			              "000000000000000002" },
			{ 0x20, 2, 0x34, "cdea04030013000200fe0ef0410a00001212010120023428f7"
			                 "000000000000000002" },
		};
		/* One frame of each geometry, built by the builders that own them, with audio
		 * the stamp must not touch: a distinct constant per channel. */
		float pcm[REAC_MAX_CHANNELS][REAC_SAMPLES_PER_PKT];
		float *planar[REAC_MAX_CHANNELS];
		for (int c = 0; c < REAC_MAX_CHANNELS; c++) {
			planar[c] = pcm[c];
			for (int s = 0; s < REAC_SAMPLES_PER_PKT; s++)
				pcm[c][s] = (float)(c + 1) / 64.0f;
		}
		uint8_t down[2048], up[2048], down0[2048], up0[2048];
		int dn = reac_downstream_build(down, (float *const *)planar,
		                               REAC_MAX_CHANNELS, REAC_SAMPLES_PER_PKT, 0x1234, S);
		size_t un = reac_ctrl_build_upstream_filler(up, M, S, 0x1234, 16, planar,
		                                            REAC_SAMPLES_PER_PKT);
		CHK(dn == 1492);            /* the master's downstream broadcast   */
		CHK(un == 628);             /* an S-1608's own 16-channel return   */
		memcpy(down0, down, (size_t)dn);
		memcpy(up0, up, un);

		for (size_t k = 0; k < sizeof HA / sizeof HA[0]; k++) {
			CHK(reac_ctrl_stamp_headamp(down, HA[k].ch, HA[k].param,
			                            HA[k].value) == 0);
			CHK(reac_ctrl_stamp_headamp(up, HA[k].ch, HA[k].param,
			                            HA[k].value) == 0);
			/* 1. the rig's bytes, on the desk's frame */
			for (int i = 0; i < 34; i++) sprintf(got + i * 2, "%02x", down[16 + i]);
			CHK(strcmp(got, HA[k].want) == 0);
			/* 2. and BYTE-IDENTICALLY on the box-width frame — the ruling */
			CHK(memcmp(down + 16, up + 16, 34) == 0);
			/* 3. the record's own DT1 checksum verifies in either carrier */
			CHK(reac_ctrl_headamp_record_verify(down) == 0);
			CHK(reac_ctrl_headamp_record_verify(up) == 0);
			/* 4. and NOTHING ELSE MOVED: the counter, both audio regions and both
			 *    end markers are the bytes their builders wrote. A stamp that
			 *    reached into the audio would land here, not on a rig. */
			CHK(memcmp(down, down0, 16) == 0);
			CHK(memcmp(down + 50, down0 + 50, (size_t)dn - 50) == 0);
			CHK(memcmp(up, up0, 16) == 0);
			CHK(memcmp(up + 50, up0 + 50, un - 50) == 0);
		}
		/* THE COMPARE CAN SEE A DIFFERENCE. Without this, a stamp that wrote nothing
		 * at all would pass every equality above. */
		CHK(reac_ctrl_stamp_headamp(up, 0x21, 2, 0x10) == 0);
		CHK(memcmp(down + 16, up + 16, 34) != 0);
		/* AND A BAD CELL IS REFUSED IN EITHER CARRIER, leaving the frame untouched —
		 * the same contract, not a role-dependent one. */
		memcpy(up0, up, un);
		CHK(reac_ctrl_stamp_headamp(up, 0x20, 0, 0x02) == -1);   /* phantom is 0/1 */
		CHK(reac_ctrl_stamp_headamp(up, 0x20, 9, 0x00) == -1);   /* no such param  */
		CHK(memcmp(up, up0, un) == 0);
	}

	printf("OK: reac_link — the join burst, the declaration and the descriptor are the bytes"
	       " two real boxes were granted for, and a head-amp SET is the same 34 bytes"
	       " whichever end of the wire carries it\n");
	return 0;
}

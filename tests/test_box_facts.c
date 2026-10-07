// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* A connected box's facts come from its frames (reac_box_facts.h, 1.7.0):
 *   1. the hw block names the family, one value per captured family, and nothing else;
 *   2. the name is the family (or the name the box sent) and the DECLARED widths, and a
 *      catalogue entry that disagrees with the box is a defect, never a correction;
 *   3. the S-0808's name record, split over two link-4 frames, is reassembled;
 *   4. an established master re-polls the identity page until it is answered.
 * Every frame here is a captured one (reac-protocol spec/fixtures/control.json) unless
 * it says otherwise. */
#include <reac/reac_box_facts.h>
#include <reac/reac_ctrlblk.h>
#include <reac/reac_identity.h>
#include <reac/reac_master.h>
#include <reac/reac_ports.h>
#include <reac/reac.h>
#include <stdio.h>
#include <string.h>

#define CHK(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

static const uint8_t BOX[6] = { 0x00, 0x40, 0xab, 0xc4, 0x80, 0x01 };
static const uint8_t US[6]  = { 0x00, 0x14, 0x5c, 0x9b, 0x28, 0x2d };

/* A frame around a captured frame[16:50] window (type word + control block). */
static size_t frame_of(uint8_t *f, const char *hex34)
{
	memset(f, 0, REAC_FRAME_BYTES);
	memcpy(f, US, 6);
	memcpy(f + 6, BOX, 6);
	f[12] = 0x88; f[13] = 0x19;
	for (int i = 0; i < 34; i++) {
		unsigned v;
		sscanf(hex34 + 2 * i, "%2x", &v);
		f[16 + i] = (uint8_t)v;
	}
	return 340;
}

/* Captured identity replies and the S-0808's split name record. */
#define S0808_CC0016 "cdea04030016000200fe11f0410a00001212050000000100000377f70000000000fc"
#define S0808_CC001A "cdea0403001a000200fe15f0410a0000121205000600000000010000000074f700f4"
#define S1608_CC001A "cdea0403001a000200fe15f0410a000012120500060000000002000300026ef700f4"
#define S4000_CC001A "cdea0403001a000200fe15f0410a0000121205000600000000020001000270f700f4"
#define S0808_NAME1  "cdea0401001b000200fe16f0410a000012120500100001532d303830380000000005"
#define S0808_NAME2  "cdea0402000d000200fe080000000000001af70000000000000000000000000000d4"
/* CAPTURED: the re-fitted S-4000S, 16 in / 24 out (s4000s-1624-announce2.pcap). */
#define S4000S_1624  "cdea0103001084000000020202020101010101010303000300000001000000000050"

static int ingest(struct reac_identity *id, struct reac_identity_frag *fr, const char *hex)
{
	uint8_t f[REAC_FRAME_BYTES];
	size_t n = frame_of(f, hex);
	uint16_t addr;
	const uint8_t *p;
	size_t pl;
	if (reac_ctrl_identity_reply(f, n, &addr, &p, &pl) == 1 ||
	    reac_ctrl_identity_fragment(fr, f, n, &addr, &p, &pl) == 1)
		return reac_identity_ingest(id, addr, p, pl);
	return 0;
}

static int names(enum reac_box_family fam, const char *said, int in, int out,
                 const char *want_name, const char *want_token, const char *want_display)
{
	char n[REAC_BOX_NAME_MAX], t[REAC_BOX_NAME_TOKEN_MAX], d[REAC_BOX_NAME_DISPLAY_MAX];
	CHK(reac_box_name(fam, said, in, out, n, sizeof n, t, sizeof t, d, sizeof d) == 0);
	if (strcmp(n, want_name) || (want_token && strcmp(t, want_token)) ||
	    (want_display && strcmp(d, want_display))) {
		fprintf(stderr, "FAIL: name %s / %s / %s\n", n, t, d);
		return 1;
	}
	return 0;
}

int main(void)
{
	/* ---- 1. the hw block names the family ---- */
	{
		struct reac_identity id;
		struct reac_identity_frag fr;
		memset(&fr, 0, sizeof fr);
		reac_identity_init(&id);
		CHK(reac_box_family_of(&id) == REAC_BOX_FAMILY_UNKNOWN);  /* no answer yet */
		CHK(ingest(&id, &fr, S1608_CC001A) == 1);
		CHK(reac_box_family_of(&id) == REAC_BOX_FAMILY_S1608);
		reac_identity_init(&id);
		CHK(ingest(&id, &fr, S4000_CC001A) == 1);
		CHK(reac_box_family_of(&id) == REAC_BOX_FAMILY_S4000S);
		reac_identity_init(&id);
		CHK(ingest(&id, &fr, S0808_CC001A) == 1);
		CHK(reac_box_family_of(&id) == REAC_BOX_FAMILY_S0808);
		id.reac_version_raw[7] = 3;           /* a REAC version nobody captured */
		CHK(reac_box_family_of(&id) == REAC_BOX_FAMILY_UNKNOWN);
	}

	/* ---- 2. the name: family + declared widths ---- */
	CHK(names(REAC_BOX_FAMILY_S1608, NULL, 16, 8, "S-1608", "s1608", "S-1608 (16 in / 8 out)") == 0);
	CHK(names(REAC_BOX_FAMILY_S0808, NULL, 8, 8, "S-0808", "s0808", NULL) == 0);
	CHK(names(REAC_BOX_FAMILY_S4000S, NULL, 32, 8, "S-4000S-3208", "s4000s-3208", NULL) == 0);
	CHK(names(REAC_BOX_FAMILY_S4000S, NULL, 8, 32, "S-4000S-0832", NULL, NULL) == 0);
	CHK(names(REAC_BOX_FAMILY_S4000S, NULL, 40, 0, "S-4000S-4000", NULL,
	          "S-4000S-4000 (40 in / 0 out)") == 0);
	CHK(names(REAC_BOX_FAMILY_S1608, NULL, 8, 8, "S-1608-0808", NULL, NULL) == 0);
	CHK(names(REAC_BOX_FAMILY_UNKNOWN, NULL, 8, 16, "REAC-0816", "reac0816", NULL) == 0);
	CHK(names(REAC_BOX_FAMILY_UNKNOWN, "S-0808", 8, 8, "S-0808", NULL, NULL) == 0);
	{
		char n[24], t[24], d[48];
		CHK(reac_box_name(REAC_BOX_FAMILY_S1608, NULL, 0, 0, n, 24, t, 24, d, 48) == -1);
		CHK(reac_box_name(REAC_BOX_FAMILY_S1608, NULL, 42, 8, n, 24, t, 24, d, 48) == -1);
	}

	/* THE DECLARATION SIZES AND NAMES; A CATALOGUE ENTRY ONLY COMPARES. The captured
	 * 16/24 announce matches no entry, and an entry that disagrees is a defect. */
	{
		uint8_t f[REAC_FRAME_BYTES];
		size_t n = frame_of(f, S4000S_1624);
		struct reac_box_ports ports;
		CHK(reac_ports_parse(f + REAC_CTRL_BLOCK_OFF, &ports) == 0);
		CHK(ports.in_ch == 16 && ports.out_ch == 24);
		CHK(reac_box_catalogue_match(f, n) == NULL);
		char nm[24], tk[24], dp[48];
		CHK(reac_box_name(REAC_BOX_FAMILY_S4000S, NULL, ports.in_ch, ports.out_ch,
		                  nm, sizeof nm, tk, sizeof tk, dp, sizeof dp) == 0);
		CHK(strcmp(nm, "S-4000S-1624") == 0);

		const struct reac_box_model *s1608 = reac_box_catalogue_by_token("s1608");
		CHK(s1608 != NULL);
		CHK(reac_box_catalogue_defect(s1608, 16, 8, "S-1608 (16 in / 8 out)") == 0);
		CHK(reac_box_catalogue_defect(s1608, 16, 24, "S-1608 (16 in / 8 out)") ==
		    REAC_BOX_DEFECT_WIDTH);
		CHK(reac_box_catalogue_defect(s1608, 16, 8, "S-4000S-1608 (16 in / 8 out)") ==
		    REAC_BOX_DEFECT_NAME);
		CHK(reac_box_catalogue_defect(NULL, 16, 8, "x") == 0);
	}

	/* ---- 3. the S-0808's name, in two frames ---- */
	{
		struct reac_identity id;
		struct reac_identity_frag fr;
		memset(&fr, 0, sizeof fr);
		reac_identity_init(&id);
		CHK(ingest(&id, &fr, S0808_NAME1) == 0);      /* FIRST alone closes nothing */
		CHK(!id.has_model_name);
		CHK(ingest(&id, &fr, S0808_NAME2) == 1);
		CHK(id.has_model_name && strcmp(id.model_name, "S-0808") == 0);
		CHK(ingest(&id, &fr, S0808_CC0016) == 1 && id.has_fw && id.fw_milli == 1003);
		/* a LAST with no FIRST is nothing */
		memset(&fr, 0, sizeof fr);
		reac_identity_init(&id);
		CHK(ingest(&id, &fr, S0808_NAME2) == 0 && !id.has_model_name);
		/* a FIRST from one box and a LAST from another do not join */
		uint8_t f1[REAC_FRAME_BYTES], f2[REAC_FRAME_BYTES];
		size_t n1 = frame_of(f1, S0808_NAME1), n2 = frame_of(f2, S0808_NAME2);
		f2[11] ^= 1;
		uint16_t a; const uint8_t *p; size_t pl;
		CHK(reac_ctrl_identity_fragment(&fr, f1, n1, &a, &p, &pl) == 0);
		CHK(reac_ctrl_identity_fragment(&fr, f2, n2, &a, &p, &pl) == 0);
		/* a corrupted name byte breaks the inner checksum across the two */
		memset(&fr, 0, sizeof fr);
		frame_of(f1, S0808_NAME1);
		CHK(f1[18 + 21] == 'S');
		f1[18 + 21] = 'R';                         /* the name's first letter */
		f1[18 + 31] = (uint8_t)(f1[18 + 31] + 1);  /* the block still sums to 0 */
		CHK(reac_ctrl_checksum_verify(f1) == 0);   /* so only the inner sum can catch it */
		frame_of(f2, S0808_NAME2);
		CHK(reac_ctrl_identity_fragment(&fr, f1, n1, &a, &p, &pl) == 0);
		CHK(reac_ctrl_identity_fragment(&fr, f2, n2, &a, &p, &pl) == 0);
	}

	/* ---- 4. the established master re-polls the identity page ----
	 * reacA-s0808-reacpw-coldboot.pcap (2026-10-07): reac-pw sent the six identity RQ1s
	 * at t = 1791361444.686, 0.8 s BEFORE the S-0808's JOIN (1791361445.4775), and the
	 * box never answered. The poll now repeats once a second after establishment until
	 * the firmware and hw block are in, at most REAC_M_IDENTITY_POLLS times. */
	{
		const int fps = 8000;
		struct reac_console_cfg cfg = { .out_channels = 8, .console_field = 1 };
		struct reac_master m;
		reac_master_init(&m, US, &cfg, fps);
		m.state = REAC_M_ESTABLISHED;
		/* the phases enter_established leaves (reset_control_cadence): cfea a quarter
		 * second in, chanmap three quarters — the arm must not sit under either */
		m.announce_tick = (3 * fps) / 4;
		m.est_chanmap_tick = fps / 4;
		/* the box keeps talking: every RX reloads link_check (reac_master_rx) */
#define HELD(m) ((m).link_check = (m).link_check_reload + 1)
		uint8_t want[REAC_GRANT_GROUPB_LEN][34];
		for (int i = 0; i < REAC_GRANT_GROUPB_LEN; i++)
			CHK(reac_ctrl_identity_poll_block(i, want[i]) == 0);
		CHK(reac_ctrl_identity_poll_block(REAC_GRANT_GROUPB_LEN, want[0]) == -1);

		int polls = 0, next = 0;
		uint8_t f[REAC_FRAME_BYTES];
		for (long s = 0; s < 2L * fps; s++) {
			uint16_t c; int idx = -1;
			HELD(m);
			if (reac_master_next(&m, &c, &idx) != REAC_M_EMIT_IDENTITY_POLL)
				continue;
			CHK(idx == next);
			memset(f, 0, sizeof f);
			CHK(reac_master_stamp(&m, f, REAC_M_EMIT_IDENTITY_POLL, idx) == 0);
			CHK(memcmp(f + 16, want[idx], 34) == 0);
			next = (next + 1) % REAC_GRANT_GROUPB_LEN;
			polls++;
		}
		CHK(polls == 2 * REAC_GRANT_GROUPB_LEN);       /* once a second, six RQ1s */

		reac_master_identity_answered(&m, 1);          /* the box answered */
		for (long s = 0; s < 3L * fps; s++) {
			uint16_t c; int idx;
			HELD(m);
			CHK(reac_master_next(&m, &c, &idx) != REAC_M_EMIT_IDENTITY_POLL);
		}

		reac_master_identity_answered(&m, 0);          /* never answers: capped */
		polls = 0;
		for (long s = 0; s < 30L * fps; s++) {
			uint16_t c; int idx;
			HELD(m);
			polls += reac_master_next(&m, &c, &idx) == REAC_M_EMIT_IDENTITY_POLL;
		}
		CHK(polls == (REAC_M_IDENTITY_POLLS - 2) * REAC_GRANT_GROUPB_LEN);
	}

	/* ---- 5. the Roland checksum is seven bits. A record whose sum before the checksum
	 * has an ODD number of 128s (here 0x00 0x00 + "AB": tag..data sum 05+00+10+00+01+41+42
	 * = 0x99, i.e. one 128) closes at 0 mod 256, not 0x80, and is still valid. */
	{
		uint8_t rec[9] = { 0x05, 0x00, 0x10, 0x00, 0x01, 0x41, 0x42, 0x00 };
		reac_ctrl_record_cksum_stamp(rec, 8);
		CHK(rec[7] < 0x80);                               /* a MIDI data byte */
		unsigned sum = 0;
		for (int i = 0; i < 8; i++) sum += rec[i];
		CHK(sum % 128 == 0 && sum % 256 == 0);            /* the case the old rule refused */
		CHK(reac_ctrl_record_cksum_verify(rec, 8) == 0);
		rec[6] ^= 1;
		CHK(reac_ctrl_record_cksum_verify(rec, 8) != 0);
	}

	printf("OK: box facts — the hw block names the family, the declaration and family "
	       "name the box, the catalogue only compares, the S-0808 name reassembles, the "
	       "identity page is re-polled after the box joins\n");
	return 0;
}

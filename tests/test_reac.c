// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include "reac/reac.h"
#include "reac/reac_ctrlblk.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } } while (0)

int main(void)
{
	/* rate snap: pps = rate/12, midpoints at 3837.5 and 6000 */
	CHECK(reac_rate_snap(3675) == 44100, "snap 3675 -> 44100");
	CHECK(reac_rate_snap(4000) == 48000, "snap 4000 -> 48000");
	CHECK(reac_rate_snap(8000) == 96000, "snap 8000 -> 96000");
	CHECK(reac_rate_snap(3837) == 44100, "snap 3837 -> 44100 (below mid)");
	CHECK(reac_rate_snap(3838) == 48000, "snap 3838 -> 48000 (above mid)");
	CHECK(reac_rate_snap(5999) == 48000, "snap 5999 -> 48000");
	CHECK(reac_rate_snap(6001) == 96000, "snap 6001 -> 96000");
	CHECK(reac_rate_snap(7900) == 96000, "snap ~8000 with jitter -> 96000");

	/* mode_for */
	CHECK(reac_mode_for(48000) == &REAC_MODE_48K, "mode_for 48000");
	CHECK(reac_mode_for(96000) == &REAC_MODE_96K, "mode_for 96000");
	CHECK(reac_mode_for(44100) == &REAC_MODE_44K1, "mode_for 44100");
	CHECK(reac_mode_for(12345) == &REAC_MODE_48K, "mode_for unknown -> 48k");
	CHECK(REAC_MODE_96K.n_channels == 40 && REAC_MODE_96K.samples_per_pkt == 12,
	      "96k is 40ch/12samp (settled double-pps, not channel-halving)");

	/* frame helpers */
	uint8_t f[20] = {0};
	f[12] = 0x88; f[13] = 0x19;            /* EtherType 0x8819 */
	f[14] = 0x34; f[15] = 0x12;            /* counter 0x1234, little-endian */
	CHECK(reac_frame_is_reac(f, sizeof f) == 1, "EtherType 0x8819 -> REAC");
	f[13] = 0x00;
	CHECK(reac_frame_is_reac(f, sizeof f) == 0, "wrong EtherType -> not REAC");
	CHECK(reac_frame_is_reac(f, 10) == 0, "too short -> not REAC");
	f[13] = 0x19;
	CHECK(reac_frame_counter(f) == 0x1234, "counter LE 0x1234");
	CHECK(reac_counter_gap(0x1233, 0x1234) == 0, "gap consecutive = 0");
	CHECK(reac_counter_gap(0x1233, 0x1236) == 2, "gap of 2");
	CHECK(reac_counter_gap(0xFFFF, 0x0001) == 1, "gap across 16-bit wrap = 1");

	/* HEAD-AMP SENS: one dB per step over all 56, and the library owns the law.
	 * It was only ever exercised from reac-pw, which is how a curve with three
	 * duplicate-gain steps lived here for as long as it did — nothing in libreac
	 * would have gone red if it were wrong. Measured 2026-08-23 on an S-0808
	 * electrical loopback; see reac/reac_ctrlblk.h. */
	CHECK(reac_headamp_sens_cdb(0x00, 0) == -1000, "SENS 0x00 pad off = -10.00 dBu");
	CHECK(reac_headamp_sens_cdb(0x37, 0) == -6500, "SENS 0x37 pad off = -65.00 dBu");
	CHECK(reac_headamp_sens_cdb(0x00, 1) ==  1000, "SENS 0x00 pad on  = +10.00 dBu");
	CHECK(reac_headamp_sens_cdb(0x37, 1) == -4500, "SENS 0x37 pad on  = -45.00 dBu");
	CHECK(reac_headamp_sens_db(0x37, 0) == -65, "whole-dB endpoint is exact, not rounded");
	{
		int uneven = 0, notid = 0;
		for (int v = 0; v < REAC_HEADAMP_SENS_MAX; v++)
			if (reac_headamp_sens_cdb((uint8_t)v, 0) -
			    reac_headamp_sens_cdb((uint8_t)(v + 1), 0) != 100)
				uneven++;
		/* Named individually because the refuted claim was about these three. */
		CHECK(reac_headamp_sens_cdb(7, 0)  != reac_headamp_sens_cdb(8, 0),  "7 and 8 differ");
		CHECK(reac_headamp_sens_cdb(23, 0) != reac_headamp_sens_cdb(24, 0), "23 and 24 differ");
		CHECK(reac_headamp_sens_cdb(39, 0) != reac_headamp_sens_cdb(40, 0), "39 and 40 differ");
		CHECK(uneven == 0, "every one of the 55 steps is exactly 100 cdB");
		for (int pad = 0; pad <= 1; pad++)
			for (int v = 0; v <= REAC_HEADAMP_SENS_MAX; v++)
				if (reac_headamp_sens_value_cdb(
					reac_headamp_sens_cdb((uint8_t)v, pad), pad) != v)
					notid++;
		CHECK(notid == 0, "the map is injective, so the round trip is the identity");
	}
	CHECK(reac_headamp_sens_value_cdb(99999, 0)  == 0x00, "hotter than min gain clamps to 0x00");
	CHECK(reac_headamp_sens_value_cdb(-99999, 0) == 0x37, "below max gain clamps to 0x37");

	/* THE REFERENCE POINT, PINNED, because it is openly disagreed on and prose
	 * has not stopped anyone converting one into the other. What libreac
	 * publishes is SENSITIVITY in dBu and it runs the other way from gain:
	 * against a 0 dBu reference the same control reads gain = 10 + value, where
	 * openmixer publishes 0..55 dB. That is a 10 dB offset between two live
	 * consumers, and ground truth is the M-200's own SENS display, unread. These
	 * two assertions go red the moment libreac's own zero moves, whichever way
	 * the dispute is closed - which is the point: it gets closed on both sides
	 * at once, not by an adapter that quietly adds ten. */
	{
		int off_law = 0, matches_gain_law = 0;
		for (int v = 0; v <= REAC_HEADAMP_SENS_MAX; v++) {
			if (reac_headamp_sens_cdb((uint8_t)v, 0) != -(10 + v) * 100)
				off_law++;
			if (reac_headamp_sens_cdb((uint8_t)v, 0) == -v * 100)
				matches_gain_law++;
		}
		CHECK(off_law == 0, "sensitivity_dBu = -10 - value across all 56 steps");
		CHECK(matches_gain_law == 0, "and it is NOT the 0..55 dB reading, at any step");
	}

	/* ONE VERSION. The digits live once, in include/reac/reac.h; the string is
	 * built from them and the running library reports that same string. It used
	 * to be defined in three places with two values and no consumer could see
	 * any of them at compile time, so no version floor could ever fail. */
	CHECK(strcmp(reac_version(), LIBREAC_VERSION) == 0,
	      "the linked library reports the header's version");
	/* DERIVED, NOT COPIED. These used to spell "0.6.0" and 600 out longhand,
	 * which made the test a fourth place the version was declared -- the exact
	 * defect the header comment above it describes. Every assertion below is
	 * built from LIBREAC_VERSION_*, so it tests the MACHINERY (the string, the
	 * comparable form, the floor's boundary) and cannot drift when the digits
	 * move. The one thing worth pinning by hand is the cross-boundary check
	 * above: the linked .so agreeing with the header it was compiled from. */
	CHECK(LIBREAC_VERSION_NUM == LIBREAC_VERSION_MAJOR * 10000 +
	                             LIBREAC_VERSION_MINOR * 100 +
	                             LIBREAC_VERSION_PATCH,
	      "the comparable form agrees with the digits");
	CHECK(LIBREAC_VERSION_AT_LEAST(LIBREAC_VERSION_MAJOR, LIBREAC_VERSION_MINOR,
	                               LIBREAC_VERSION_PATCH),
	      "a floor at the current version holds");
	CHECK(!LIBREAC_VERSION_AT_LEAST(LIBREAC_VERSION_MAJOR, LIBREAC_VERSION_MINOR,
	                                LIBREAC_VERSION_PATCH + 1),
	      "and one above it does not");
	/* The soname's major is a number a consumer can read at compile time, so a
	 * build can refuse a library whose ABI it was not written against. It only
	 * ever goes up: 0.6.0 removed symbols and left it at 0, and that is what
	 * let an old reac-pw load the new library and die on `undefined symbol`. */
	CHECK(LIBREAC_ABI >= 1, "the ABI major is set and has left its pre-break value");

	if (fails == 0) printf("OK: all libreac tests passed\n");
	else printf("%d libreac test(s) failed\n", fails);

	/* THE GEOMETRY IS THE ROLE: 1492 is the only master downstream; every legal
	 * smaller geometry is a box upstream of that width. A stagebox in master mode
	 * still emits its box width, which is what keeps a misconfigured box from
	 * being followed as a master. */
	CHECK(reac_frame_is_master_downstream(REAC_FRAME_BYTES), "1492 is the master downstream");
	CHECK(!reac_frame_is_master_downstream(1204), "1204 (S-4000S 32ch) is a box upstream, not a master");   /* S-4000S, 32 ch */
	CHECK(!reac_frame_is_master_downstream(628), "628 (S-1608 16ch) is a box upstream");    /* S-1608, 16 ch */
	CHECK(!reac_frame_is_master_downstream(340), "340 (S-0808 8ch) is a box upstream");    /* S-0808, 8 ch  */
	CHECK(!reac_frame_is_master_downstream(REAC_FRAME_BYTES_OHRCA), "FCS residue is not a geometry"); /* residue is not a geometry */

	CHECK(reac_frame_channels(REAC_FRAME_BYTES) == 40, "width of 1492 is 40");
	CHECK(reac_frame_channels(1204) == 32, "width of 1204 is 32");
	CHECK(reac_frame_channels(628) == 16, "width of 628 is 16");
	CHECK(reac_frame_channels(340) == 8, "width of 340 is 8");
	CHECK(reac_frame_channels(REAC_FRAME_BYTES_OHRCA) == 0, "1494 is not 52 + n*36"); /* 1494: not 52 + n*36 */
	CHECK(reac_frame_channels(51) == 0, "below the header is no geometry");                     /* below the header */
	CHECK(reac_frame_channels(REAC_UPSTREAM_OVERHEAD) == 0, "a zero-width frame is no geometry"); /* a zero-width frame */

	return fails ? 1 : 0;
}

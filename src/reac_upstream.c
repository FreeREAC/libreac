// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
// Braid layout per norihiro/obs-h8819-source convert_to_pcm24lep (GPL-3.0-or-later),
// confirmed for the S-1608/S-0808 upstream against rig captures (reac-pw task #108).

#include "reac/reac_upstream.h"

#include "reac/reac.h"
#include "reac/reac_braid.h"

int reac_upstream_channels(size_t len)
{
	/* THE LENGTH MUST ALREADY BE CLEAN. A REAC frame is 52 + n*36, full stop; the
	 * +2 some capture paths leave after the C2 EA end marker is the CAPTURE's and
	 * does not occur on the wire (0 residue frames in 592,762 off a plain NIC,
	 * census 2026-09-21). INGEST strips it — reac_frame_clean_len() in reac_rx,
	 * reac_tap, reac_pacer_rx_ingest, reac_hunt_observe — and this parser refuses
	 * what ingest failed to strip, because a parser that strips silently accepts a
	 * frame two bytes longer than the protocol's own law and hides a reader bug.
	 * 1206 / 630 / 342 therefore come back -1, exactly like 1205 or 629. */
	if (len < REAC_UPSTREAM_OVERHEAD + 2 * REAC_UPSTREAM_BYTES_PER_CH)
		return -1;
	if ((len - REAC_UPSTREAM_OVERHEAD) % REAC_UPSTREAM_BYTES_PER_CH != 0)
		return -1;
	size_t nch = (len - REAC_UPSTREAM_OVERHEAD) / REAC_UPSTREAM_BYTES_PER_CH;
	/* A box width (reac_box_width_ok): an even 2..40 — the braid carries channel
	 * PAIRS, and a box may fill the whole fabric (operator ruling 2026-09-25), so a
	 * 1492 B return is a 40-wide box's, not a refusal. */
	if (!reac_box_width_ok((int)nch))
		return -1;
	return (int)nch;
}

int reac_upstream_decode(const uint8_t *raw, size_t len, uint8_t *out)
{
	if (!raw || !out)
		return -1;
	int nch = reac_upstream_channels(len);
	if (nch < 0)
		return -1;
	if (raw[12] != 0x88 || raw[13] != 0x19)
		return -1;
	/* The end marker is the frame's last two bytes — `len` is clean (see
	 * reac_upstream_channels) so the frame ends where the buffer does. */
	if (raw[len - 2] != REAC_END_MARKER_0 || raw[len - 1] != REAC_END_MARKER_1)
		return -1;

	const uint8_t *audio = raw + REAC_L2_HEADER_LEN;

	uint8_t *dptr = out;
	for (int ch = 0; ch < nch; ch++) {
		for (int s = 0; s < REAC_SAMPLES_PER_PKT; s++) {
			size_t pos[3];
			reac_braid_pos(s, ch, nch, pos); /* the layout oracle */
			*dptr++ = audio[pos[0]];  /* lo  */
			*dptr++ = audio[pos[1]];  /* mid */
			*dptr++ = audio[pos[2]];  /* hi  */
		}
	}
	return REAC_SAMPLES_PER_PKT;
}

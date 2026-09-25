// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include <reac/reac_ctrl.h>
#include <reac/reac.h>
#include <reac/reac_encode.h>  /* reac_braid_encode — the layout oracle's encode side */
#include <string.h>
#include <math.h>

int reac_ctrl_classify_box_frame(const uint8_t *frame, size_t len,
                                 const uint8_t our_mac[6],
                                 struct reac_ctrl_parsed *out,
                                 enum reac_master_rx_event *ev)
{
	if (reac_ctrl_parse(frame, len, out) == REAC_CTRL_NONE)
		return -1;                                   /* not a 0x8819 frame */
	/* Roland OUI only, and never our own echo (mandatory belt-and-braces:
	 * PACKET_IGNORE_OUTGOING is best-effort and hubs/loopbacks echo). */
	if (out->src[0] != 0x00 || out->src[1] != 0x40 || out->src[2] != 0xab)
		return -1;
	if (memcmp(out->src, our_mac, 6) == 0)
		return -1;

	const int to_us = (memcmp(out->dst, our_mac, 6) == 0);

	/* A cdea/cfea control frame with an invalid checksum is corrupt — never a
	 * JOIN, never a heartbeat, never evidence of anything. FILLER (type 0000)
	 * is checksum-exempt (the block is the audio descriptor). */
	if (out->kind != REAC_CTRL_FILLER && reac_ctrl_checksum_verify(frame) != 0)
		return -1;

	/* The box cold-connect JOIN: a link-4 SINGLE record container whose DT1 TAG
	 * is the join grant or the box-ready record. THE TAG IS THE DISCRIMINATOR,
	 * NOT THE LENGTH. This used to read block[2:4] and accept 0x0013/0x0014,
	 * which is the same two records — the four cold-connect containers happen to
	 * have four different lengths, so a length test looks like it works right up
	 * to a container that is not full. Keyed on the container header + the tag +
	 * the checksum; the tail is device inventory (0x41 is NOT a MAC tail).
	 * Broadcast AND unicast accepted (the box emits it x3 on PHY-up while still
	 * in broadcast mode). */
	if (out->kind == REAC_CTRL_GRANT) {
		if (out->opcode == 0x00 &&        /* the DT1 container's own header byte */
		    (out->dt1_tag == REAC_DT1_TAG_JOIN ||
		     out->dt1_tag == REAC_DT1_TAG_BOX_READY)) {
			*ev = REAC_M_RX_BOX_JOIN;
			return 0;
		}
		/* A link-4 record we do not understand — don't guess (never generic
		 * UNICAST: that could falsely close a grant window). The live log dumps
		 * the block so the matcher can be extended from a real capture. */
		return -1;
	}

	if (out->is_broadcast) {
		if (out->kind == REAC_CTRL_FILLER) {
			*ev = REAC_M_RX_BOX_BCAST_FILLER;        /* the presence-flood */
			return 0;
		}
		return -1;   /* another master's probe/announce/… — not a box frame */
	}

	if (!to_us)
		return -1;   /* unicast between other parties */

	/* Unicast-to-us box heartbeat: opcode 0x81, the box's ESTABLISHED "I am
	 * locked" signal, symmetric to the heartbeat our slave emits. */
	if (out->kind == REAC_CTRL_BOX_HB) {
		*ev = REAC_M_RX_BOX_HEARTBEAT;
		return 0;
	}
	/* THE BYE IS THE HEARTBEAT WITH ITS OPCODE CLEARED — link 1, SINGLE, opcode
	 * 0x00 — and on link 1 that opcode is the master's bulk scene push, so
	 * libreac classifies it as SCENE_TRANSFER. The two are the same four header
	 * bytes and no field separates them; what separates them here is DIRECTION.
	 * This function only ever sees frames a box sent to us while we are the
	 * master, and a box never pushes a scene, so a link-1 SINGLE bulk frame
	 * arriving unicast from a box is its disconnect. Anything that widens this
	 * function's input (a promiscuous tap, a splitter) must re-derive it. */
	if (out->kind == REAC_CTRL_SCENE_TRANSFER && out->seg == REAC_SEG_SINGLE) {
		*ev = REAC_M_RX_BOX_BYE;
		return 0;
	}
	/* The box's config-announce (cdea 01 03 0010) is its SETUP DECLARATION — the
	 * frame a mixer enrols the box from. A box that was previously synced does a
	 * WARM RELINK: it skips the flood + cold-connect JOIN and re-appears streaming
	 * unicast, re-declaring itself with this frame (verified live: a real S-0808
	 * to reac-pw-as-master sends config-announce + unicast, never a 04 03 JOIN).
	 * Surface it distinctly so the master FSM can establish on it. */
	if (out->kind == REAC_CTRL_CONFIG_ANNOUNCE) {
		*ev = REAC_M_RX_BOX_CONFIG;
		return 0;
	}
	/* Any other unicast-to-us box frame — upstream FILLER (628/340 B), unknown
	 * ctrl — proves the box linked to us. */
	*ev = REAC_M_RX_BOX_UNICAST;
	return 0;
}

int reac_box_pin_notice(const char **pin, const char *recognized_token)
{
	if (!pin || !*pin || !recognized_token)
		return 0;
	/* The pin's model token is everything before the optional ":label". */
	size_t toklen = strcspn(*pin, ":");
	int disagrees = strlen(recognized_token) != toklen ||
	                strncmp(*pin, recognized_token, toklen) != 0;
	*pin = NULL;              /* consumed: at most one notice per pin, ever */
	return disagrees;
}

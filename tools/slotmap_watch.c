// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* The slot-map window, unrolled into per-slot state over time.
 *
 * The link-1 opcode-0x01 message is a SLIDING WINDOW of eight {slot, flags,
 * sens} entries, and it advances every poll. Watching its raw bytes therefore
 * reports a change on almost every frame — the window moved, not the state —
 * which is exactly the confusion that makes a granularity question unanswerable
 * from a byte diff. This keys on the SLOT each entry names, so a report of "slot
 * 0x24 changed and 0x25..0x27 did not" is about the desk's state and not about
 * where the window happened to be.
 *
 * The slot-map record is also the one place a per-four field is known to exist
 * (the high nibble the box hands to its inventory arrays), so this prints the
 * nibbles apart: a change confined to the high nibble is the inventory cell, and
 * a change in the low nibble or the sens byte is a head-amp parameter.
 */

#include <reac/reac.h>
#include <reac/reac_ctrlblk.h>
#include <reac/pcap_source.h>

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define NSLOT 256
#define ENTRY_OFF  5    /* block[5] — first entry, after link/seg/len/opcode */
#define ENTRY_LEN  3    /* {slot, flags, sens} */

int main(int argc, char **argv)
{
	if (argc < 2) { fprintf(stderr, "usage: slotmap_watch FILE.pcap\n"); return 2; }
	struct pcap_source ps;
	if (pcap_source_open(&ps, argv[1]) != 0) { fprintf(stderr, "cannot open\n"); return 1; }

	static uint8_t buf[4096];
	static int seen[NSLOT];
	static uint8_t flags[NSLOT], sens[NSLOT];
	static unsigned long visits[NSLOT];
	uint64_t t0 = 0;
	unsigned long msgs = 0, entries = 0, changes = 0;

	for (;;) {
		uint64_t ts = 0;
		long n = pcap_source_next(&ps, buf, sizeof buf, &ts);
		if (n <= 0) break;
		if (!t0) t0 = ts;
		if (!reac_frame_is_reac(buf, (size_t)n)) continue;
		if ((size_t)n < REAC_CTRL_BLOCK_END) continue;
		struct reac_ctrl_parsed p;
		if (reac_ctrl_parse(buf, (size_t)n, &p) != REAC_CTRL_MASTER_HB) continue;
		msgs++;
		const uint8_t *blk = buf + REAC_CTRL_BLOCK_OFF;
		/* blk_len counts from block[4] inclusive: 1 opcode + 3*N entries. */
		unsigned n_ent = p.blk_len > 1 ? (unsigned)((p.blk_len - 1) / ENTRY_LEN) : 0;
		if (ENTRY_OFF + n_ent * ENTRY_LEN > REAC_CTRL_BLOCK_LEN)
			n_ent = (REAC_CTRL_BLOCK_LEN - ENTRY_OFF) / ENTRY_LEN;
		printf("%10.6f WINDOW n=%u:", (double)(ts - t0) / 1e6, n_ent);
		for (unsigned e = 0; e < n_ent; e++) {
			const uint8_t *r = blk + ENTRY_OFF + e * ENTRY_LEN;
			printf(" %02x:%02x/%02x", r[0], r[1], r[2]);
		}
		printf("\n");
		for (unsigned e = 0; e < n_ent; e++) {
			const uint8_t *r = blk + ENTRY_OFF + e * ENTRY_LEN;
			unsigned s = r[0];
			entries++;
			visits[s]++;
			if (!seen[s]) {
				seen[s] = 1; flags[s] = r[1]; sens[s] = r[2];
				continue;
			}
			if (flags[s] == r[1] && sens[s] == r[2])
				continue;
			changes++;
			printf("%10.6f SLOT 0x%02x flags %02x->%02x (hi %x->%x lo %x->%x) "
			       "sens %02x->%02x\n",
			       (double)(ts - t0) / 1e6, s, flags[s], r[1],
			       flags[s] >> 4, r[1] >> 4, flags[s] & 0xf, r[1] & 0xf,
			       sens[s], r[2]);
			flags[s] = r[1]; sens[s] = r[2];
		}
	}
	pcap_source_close(&ps);

	fprintf(stderr, "slotmap_watch: messages=%lu entries=%lu slot_state_changes=%lu\n",
	        msgs, entries, changes);
	fprintf(stderr, "  slots visited:");
	int nv = 0;
	for (int s = 0; s < NSLOT; s++)
		if (visits[s]) { fprintf(stderr, " %02x(%lu)", s, visits[s]); nv++; }
	fprintf(stderr, "%s\n", nv ? "" : " none");
	if (!msgs) {
		fprintf(stderr, "slotmap_watch: no slot-map message — an empty scan is not a result\n");
		return 1;
	}
	return 0;
}

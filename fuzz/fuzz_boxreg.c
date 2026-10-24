// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* libFuzzer harness: the MULTI-BOX REGISTRY (src/reac_boxreg.c), which places each
 * box a segment carries at its own slice of the 40-slot audio fabric.
 *
 * The registry parses no bytes itself, but everything it is handed comes off the
 * wire: a JOIN's source MAC and the width a box declared. The input is a sequence
 * of the four calls a master makes, so the table is built from the input the way
 * a segment builds it.
 *
 * Input:  [0] fabric size (anything; init clamps it)
 *         then operations, at most 64, each [op] followed by its arguments:
 *           op & 3 == 0   declare: [nch][s8 base][name len % 48][name bytes]
 *           op & 3 == 1   add:     [mac][nch]
 *           op & 3 == 2   find:    [mac]
 *           op & 3 == 3   set_name:[s8 idx][name len % 48][name bytes]
 *         A [mac] is one byte, 00:40:ab:00:00:<b>, so boxes collide often; with
 *         op & 4 it is six bytes, so the all-zero "unbound" MAC is reachable too.
 *         A name is its own NUL-terminated allocation, whatever bytes it holds.
 *
 * Properties, each an abort():
 *   - no more than REAC_BOXREG_MAX_BOXES boxes, each an even 2..40 width at a base
 *     whose slice lies inside the fabric;
 *   - no two boxes' slices overlap and no MAC is registered twice;
 *   - a pinned declaration lands at the base it asked for;
 *   - add is idempotent and find finds what add registered; the zero MAC is never
 *     a box;
 *   - every name is NUL-terminated inside its field.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <reac/reac.h>
#include <reac/reac_boxreg.h>

#include "fuzz.h"

#define MAX_OPS 64

static const uint8_t ZERO[6];

static void read_mac(struct fuzz_in *in, int full, uint8_t mac[6])
{
	if (full) {
		for (int i = 0; i < 6; i++)
			mac[i] = fuzz_u8(in);
	} else {
		static const uint8_t base[5] = { 0x00, 0x40, 0xab, 0x00, 0x00 };
		memcpy(mac, base, 5);
		mac[5] = fuzz_u8(in);
	}
}

static char *read_name(struct fuzz_in *in)
{
	const uint8_t *p;
	size_t n = fuzz_take(in, fuzz_u8(in) % 48, &p);
	char *s = malloc(n + 1);
	if (!s)
		abort();
	memcpy(s, p, n);
	s[n] = '\0';
	return s;
}

static void check(const struct reac_boxreg *r, int fabric)
{
	if (r->n < 0 || r->n > REAC_BOXREG_MAX_BOXES || r->fabric != fabric)
		abort();
	for (int i = 0; i < r->n; i++) {
		const struct reac_box *b = &r->box[i];
		if (!reac_box_width_ok(b->nch) || b->base < 0 || b->base + b->nch > r->fabric)
			abort();
		if (memchr(b->name, 0, sizeof b->name) == NULL)
			abort();
		for (int j = i + 1; j < r->n; j++) {
			const struct reac_box *c = &r->box[j];
			if (b->base < c->base + c->nch && c->base < b->base + b->nch)
				abort();   /* two boxes on one slot */
			if (memcmp(b->mac, ZERO, 6) != 0 && memcmp(b->mac, c->mac, 6) == 0)
				abort();
		}
	}
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	if (size >= FUZZ_INPUT_MAX)
		return 0;
	struct fuzz_in in = fuzz_in(data, size);
	/* Its own allocation, so a box written past the table is a sanitizer report. */
	struct reac_boxreg *r = malloc(sizeof *r);
	if (!r)
		abort();
	int fabric_in = (int8_t)fuzz_u8(&in);
	reac_boxreg_init(r, fabric_in);
	int fabric = (fabric_in > 0 && fabric_in <= REAC_BOXREG_FABRIC) ? fabric_in
	                                                                : REAC_BOXREG_FABRIC;
	check(r, fabric);

	for (int i = 0; i < MAX_OPS && in.n; i++) {
		uint8_t op = fuzz_u8(&in);
		uint8_t mac[6];
		int n_before = r->n;
		switch (op & 3) {
		case 0: {
			int nch = fuzz_u8(&in);
			int base = (int8_t)fuzz_u8(&in);
			char *name = read_name(&in);
			int idx = reac_boxreg_declare(r, nch, (op & 8) ? NULL : name, base);
			if (idx != -1 && idx != n_before)
				abort();
			if (idx >= 0) {
				if (r->n != n_before + 1 || r->box[idx].nch != nch ||
				    memcmp(r->box[idx].mac, ZERO, 6) != 0)
					abort();
				if (base >= 0 && (r->box[idx].base != base || !r->box[idx].pinned))
					abort();
			} else if (r->n != n_before) {
				abort();
			}
			free(name);
			break;
		}
		case 1: {
			read_mac(&in, op & 4, mac);
			int nch = fuzz_u8(&in);
			int known = reac_boxreg_find(r, mac);
			int idx = reac_boxreg_add(r, mac, nch);
			if (memcmp(mac, ZERO, 6) == 0 && idx != -1)
				abort();   /* the zero MAC marks an unbound slot, never a box */
			if (known >= 0 && reac_box_width_ok(nch) && nch <= r->fabric && idx != known)
				abort();   /* idempotent */
			if (idx >= 0) {
				if (idx >= r->n || memcmp(r->box[idx].mac, mac, 6) != 0 ||
				    reac_boxreg_find(r, mac) != idx ||
				    reac_boxreg_add(r, mac, nch) != idx)
					abort();
				if (known < 0 && r->box[idx].nch != nch)
					abort();
			}
			break;
		}
		case 2: {
			read_mac(&in, op & 4, mac);
			int idx = reac_boxreg_find(r, mac);
			if (idx < -1 || idx >= r->n)
				abort();
			if (idx >= 0 && memcmp(r->box[idx].mac, mac, 6) != 0)
				abort();
			if (memcmp(mac, ZERO, 6) == 0 && idx != -1)
				abort();
			break;
		}
		default: {
			int idx = (int8_t)fuzz_u8(&in);
			char *name = read_name(&in);
			reac_boxreg_set_name(r, idx, (op & 8) ? NULL : name);
			if (idx >= 0 && idx < r->n) {
				const char *want = (op & 8) ? "" : name;
				if (strncmp(r->box[idx].name, want, REAC_BOXREG_NAME_MAX - 1) != 0)
					abort();
			}
			free(name);
			break;
		}
		}
		check(r, fabric);
	}
	free(r);
	return 0;
}

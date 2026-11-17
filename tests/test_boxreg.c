// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_boxreg — where a box's AUDIO lands in the 40-slot fabric.
 *
 * 1. THE FABRIC BOUND (include/reac/reac_slots.h, #69). The allocator's space is
 *    the 40 slots a downstream frame carries, never the 48-wide head-amp space.
 *    An S-1608 (16 wide) at audio slot 32 would end at 47: it must be refused,
 *    pinned or auto-allocated. This is the guard reac_slots.h's MUTATION-CHECKED
 *    note names: widening REAC_AUDIO_FABRIC_SLOTS to 48 turns this file red
 *    (sabotage-verified 2026-09-25; before this file, `make test` stayed green).
 * 2. A PINNED BASE CANNOT OVERFLOW the bound (libreac review 2026-09-25, M5): a
 *    CLI base near INT_MAX once wrapped `base + nch` negative and registered a
 *    box at slot 2147483643.
 * 3. The ordinary contract: lowest-free allocation, pre-declared slots bind by
 *    width, a known MAC is idempotent.
 * 4. The cases carried over from reac-pw's copy of this test, which it never had:
 *    a departed box's gap is reused, a pinned slot binds on JOIN by width, a pinned
 *    collision and out-of-range widths are refused, set_name overwrites, and the
 *    fabric fills with 8-wide boxes and refuses the next. */
#include <reac/reac_boxreg.h>
#include <reac/reac.h>

#include <limits.h>
#include <stdio.h>
#include <string.h>

static int fails;
#define CHK(cond) do { \
	if (!(cond)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

static const uint8_t S4000[6] = { 0x00, 0x40, 0xab, 0xc4, 0x06, 0x80 };
static const uint8_t S1608[6] = { 0x00, 0x40, 0xab, 0x16, 0x08, 0x01 };
static const uint8_t S0808[6] = { 0x00, 0x40, 0xab, 0x08, 0x08, 0x01 };
static const uint8_t ZERO[6] = { 0 };

int main(void)
{
	struct reac_boxreg r;

	/* ---- 1. the fabric is the frame's width ---- */
	CHK(REAC_BOXREG_FABRIC == REAC_MAX_CHANNELS);   /* bound to the facts by test_facts */
	reac_boxreg_init(&r, 0);
	CHK(r.fabric == REAC_MAX_CHANNELS);
	CHK(reac_boxreg_declare(&r, 16, "late S-1608", 32) == -1);   /* would end at 47 */
	CHK(reac_boxreg_declare(&r, 8, "last S-0808", 32) == 0);     /* ends at 39: fits */

	/* The same bound through the JOIN door: an S-4000S takes 0..31, and a 16-wide
	 * box that joins after it has nowhere to land inside the frame. */
	reac_boxreg_init(&r, 0);
	CHK(reac_boxreg_add(&r, S4000, 32) == 0 && r.box[0].base == 0);
	CHK(reac_boxreg_add(&r, S1608, 16) == -1);
	int i = reac_boxreg_add(&r, S0808, 8);
	CHK(i == 1 && r.box[1].base == 32 && r.box[1].base + r.box[1].nch <= REAC_MAX_CHANNELS);

	/* ---- 2. a pinned base cannot overflow the bound ---- */
	reac_boxreg_init(&r, 0);
	CHK(reac_boxreg_declare(&r, 8, "late", 36) == -1);
	CHK(reac_boxreg_declare(&r, 8, "huge", INT_MAX - 4) == -1);
	CHK(reac_boxreg_declare(&r, 2, "huge", INT_MAX) == -1);
	CHK(r.n == 0);

	/* ---- 3. the contract ---- */
	reac_boxreg_init(&r, 0);
	CHK(reac_boxreg_declare(&r, 16, "stage left", -1) == 0 && r.box[0].base == 0);
	CHK(reac_boxreg_add(&r, S0808, 8) == 1 && r.box[1].base == 16);   /* no 8-wide slot declared */
	CHK(reac_boxreg_add(&r, S1608, 16) == 0);                          /* binds the declared one */
	CHK(reac_boxreg_add(&r, S1608, 16) == 0 && r.n == 2);              /* idempotent */
	CHK(reac_boxreg_find(&r, S1608) == 0 && reac_boxreg_find(&r, S0808) == 1);
	CHK(reac_boxreg_add(&r, S4000, 7) == -1);                          /* odd width */
	CHK(reac_boxreg_add(&r, ZERO, 8) == -1 && r.n == 2);   /* "unbound" is not a box */

	/* ---- 4. carried over from reac-pw's copy ---- */
	{
		static const uint8_t A[6] = { 0x00, 0x40, 0xab, 0x00, 0x00, 0x0a };
		static const uint8_t B[6] = { 0x00, 0x40, 0xab, 0x00, 0x00, 0x0b };
		static const uint8_t C[6] = { 0x00, 0x40, 0xab, 0x00, 0x00, 0x0c };
		static const uint8_t D[6] = { 1, 2, 3, 4, 5, 6 };
		struct reac_boxreg g, d, f;

		/* invalid widths: odd, zero, wider than the fabric */
		reac_boxreg_init(&g, 0);
		CHK(reac_boxreg_add(&g, D, 7) == -1);
		CHK(reac_boxreg_add(&g, D, 0) == -1);
		CHK(reac_boxreg_add(&g, D, REAC_MAX_CHANNELS + 2) == -1);
		CHK(reac_boxreg_find(&g, ZERO) == -1);

		/* a departed box's slots are reclaimed by the next add of that width */
		reac_boxreg_add(&g, A, 16);                 /* 0..15 */
		reac_boxreg_add(&g, B, 8);                  /* 16..23 */
		g.box[0] = g.box[1]; g.n = 1;               /* box A leaves: only B, at base 16 */
		int nb = reac_boxreg_add(&g, C, 16);        /* lowest free 16-wide range is 0 */
		CHK(nb == 1 && g.box[1].base == 0);

		/* a pinned slot binds on JOIN by width, keeping its base and name */
		reac_boxreg_init(&d, 0);
		CHK(reac_boxreg_declare(&d, 16, "Drums", -1) == 0 && d.box[0].base == 0);
		CHK(reac_boxreg_declare(&d, 8, "Vocals", 24) == 1 && d.box[1].base == 24 && d.box[1].pinned);
		CHK(reac_boxreg_add(&d, A, 16) == 0 && !memcmp(d.box[0].mac, A, 6) && !strcmp(d.box[0].name, "Drums"));
		CHK(reac_boxreg_add(&d, B, 8) == 1 && d.box[1].base == 24 && !strcmp(d.box[1].name, "Vocals"));
		CHK(reac_boxreg_declare(&d, 8, "X", 0) == -1);          /* base 0 is taken */
		reac_boxreg_set_name(&d, 0, "Kit");
		CHK(!strcmp(d.box[0].name, "Kit"));

		/* the fabric fills with 8-wide boxes; one more is refused */
		reac_boxreg_init(&f, 0);
		uint8_t m[6] = { 0x00, 0x40, 0xab, 0, 0, 0 };
		int placed = 0;
		for (int k = 0; k < 8; k++) {
			m[5] = (uint8_t)k;
			if (reac_boxreg_add(&f, m, 8) >= 0)
				placed++;
		}
		CHK(placed == REAC_MAX_CHANNELS / 8);
	}

	if (fails) {
		printf("%d reac_boxreg check(s) failed\n", fails);
		return 1;
	}
	printf("OK: reac_boxreg — audio lands inside the 40-slot frame (a 16-wide box at "
	       "slot 32 is refused, pinned or joined), a pinned base cannot overflow the "
	       "bound, and allocation is lowest-free with declared slots bound by width\n");
	return 0;
}

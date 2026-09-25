// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* REVIEW 2026-09-25, finding M5 (docs/audits/2026-09-25-libreac-review.md).
 *
 * reac_boxreg_declare() bounds a pinned base with `base + nch > r->fabric`, in
 * signed int. A CLI-supplied base near INT_MAX overflows that sum (undefined
 * behaviour; on every compiler this tree builds with it wraps negative), the
 * bound passes, range_taken() sees no overlap, and the box is registered with
 * base 2147483643 — which the RX then uses as an AUDIO FABRIC SLOT index. */
#include <reac/reac_boxreg.h>

#include <limits.h>
#include <stdio.h>

static int fails;
#define CHK(cond) do { \
	if (!(cond)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

int main(void)
{
	struct reac_boxreg r;

	/* CONTROL: an honest out-of-fabric pin is refused. */
	reac_boxreg_init(&r, 0);
	if (reac_boxreg_declare(&r, 8, "late", 36) != -1) {
		printf("NOT A RESULT: test_review_boxreg — the plain out-of-fabric control "
		       "was accepted\n");
		return 2;
	}

	/* VERDICT: the same refusal must hold for a base whose sum overflows. */
	reac_boxreg_init(&r, 0);
	int idx = reac_boxreg_declare(&r, 8, "huge", INT_MAX - 4);
	CHK(idx == -1);
	if (idx >= 0) {
		CHK(r.box[idx].base + 0 < r.fabric);   /* what the RX would index with */
		fprintf(stderr, "  registered at audio slot base %d of a %d-slot fabric\n",
		        r.box[idx].base, r.fabric);
	}

	if (fails)
		return 1;
	printf("OK: test_review_boxreg — no pinned base lands outside the fabric\n");
	return 0;
}

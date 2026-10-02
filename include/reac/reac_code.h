// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_code — ONE stable token vocabulary for every refusal, failure and notable
 * status line libreac (and a consumer such as reac-pw) prints.
 *
 * Moved here from reac-pw's src/reac_code.h (docs/design/specs/
 * 2026-09-17-tunables-api-and-shared-refusal-codes.md): "We need the error codes and
 * not only messages" (operator, 2026-09-17). `reac_code_emit` puts the TOKEN first,
 * always, so prose and script/log-scraper matching can move independently.
 *
 * Header-only, so linking against this changes no ABI. THIS IS THE ONE LIST: every
 * token reac-pw prints is declared here too, so a consumer's copy becomes a thin
 * `#include <reac/reac_code.h>`. A token is appended, never inserted: the
 * enumerators' values are what a consumer built against an older header compiled in.
 *
 * X-MACRO so the enum, the token table and any enumeration (a conformance test that
 * lists every token) derive from ONE list and cannot drift apart. */
#ifndef REAC_CODE_H
#define REAC_CODE_H

#include <stdarg.h>
#include <stdio.h>

#define REAC_CODE_LIST(X) \
	/* refusals / failures — reac-pw's own (main.c) */ \
	X(RC_E_SIZING,          "E_SIZING") \
	X(RC_E_ROOT_REFUSED,    "E_ROOT_REFUSED") \
	X(RC_E_SEGMENT_HELD,    "E_SEGMENT_HELD") \
	X(RC_E_ENROLL_REFUSED,  "E_ENROLL_REFUSED") \
	/* status — reac-pw's own (main.c) */ \
	X(RC_S_SEGMENT_HEARD,   "S_SEGMENT_HEARD") \
	X(RC_S_SEGMENT_UP,      "S_SEGMENT_UP") \
	X(RC_S_SEGMENT_DROPPED, "S_SEGMENT_DROPPED") \
	X(RC_S_KNOB_SET,        "S_KNOB_SET") \
	X(RC_S_KNOB_SUMMARY,    "S_KNOB_SUMMARY") \
	/* refusals / failures — libreac-transport's own */ \
	X(RC_E_PROMISC_FAILED,  "E_PROMISC_FAILED") \
	X(RC_E_CAPTURE_FAILED,  "E_CAPTURE_FAILED") \
	X(RC_E_QDISC_READ_FAILED, "E_QDISC_READ_FAILED") \
	X(RC_E_ETF_REFUSED,     "E_ETF_REFUSED") \
	X(RC_S_KNOB_IGNORED,    "S_KNOB_IGNORED") \
	X(RC_S_HEADAMP_SUPPRESSED, "S_HEADAMP_SUPPRESSED") \
	/* refusals / failures — reac-pw's own, appended so every enumerator above keeps \
	 * its value */ \
	X(RC_E_LINK_BUDGET,     "E_LINK_BUDGET") \
	/* A listener still held a node pair where it must not have, or an open that \
	 * failed had already built one: the pair is destroyed at the code, so the ghost \
	 * is a searchable event rather than a node nobody can account for. */ \
	X(RC_E_ORPHAN_PAIR,     "E_ORPHAN_PAIR") \
	/* A roster property REMOVAL cannot be delivered: PipeWire merges only the keys a \
	 * client sends, so an absent key is never removed. Refused, never pretended. */ \
	X(RC_E_ROSTER_REMOVE,   "E_ROSTER_REMOVE") \
	/* There is no roster on the graph: the node could not be created or rebuilt. \
	 * Not fatal, but a console reading the roster reads nothing and must be able to \
	 * tell that from a daemon with no segments. */ \
	X(RC_E_ROSTER_NODE,     "E_ROSTER_NODE") \
	X(RC_E_UNKNOWN_KNOB,    "E_UNKNOWN_KNOB") \
	/* status — reac-pw's own */ \
	X(RC_S_BUDGET_YIELDED,  "S_BUDGET_YIELDED") \
	X(RC_S_NO_OVERRIDES,    "S_NO_OVERRIDES")

enum reac_code {
	RC_NONE = 0,
#define X(name, token) name,
	REAC_CODE_LIST(X)
#undef X
};

/* Never NULL, including RC_NONE ("?" — a caller passing RC_NONE to reac_code_emit is a
 * bug in the caller, not something to hide behind a plausible-looking token). */
static inline const char *reac_code_token(enum reac_code c)
{
	switch (c) {
#define X(name, token) case name: return token;
	REAC_CODE_LIST(X)
#undef X
	case RC_NONE: break;
	}
	return "?";
}

/* Every refusal/failure/notable-status line goes through here, so the code is ALWAYS
 * the first field after the program tag: "<prog>: <TOKEN> <prose>" (the caller's `fmt`
 * supplies the prose and its own trailing '\n'). Prose may reword freely; the token may
 * not move or change — that is the whole point of this file. */
static inline void reac_code_emit(FILE *out, const char *prog, enum reac_code code,
                                   const char *fmt, ...)
{
	fprintf(out, "%s: %s ", prog, reac_code_token(code));
	va_list ap;
	va_start(ap, fmt);
	vfprintf(out, fmt, ap);
	va_end(ap);
}

#endif /* REAC_CODE_H */

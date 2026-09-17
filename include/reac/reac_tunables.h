// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_tunables — the daemon SETS what this library used to read from the environment
 * itself (docs/design/specs/2026-09-17-tunables-api-and-shared-refusal-codes.md).
 *
 * Every field here is a PROCESS-WIDE knob (reac_envflag.h's own description, unchanged):
 * read once, before the transport starts, never per-segment. That is why this is three
 * plain structs and three setters — one per subsystem translation unit — rather than one
 * struct threaded through every `struct reac_master`/`reac_pacer` instance: the value is
 * the same for every instance in the process, exactly as the `getenv` calls it replaces
 * were.
 *
 * Each `_DEFAULT` macro reproduces the value the deleted `getenv` call produced when the
 * variable was unset, so a caller that never calls a setter here gets byte-identical
 * behaviour to before this file existed. Call the relevant setter(s) once, at startup,
 * before any segment's pacer/master runs — a setter called after a subsystem has already
 * read its tunables mid-run is a bug in the caller (there is no locking here; these are
 * not runtime-hot-swappable, same as `getenv` never was either). */
#ifndef REAC_TUNABLES_H
#define REAC_TUNABLES_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---- reac_master.c (src/) ------------------------------------------------------- */

struct reac_master_tunables {
	/* REACPW_GRANT_ON_DECLARE: end the ENROLL->grant dwell early once the box has
	 * declared and been armed, instead of running the full wall-clock dwell.
	 * Boolean; default ON (1). */
	int grant_on_declare;
	/* REACPW_GRANT_DWELL_MS / REACPW_GRANT_DWELL_S: override the compiled dwell.
	 * <= 0 = unset (use the compiled default); ms takes precedence over s when both
	 * are positive, matching the getenv-era precedence. Valid ranges are enforced by
	 * the setter, same bounds the deleted getenv parse used: ms in (0, 600000), s in
	 * (0, 3600]. An out-of-range value is IGNORED (falls back to unset), same as an
	 * unparseable string was. */
	long grant_dwell_ms;
	long grant_dwell_s;
	/* REACPW_NO_ENROLL: suppress the pre-grant ENROLL for a box of known width.
	 * Boolean; default OFF (0). */
	int no_enroll;
	/* REACPW_EST_SCENE: keep pushing scene frames once ESTABLISHED. Boolean;
	 * default OFF (0). */
	int est_scene;
};

#define REAC_MASTER_TUNABLES_DEFAULT ((struct reac_master_tunables){ \
	.grant_on_declare = 1, .grant_dwell_ms = 0, .grant_dwell_s = 0, \
	.no_enroll = 0, .est_scene = 0 })

/* Copies `*t`; passing NULL resets to REAC_MASTER_TUNABLES_DEFAULT. */
void reac_master_tunables_set(const struct reac_master_tunables *t);

/* ---- transport/src/reac_pacer.c -------------------------------------------------- */

struct reac_pacer_tunables {
	/* REACPW_GUARD_FLOOR_FRAMES: override the compiled ring-guard floor. 0 = unset
	 * (use the compiled default, REAC_PACER_GUARD_FLOOR_FRAMES). Out of
	 * [REAC_PACER_GUARD_FLOOR_MIN, REAC_PACER_GUARD_FLOOR_MAX] is IGNORED and
	 * reported via reac_code_emit, same as the deleted getenv parse. */
	unsigned int guard_floor_frames;
	/* REACPW_NO_HEADAMP: diagnostic-only — suppress the master's own head-amp
	 * scene push on ESTABLISHED. Boolean; default OFF (0). */
	int no_headamp;
};

#define REAC_PACER_TUNABLES_DEFAULT ((struct reac_pacer_tunables){ \
	.guard_floor_frames = 0, .no_headamp = 0 })

void reac_pacer_tunables_set(const struct reac_pacer_tunables *t);

/* ---- transport/src/reac_ifscan.c, transport/src/reac_rx.c ----------------------- */

struct reac_transport_tunables {
	/* REAC_IFACES_ALLOW_WIRELESS: forwarded verbatim into
	 * reac_ifscan_wireless_allowed(allow_wireless, ifname), whose signature already
	 * took this shape (a NULL/empty string, "*", or a comma-separated allowlist) —
	 * no redesign, just moving where the string comes from. Copied into a fixed
	 * internal buffer by the setter, so the caller's storage need not outlive the
	 * call (unlike REACPW_CLOCK_REF on the reac-pw side, which forwards into a
	 * long-lived node and stays a documented exception there). NULL/"" = no
	 * wireless interface is ever allowed, the same default an unset env var gave. */
	const char *allow_wireless;
	/* REAC_DEBUG: opt-in per-second RX telemetry (reac_rx.c). Boolean; default
	 * OFF (0). */
	int debug;
};

#define REAC_TRANSPORT_TUNABLES_DEFAULT ((struct reac_transport_tunables){ \
	.allow_wireless = NULL, .debug = 0 })

void reac_transport_tunables_set(const struct reac_transport_tunables *t);

#ifdef __cplusplus
}
#endif

#endif /* REAC_TUNABLES_H */

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* fuzz/records.sh compiles each harness with this header in front of it, and only
 * the harness: every abort() of the harness names its own file and line on stderr
 * and then aborts, so a record is met only when the check that carries it is the
 * one that stopped the program, and not some other check on the same input. The
 * library is compiled without it, and the exit status stays the 134 of abort(). */
#ifndef LIBREAC_FUZZ_RECORDS_ABORT_H
#define LIBREAC_FUZZ_RECORDS_ABORT_H

/* The first libc header fixes the feature set, and this one comes before the
 * harness's own: so it asks for what fuzz_pcap.c asks for (fmemopen), with the same
 * definition, which the harness may then repeat. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>

_Noreturn static inline void fuzz_records_abort(const char *file, int line)
{
	fprintf(stderr, "records: abort at %s:%d\n", file, line);
	fflush(stderr);
	(abort)();
}

#define abort() fuzz_records_abort(__FILE__, __LINE__)

#endif /* LIBREAC_FUZZ_RECORDS_ABORT_H */

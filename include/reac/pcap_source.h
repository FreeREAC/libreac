// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* Read REAC frames from a classic pcap file (off-site dev / test source).
 *
 * Pairs with reac-tools' fixture writer. Classic libpcap only (magic
 * 0xA1B2C3D4 / swapped), link-type 1 (Ethernet). Yields full ethernet frames
 * so the same reac_decode path runs as for live AF_PACKET capture.
 */
#ifndef PCAP_SOURCE_H
#define PCAP_SOURCE_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

struct pcap_source {
	FILE *f;
	int swapped;        /* byte-swap per-packet headers                     */
	uint32_t last_orig_len;  /* origlen of the record pcap_source_next just
	                          * returned. A CAPTURE TAKEN WITH A SNAPLEN
	                          * HANDS BACK A SHORT BUFFER THAT IS NOT A SHORT
	                          * FRAME: several corpus captures ran at snaplen
	                          * 64/128/200/400, and a truncated record's caplen
	                          * can land on 52+36n by coincidence, so it reaches
	                          * a frame decoder and fails there for a reason
	                          * that has nothing to do with the protocol. Every
	                          * pcap record carries caplen AND origlen; the
	                          * reader kept only caplen, so a caller could not
	                          * tell the two apart. Compare this against the
	                          * return value: greater means truncated.        */
};

/* Open a pcap file. Returns 0 on success, -1 on error. */
int pcap_source_open(struct pcap_source *ps, const char *path);

/* Read the next packet into buf (capacity cap). Returns the packet length,
 * 0 at EOF, or -1 on error. ts_usec, if non-NULL, gets the capture time in
 * microseconds since epoch. */
long pcap_source_next(struct pcap_source *ps, uint8_t *buf, size_t cap, uint64_t *ts_usec);

void pcap_source_close(struct pcap_source *ps);

#endif /* PCAP_SOURCE_H */

# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# Native build of libreac (static lib) + unit tests. The OpenWrt package build
# is under openwrt/libreac/ (built via scripts/build-apk.sh against the SDK).
#
# libreac is the shared REAC byte-layout core: wire constants + rate detect
# (reac.c), the braided downstream decode + its plain-LE diagnostic
# (reac_decode.c), the braided box-upstream decode (reac_upstream.c) and the
# braided ENCODE + downstream frame builder (reac_encode.c), all over the
# braid/sample header oracles
# (reac_braid.h / reac_sample.h), live AF_PACKET capture (reac_capture.c), and
# the offline pcap reader (pcap_source.c). Consumed by reac-aes67 and reac-pw.
#
# reac_encode.c pulls in lrintf, so anything linking libreac's encode path needs
# -lm; the test targets below link it unconditionally.

CC      ?= cc
AR      ?= ar
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra
INC     := -Iinclude

OBJS = reac.o reac_ctrlblk.o reac_ports.o reac_decode.o reac_upstream.o reac_encode.o reac_capture.o pcap_source.o

all: libreac.a

%.o: src/%.c
	$(CC) $(CFLAGS) $(INC) -c $< -o $@

libreac.a: $(OBJS)
	$(AR) rcs $@ $(OBJS)

test: tests/test_reac.c tests/test_capture.c tests/test_braid.c tests/test_upstream.c tests/test_encode.c tests/test_decode.c tests/test_ports.c libreac.a
	$(CC) $(CFLAGS) $(INC) tests/test_reac.c libreac.a -lm -o test_reac
	./test_reac
	$(CC) $(CFLAGS) $(INC) tests/test_capture.c libreac.a -lm -o test_capture
	./test_capture
	$(CC) $(CFLAGS) $(INC) tests/test_braid.c libreac.a -lm -o test_braid
	./test_braid
	$(CC) $(CFLAGS) $(INC) tests/test_upstream.c libreac.a -lm -o test_upstream
	./test_upstream
	$(CC) $(CFLAGS) $(INC) tests/test_encode.c libreac.a -lm -o test_encode
	./test_encode
	$(CC) $(CFLAGS) $(INC) tests/test_decode.c libreac.a -lm -o test_decode
	./test_decode
	$(CC) $(CFLAGS) $(INC) tests/test_ports.c libreac.a -lm -o test_ports
	./test_ports

clean:
	rm -f $(OBJS) libreac.a test_reac test_capture test_braid test_upstream test_encode test_decode test_ports

.PHONY: all test clean

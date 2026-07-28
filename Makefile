# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# Native build of libreac (static lib) + unit tests. The OpenWrt package build
# is under openwrt/libreac/ (built via scripts/build-apk.sh against the SDK).
#
# libreac is the shared REAC RX core: wire constants + rate detect (reac.c),
# frame decode (reac_decode.c), live AF_PACKET capture (reac_capture.c), and the
# offline pcap reader (pcap_source.c). Consumed by reac-aes67 and reac-pw.

CC      ?= cc
AR      ?= ar
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra
INC     := -Iinclude

OBJS = reac.o reac_decode.o reac_capture.o pcap_source.o

all: libreac.a

%.o: src/%.c
	$(CC) $(CFLAGS) $(INC) -c $< -o $@

libreac.a: $(OBJS)
	$(AR) rcs $@ $(OBJS)

test: tests/test_reac.c tests/test_capture.c libreac.a
	$(CC) $(CFLAGS) $(INC) tests/test_reac.c libreac.a -o test_reac
	./test_reac
	$(CC) $(CFLAGS) $(INC) tests/test_capture.c libreac.a -o test_capture
	./test_capture

clean:
	rm -f $(OBJS) libreac.a test_reac test_capture

.PHONY: all test clean

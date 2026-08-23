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

# -MMD -MP emits a .d per object listing the headers it included, and the include
# below feeds them back to make. WITHOUT THIS A HEADER EDIT REBUILDS NOTHING:
# `make` reported "libreac.a is up to date" after a header change, so the archive
# kept objects compiled against the OLD header and every test linked against them
# silently. Found when a deliberate sabotage — breaking a constant a test asserts
# — failed to break the test.
%.o: src/%.c
	$(CC) $(CFLAGS) -MMD -MP $(INC) -c $< -o $@

-include $(OBJS:.o=.d)

libreac.a: $(OBJS)
	$(AR) rcs $@ $(OBJS)

test: tests/test_reac.c tests/test_capture.c tests/test_braid.c tests/test_upstream.c tests/test_encode.c tests/test_decode.c tests/test_ports.c tests/test_ctrl.c tests/test_facts.c libreac.a
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
	$(CC) $(CFLAGS) -Itests $(INC) tests/test_ctrl.c libreac.a -lm -o test_ctrl
	./test_ctrl
	$(CC) $(CFLAGS) -Itests $(INC) tests/test_facts.c libreac.a -lm -o test_facts
	./test_facts

# THE CAPTURE CORPUS IS A REGRESSION SUITE. The unit suite above runs on
# goldens; a change that decodes the frames in front of you better and quietly
# stops decoding a capture that used to work leaves it green. `make corpus`
# decodes every capture and diffs the result against tests/corpus-baseline.txt.
# The corpus is not in this repo, so this is a dev-only gate — see
# tools/run-corpus.sh, and run it with --self-test before believing a clean run.
corpus_check: tools/corpus_check.c libreac.a
	$(CC) $(CFLAGS) $(INC) tools/corpus_check.c libreac.a -lm -o corpus_check

corpus: corpus_check
	tools/run-corpus.sh

clean:
	rm -f $(OBJS) $(OBJS:.o=.d) libreac.a test_reac test_capture test_braid test_upstream test_encode test_decode test_ports test_ctrl test_facts corpus_check

.PHONY: all test corpus clean

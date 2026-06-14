# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# Native build of libreac (static lib) + unit tests. The OpenWrt package build
# is under openwrt/libreac/ (built via scripts/build-apk.sh against the SDK).

CC      ?= cc
AR      ?= ar
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra
INC     := -Iinclude

all: libreac.a

reac.o: src/reac.c include/reac/reac.h
	$(CC) $(CFLAGS) $(INC) -c src/reac.c -o $@

libreac.a: reac.o
	$(AR) rcs $@ $<

test: tests/test_reac.c libreac.a
	$(CC) $(CFLAGS) $(INC) tests/test_reac.c libreac.a -o test_reac
	./test_reac

clean:
	rm -f reac.o libreac.a test_reac

.PHONY: all test clean

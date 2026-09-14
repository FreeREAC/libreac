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

# The wire layer, and since 0.8.0 the CONTROL PLANE beside it (docs/REAC-CONTROL-PLANE.md).
# The operator's ruling: a daemon is sockets and PipeWire, it does not speak REAC control.
# Every file in the second list is PURE - no socket, no thread, no clock - which is what let
# them move here unchanged from reac-pw, where they had already been written that way.
OBJS = reac.o reac_ctrlblk.o reac_identity.o reac_ports.o reac_decode.o reac_upstream.o reac_encode.o reac_capture.o pcap_source.o \
       reac_fsm.o reac_master.o reac_master_fsm.o reac_hunt.o reac_arbitration.o \
       reac_grant.o reac_headamp_tx.o reac_ctrl.o reac_scene_body.o \
       reac_link_state.o reac_disco.o reac_boxreg.o reac_clock.o reac_link.o reac_macaddr.o

# tests/reac_facts_assert.h binds libreac's own macros to reac-protocol's
# spec/protocol-facts.yaml (see the header for what it checks). Two builds:
#
#   schema reachable (a dev checkout, CI, or REAC_PROTOCOL pointed at one) —
#   generate it fresh into build/ from the yaml, every time, and require the
#   committed tests/reac_facts_assert.h to be byte-identical to that fresh
#   generation. A committed copy that has quietly fallen behind the schema
#   fails the build here, by design.
#
#   schema not reachable (an RPM/OpenWrt build from a release tarball, which
#   ships neither reac-protocol nor its Python dependency) — use the committed
#   header as-is, the same way autotools ships a generated `configure`.
#
# REAC_PROTOCOL follows the LIBREAC= convention reac-protocol's own spec/Makefile
# uses in the other direction.
REAC_PROTOCOL ?= ../reac-protocol
FACTS_SCHEMA  := $(REAC_PROTOCOL)/spec/protocol-facts.yaml
FACTS_GEN     := $(REAC_PROTOCOL)/spec/gen-facts.py
FACTS_TOOL    := tools/gen-facts-header.py
BUILD_DIR     := build

HAVE_SCHEMA := $(shell test -f "$(FACTS_SCHEMA)" -a -f "$(FACTS_GEN)" && \
                       command -v python3 >/dev/null 2>&1 && \
                       python3 -c "import yaml" >/dev/null 2>&1 && echo 1)

ifeq ($(HAVE_SCHEMA),1)
FACTS_ASSERT_H := $(BUILD_DIR)/reac_facts_assert.h
else
FACTS_ASSERT_H := tests/reac_facts_assert.h
endif

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

# Real prerequisites, not a phony rerun-always: touching the yaml (or the
# generator) is what invalidates this file, same shape as the -MMD -MP object
# rule above.
$(BUILD_DIR)/reac_facts_assert.h: $(FACTS_SCHEMA) $(FACTS_GEN) $(FACTS_TOOL)
	@mkdir -p $(BUILD_DIR)
	python3 $(FACTS_TOOL) $(FACTS_SCHEMA) $(FACTS_GEN) $@

# The drift gate: only meaningful when the schema is reachable (nothing to
# compare the fallback against otherwise). Regenerates and diffs against the
# committed tests/reac_facts_assert.h; a difference fails the build rather
# than passing quietly on a stale copy.
.PHONY: facts-drift-check
ifeq ($(HAVE_SCHEMA),1)
facts-drift-check: $(BUILD_DIR)/reac_facts_assert.h
	@diff -u tests/reac_facts_assert.h $(BUILD_DIR)/reac_facts_assert.h || \
	  { echo "tests/reac_facts_assert.h has drifted from $(FACTS_SCHEMA)."; \
	    echo "Refresh it: cp $(BUILD_DIR)/reac_facts_assert.h tests/reac_facts_assert.h"; \
	    exit 1; }
else
facts-drift-check:
	@echo "REAC_PROTOCOL not reachable at $(REAC_PROTOCOL); skipping the facts drift gate (standalone build, using the shipped tests/reac_facts_assert.h)"
endif

test: tests/test_reac_etf.c tests/test_reac_etf_qdisc.c transport/src/reac_etf.c transport/src/reac_etf.h transport/src/reac_etf_qdisc.c include/reac/transport/reac_etf_qdisc.h tests/test_abi_layout.c tests/abi-layout.inc tests/test_master_capture.c tests/test_master_carriers.c tests/test_link.c tests/test_reac.c tests/test_capture.c tests/test_braid.c tests/test_upstream.c tests/test_encode.c tests/test_decode.c tests/test_ports.c tests/test_ctrl.c tests/test_facts.c tests/test_identity.c libreac.a $(FACTS_ASSERT_H)
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
	$(CC) $(CFLAGS) -Itests $(INC) tests/test_link.c libreac.a -lm -o test_link
	./test_link
	$(CC) $(CFLAGS) -I$(dir $(FACTS_ASSERT_H)) $(INC) tests/test_facts.c libreac.a -lm -o test_facts
	./test_facts
	$(CC) $(CFLAGS) $(INC) tests/test_identity.c libreac.a -lm -o test_identity
	./test_identity
	$(CC) $(CFLAGS) $(INC) tests/test_master_carriers.c libreac.a -lm -o test_master_carriers
	./test_master_carriers
	$(CC) $(CFLAGS) $(INC) tests/test_master_capture.c libreac.a -lm -o test_master_capture
	./test_master_capture
	# The ETF pacing backend's arithmetic and preconditions. It joins `make test`
	# rather than `test-transport` because reac_etf.c reaches no libreac symbol and
	# no reac-pw header: it is pure arithmetic plus two syscalls, so it builds
	# wherever the library does, including a release tarball with no REACPW_INCLUDE.
	$(CC) $(CFLAGS) -D_GNU_SOURCE $(INC) -Itransport/src \
	    tests/test_reac_etf.c transport/src/reac_etf.c -o test_reac_etf
	./test_reac_etf
	# THE QDISC THE DAEMON OWNS. Byte-exact against what iproute2 puts on a netlink
	# socket (captured with strace inside `unshare -rn`; the capture is in the test's
	# header), because a wrong attribute length fails exactly like a missing module.
	$(CC) $(CFLAGS) -D_GNU_SOURCE $(INC) -Itransport/src \
	    tests/test_reac_etf_qdisc.c transport/src/reac_etf_qdisc.c \
	    -o test_reac_etf_qdisc
	./test_reac_etf_qdisc
	# THE ABI RATCHET. Every other test is rebuilt against the headers it is
	# testing and therefore cannot see a struct member move; reac-pw is not.
	# Needs the transport headers' vendored reac-pw ones, like the transport
	# build does.
	$(CC) $(CFLAGS) -D_GNU_SOURCE -Itests $(INC) -Ipackaging/vendor/reac-pw-headers \
	    tests/test_abi_layout.c libreac.a -lm -o test_abi_layout
	./test_abi_layout
	# A SOURCE-SHAPE ARM, not a value arm. The head-amp base must have exactly
	# one source in the code — the announced strap. A per-width table agrees
	# with the announce on every chassis we own, so no test built from our own
	# captures can catch its return; only the shape of the code can.
	tools/conformance-headamp-base.sh
	@$(MAKE) --no-print-directory facts-drift-check

# THE CAPTURE CORPUS IS A REGRESSION SUITE. The unit suite above runs on
# goldens; a change that decodes the frames in front of you better and quietly
# stops decoding a capture that used to work leaves it green. `make corpus`
# decodes every capture and diffs the result against tests/corpus-baseline.txt.
# The corpus is not in this repo, so this is a dev-only gate — see
# tools/run-corpus.sh, and run it with --self-test before believing a clean run.
corpus_check: tools/corpus_check.c libreac.a
	$(CC) $(CFLAGS) $(INC) tools/corpus_check.c libreac.a -lm -o corpus_check

conformance:
	tools/conformance-headamp-base.sh
	# THE HARNESS IS AN INSTRUMENT, AND AN INSTRUMENT IS GATED LIKE ONE. The
	# pacer comparison (2026-09-13-reac-kernel-module-backend.md, lane 1) is
	# decided by a table; a table whose two columns cannot be made to differ
	# would read as "no difference" on the day. This drives pace_hist and the
	# table renderer with a clean grid and with the userspace pacer's own
	# measured miss distribution, and requires them to separate.
	tools/conformance-pace-harness.sh

corpus: corpus_check
	tools/run-corpus.sh

# THE WIRE TOOLS ANSWER A DIFFERENT SHAPE OF QUESTION FROM corpus_check. That
# one TALLIES — "does this build still decode the corpus". A question about
# protocol LAW needs individual records in time order, with the talker attached,
# and it usually turns on something that did NOT happen. A count cannot see an
# absence, and an absence is only a finding once the instrument has been shown
# to detect the corresponding presence, so each of these carries its own control
# and says so in its header. They are dev-only, they never open a socket, and
# they are built here rather than pasted into a session because a verdict whose
# instrument was thrown away is not reproducible.
#
#   headamp_trace   every head-amp record, in order, with src MAC and truncation
#   wire_census     who talks on a segment and in what frame shapes
#   ctrl_delta      which control-block bytes move, per talker and message kind
#   upstream_watch  every byte a box's own frames change, classed by how often
#   slotmap_watch   the sliding slot-map window unrolled into per-slot state
#   seq_gaps        per-talker frame-counter holes — the control for any
#                   "nothing was sent" claim
#   pace_hist       the inter-frame INTERVAL distribution per talker -- the
#                   external truth for a pacer comparison, with its own
#                   --self-test control (tools/pace-compare.sh runs both)
#   group_map_scan  every ENROLL group map with its talker, VLAN-tag aware, with
#                   a per-talker census as the control for a missing shape
# The pcap readers: one pattern rule serves all of them.
WIRE_TOOLS_PCAP = headamp_trace wire_census ctrl_delta upstream_watch slotmap_watch seq_gaps group_map_scan pace_hist
# fake_box is a wire tool too, but it TRANSMITS: it opens an AF_PACKET socket
# where the others only read a file, so it has its own recipe below.
WIRE_TOOLS = $(WIRE_TOOLS_PCAP) fake_box

wire-tools: $(WIRE_TOOLS)

$(WIRE_TOOLS_PCAP): %: tools/%.c libreac.a
	$(CC) $(CFLAGS) $(INC) $< libreac.a -lm -o $@

# fake_box opens an AF_PACKET socket, so it needs the GNU headers the other wire
# tools (pure pcap readers) do not. Its own rule rather than widening theirs.
# etf_probe transmits and configures nothing outside a namespace it is handed; it
# reaches libreac not at all, only transport/src/reac_etf.c. Not in $(WIRE_TOOLS):
# those are pcap readers and this one opens a socket.
etf_probe: tools/etf_probe.c transport/src/reac_etf.c
	$(CC) $(CFLAGS) -D_GNU_SOURCE $(INC) -Itransport/src $^ -o $@

fake_box: tools/fake_box.c libreac.a
	$(CC) $(CFLAGS) -D_GNU_SOURCE $(INC) $< libreac.a -lm -o $@

# --- libreac-transport: sockets, pacer, RT threads, VLAN/topology, ring, segment lock ---
# The pieces of reac-pw that never touch PipeWire
# (docs/design/specs/2026-09-11-reac-transport-library.md). A second, PARALLEL object
# family -- never folded into the OBJS glob above, or every transport file becomes part
# of libreac's own soname and the whole point of a second library is lost.
#
# REACPW_INCLUDE points at a reac-pw checkout's src/ for the two headers that stay there
# (reac_rate_cfg.h, reac_role_cfg.h) but are #include-d by a moved header for their pure
# declarations only (spec §2/§5 names the seam). Unset by default: every transport object
# that does not reach those two headers still builds; the two that do fail loudly at
# compile time rather than silently skipping.
REACPW_INCLUDE ?=
TRANSPORT_SRC_DIR := transport/src
TRANSPORT_OBJS := $(patsubst $(TRANSPORT_SRC_DIR)/%.c,transport/%.o,$(wildcard $(TRANSPORT_SRC_DIR)/*.c))

# -D_GNU_SOURCE: these files read IFNAMSIZ, sockaddr_ll, ETH_P_ALL and friends from
# net/if.h et al., which glibc gates behind _DEFAULT_SOURCE/_GNU_SOURCE -- absent under
# plain -std=c11. reac-pw's own meson.build sets this project-wide for the same reason
# (SPA's inline string.h needs it too); libreac's own OBJS never needed it before now.
transport/%.o: $(TRANSPORT_SRC_DIR)/%.c
	@mkdir -p transport
	$(CC) $(CFLAGS) -D_GNU_SOURCE -MMD -MP -Iinclude -I$(TRANSPORT_SRC_DIR) $(if $(REACPW_INCLUDE),-I$(REACPW_INCLUDE)) -c $< -o $@

-include $(TRANSPORT_OBJS:.o=.d)

libreac-transport.a: $(TRANSPORT_OBJS)
	$(AR) rcs $@ $(TRANSPORT_OBJS)

transport: libreac-transport.a

# The transport tier's own harness (spec 2026-09-11-reac-transport-library §8 leaves
# room for one; before this there was none and transport ran only under reac-pw's
# meson test). It needs libreac-transport.a, so it is NOT part of `make test`:
# building the transport tier needs REACPW_INCLUDE pointed at a reac-pw checkout for
# the two headers that stay there, and a release tarball has none.
#
# THE CORPUS ARM IS OPT-IN AND SAYS SO. REAC_TAP_CAPTURE names a VLAN-STRIPPED pcap of
# a REAC segment (`tcprewrite --enet-vlan=del`, because nothing in libreac parses an
# 802.1Q tag). Absent, the test prints CORPUS ARM NOT RUN rather than passing quietly.
REAC_TAP_CAPTURE ?=

test-transport: tests/test_tap.c libreac-transport.a libreac.a
	$(CC) $(CFLAGS) $(INC) tests/test_tap.c libreac-transport.a libreac.a -lm -lpthread -o test_tap
	./test_tap $(REAC_TAP_CAPTURE)
	tools/conformance-tap-silent.sh

clean:
	rm -f $(OBJS) $(OBJS:.o=.d) libreac.a test_reac test_capture test_braid test_upstream test_encode test_decode test_ports test_ctrl test_link test_facts test_identity test_master_carriers test_master_capture test_abi_layout test_reac_etf test_reac_etf_qdisc etf_probe corpus_check $(WIRE_TOOLS)
	rm -f $(TRANSPORT_OBJS) $(TRANSPORT_OBJS:.o=.d) libreac-transport.a test_tap
	rm -rf $(BUILD_DIR) transport/*.o transport/*.d

.PHONY: all test test-transport conformance corpus wire-tools clean transport

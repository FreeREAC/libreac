// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// GENERATED FILE - DO NOT EDIT.
//
// Source:    spec/protocol-facts.yaml   (in FreeREAC/reac-protocol)
// Generator: spec/gen-facts.py
//
// Edit the schema and regenerate. A hand-edit here is erased by the next run
// and, worse, is invisible to the cross-check that keeps reac.ksy and libreac
// agreeing - which is the whole reason this file is generated.
//
// Compile this against libreac's headers. Every assertion that fails names
// a constant the two expressions of the protocol no longer agree on.

#ifndef REAC_FACTS_ASSERT_H
#define REAC_FACTS_ASSERT_H

#include <reac/reac.h>
#include <reac/reac_ctrlblk.h>
#include <reac/reac_ports.h>

_Static_assert(REAC_ETHERTYPE == 0x8819,
               "REAC_ETHERTYPE has drifted from protocol-facts.yaml ETHERTYPE");
_Static_assert(REAC_L2_HEADER_LEN == 50,
               "REAC_L2_HEADER_LEN has drifted from protocol-facts.yaml L2_HEADER_LEN");
_Static_assert(REAC_AUDIO_OFFSET == 50,
               "REAC_AUDIO_OFFSET has drifted from protocol-facts.yaml AUDIO_OFFSET");
_Static_assert(REAC_HDR_COUNTER_OFF == 14,
               "REAC_HDR_COUNTER_OFF has drifted from protocol-facts.yaml HDR_COUNTER_OFF");
_Static_assert(REAC_UPSTREAM_OVERHEAD == 52,
               "REAC_UPSTREAM_OVERHEAD has drifted from protocol-facts.yaml FRAME_OVERHEAD");
_Static_assert(REAC_UPSTREAM_BYTES_PER_CH == 36,
               "REAC_UPSTREAM_BYTES_PER_CH has drifted from protocol-facts.yaml BYTES_PER_CHANNEL");
_Static_assert(REAC_SAMPLES_PER_PKT == 12,
               "REAC_SAMPLES_PER_PKT has drifted from protocol-facts.yaml SAMPLES_PER_PKT");
_Static_assert(REAC_RESOLUTION == 3,
               "REAC_RESOLUTION has drifted from protocol-facts.yaml RESOLUTION");
_Static_assert(REAC_MAX_CHANNELS == 40,
               "REAC_MAX_CHANNELS has drifted from protocol-facts.yaml MAX_CHANNELS");
_Static_assert(REAC_FRAME_BYTES == 1492,
               "REAC_FRAME_BYTES has drifted from protocol-facts.yaml FRAME_BYTES");
_Static_assert(REAC_AUDIO_BYTES == 1440,
               "REAC_AUDIO_BYTES has drifted from protocol-facts.yaml AUDIO_BYTES");
_Static_assert(REAC_END_MARKER_0 == 0xc2,
               "REAC_END_MARKER_0 has drifted from protocol-facts.yaml END_MARKER_0");
_Static_assert(REAC_END_MARKER_1 == 0xea,
               "REAC_END_MARKER_1 has drifted from protocol-facts.yaml END_MARKER_1");
_Static_assert(REAC_CTRL_BLOCK_OFF == 18,
               "REAC_CTRL_BLOCK_OFF has drifted from protocol-facts.yaml CTRL_BLOCK_OFF");
_Static_assert(REAC_CTRL_BLOCK_END == 50,
               "REAC_CTRL_BLOCK_END has drifted from protocol-facts.yaml CTRL_BLOCK_END");
_Static_assert(REAC_CTRL_BLOCK_LEN == 32,
               "REAC_CTRL_BLOCK_LEN has drifted from protocol-facts.yaml CTRL_BLOCK_LEN");
_Static_assert(REAC_CTRL_CKSUM_OFF == 49,
               "REAC_CTRL_CKSUM_OFF has drifted from protocol-facts.yaml CTRL_CKSUM_OFF");
_Static_assert(REAC_SCENE_BYTES == 8904,
               "REAC_SCENE_BYTES has drifted from protocol-facts.yaml SCENE_BYTES");
_Static_assert(REAC_SCENE_HEAD_BYTES == 24,
               "REAC_SCENE_HEAD_BYTES has drifted from protocol-facts.yaml SCENE_HEAD_BYTES");
_Static_assert(REAC_SCENE_CHUNK_BYTES == 26,
               "REAC_SCENE_CHUNK_BYTES has drifted from protocol-facts.yaml SCENE_CHUNK_BYTES");
_Static_assert(REAC_SCENE_TAIL_BYTES == 14,
               "REAC_SCENE_TAIL_BYTES has drifted from protocol-facts.yaml SCENE_TAIL_BYTES");
_Static_assert(REAC_SCENE_CHUNKS == 341,
               "REAC_SCENE_CHUNKS has drifted from protocol-facts.yaml SCENE_CHUNKS");
_Static_assert(REAC_SCENE_STEPS == 343,
               "REAC_SCENE_STEPS has drifted from protocol-facts.yaml SCENE_STEPS");
_Static_assert(REAC_SCENE_TAG_ID_OFF == 0x000,
               "REAC_SCENE_TAG_ID_OFF has drifted from protocol-facts.yaml SCENE_TAG_ID_OFF");
_Static_assert(REAC_SCENE_TAG_SYSP_OFF == 0x368,
               "REAC_SCENE_TAG_SYSP_OFF has drifted from protocol-facts.yaml SCENE_TAG_SYSP_OFF");
_Static_assert(REAC_SCENE_TAG_SCEN_OFF == 0x37c,
               "REAC_SCENE_TAG_SCEN_OFF has drifted from protocol-facts.yaml SCENE_TAG_SCEN_OFF");
_Static_assert(REAC_SCENE_MAC_OFF == 0x340,
               "REAC_SCENE_MAC_OFF has drifted from protocol-facts.yaml SCENE_MAC_OFF");
_Static_assert(REAC_HEADAMP_SENS_MAX == 0x37,
               "REAC_HEADAMP_SENS_MAX has drifted from protocol-facts.yaml HEADAMP_SENS_MAX");
_Static_assert(REAC_HEADAMP_MAX_CH == 0x30,
               "REAC_HEADAMP_MAX_CH has drifted from protocol-facts.yaml HEADAMP_CH_SPAN");
_Static_assert(REAC_HEADAMP_GRAN_SENS_SHIFT == 0,
               "REAC_HEADAMP_GRAN_SENS_SHIFT has drifted from protocol-facts.yaml HEADAMP_GRAN_SENS_SHIFT");
_Static_assert(REAC_HEADAMP_GRAN_FLAGS_SHIFT == 0,
               "REAC_HEADAMP_GRAN_FLAGS_SHIFT has drifted from protocol-facts.yaml HEADAMP_GRAN_FLAGS_SHIFT");
_Static_assert(REAC_HEADAMP_GRAN_PHANTOM_SHIFT == 0,
               "REAC_HEADAMP_GRAN_PHANTOM_SHIFT has drifted from protocol-facts.yaml HEADAMP_GRAN_PHANTOM_SHIFT");
_Static_assert(REAC_HEADAMP_NPARAMS == 3,
               "REAC_HEADAMP_NPARAMS has drifted from protocol-facts.yaml HEADAMP_SWEEP_RECORDS_PER_CH");
_Static_assert(REAC_HEADAMP_BASE_FROM_CONFIG_BYTE7 == 1,
               "REAC_HEADAMP_BASE_FROM_CONFIG_BYTE7 has drifted from protocol-facts.yaml HEADAMP_BASE_FROM_CONFIG_BYTE7");
_Static_assert(REAC_HEADAMP_BASE_MULTIPLIER == 0x10,
               "REAC_HEADAMP_BASE_MULTIPLIER has drifted from protocol-facts.yaml HEADAMP_BASE_MULTIPLIER");
_Static_assert(REAC_HEADAMP_BASE_IS_CHASSIS_NOT_GRANT == 1,
               "REAC_HEADAMP_BASE_IS_CHASSIS_NOT_GRANT has drifted from protocol-facts.yaml HEADAMP_BASE_IS_CHASSIS_NOT_GRANT");
_Static_assert(REAC_HEADAMP_APPLY_UNIT_SLOTS == 8,
               "REAC_HEADAMP_APPLY_UNIT_SLOTS has drifted from protocol-facts.yaml HEADAMP_APPLY_UNIT_SLOTS");
_Static_assert(REAC_HEADAMP_SENS_REF_CDB == -1000,
               "REAC_HEADAMP_SENS_REF_CDB has drifted from protocol-facts.yaml HEADAMP_SENS_REF_CDB");
_Static_assert(REAC_HEADAMP_SENS_STEP_CDB == 100,
               "REAC_HEADAMP_SENS_STEP_CDB has drifted from protocol-facts.yaml HEADAMP_SENS_STEP_CDB");
_Static_assert(REAC_HEADAMP_PAD_CDB == 2000,
               "REAC_HEADAMP_PAD_CDB has drifted from protocol-facts.yaml HEADAMP_PAD_CDB");
_Static_assert(REAC_PORTS_TABLE_OFF == 8,
               "REAC_PORTS_TABLE_OFF has drifted from protocol-facts.yaml PORTS_TABLE_OFF");
_Static_assert(REAC_PORTS_TABLE_SLOTS == 12,
               "REAC_PORTS_TABLE_SLOTS has drifted from protocol-facts.yaml PORTS_TABLE_SLOTS");
_Static_assert(REAC_PORTS_CH_PER_SLOT == 4,
               "REAC_PORTS_CH_PER_SLOT has drifted from protocol-facts.yaml PORTS_CH_PER_SLOT");
_Static_assert(REAC_PORT_SLOT_OUT == 0x01,
               "REAC_PORT_SLOT_OUT has drifted from protocol-facts.yaml PORT_SLOT_OUT");
_Static_assert(REAC_PORT_SLOT_IN == 0x02,
               "REAC_PORT_SLOT_IN has drifted from protocol-facts.yaml PORT_SLOT_IN");
_Static_assert(REAC_PORT_SLOT_EMPTY == 0x03,
               "REAC_PORT_SLOT_EMPTY has drifted from protocol-facts.yaml PORT_SLOT_EMPTY");

#define REAC_FACTS_ASSERTIONS 46

#endif /* REAC_FACTS_ASSERT_H */

// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "zap_common.h"
#include <cstdint>

// Build the endpoint probe order for Basic-cluster identity reads.
// Endpoints that advertise the Basic cluster (0x0000) in their in-cluster
// list are probed first, preserving the original endpoint order. Remaining
// endpoints follow in their original order.
uint8_t zigbee_interview_build_basic_probe_order(const ZapDevice& dev,
                                                 uint8_t* out_eps,
                                                 uint8_t out_cap);

// Parse Basic-cluster (0x0000) identity attributes out of a ZCL frame payload
// (either Read Attributes Response, cmd 0x01, or Report Attributes, cmd 0x0A).
// `data` points at the ZCL frame body (past AF_INCOMING_MSG header up to ZCL
// payload, i.e. frame_ctrl..data_end).
//
// Writes NUL-terminated strings to model_out / mfg_out when attr 0x0005
// (modelIdentifier) / 0x0004 (manufacturerName) are present; writes the
// little-endian u16 manufacturerCode to mfg_code_out when attr 0x0001 is
// present. Any of the out-pointers may be NULL (skip).
//
// Returns true if at least one identity field was extracted. Tolerates
// partial frames and unknown attribute types (skips them where possible).
bool zigbee_parse_basic_identity(const uint8_t* data, uint8_t data_len,
                                 char* model_out, uint8_t model_cap,
                                 char* mfg_out, uint8_t mfg_cap,
                                 uint16_t* mfg_code_out);

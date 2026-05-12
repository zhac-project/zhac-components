// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// znp_confirm.h — correlate AF_DATA_REQUEST transaction IDs with
// their asynchronous AF_DATA_CONFIRM AREQs (cmd0=0x44 cmd1=0x80).
//
// The ZNP worker checks the SRSP for AF_DATA_REQUEST — that confirms
// the NCP *accepted* the request, but MAC-level delivery happens later
// and arrives as an AREQ carrying a status byte (0x00 success,
// 0xF0 ZMacTransactionExpired, …). Callers that care about MAC
// delivery reserve a ring slot before sending the request, then block
// on the slot until the confirm arrives (or a timeout).
//
// Usage:
//
//   const uint8_t trans_id = zcl_seq_next();
//   int slot = znp_confirm_reserve(trans_id);
//   // ... send AF_DATA_REQUEST with this trans_id ...
//   int status = znp_confirm_wait(slot, 1000);
//   if (status < 0) { /* MAC timeout */ }
//   if (status == 0xF0) { /* ZMacTransactionExpired */ }
//
// The ring is fixed-size (16 slots) — more than enough for the
// Z-Stack in-flight cap (~8 AF_DATA_REQUEST).
//
// Reserve MUST happen before the request is transmitted; otherwise a
// fast confirm may arrive and be dropped before any slot is waiting.

#pragma once
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

// Reserve a slot to await the confirm for `trans_id`. Returns a slot
// handle (0..15) or -1 if the ring is full.
int znp_confirm_reserve(uint8_t trans_id);

// Block up to `timeout_ms` for the matching AF_DATA_CONFIRM. Returns
// the MAC status byte (0x00 success, 0xF0 MacTransactionExpired, …)
// or -1 on timeout. The slot is freed on return either way.
int znp_confirm_wait(int slot, uint32_t timeout_ms);

// Free a reserved slot without waiting. Use when the send path failed
// after reserve but before the confirm could arrive.
void znp_confirm_release(int slot);

#ifdef __cplusplus
}
#endif

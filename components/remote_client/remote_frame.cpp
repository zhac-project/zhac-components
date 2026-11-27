// SPDX-FileCopyrightText: 2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The cloud link takes whole frames only (review 2026-11-26 HL-01). See remote_frame.h.
#include "remote_frame.h"

RemoteFrameVerdict remote_frame_verdict(uint8_t op_code, int data_len, int payload_len, int payload_offset, bool fin) {
    // A later piece starts mid-frame and a continuation frame mid-message: nothing in them may be taken for a
    // frame, and the frame they belong to was reported at its first piece.
    if (op_code != 0x01 || payload_offset != 0 || data_len <= 0) return RF_DROP;
    // 8192 == the websocket client's buffer_size (start_ws_client) and publish_event's cap.
    if (!fin || data_len != payload_len || payload_len > 8192) return RF_DROP_TOO_LARGE;
    return RF_ACCEPT;
}

bool remote_json_only_trailing_space(const char* buf, size_t len, size_t parsed_end) {
    for (size_t i = parsed_end; i < len; i++)
        if (buf[i] != ' ' && buf[i] != '\t' && buf[i] != '\r' && buf[i] != '\n') return false;
    return true;
}

// SPDX-FileCopyrightText: 2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
//
// The cloud link takes whole frames only (review 2026-11-26 HL-01). Pure, host-tested in
// test/remote_frame_test.cpp.

#include <cstddef>
#include <cstdint>

enum RemoteFrameVerdict { RF_ACCEPT, RF_DROP, RF_DROP_TOO_LARGE };

// The receive handler's verdict on one WEBSOCKET_EVENT_DATA piece. RF_ACCEPT: the piece is a whole text frame
// (op_code 0x01, fin, payload_offset 0, data_len == payload_len, 1 to 8,192 bytes). RF_DROP_TOO_LARGE: the
// first piece of a text frame that is not whole (longer than the 8 KB buffer, fragmented, or split across
// reads); the handler reports it once. RF_DROP: every other piece (a later piece, a continuation frame,
// binary, control, empty), never reported.
RemoteFrameVerdict remote_frame_verdict(uint8_t op_code, int data_len, int payload_len, int payload_offset, bool fin);

// True when only JSON whitespace (space, tab, CR, LF) follows the parsed value in buf[parsed_end, len).
bool remote_json_only_trailing_space(const char* buf, size_t len, size_t parsed_end);

// handle_rx_frame's parser input: deserializeJson reads the frame through it byte by byte and stops right after
// an object, array, string or literal, so `pos` is where that value ends. (A root number is read one byte past
// its end; it is never a command.)
struct RemoteJsonReader {
    const char* buf;
    size_t      len;
    size_t      pos = 0;
    int read() { return pos < len ? static_cast<unsigned char>(buf[pos++]) : -1; }
};

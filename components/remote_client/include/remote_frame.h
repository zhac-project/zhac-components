// SPDX-FileCopyrightText: 2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
//
// The cloud link parses whole frames only (review 2026-11-26 HL-01). Pure, host-tested in
// test/remote_frame_test.cpp.

#include <cstddef>
#include <cstdint>

// One text frame put back together from the pieces esp_websocket_client hands over as WEBSOCKET_EVENT_DATA when
// the frame does not come in one read: each piece carries the frame's opcode, fin and payload_len, and its own
// payload_offset. alloc/release: PSRAM on the hub, malloc in the host tests.
struct RemoteFrameAsm {
    void* (*alloc)(size_t);
    void  (*release)(void*);
    char* buf  = nullptr;   // payload_len + 1 bytes while a frame is put together
    int   len  = 0;         // that frame's payload_len
    int   held = 0;         // its bytes so far
    bool  lost = false;     // the link lost its place in a frame; only remote_frame_reset clears it
};

// What one piece gave.
struct RemoteFrameStep {
    char*  frame     = nullptr;   // a whole frame, NUL-terminated, `len` bytes; the caller now owns it (release)
    size_t len       = 0;
    bool   too_large = false;     // a text frame over 8,192 bytes, or the first of a message in several frames
    bool   broken    = false;     // a frame being put together was dropped before it was whole
    bool   lost      = false;     // this piece showed the link lost its place: once, until remote_frame_reset
};

// Feeds one piece. A piece at payload_offset 0 starts a frame, after dropping one not yet whole; a text frame
// (op_code 0x01), final, 1 to 8,192 bytes long, is put together. Each next piece must carry the same opcode, fin
// and payload_len, at the offset of the bytes held so far; anything else drops the frame. Binary and control
// frames, continuation frames (op_code 0x00) and empty frames are never taken. A piece with no bytes in a frame
// that has some (a stalled read) marks the link lost: the frame is dropped and every piece refused until the reset.
RemoteFrameStep remote_frame_feed(RemoteFrameAsm& a, uint8_t op_code, const char* data, int data_len,
                                  int payload_len, int payload_offset, bool fin);

// Drops a frame being put together and clears a lost place (the link connected, closed or failed, or its client
// was stopped). True when a frame was held.
bool remote_frame_reset(RemoteFrameAsm& a);

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

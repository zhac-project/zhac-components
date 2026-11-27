// SPDX-FileCopyrightText: 2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The cloud link parses whole frames only (review 2026-11-26 HL-01). See remote_frame.h.
#include "remote_frame.h"
#include <cstring>

bool remote_frame_reset(RemoteFrameAsm& a) {
    const bool had = a.buf != nullptr;
    if (had) a.release(a.buf);
    a.buf  = nullptr;
    a.len  = 0;
    a.held = 0;
    a.lost = false;
    return had;
}

RemoteFrameStep remote_frame_feed(RemoteFrameAsm& a, uint8_t op_code, const char* data, int data_len,
                                  int payload_len, int payload_offset, bool fin) {
    RemoteFrameStep s;
    if (a.lost) return s;
    // A piece with no bytes in a frame that has some is a payload read that timed out (the transport's timeout
    // code is 0). IDF v6.0's transport_ws then forgets where the frame ends and reads the rest of it as new frame
    // headers, so the pieces after it, even at offset 0, may be bytes from inside the frame (HL-01, fix round 1).
    // Nothing more is taken until the link reconnects; v6.1 keeps its place, but both boards reconnect.
    if (data_len == 0 && payload_len > 0) {
        remote_frame_reset(a);
        a.lost = s.lost = true;
        return s;
    }
    if (payload_offset == 0) {
        // A new frame: one not yet whole can no longer be finished.
        s.broken = remote_frame_reset(a);
        if (op_code != 0x01 || payload_len == 0) return s;   // binary, control, continuation, empty
        // 8192 == the websocket client's buffer_size (start_ws_client) and publish_event's cap. The cloud's
        // Tomcat sends a message over its 8 KB buffer as a first frame with fin 0 and continuation frames.
        if (!fin || payload_len < 0 || payload_len > 8192) {
            s.too_large = true;
            return s;
        }
        a.buf = static_cast<char*>(a.alloc(static_cast<size_t>(payload_len) + 1));
        if (!a.buf) {
            s.broken = true;
            return s;
        }
        a.len = payload_len;
    } else if (!a.buf) {
        return s;   // the rest of a frame already dropped, and reported then
    } else if (op_code != 0x01 || !fin || payload_len != a.len || payload_offset != a.held) {
        s.broken = remote_frame_reset(a);   // a gap, an overlap, or not this frame's piece
        return s;
    }
    if (data_len < 0 || data_len > a.len - a.held) {   // more than the frame has left
        s.broken = remote_frame_reset(a);
        return s;
    }
    if (data_len > 0) std::memcpy(a.buf + a.held, data, static_cast<size_t>(data_len));
    a.held += data_len;
    if (a.held < a.len) return s;
    a.buf[a.len] = '\0';
    s.frame = a.buf;   // the caller's now
    s.len   = static_cast<size_t>(a.len);
    a.buf   = nullptr;
    a.len   = 0;
    a.held  = 0;
    return s;
}

bool remote_json_only_trailing_space(const char* buf, size_t len, size_t parsed_end) {
    for (size_t i = parsed_end; i < len; i++)
        if (buf[i] != ' ' && buf[i] != '\t' && buf[i] != '\r' && buf[i] != '\n') return false;
    return true;
}

// SPDX-FileCopyrightText: 2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The cloud link takes whole frames only (review 2026-11-26 HL-01): the receive handler's verdict on each
// piece esp_websocket_client hands over, and the parse that refuses bytes after the JSON value.
#include "remote_frame.h"
#include "ArduinoJson.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>

static void wholeTextFrameAccepted() {
    assert(remote_frame_verdict(0x01, 8192, 8192, 0, true) == RF_ACCEPT);
    assert(remote_frame_verdict(0x01, 1, 1, 0, true) == RF_ACCEPT);
}

static void firstPieceOfLongFrameFlagged() {
    // A frame over the 8 KB buffer comes in pieces, each with the frame's opcode: the first is flagged, the rest dropped.
    assert(remote_frame_verdict(0x01, 8192, 8193, 0, true) == RF_DROP_TOO_LARGE);
    assert(remote_frame_verdict(0x01, 1, 8193, 8192, true) == RF_DROP);
    // Over 8192 even when read in one piece (the cap holds if the buffer grows).
    assert(remote_frame_verdict(0x01, 8193, 8193, 0, true) == RF_DROP_TOO_LARGE);
    // A frame that fits but came in two reads: neither piece is taken for the frame.
    assert(remote_frame_verdict(0x01, 60, 100, 0, true) == RF_DROP_TOO_LARGE);
    assert(remote_frame_verdict(0x01, 40, 100, 60, true) == RF_DROP);
}

static void notFinalDropped() {
    // A fragmented message: the first fragment is flagged; a ping between the fragments and the
    // continuation (opcode 0x00) are dropped.
    assert(remote_frame_verdict(0x01, 100, 100, 0, false) == RF_DROP_TOO_LARGE);
    assert(remote_frame_verdict(0x09, 0, 0, 0, true) == RF_DROP);
    assert(remote_frame_verdict(0x00, 100, 100, 0, true) == RF_DROP);
    assert(remote_frame_verdict(0x00, 100, 100, 0, false) == RF_DROP);
}

static void emptyOrNonTextDropped() {
    assert(remote_frame_verdict(0x01, 0, 0, 0, true) == RF_DROP);
    assert(remote_frame_verdict(0x02, 100, 100, 0, true) == RF_DROP);
}

// Where the parse ends when ArduinoJson reads through RemoteJsonReader, as handle_rx_frame does; SIZE_MAX on an error.
static size_t parsed_end(const std::string& s) {
    JsonDocument doc;
    RemoteJsonReader in{s.data(), s.size()};
    if (deserializeJson(doc, in)) return SIZE_MAX;
    return in.pos;
}

static void trailingBytesRefused() {
    // ArduinoJson takes the first value without an error and stops right after it; the check refuses the rest.
    const std::string two = "{\"cmd\":\"a\"} {\"cmd\":\"b\"}";
    assert(parsed_end(two) == 11);
    assert(!remote_json_only_trailing_space(two.data(), two.size(), 11));
    const std::string spaced = "{\"cmd\":\"a\"}  \n";
    assert(parsed_end(spaced) == 11);
    assert(remote_json_only_trailing_space(spaced.data(), spaced.size(), 11));
    const std::string exact = "{\"cmd\":\"a\"}";
    assert(parsed_end(exact) == exact.size());
    assert(remote_json_only_trailing_space(exact.data(), exact.size(), exact.size()));
}

int main() {
    wholeTextFrameAccepted();
    firstPieceOfLongFrameFlagged();
    notFinalDropped();
    emptyOrNonTextDropped();
    trailingBytesRefused();
    printf("remote_frame_tests: ok\n");
    return 0;
}

// SPDX-FileCopyrightText: 2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The cloud link parses whole frames only (review 2026-11-26 HL-01): one frame's transport pieces are put back
// together before anything is parsed, and the parse refuses bytes after the JSON value.
#include "remote_frame.h"
#include "ArduinoJson.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static int s_live = 0;   // blocks taken through the assembler and not released yet
static void* t_alloc(size_t n) { s_live++; return std::malloc(n); }
static void t_release(void* p) { s_live--; std::free(p); }

struct Fed {
    std::vector<std::string> frames;
    int too_large = 0;
    int broken = 0;
    int lost = 0;
};

// One DATA event. A frame that comes out is kept, then released as task_remote releases it after the parse.
static void feed(RemoteFrameAsm& a, Fed& f, uint8_t op, const std::string& data, int payload_len, int offset,
                 bool fin = true) {
    const RemoteFrameStep s = remote_frame_feed(a, op, data.data(), (int)data.size(), payload_len, offset, fin);
    f.too_large += s.too_large;
    f.broken += s.broken;
    f.lost += s.lost;
    if (s.frame) {
        assert(s.frame[s.len] == '\0');
        f.frames.emplace_back(s.frame, s.len);
        t_release(s.frame);
    }
}

// `text` as one text frame cut at `cuts`: every piece carries the frame's payload_len and its own offset.
static void feed_cut(RemoteFrameAsm& a, Fed& f, const std::string& text, std::vector<size_t> cuts) {
    cuts.push_back(text.size());
    size_t at = 0;
    for (size_t c : cuts) {
        feed(a, f, 0x01, text.substr(at, c - at), (int)text.size(), (int)at);
        at = c;
    }
}

static const std::string kCmd = "{\"id\":7,\"cmd\":\"script.write\",\"args\":{\"name\":\"heating\"}}";

static void wholeFrameInOnePiece() {
    RemoteFrameAsm a{t_alloc, t_release};
    Fed f;
    const std::string max(8192, 'x');
    feed(a, f, 0x01, max, 8192, 0);
    feed(a, f, 0x01, kCmd, (int)kCmd.size(), 0);
    assert(f.frames.size() == 2 && f.frames[0] == max && f.frames[1] == kCmd);
    assert(f.too_large == 0 && f.broken == 0 && s_live == 0);
}

static void piecesOfOneFramePutTogether() {
    RemoteFrameAsm a{t_alloc, t_release};
    Fed f;
    feed_cut(a, f, kCmd, {20});            // two pieces
    feed_cut(a, f, kCmd, {5, 31});         // three pieces
    const std::string max(8192, 'y');
    feed_cut(a, f, max, {3000, 6000});     // 8192 bytes in three pieces
    assert(f.frames.size() == 3 && f.frames[0] == kCmd && f.frames[1] == kCmd && f.frames[2] == max);
    assert(f.too_large == 0 && f.broken == 0 && s_live == 0);
}

static void tooLargeRefused() {
    RemoteFrameAsm a{t_alloc, t_release};
    Fed f;
    // 8193 bytes: refused at its first piece, once; the rest of it gives nothing.
    feed_cut(a, f, std::string(8193, 'z'), {4096, 8192});
    assert(f.frames.empty() && f.too_large == 1 && f.broken == 0);
    // A message in several frames, as the cloud's Tomcat sends one over its 8 KB buffer: fin 0, a ping between,
    // then a continuation frame (opcode 0). Refused once.
    feed(a, f, 0x01, kCmd, (int)kCmd.size(), 0, false);
    feed(a, f, 0x09, "", 0, 0);
    feed(a, f, 0x00, kCmd, (int)kCmd.size(), 0);
    assert(f.frames.empty() && f.too_large == 2 && f.broken == 0 && s_live == 0);
}

static void piecesThatDoNotFitRefused() {
    const int n = (int)kCmd.size();
    const std::string head = kCmd.substr(0, 20), tail = kCmd.substr(20);
    struct Bad { uint8_t op; std::string data; int payload_len; int offset; bool fin; };
    const Bad bad[] = {
        {0x01, tail, n, 21, true},          // a gap
        {0x01, tail, n, 19, true},          // an overlap
        {0x01, tail, n + 1, 20, true},      // another payload_len
        {0x00, tail, n, 20, true},          // opcode 0
        {0x01, tail, n, 20, false},         // fin 0
        {0x01, tail + "x", n, 20, true},    // past the frame's end
    };
    for (const Bad& b : bad) {
        RemoteFrameAsm a{t_alloc, t_release};
        Fed f;
        feed(a, f, 0x01, head, n, 0);
        feed(a, f, b.op, b.data, b.payload_len, b.offset, b.fin);
        feed(a, f, 0x01, tail, n, 20);      // the right piece, too late: nothing
        assert(f.frames.empty() && f.broken == 1 && f.too_large == 0 && s_live == 0);
    }
}

static void newFrameRestarts() {
    RemoteFrameAsm a{t_alloc, t_release};
    Fed f;
    const int n = (int)kCmd.size();
    feed(a, f, 0x01, kCmd.substr(0, 20), n, 0);     // a frame begun,
    feed_cut(a, f, kCmd, {10});                      // then a new one from its offset 0,
    feed(a, f, 0x01, kCmd.substr(20), n, 20);       // then the first one's rest: nothing
    assert(f.frames.size() == 1 && f.frames[0] == kCmd && f.broken == 1 && s_live == 0);
    // A ping (a frame of its own, so offset 0) in the middle of a frame drops it too.
    feed(a, f, 0x01, kCmd.substr(0, 20), n, 0);
    feed(a, f, 0x09, "", 0, 0);
    assert(f.frames.size() == 1 && f.broken == 2 && s_live == 0);
}

static void resetDropsPartial() {
    RemoteFrameAsm a{t_alloc, t_release};
    Fed f;
    const int n = (int)kCmd.size();
    feed(a, f, 0x01, kCmd.substr(0, 20), n, 0);
    assert(s_live == 1);
    assert(remote_frame_reset(a) && s_live == 0);   // CONNECTED, DISCONNECTED or ERROR between the pieces
    feed(a, f, 0x01, kCmd.substr(20), n, 20);
    assert(f.frames.empty() && f.broken == 0 && !remote_frame_reset(a));
}

static void lostPlaceRefusedUntilReset() {
    RemoteFrameAsm a{t_alloc, t_release};
    Fed f;
    const int n = (int)kCmd.size();
    // A stalled read inside a frame comes as a piece with no bytes. IDF v6.0's transport then reads the rest of
    // the frame as new frames, so everything after it is refused, even a whole frame at offset 0, until the reset.
    feed(a, f, 0x01, kCmd.substr(0, 20), n, 0);
    feed(a, f, 0x01, "", n, 20);                     // the stall
    assert(f.lost == 1 && f.broken == 0 && s_live == 0);
    feed(a, f, 0x01, kCmd.substr(20), n, 20);        // the same receive call goes on at that offset,
    feed(a, f, 0x01, kCmd, n, 0);                    // the next one starts at offset 0
    feed(a, f, 0x01, kCmd.substr(0, 20), n, 0);
    assert(f.frames.empty() && f.lost == 1 && f.broken == 0 && f.too_large == 0 && s_live == 0);
    assert(!remote_frame_reset(a));                  // nothing held; the reconnect clears the lost place
    feed(a, f, 0x01, kCmd, n, 0);
    assert(f.frames.size() == 1 && f.frames[0] == kCmd);
    // A stall at a frame's first read counts the same (no "dropped" warning on top).
    feed(a, f, 0x01, "", n, 0);
    assert(f.lost == 2 && f.broken == 0 && s_live == 0);
    remote_frame_reset(a);
}

static void otherFramesIgnored() {
    RemoteFrameAsm a{t_alloc, t_release};
    Fed f;
    feed(a, f, 0x01, "", 0, 0);                      // empty text
    feed(a, f, 0x02, kCmd, (int)kCmd.size(), 0);     // binary
    feed(a, f, 0x09, "", 0, 0);                      // ping
    feed(a, f, 0x00, kCmd, (int)kCmd.size(), 0);     // a continuation with no message before it
    assert(f.frames.empty() && f.too_large == 0 && f.broken == 0 && f.lost == 0 && s_live == 0);
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
    wholeFrameInOnePiece();
    piecesOfOneFramePutTogether();
    tooLargeRefused();
    piecesThatDoNotFitRefused();
    newFrameRestarts();
    resetDropsPartial();
    lostPlaceRefusedUntilReset();
    otherFramesIgnored();
    trailingBytesRefused();
    printf("remote_frame_tests: ok\n");
    return 0;
}

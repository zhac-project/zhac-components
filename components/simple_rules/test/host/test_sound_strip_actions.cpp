// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// strip.set / strip.off / sound.play / sound.stop (hub sounds): the grammar, the firmware's
// checks at save time, and what reaches the run hooks at fire time.
#include "simple_rules.h"
#include "event_bus.h"
#include "zigbee_pool.h"
#include "zcl_attribute.h"

#include <cstdio>
#include <cstring>
#include <cstdint>

extern void stub_pool_seed(const ZapDevice* dev);

static int s_failures = 0;
#define CHECK(cond, msg) do {                                         \
    if (cond) { printf("PASS: %s\n", msg); }                          \
    else      { printf("FAIL: %s\n", msg); s_failures++; }            \
} while (0)

static constexpr uint64_t kIeee = 0xAABBCCDDEEFF0042ULL;

// ── the IO hooks: pin "hall" is a strip, "relay" is not ──
static struct { int calls; SimpleRulesIoOp op; char pin[32]; char value[32]; bool result; } g_io;
static bool on_io_run(SimpleRulesIoOp op, const char* pin, const char* value) {
    g_io.calls++;
    g_io.op = op;
    std::snprintf(g_io.pin, sizeof(g_io.pin), "%s", pin);
    std::snprintf(g_io.value, sizeof(g_io.value), "%s", value);
    return g_io.result;
}
static void io_reset() { std::memset(&g_io, 0, sizeof(g_io)); g_io.result = true; }

static bool on_io_check(SimpleRulesIoOp op, const char* pin, char* err, size_t cap) {
    if (op != SimpleRulesIoOp::STRIP) return true;
    if (std::strcmp(pin, "hall") == 0) return true;
    std::snprintf(err, cap, "pin '%s' is not a strip", pin);
    return false;
}

// ── the sound hooks ──
static constexpr size_t kLongLen = 170;
static char g_long[kLongLen + 1];   // the refusal text for an unknown sound
static void make_long() {
    for (size_t i = 0; i < kLongLen; i++) g_long[i] = static_cast<char>('a' + i % 26);
    g_long[kLongLen] = '\0';
}

static struct { int calls; bool play; char name[32]; int volume; bool loop; bool result; } g_snd;
static bool on_snd_run(bool play, const char* name, int volume, bool loop) {
    g_snd.calls++;
    g_snd.play = play;
    std::snprintf(g_snd.name, sizeof(g_snd.name), "%s", name);
    g_snd.volume = volume;
    g_snd.loop = loop;
    return g_snd.result;
}
static void snd_reset() { std::memset(&g_snd, 0, sizeof(g_snd)); g_snd.result = true; }

static bool on_snd_check(const char* name, bool loop, char* err, size_t cap) {
    if (std::strcmp(name, "westminster") == 0) {
        if (!loop) return true;
        std::snprintf(err, cap, "westminster cannot loop");
        return false;
    }
    if (!*name || !std::strcmp(name, "beep") || !std::strcmp(name, "alarm")) return true;
    std::snprintf(err, cap, "%s", g_long);
    return false;
}

static void publish_attr(const char* key, int32_t v) {
    Event ev{};
    ev.type = EventType::ZCL_ATTR;
    auto& ze = *reinterpret_cast<ZclAttrEvent*>(ev.data);
    ze.ieee = kIeee;
    ze.val_type = VAL_INT;
    std::snprintf(ze.key, ATTR_KEY_MAX, "%s", key);
    ze.int_val = v;
    event_bus_publish(ev);
}
static void drain() { for (int i = 0; i < 8; i++) if (event_bus_drain(EventType::ZCL_ATTR, 0) == 0) break; }

static bool parses(const char* actions) {
    char dsl[200];
    std::snprintf(dsl, sizeof(dsl), "ON io#p DO %s ENDON", actions);
    char err[256];
    return simple_rules_check(dsl, err, sizeof(err));
}

// Refused, with exactly this reason.
static bool refused(const char* actions, const char* reason) {
    return !parses(actions) && std::strcmp(dsl_last_error(), reason) == 0;
}

static uint16_t add(const char* dsl) {
    uint16_t id = 0;
    if (!simple_rules_add("s", dsl, &id)) { printf("  (add failed for '%s': %s)\n", dsl, dsl_last_error()); return 0; }
    return id;
}

static bool last_skip_is(uint16_t id, const char* want) {
    SimpleRuleStatus st[16];
    const uint16_t n = simple_rules_status(st, 16);
    for (uint16_t i = 0; i < n; i++) if (st[i].rule_id == id) return std::strcmp(st[i].last_skip, want) == 0;
    return false;
}

int main() {
    event_bus_init();
    simple_rules_init();
    make_long();

    ZapDevice dev{};
    dev.ieee_addr = kIeee;
    std::snprintf(dev.friendly_name, sizeof(dev.friendly_name), "io");
    stub_pool_seed(&dev);

    simple_rules_set_io_hooks(on_io_check, on_io_run);
    simple_rules_set_sound_hooks(on_snd_check, on_snd_run);

    // ── grammar: accepted ──
    CHECK(parses("strip.set hall #FF0000") && parses("strip.set hall #ff8800 50") &&
          parses("strip.set hall #FF0000 0") && parses("strip.set hall #FF0000 100") && parses("strip.off hall"),
          "strip.set <pin> <#RRGGBB> [0-100], strip.off <pin>");
    CHECK(parses("sound.play westminster") && parses("sound.play beep 60 loop") &&
          parses("sound.play beep loop") && parses("sound.play beep 0"),
          "sound.play <name> [0-100] [loop]");
    CHECK(parses("sound.stop") && parses("sound.stop alarm"), "sound.stop [<name>]");
    CHECK(parses("strip.set hall #00ff00; sound.play beep 20; strip.off hall; sound.stop"), "the new verbs chain with ;");

    // ── grammar: refused, with the parser's own words ──
    CHECK(refused("strip.set hall red", "strip.set: colour must be #RRGGBB"), "strip.set: a colour name is refused");
    CHECK(refused("strip.set hall #FF00", "strip.set: colour must be #RRGGBB"), "strip.set: a short colour is refused");
    CHECK(refused("strip.set hall #FF000000", "strip.set: colour must be #RRGGBB"), "strip.set: a long colour is refused");
    CHECK(refused("strip.set hall #GG0000", "strip.set: colour must be #RRGGBB"), "strip.set: a non-hex colour is refused");
    CHECK(refused("strip.set hall", "strip.set: colour missing"), "strip.set: no colour");
    CHECK(refused("strip.set hall #FF0000 101", "strip.set: brightness must be 0-100"), "strip.set: brightness 101");
    CHECK(refused("strip.set hall #FF0000 x", "strip.set: brightness must be 0-100"), "strip.set: brightness not a number");
    CHECK(refused("strip.set hall #FF0000 -5", "strip.set: brightness must be 0-100"), "strip.set: brightness with a sign");
    CHECK(refused("strip.set hall #FF0000 5.5", "strip.set: brightness must be 0-100"), "strip.set: brightness with decimals");
    CHECK(refused("strip.set hall #FF0000 50 x", "strip.set: unexpected 'x'"), "strip.set: a trailing token");
    CHECK(refused("strip.off hall now", "strip.off takes one pin"), "strip.off: a trailing token");
    CHECK(refused("strip.set relay #FF0000", "pin 'relay' is not a strip"), "strip.set: the firmware's pin refusal, as is");
    CHECK(refused("strip.off relay", "pin 'relay' is not a strip"), "strip.off: the firmware's pin refusal, as is");
    CHECK(refused("strip.set", "strip.set: pin missing") && refused("strip.off", "strip.off: pin missing"), "no pin");

    CHECK(refused("sound.play", "sound.play: sound name missing"), "sound.play: no name");
    CHECK(refused("sound.play beep 101", "sound.play: volume must be 0-100"), "sound.play: volume 101");
    CHECK(refused("sound.play beep 6.5", "sound.play: volume must be 0-100"), "sound.play: volume with decimals");
    CHECK(refused("sound.play beep x", "sound.play: volume must be 0-100"), "sound.play: volume not a number");
    CHECK(refused("sound.play beep loop 60", "sound.play: unexpected '60'"), "sound.play: a token after loop");
    CHECK(refused("sound.play beep 60 loop x", "sound.play: unexpected 'x'"), "sound.play: a token after volume and loop");
    CHECK(refused("sound.play beep 60 x", "sound.play: unexpected 'x'"), "sound.play: a third token that is not loop");
    CHECK(refused("sound.play abcdefghijabcdefghijabcdefghijabc", "sound.play: sound name too long (max 31)"),
          "sound.play: a 33-character name is refused, not cut");
    CHECK(refused("sound.play westminster loop", "westminster cannot loop"), "sound.play: the firmware's loop refusal, as is");
    CHECK(!parses("sound.play nosuch") && std::strlen(dsl_last_error()) == kLongLen &&
          std::strcmp(dsl_last_error(), g_long) == 0, "sound.play: a 170-character refusal reaches dsl_last_error whole");
    {
        char err[256];
        CHECK(!simple_rules_check("ON io#p DO sound.play nosuch ENDON", err, sizeof(err)) && std::strcmp(err, g_long) == 0,
              "sound.play: the 170-character refusal reaches the caller's err[256] whole");
    }
    CHECK(refused("sound.stop beep x", "sound.stop takes at most one name"), "sound.stop: two names");
    CHECK(!parses("sound.stop nosuch") && std::strcmp(dsl_last_error(), g_long) == 0, "sound.stop checks the name too");

    // ── fire: what the run hooks receive ──
    io_reset();
    const uint16_t r1 = add("ON io#a DO strip.set hall #FF8800 50 ENDON");
    publish_attr("a", 1); drain();
    CHECK(r1 && g_io.calls == 1 && g_io.op == SimpleRulesIoOp::STRIP && std::strcmp(g_io.pin, "hall") == 0 &&
          std::strcmp(g_io.value, "#FF8800 50") == 0, "strip.set hall #FF8800 50 -> run(STRIP, hall, \"#FF8800 50\")");

    io_reset();
    add("ON io#b DO strip.set hall #ff8800 ENDON");
    publish_attr("b", 1); drain();
    CHECK(g_io.calls == 1 && std::strcmp(g_io.value, "#ff8800") == 0, "no brightness: the value is the colour as typed");

    io_reset();
    add("ON io#c DO strip.set hall   #ff8800     50 ENDON");
    publish_attr("c", 1); drain();
    CHECK(g_io.calls == 1 && std::strcmp(g_io.value, "#ff8800 50") == 0, "extra blanks: the tokens are joined by one space");

    io_reset();
    add("ON io#d DO strip.off hall ENDON");
    publish_attr("d", 1); drain();
    CHECK(g_io.calls == 1 && g_io.op == SimpleRulesIoOp::STRIP && std::strcmp(g_io.pin, "hall") == 0 &&
          std::strcmp(g_io.value, "#000000") == 0, "strip.off hall -> run(STRIP, hall, \"#000000\")");

    snd_reset();
    add("ON io#e DO sound.play beep 60 loop ENDON");
    publish_attr("e", 1); drain();
    CHECK(g_snd.calls == 1 && g_snd.play && std::strcmp(g_snd.name, "beep") == 0 && g_snd.volume == 60 && g_snd.loop,
          "sound.play beep 60 loop -> run(true, beep, 60, true)");

    snd_reset();
    add("ON io#f DO sound.play westminster ENDON");
    publish_attr("f", 1); drain();
    CHECK(g_snd.calls == 1 && g_snd.play && std::strcmp(g_snd.name, "westminster") == 0 && g_snd.volume == -1 && !g_snd.loop,
          "sound.play westminster -> run(true, westminster, -1, false)");

    snd_reset();
    add("ON io#g DO sound.play beep loop ENDON");
    publish_attr("g", 1); drain();
    CHECK(g_snd.calls == 1 && g_snd.volume == -1 && g_snd.loop, "sound.play beep loop -> default volume, looping");

    snd_reset();
    add("ON io#h DO sound.play beep 0 ENDON");
    publish_attr("h", 1); drain();
    CHECK(g_snd.calls == 1 && g_snd.volume == 0, "volume 0 is a volume, not the default");

    snd_reset();
    add("ON io#i DO sound.stop ENDON");
    publish_attr("i", 1); drain();
    CHECK(g_snd.calls == 1 && !g_snd.play && g_snd.name[0] == '\0' && g_snd.volume == -1 && !g_snd.loop,
          "sound.stop -> run(false, \"\", -1, false)");

    snd_reset();
    add("ON io#j DO sound.stop alarm ENDON");
    publish_attr("j", 1); drain();
    CHECK(g_snd.calls == 1 && !g_snd.play && std::strcmp(g_snd.name, "alarm") == 0 && g_snd.volume == -1 && !g_snd.loop,
          "sound.stop alarm -> run(false, alarm, -1, false)");

    // ── a refused run is an action error in the rule status ──
    snd_reset(); g_snd.result = false;
    const uint16_t r2 = add("ON io#k DO sound.play beep ENDON");
    publish_attr("k", 1); drain();
    CHECK(g_snd.calls == 1 && last_skip_is(r2, "action_error:sound.play"), "a sound run that returns false: action error");

    io_reset(); g_io.result = false;
    const uint16_t r3 = add("ON io#l DO strip.set hall #FF0000 ENDON");
    publish_attr("l", 1); drain();
    CHECK(g_io.calls == 1 && last_skip_is(r3, "action_error:strip.set"), "a strip run that returns false: action error");

    io_reset();
    const uint16_t r4 = add("ON io#m DO strip.off hall ENDON");
    g_io.result = false;
    publish_attr("m", 1); drain();
    CHECK(last_skip_is(r4, "action_error:strip.off"), "strip.off names its verb in the action error");

    snd_reset();
    const uint16_t r5 = add("ON io#n DO sound.stop ENDON");
    simple_rules_set_sound_hooks(on_snd_check, nullptr);
    publish_attr("n", 1); drain();
    CHECK(g_snd.calls == 0 && last_skip_is(r5, "action_error:sound.stop"), "no sound run hook: action error, nothing runs");
    const uint16_t r5p = add("ON io#nplay DO sound.play beep ENDON");
    publish_attr("nplay", 1); drain();
    CHECK(g_snd.calls == 0 && last_skip_is(r5p, "action_error:sound.play"), "no sound run hook: sound.play is an action error too");

    // ── run cap: a strip write is a feed-back action, a sound is not ──
    simple_rules_set_sound_hooks(on_snd_check, on_snd_run);
    io_reset();
    const uint16_t r6 = add("ON io#o DO strip.set hall #FF0000 ENDON");
    for (int v = 1; v <= 21; v++) { publish_attr("o", v); drain(); }
    CHECK(g_io.calls == 20 && last_skip_is(r6, "rate_limited"), "strip.set: 21 reports in a minute run 20 times, then rate_limited");

    snd_reset();
    const uint16_t r7 = add("ON io#q DO sound.play beep ENDON");
    for (int v = 1; v <= 21; v++) { publish_attr("q", v); drain(); }
    CHECK(g_snd.calls == 21 && !last_skip_is(r7, "rate_limited"), "sound.play: 21 reports in a minute run 21 times");

    // ── a reload skips the sound check: a sound removed later fails at fire time, the rule stays ──
    simple_rules_set_sound_hooks(nullptr, nullptr);
    snd_reset();
    const uint16_t r8 = add("ON io#r DO sound.play nosuch ENDON");
    CHECK(r8 != 0, "no sound hooks: any name is saved");
    simple_rules_set_sound_hooks(on_snd_check, on_snd_run);
    CHECK(!parses("sound.play nosuch"), "the hooks set: the same name is refused when saved");
    simple_rules_reload();
    publish_attr("r", 1); drain();
    CHECK(g_snd.calls == 1 && g_snd.play && std::strcmp(g_snd.name, "nosuch") == 0,
          "a reload keeps the rule whose sound is unknown; it fires with that name");
    CHECK(!parses("sound.play nosuch"), "after the reload the check is back for new rules");

    simple_rules_set_io_hooks(nullptr, nullptr);
    simple_rules_set_sound_hooks(nullptr, nullptr);

    if (s_failures) { printf("%d check(s) failed\n", s_failures); return 1; }
    printf("sound/strip actions: all checks passed\n");
    return 0;
}

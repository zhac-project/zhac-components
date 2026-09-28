// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Registry lookup memo (src/def_memo.hpp): hits, remembered misses, round-robin
// replacement, and the keys it refuses to store.
#include <cassert>
#include <cstdio>
#include <string>

#include "def_memo.hpp"

using zhac_def_memo::kKeyMax;
using zhac_def_memo::kSlots;
using Memo = zhac_def_memo::Memo<const int*>;

static const int kSwitch = 1, kValve = 2, kOther = 3;

static std::string key(std::size_t i) { return "m" + std::to_string(i); }

static void test_hit() {
    Memo m;
    const int* got = nullptr;
    assert(!m.get("TS0001", "_TZ3000_fdxihpp7", &got));
    m.put("TS0001", "_TZ3000_fdxihpp7", &kSwitch);
    m.put("TS0601", "_TZE204_dwcarsat", &kValve);
    assert(m.get("TS0001", "_TZ3000_fdxihpp7", &got) && got == &kSwitch);
    assert(m.get("TS0601", "_TZE204_dwcarsat", &got) && got == &kValve);
    // Same model from another manufacturer is another device.
    assert(!m.get("TS0601", "_TZE204_d7lpruvi", &got));
    // Both strings must match exactly.
    assert(!m.get("TS0001", "_TZ3000_fdxihpp", &got));
    assert(!m.get("TS000", "_TZ3000_fdxihpp7", &got));
}

static void test_miss_is_remembered_as_nullptr() {
    Memo m;
    m.put("ZZZ-unknown", "_TZ3000_zzzzzzzz", nullptr);
    const int* got = &kOther;
    assert(m.get("ZZZ-unknown", "_TZ3000_zzzzzzzz", &got));
    assert(got == nullptr);
}

static void test_full_memo_replaces_round_robin() {
    Memo m;
    for (std::size_t i = 0; i < kSlots; ++i) m.put(key(i).c_str(), "x", &kSwitch);
    const int* got = nullptr;
    for (std::size_t i = 0; i < kSlots; ++i) assert(m.get(key(i).c_str(), "x", &got));
    m.put("new", "x", &kValve);                     // replaces the oldest, m0
    assert(!m.get("m0", "x", &got));
    assert(m.get("m1", "x", &got) && got == &kSwitch);
    assert(m.get("new", "x", &got) && got == &kValve);
    m.put("newer", "x", &kValve);                   // then m1
    assert(!m.get("m1", "x", &got));
    assert(m.get("m2", "x", &got) && m.get("new", "x", &got) && m.get("newer", "x", &got));
}

static void test_repeated_put_takes_no_slot() {
    // Two tasks can miss on the same device at once and both store it.
    Memo m;
    m.put("TS0207", "_TZ3000_u9mio4qb", &kSwitch);
    m.put("TS0201", "_TZ3000_fllyghyj", &kValve);
    m.put("TS0201", "_TZ3000_fllyghyj", &kValve);
    for (std::size_t i = 2; i < kSlots; ++i) m.put(key(i).c_str(), "x", &kOther);
    const int* got = nullptr;
    assert(m.get("TS0207", "_TZ3000_u9mio4qb", &got) && got == &kSwitch);   // 64 keys, none out
}

static void test_long_keys_bypass() {
    Memo m;
    const std::string max(kKeyMax, 'A');            // 32 chars: stored
    const std::string over(kKeyMax + 1, 'A');       // 33: its first 32 chars are `max`
    m.put(max.c_str(), "x", &kSwitch);
    m.put("x", max.c_str(), &kValve);
    const int* got = nullptr;
    assert(m.get(max.c_str(), "x", &got) && got == &kSwitch);
    assert(m.get("x", max.c_str(), &got) && got == &kValve);
    // Never truncated onto a stored key...
    assert(!m.get(over.c_str(), "x", &got));
    assert(!m.get("x", over.c_str(), &got));
    // ...and never stored: such a lookup walks the registry every time.
    m.put(over.c_str(), "x", &kOther);
    m.put("x", over.c_str(), &kOther);
    assert(!m.get(over.c_str(), "x", &got));
    assert(!m.get("x", over.c_str(), &got));
    for (std::size_t i = 2; i < kSlots; ++i) m.put(key(i).c_str(), "x", &kOther);
    assert(m.get(max.c_str(), "x", &got) && got == &kSwitch);           // refused keys took no slot
}

static void test_null_args() {
    Memo m;
    m.put(nullptr, "x", &kSwitch);
    m.put("x", nullptr, &kSwitch);
    m.put(nullptr, nullptr, &kSwitch);
    const int* got = &kOther;
    assert(!m.get(nullptr, "x", &got));
    assert(!m.get("x", nullptr, &got));
    assert(!m.get(nullptr, nullptr, &got));
    assert(got == &kOther);
    // A null manufacturer is not "": zhc::find_definition treats them apart.
    m.put("x", "", &kValve);
    assert(!m.get("x", nullptr, &got));
    assert(m.get("x", "", &got) && got == &kValve);
}

int main() {
    test_hit();
    test_miss_is_remembered_as_nullptr();
    test_full_memo_replaces_round_robin();
    test_repeated_put_takes_no_slot();
    test_long_keys_bypass();
    test_null_args();
    std::printf("def_memo host tests: ok\n");
    return 0;
}

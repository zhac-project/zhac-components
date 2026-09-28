// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Memo for the adapter's registry lookup: (modelId, manufacturerName) -> def.
//
// The merged registry is built once at init and never changes, so the lookup
// is a pure function of the two strings: an entry never goes stale, and a miss
// (nullptr) is remembered like a hit. device.list, Home Assistant discovery and
// the device page resolve every device again on every call; without the memo
// each of those lookups walks all ~5,500 definitions.
//
// Keys are exact copies. A null key, or one longer than kKeyMax, is neither
// stored nor found -- never truncated, since a cut key could name another
// device -- so that lookup walks every time. Round-robin replacement once full.
// Pure C++, no ESP-IDF, so it has a host test (test/host); the adapter adds the
// lock.
#pragma once

#include <cstddef>
#include <cstring>

namespace zhac_def_memo {

// ponytail: 64 fingerprints, round-robin. A network with more distinct
// (model, manufacturer) pairs cycles through it on every device.list and gets
// no hits there; raise kSlots then (72 B of PSRAM per slot).
constexpr std::size_t kSlots  = 64;
constexpr std::size_t kKeyMax = 32;   // ZCL modelIdentifier / manufacturerName

template <typename V>
class Memo {
public:
    // True and *out set when (model, manu) is stored.
    bool get(const char* model, const char* manu, V* out) const {
        const Slot* s = find(model, manu);
        if (!s) return false;
        *out = s->value;
        return true;
    }

    // Remember a lookup result. A key already stored (two tasks missed on it
    // at once) or one that does not fit is left out.
    void put(const char* model, const char* manu, V value) {
        if (!fits(model) || !fits(manu) || find(model, manu)) return;
        Slot& s = slots_[next_];
        std::strcpy(s.model, model);
        std::strcpy(s.manu, manu);
        s.value = value;
        next_ = (next_ + 1) % kSlots;
        if (used_ < kSlots) ++used_;
    }

private:
    struct Slot {
        char model[kKeyMax + 1];
        char manu[kKeyMax + 1];
        V    value;
    };

    static bool fits(const char* s) { return s && std::strlen(s) <= kKeyMax; }

    // A longer key cannot equal a stored one, so it is never found.
    const Slot* find(const char* model, const char* manu) const {
        if (!model || !manu) return nullptr;
        for (std::size_t i = 0; i < used_; ++i) {
            const Slot& s = slots_[i];
            if (std::strcmp(s.model, model) == 0 && std::strcmp(s.manu, manu) == 0) return &s;
        }
        return nullptr;
    }

    Slot        slots_[kSlots] = {};
    std::size_t used_ = 0;   // slots written so far
    std::size_t next_ = 0;   // the slot the next put writes
};

}  // namespace zhac_def_memo

// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Generic fallback for devices that don't match any library definition.
//
// When a newly-joined device has an unknown model / manufacturer but
// exposes standard ZCL clusters (0x0006 OnOff, 0x0402 TempMeas, …)
// the interview feeds the parsed Simple_Desc_rsp cluster list here;
// `synth_definition()` lazily builds a minimal `PreparedDefinition`
// from a static cluster→expose mapping table, reusing the library's
// generic FZ/TZ converters. The caller then drives the rest of the
// pipeline (decode / configure) through the synthetic def exactly as
// with a real one.

#pragma once

#include <cstdint>
#include <cstddef>

namespace zhc { struct PreparedDefinition; }

namespace zhc_fallback {

// Called by zigbee_interview after each Simple_Desc_rsp. Subsequent
// calls for the same (ieee, endpoint) overwrite prior data.
void register_endpoint(std::uint64_t ieee,
                       std::uint8_t  endpoint,
                       std::uint16_t profile_id,
                       std::uint16_t device_id,
                       const std::uint16_t* in_clusters,  std::size_t n_in,
                       const std::uint16_t* out_clusters, std::size_t n_out);

// Drop all fallback state for this IEEE (device removal / re-pair).
void clear(std::uint64_t ieee);

// Return true if we hold cluster data for this IEEE. Used by has_def
// to flip UNMATCHED → MATCHED when a synthetic def can carry the
// device.
bool has_data(std::uint64_t ieee);

// Return a synthesized PreparedDefinition, or nullptr if no cluster
// data is registered or no mapped clusters were found. `model` /
// `manufacturer` seed `def.model` / `.vendor` so logs / UI render
// sensibly; pass the ZapDevice fields verbatim.
//
// Pointer stability / concurrency contract:
//   * The pool is internally mutex-protected; all functions here are
//     safe to call from any task.
//   * A definition is NEVER rebuilt in place. Each slot keeps an A/B
//     double buffer: a rebuild (triggered by new endpoint data, a
//     `base` change, or changed model/manufacturer labels) writes the
//     inactive half and publishes it by flipping the active index, so
//     a previously returned pointer keeps referencing fully-consistent
//     (if stale) bytes.
//   * Residual one-generation rule: a returned pointer's bytes stay
//     intact until the SECOND rebuild of the same device's slot (the
//     next rebuild writes the other half). Callers must not hold the
//     pointer across rebuild triggers longer than one resolve cycle —
//     re-resolve via synth_definition() (or the adapter's cached_def,
//     which eviction/clear invalidates) rather than stashing it.
//   * `clear()` / LRU eviction drop the slot's identity and notify
//     zhc_adapter to forget cached pointers into the slot, but leave
//     the published bytes intact for one more generation so an
//     in-flight dispatch finishes against consistent data.
//
// When `base` is non-null, the synth emits only clusters NOT already
// bound by the base definition — use this to build a supplementary
// def that augments a registry match (e.g. a DIY fork that extends a
// known device with extra clusters). When `base` is null the synth
// stands alone and covers every mapped cluster the device advertises.
const zhc::PreparedDefinition* synth_definition(std::uint64_t ieee,
                                                 const char* model,
                                                 const char* manufacturer,
                                                 const zhc::PreparedDefinition* base = nullptr);

}  // namespace zhc_fallback

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alejandro Bustos
#pragma once

#include "frames/keyframer.h"

#include <lv2/state/state.h>
#include <lv2/urid/urid.h>

#include <cstdint>

namespace reel {

// LV2 state keys (implementation plan, section 3). Permanent once a patch has
// been saved with them.
inline constexpr char kStateVersionURI[] = "https://github.com/ABustos2606/reel#version";
inline constexpr char kStateKeyframesURI[] = "https://github.com/ABustos2606/reel#keyframes";
inline constexpr int32_t kStateVersion = 1;
// Each keyframe is six atom:Ints: position, id, level 1-4, all 0-65535.
inline constexpr uint32_t kIntsPerKeyframe = 2 + frames::kNumChannels;
inline constexpr uint32_t kMaxKeyframes = frames::kMaxNumKeyframe;

struct StateKeys {
    LV2_URID version = 0;
    LV2_URID keyframes = 0;
    LV2_URID atom_int = 0;
    LV2_URID atom_vector = 0;
    bool valid() const { return version && keyframes && atom_int && atom_vector; }
    static StateKeys map(const LV2_URID_Map* map);
};

// A keyframe list in position order, as saved: what Keyframer holds, without
// its working state.
struct KeyframeList {
    uint32_t count = 0;
    frames::Keyframe keyframes[kMaxKeyframes]{};
};

enum class ParseResult { ok, bad_type, bad_size, too_many, out_of_range, unsorted };

// Checks a retrieved reel#keyframes value: an atom:Vector of atom:Int whose
// length is a multiple of six, at most 64 keyframes, every value 0-65535 and
// positions strictly increasing. On anything else `list` is left empty.
ParseResult parse_keyframes(const StateKeys& keys, const void* value, size_t size, uint32_t type, KeyframeList& list);

// Serialises `list` into `buffer` as an atom:Vector body of atom:Int; returns
// its size in bytes. The buffer holds the largest list.
inline constexpr size_t kVectorBytes = 8 + kMaxKeyframes * kIntsPerKeyframe * sizeof(int32_t);
size_t serialise_keyframes(const StateKeys& keys, const KeyframeList& list, uint8_t (&buffer)[kVectorBytes]);

// Stores reel#version and, when there are keyframes, reel#keyframes (a missing
// key means no keyframes).
LV2_State_Status save_keyframes(const StateKeys& keys, const KeyframeList& list, LV2_State_Store_Function store,
                                LV2_State_Handle handle);

// Retrieves and checks the saved list into `list`. A missing keyframes key is
// an empty list; a version newer than this build understands, or a malformed
// list, is rejected and leaves `list` empty.
LV2_State_Status restore_keyframes(const StateKeys& keys, LV2_State_Retrieve_Function retrieve, LV2_State_Handle handle,
                                   KeyframeList& list);

} // namespace reel

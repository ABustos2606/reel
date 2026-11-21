// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alejandro Bustos
#pragma once

#include "frames/keyframer.h"

#include <lv2/state/state.h>
#include <lv2/urid/urid.h>

#include <atomic>
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

// The keyframer's list as KeyframeList, and back: Clear(), one AddKeyframe
// per keyframe, then each id through mutable_keyframe (plan, section 5).
// Keyframer has no setter for its id counter, so new ids continue from the
// number of keyframes; ids only pick colours, so a repeat is harmless.
void copy_keyframes(const frames::Keyframer& keyframer, KeyframeList& list) noexcept;
void load_keyframes(frames::Keyframer& keyframer, const KeyframeList& list) noexcept;

// The list run() publishes after every change, for save (plan, section 5): a
// sequence lock with a single writer, the audio thread, which never waits.
// Readers on any thread retry while a write is under way. The words are
// atomics so that a read racing a write is well defined; a torn read is
// detected by the sequence and retried.
class PublishedList {
public:
    void publish(const frames::Keyframer& keyframer) noexcept; // audio thread only
    void read(KeyframeList& list) const noexcept;              // any thread

private:
    static constexpr uint32_t kWordsPerKeyframe = 3; // position|id, levels 1|2, levels 3|4
    std::atomic<uint32_t> sequence_{0};
    std::atomic<uint32_t> count_{0};
    std::atomic<uint32_t> words_[kMaxKeyframes * kWordsPerKeyframe]{};
};

// A restored list waiting for run() to take it (plan, section 5). restore
// puts it and returns; run() takes it at its next grid frame without waiting
// (if the slot is busy it tries again at the next one). A list put while an
// older one waits replaces it. put() and peek() are not real-time: they yield
// while the audio thread is copying the slot.
class PendingList {
public:
    void put(const KeyframeList& list) noexcept;  // restore
    // The audio thread takes the list in two steps, so that a save in between
    // never sees neither the pending nor the published list: take() copies it
    // out, the caller publishes it, release() empties the slot.
    bool take(KeyframeList& list) noexcept;
    void release() noexcept;
    bool peek(KeyframeList& list) noexcept; // save: a list restored but not yet taken

private:
    enum : uint32_t { kEmpty, kBusy, kReady, kTaking };
    std::atomic<uint32_t> state_{kEmpty};
    KeyframeList list_;
};

} // namespace reel

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alejandro Bustos
//
// Keyframe editing (implementation plan, 4.2), ported from frames/ui.cc:
// OnSwitchReleased (short presses of Add and Delete, and the very long Delete
// that is Clear all), OnPotChanged, FindNearestKeyframe and SyncWithPots.
// Applied by run() at grid frames, on the audio thread.
#pragma once

#include "frames/keyframer.h"

#include <cstdint>

namespace reel {

// What the controls say at a grid frame, in the keyframer's 16-bit units.
struct EditInputs {
    uint16_t frame = 0;                         // the Frame control alone (no CV)
    uint16_t levels[frames::kNumChannels] = {}; // the Level controls
    bool add = false;                           // rising edges since the last grid frame
    bool remove = false;
    bool clear = false;
};

class Editor {
public:
    // ui.cc's kAdcThreshold (1/1024 of full scale, the ADC event step) and
    // kKeyframeGridTolerance (1/32).
    static constexpr int32_t kThreshold = 1 << (16 - 10);
    static constexpr uint16_t kTolerance = 2048;

    // The next apply() takes the controls as its baseline: they are not edits
    // (after activate and after a restore). For `load_steps` grid frames
    // after that, Level changes are still taken as the loaded values, not
    // edits: Ingen restores state when it creates a block and sets the
    // patch's port values afterwards, one event per port, while the block
    // may already be running (Parser.cpp, parse_graph).
    void reset(uint32_t load_steps = 0) noexcept;
    // Applies one grid frame's controls; returns true when the keyframe list
    // changed (to publish it for save).
    bool apply(frames::Keyframer& keyframer, const EditInputs& in) noexcept;
    // Moves keyframe `index` to `position` (the page's commands, plan 4.8;
    // not on the hardware). It stays between its neighbours and at least
    // 1/32 from them, as Add leaves keyframes, so the order never changes;
    // a keyframe already closer to a neighbour than that (only state can
    // give one) doesn't move. Ignored while loading, as Level changes are.
    // Returns true when the list changed.
    bool move(frames::Keyframer& keyframer, uint8_t index, uint16_t position, uint16_t frame) noexcept;
    // The active keyframe, the editing target, or -1.
    int16_t active() const noexcept { return active_; }

private:
    void find_nearest(frames::Keyframer& keyframer, uint16_t frame) noexcept;
    static void sync_with_pots(frames::Keyframer& keyframer, const EditInputs& in) noexcept;

    bool baseline_ = true;
    uint32_t loading_ = 0; // grid frames left in which Level changes are not edits
    int16_t active_ = -1;
    // The last values that counted as moves (ui.cc's adc_value_).
    uint16_t frame_seen_ = 0;
    uint16_t levels_seen_[frames::kNumChannels] = {};
};

} // namespace reel

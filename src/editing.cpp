// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alejandro Bustos
#include "editing.hpp"

#include <cstdlib>

namespace reel {

void Editor::reset(uint32_t load_steps) noexcept
{
    baseline_ = true;
    loading_ = load_steps;
}

// Ui::FindNearestKeyframe: the keyframe within 1/32 of the Frame control.
void Editor::find_nearest(frames::Keyframer& keyframer, uint16_t frame) noexcept
{
    active_ = keyframer.FindNearestKeyframe(frame, kTolerance);
}

// Ui::SyncWithPots: with no keyframes, the levels are the Level controls.
void Editor::sync_with_pots(frames::Keyframer& keyframer, const EditInputs& in) noexcept
{
    if (!keyframer.num_keyframes()) {
        for (uint8_t c = 0; c < frames::kNumChannels; ++c) { keyframer.set_immediate(c, in.levels[c]); }
    }
}

bool Editor::move(frames::Keyframer& keyframer, uint8_t index, uint16_t position, uint16_t frame) noexcept
{
    const uint16_t count = keyframer.num_keyframes();
    if (loading_ || index >= count) { return false; }
    const int32_t lo = index ? keyframer.keyframe(index - 1).timestamp + kTolerance : 0;
    const int32_t hi = index + 1 < count ? keyframer.keyframe(index + 1).timestamp - kTolerance : 65535;
    if (lo > hi) { return false; }
    const int32_t to = position < lo ? lo : position > hi ? hi : position;
    frames::Keyframe* keyframe = keyframer.mutable_keyframe(index);
    if (keyframe->timestamp == to) { return false; }
    keyframe->timestamp = static_cast<uint16_t>(to);
    // The Frame control may now be on another keyframe, or none.
    find_nearest(keyframer, frame);
    return true;
}

bool Editor::apply(frames::Keyframer& keyframer, const EditInputs& in) noexcept
{
    bool changed = false;
    if (baseline_) {
        baseline_ = false;
        frame_seen_ = in.frame;
        for (uint8_t c = 0; c < frames::kNumChannels; ++c) { levels_seen_[c] = in.levels[c]; }
        find_nearest(keyframer, in.frame);
    } else {
        // Ui::OnPotChanged, kFrameAdcChannel: a move of the Frame control
        // looks the active keyframe up again.
        if (std::abs(in.frame - frame_seen_) >= kThreshold) {
            frame_seen_ = in.frame;
            find_nearest(keyframer, in.frame);
        }
        // Ui::OnPotChanged, channels 0-3: a move edits the active keyframe's
        // level, or with none the level used while there are no keyframes.
        for (uint8_t c = 0; c < frames::kNumChannels; ++c) {
            if (std::abs(in.levels[c] - levels_seen_[c]) < kThreshold) { continue; }
            levels_seen_[c] = in.levels[c];
            if (loading_) {
                // A value the host loads with, not a move.
            } else if (active_ != -1) {
                keyframer.mutable_keyframe(active_)->values[c] = in.levels[c];
                changed = true;
            } else {
                keyframer.set_immediate(c, in.levels[c]);
            }
        }
    }
    if (loading_) { --loading_; }

    // Ui::OnSwitchReleased, SWITCH_ADD_FRAME, short press: with no active
    // keyframe, store one at the Frame control with the four levels (nothing
    // when 64 are stored). The hidden five-Add gesture is not ported.
    if (in.add) {
        if (active_ == -1) {
            uint16_t values[frames::kNumChannels];
            for (uint8_t c = 0; c < frames::kNumChannels; ++c) { values[c] = in.levels[c]; }
            changed |= keyframer.AddKeyframe(in.frame, values);
        }
        find_nearest(keyframer, in.frame);
    }
    // SWITCH_DELETE_FRAME, short press: remove the active keyframe.
    if (in.remove) {
        if (active_ != -1) {
            changed |= keyframer.RemoveKeyframe(keyframer.keyframe(active_).timestamp);
        }
        find_nearest(keyframer, in.frame);
        sync_with_pots(keyframer, in);
    }
    // SWITCH_DELETE_FRAME, very long press: erase every keyframe.
    if (in.clear) {
        changed |= keyframer.num_keyframes() != 0;
        keyframer.Clear();
        find_nearest(keyframer, in.frame);
        sync_with_pots(keyframer, in);
    }
    // With no keyframes the levels follow the Level controls at every grid
    // frame, not only on moves of 1/1024 (plan, 4.2).
    sync_with_pots(keyframer, in);
    return changed;
}

} // namespace reel

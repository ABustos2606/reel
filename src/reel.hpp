// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alejandro Bustos
#pragma once

#include "editing.hpp"
#include "keyframe_state.hpp"
#include "ports.hpp"
#include "trigger.hpp"

#include "frames/keyframer.h"

#include <lv2/core/lv2.h>
#include <lv2/state/state.h>

#include <cstdint>

namespace reel {

extern const LV2_Descriptor descriptor;
extern const LV2_Descriptor lfo_descriptor;

// The keyframer is evaluated every kGridFrames frames counted from activate
// (6 kHz at 48 kHz), whatever the slicing (implementation plan, 4.1).
inline constexpr uint32_t kGridFrames = 8;
// Smoothing of the Frame control (tune) and of the Output gain.
inline constexpr double kFrameSmoothingSeconds = 0.005;
inline constexpr double kOutputSmoothingSeconds = 0.01;
// The trigger pulse (tune) and the bypass glide.
inline constexpr double kTriggerSeconds = 0.010;
inline constexpr double kBypassSeconds = 0.010;
// After a restore, Level changes are taken as the patch's loaded values for
// this long, not as edits (tune; editing.hpp, Editor::reset).
inline constexpr double kLoadSeconds = 1.0;

class Reel {
public:
    static Reel* create(double rate, const LV2_Feature* const* features) noexcept;
    Reel(const Reel&) = delete;
    Reel& operator=(const Reel&) = delete;

    void connect(uint32_t index, void* data) noexcept;
    void activate() noexcept;
    void run(uint32_t frames) noexcept;
    LV2_State_Status save(LV2_State_Store_Function store, LV2_State_Handle handle) noexcept;
    LV2_State_Status restore(LV2_State_Retrieve_Function retrieve, LV2_State_Handle handle) noexcept;

private:
    Reel(double rate, const StateKeys& keys) noexcept;
    float control(Port port, float fallback, float minimum, float maximum) const noexcept;
    float signal(Port port, uint32_t frame) const noexcept;
    void write(Port port, float value) const noexcept;
    void read_controls() noexcept;
    void evaluate(uint32_t frame) noexcept;
    int16_t nearest_index() const noexcept;

    float* ports_[kPortCount] = {};
    StateKeys keys_;
    frames::Keyframer keyframer_;
    Editor editor_;
    Trigger trigger_;

    float frame_smoothing_;
    float output_smoothing_;
    uint32_t trigger_frames_;
    uint32_t bypass_frames_;
    uint32_t load_steps_;
    // Frames since the last grid frame, 0 to kGridFrames - 1.
    uint32_t grid_ = 0;
    // Gains move linearly from `from` to `to` over the kGridFrames frames
    // that follow each grid frame.
    float gain_from_[kChannels] = {};
    float gain_to_[kChannels] = {};
    // The last gain computed for each channel, with its level and response,
    // so that an unchanged channel needs no log and power.
    uint16_t gain_level_[kChannels] = {};
    uint8_t gain_response_[kChannels] = {};
    float gain_[kChannels] = {};
    bool gain_valid_ = false;
    // Level outs while bypassed: the gains when the bypass began.
    float held_[kChannels] = {};
    float frame_ = 0;  // the smoothed Frame control
    float output_ = 1; // the smoothed Output gain
    // Bypass glide: 0 on, bypass_frames_ bypassed; gains blend toward unity.
    uint32_t bypass_ = 0;
    uint16_t effective_frame_ = 0;
    bool first_ = true;

    // Controls read once per run(), applied at its grid frames.
    EditInputs inputs_;
    bool add_ = false, remove_ = false, clear_ = false; // last values, for edges
    frames::EasingCurve easing_[kChannels] = {};
    uint8_t response_[kChannels] = {};

    // State (plan, section 5): the list save reads, and the list restore
    // hands over. `scratch_` is the audio thread's copy, kept out of run()'s
    // stack.
    PublishedList published_;
    PendingList pending_;
    KeyframeList scratch_;
};

} // namespace reel

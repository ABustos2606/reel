// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alejandro Bustos
#pragma once

#include "keyframe_state.hpp"
#include "ports.hpp"

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
    void evaluate(uint32_t frame) noexcept;

    float* ports_[kPortCount] = {};
    StateKeys keys_;
    frames::Keyframer keyframer_;

    float frame_smoothing_;
    float output_smoothing_;
    // Frames since the last grid frame, 0 to kGridFrames - 1.
    uint32_t grid_ = 0;
    // Gains move linearly from `from` to `to` over the kGridFrames frames
    // that follow each grid frame.
    float gain_from_[kChannels] = {};
    float gain_to_[kChannels] = {};
    float frame_ = 0;  // the smoothed Frame control
    float output_ = 1; // the smoothed Output gain
    uint16_t effective_frame_ = 0;
    bool first_ = true;

    // The list `save` writes. Milestone 0 has no keyframe editing, so it is
    // always empty; milestone 1 publishes the keyframer's list here under a
    // sequence lock (implementation plan, section 5).
    KeyframeList saved_;
};

} // namespace reel

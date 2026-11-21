// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alejandro Bustos
//
// The trigger output (implementation plan, 4.4), from frames.cc's
// TIM1_UP_IRQHandler: a pulse when Frame enters another interval between
// keyframes, armed only when the nearest keyframe has changed since the last
// pulse. In effect one pulse each time Frame passes a keyframe, none while it
// wobbles around one.
#pragma once

#include <cstdint>

namespace reel {

class Trigger {
public:
    // frames.cc's statics start at -2, -2 and disarmed.
    void reset(uint32_t pulse_frames) noexcept
    {
        previous_position_ = -2;
        previous_nearest_ = -2;
        armed_ = false;
        remaining_ = 0;
        length_ = pulse_frames;
    }

    // At a grid frame, after Keyframer::Evaluate: position() and
    // nearest_keyframe(), in upstream's order (position first, then nearest).
    void check(int16_t position, int16_t nearest) noexcept
    {
        if (previous_position_ != position) {
            previous_position_ = position;
            if (armed_) {
                remaining_ = length_;
                armed_ = false;
            }
        }
        if (previous_nearest_ != nearest) {
            previous_nearest_ = nearest;
            armed_ = true;
        }
    }

    // The output for one frame: 1 while a pulse lasts, else 0.
    float next() noexcept
    {
        if (!remaining_) { return 0.0f; }
        --remaining_;
        return 1.0f;
    }

private:
    int16_t previous_position_ = -2;
    int16_t previous_nearest_ = -2;
    bool armed_ = false;
    uint32_t remaining_ = 0;
    uint32_t length_ = 0;
};

} // namespace reel

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alejandro Bustos
//
// The VCA's response curve (implementation plan, 4.3): what
// Keyframer::ConvertToDacCode and the 2164 do on the hardware, computed in
// floating point without the 12-bit DAC's steps.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace reel {

// The Response control (0 to 1) as the hardware's response setting, 0-255.
inline uint8_t response_code(float response)
{
    return static_cast<uint8_t>(std::lround(std::clamp(response, 0.0f, 1.0f) * 255.0f));
}

// The blend of the exponential control voltage into the linear one, as
// lut_response_balance holds it (resources/lookup_tables.py:
// round(32767 x^1.5)) and ConvertToDacCode applies it (>> 15).
inline double response_balance(uint8_t response)
{
    return std::round(32767.0 * std::pow(response / 255.0, 1.5)) / 32768.0;
}

// The gain for a level from 0 to 1. The 2164 gives 30 dB per volt of control
// voltage over 2.5 V. Linear response: the voltage lut_vca_linear holds,
// -(2/3) log10(level), floored at 1/4096 (-72 dB); exponential: 2.5 (1 -
// level), -75 dB at 0. Level 0 is silence (spec, "Levels, floor and
// silence"); the hardware leaves -72 to -75 dB through there. A level of
// 1/65535 is silence too: Keyframer::Easing lands one code short of its
// target when it falls (a right shift of a negative product rounds down), so
// a fade to 0 ends at 1/65535 until Frame reaches the keyframe.
inline constexpr double kSilence = 1.5 / 65535.0;

inline float vca_gain(double level, double balance)
{
    if (!(level >= kSilence)) { return 0.0f; }
    level = std::min(level, 1.0);
    const double floored = std::max(level, 1.0 / 4096.0);
    // Exactly the level at linear response, without a log and a power.
    if (balance == 0.0) { return static_cast<float>(floored); }
    const double v_linear = -(2.0 / 3.0) * std::log10(floored);
    const double v_exponential = 2.5 * (1.0 - level);
    return static_cast<float>(std::pow(10.0, -1.5 * (v_linear + balance * (v_exponential - v_linear))));
}

// The gain of a 12-bit DAC code through the 2164, as the hardware's VCAs give
// it: the code is a control voltage (4095 is 2.5 V; drivers/dac.h), at -30 dB
// per volt. Reel LFO's outputs (plan, 4.5): PolyLfo's codes at linear
// response, so (value + 32768) / 65535 to the DAC's precision (0.02 dB per
// code), from 1/4096 (-72 dB) to 1.
inline float dac_gain(uint16_t code)
{
    const double volts = code * 16.0 / 65535.0 * 2.5;
    return static_cast<float>(std::pow(10.0, -30.0 * volts / 20.0));
}

} // namespace reel

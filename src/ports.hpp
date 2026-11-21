// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alejandro Bustos
#pragma once

#include <cstdint>

namespace reel {

inline constexpr char kURI[] = "https://github.com/ABustos2606/reel#reel";
inline constexpr char kLfoURI[] = "https://github.com/ABustos2606/reel#lfo";

// Permanent port ABI (implementation plan, section 3): never renumber or
// rename; new ports are appended. The kind comments are read by
// tests/ttl_check.py.
enum class Port : uint32_t {
    in_1 = 0,             // audio_in
    in_2 = 1,             // audio_in
    in_3 = 2,             // audio_in
    in_4 = 3,             // audio_in
    out_1 = 4,            // audio_out
    out_2 = 5,            // audio_out
    out_3 = 6,            // audio_out
    out_4 = 7,            // audio_out
    mix = 8,              // audio_out
    frame = 9,            // control_in
    frame_cv_amount = 10, // control_in
    ch_1 = 11,            // control_in
    ch_2 = 12,            // control_in
    ch_3 = 13,            // control_in
    ch_4 = 14,            // control_in
    add = 15,             // control_in
    delete_ = 16,         // control_in
    clear_all = 17,       // control_in
    easing_1 = 18,        // control_in
    easing_2 = 19,        // control_in
    easing_3 = 20,        // control_in
    easing_4 = 21,        // control_in
    response_1 = 22,      // control_in
    response_2 = 23,      // control_in
    response_3 = 24,      // control_in
    response_4 = 25,      // control_in
    mode = 26,            // control_in
    output = 27,          // control_in
    frame_cv = 28,        // cv_in
    level_1 = 29,         // cv_out
    level_2 = 30,         // cv_out
    level_3 = 31,         // cv_out
    level_4 = 32,         // cv_out
    trigger = 33,         // cv_out
    enabled = 34,         // control_in
    t_frame = 35,         // control_out
    t_count = 36,         // control_out
    t_active = 37,        // control_out
    t_nearest = 38,       // control_out
    t_step = 39,          // control_out
    t_keyframes = 40,     // atom_out
    count = 41
};

// Reel LFO.
enum class LfoPort : uint32_t {
    rate = 0,         // control_in
    shape = 1,        // control_in
    shape_spread = 2, // control_in
    spread = 3,       // control_in
    coupling = 4,     // control_in
    rate_cv = 5,      // cv_in
    out_1 = 6,        // cv_out
    out_2 = 7,        // cv_out
    out_3 = 8,        // cv_out
    out_4 = 9,        // cv_out
    bpm = 10,         // control_in
    sync = 11,        // control_in
    note = 12,        // control_in
    count = 13
};

inline constexpr uint32_t kPortCount = static_cast<uint32_t>(Port::count);
inline constexpr uint32_t kLfoPortCount = static_cast<uint32_t>(LfoPort::count);
inline constexpr uint32_t kChannels = 4;

inline constexpr uint32_t index(Port port) { return static_cast<uint32_t>(port); }
inline constexpr uint32_t index(LfoPort port) { return static_cast<uint32_t>(port); }
// Port n of a group of four (In 1-4, Level 1-4, ...), counting from 0.
inline constexpr Port nth(Port first, uint32_t n) { return static_cast<Port>(index(first) + n); }
inline constexpr LfoPort nth(LfoPort first, uint32_t n) { return static_cast<LfoPort>(index(first) + n); }

// Ports that carry one value per frame (audio and CV), not one per run().
inline constexpr bool is_signal(uint32_t i)
{
    return i <= index(Port::mix) || (i >= index(Port::frame_cv) && i <= index(Port::trigger));
}

inline constexpr bool is_lfo_signal(uint32_t i) { return i >= index(LfoPort::rate_cv) && i <= index(LfoPort::out_4); }

} // namespace reel

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alejandro Bustos
//
// Milestones 1 and 2 (implementation plan, 6.1): editing, easing, vca,
// trigger, sequencer, telemetry, state, slicing, bypass, instances,
// allocation and rt_timing
// (--timing). The plugin is compared with the vendored Keyframer driven
// directly.
#include "host.hpp"

#include "../src/keyframe_state.hpp"
#include "../src/vca.hpp"

#include "frames/keyframer.h"

#include <lv2/atom/atom.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <thread>

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define REEL_COUNT_ALLOCATIONS 0
#else
#define REEL_COUNT_ALLOCATIONS 1
// Counts allocations made while `counting` is set on the calling thread
// (sanitizers replace malloc themselves, so not under SAN=1 or SAN=thread).
namespace {
thread_local bool counting = false;
std::atomic<int> allocations{0};
} // namespace
extern "C" void* __libc_malloc(size_t);
extern "C" void* __libc_calloc(size_t, size_t);
extern "C" void* malloc(size_t size)
{
    if (counting) { allocations.fetch_add(1); }
    return __libc_malloc(size);
}
extern "C" void* calloc(size_t count, size_t size)
{
    if (counting) { allocations.fetch_add(1); }
    return __libc_calloc(count, size);
}
#endif

using reel::index;
using reel::KeyframeList;
using reel::nth;
using reel::Port;
using test::Instance;
using test::require;
using Levels = std::array<float, 4>;
using Outputs = std::vector<std::vector<float>>;

namespace {

constexpr double kRate = 48000;
constexpr double kPi = 3.14159265358979323846;

// A control as the plugin turns it into the keyframer's units.
uint16_t code(float x) { return static_cast<uint16_t>(std::lround(x * 65535.0f)); }

float noise(uint64_t frame, uint32_t salt)
{
    uint32_t x = static_cast<uint32_t>(frame) * 2654435761u + salt * 40503u;
    x ^= x >> 15; x *= 2246822519u; x ^= x >> 13;
    return static_cast<float>(x % 2001) / 2000.0f - 0.5f;
}

void inputs(Instance& instance, uint32_t i, uint64_t frame)
{
    for (uint32_t c = 0; c < reel::kChannels; ++c) { instance.signal(nth(Port::in_1, c), i) = noise(frame, c); }
}

void require_finite(const Outputs& outputs)
{
    for (const auto& port : outputs) {
        for (float x : port) { require(std::isfinite(x), "finite output"); }
    }
}

int telemetry(Instance& in, Port port) { return static_cast<int>(in.control(port)); }

// Momentary buttons: 1 for one period, then 0 for one.
void press(Instance& in, Port port)
{
    in.control(port) = 1;
    in.render(in.block);
    in.control(port) = 0;
    in.render(in.block);
}

// As on the hardware: move Frame (the active keyframe is looked up again),
// set the four levels, press Add.
void add_keyframe(Instance& in, float position, const Levels& levels)
{
    in.control(Port::frame) = position;
    in.render(in.block);
    for (uint32_t c = 0; c < 4; ++c) { in.control(nth(Port::ch_1, c)) = levels[c]; }
    in.render(in.block);
    press(in, Port::add);
}

reel::StateKeys keys_of(Instance& in)
{
    return {in.urids.map(reel::kStateVersionURI), in.urids.map(reel::kStateKeyframesURI),
            in.urids.map(LV2_ATOM__Int), in.urids.map(LV2_ATOM__Vector)};
}

// The keyframe list, as save gives it.
KeyframeList saved_list(Instance& in)
{
    test::State state;
    require(in.save(state) == LV2_STATE_SUCCESS, "save");
    const reel::StateKeys keys = keys_of(in);
    KeyframeList list;
    const auto found = state.values.find(keys.keyframes);
    if (found == state.values.end()) { return list; }
    require(reel::parse_keyframes(keys, found->second.data.data(), found->second.data.size(), found->second.type,
                                  list) == reel::ParseResult::ok,
            "saved list parses");
    return list;
}

bool same(const KeyframeList& a, const KeyframeList& b)
{
    if (a.count != b.count) { return false; }
    for (uint32_t k = 0; k < a.count; ++k) {
        const auto& x = a.keyframes[k];
        const auto& y = b.keyframes[k];
        if (x.timestamp != y.timestamp || x.id != y.id ||
            std::memcmp(x.values, y.values, sizeof(x.values)) != 0) {
            return false;
        }
    }
    return true;
}

// Level outs 1-4 at each of `positions`, reached through Frame CV (Frame 0,
// Frame CV amount 1, so the effective Frame is the CV itself). Each position
// holds for two grid steps; the second step's gain is read, with nothing
// left to glide.
std::vector<Levels> levels_at(Instance& in, const std::vector<float>& positions)
{
    constexpr uint32_t kHold = 16;
    in.control(Port::frame) = 0;
    in.control(Port::frame_cv_amount) = 1;
    in.render(200 * in.block); // the Frame smoother settles far below one code
    require(in.frame % kHold == 0, "levels_at starts on a grid frame");
    const uint64_t start = in.frame;
    const auto o = in.render(positions.size() * kHold, [&](Instance& x, uint32_t i, uint64_t frame) {
        const uint64_t k = std::min<uint64_t>((frame - start) / kHold, positions.size() - 1);
        x.signal(Port::frame_cv, i) = positions[k];
    });
    std::vector<Levels> result(positions.size());
    for (size_t k = 0; k < positions.size(); ++k) {
        for (uint32_t c = 0; c < 4; ++c) { result[k][c] = o[index(nth(Port::level_1, c))][k * kHold + kHold - 1]; }
    }
    return result;
}

std::vector<float> sweep(uint32_t count)
{
    std::vector<float> positions(count);
    for (uint32_t k = 0; k < count; ++k) { positions[k] = static_cast<float>(k) / (count - 1); }
    return positions;
}

// The gain a level from the vendored Keyframer gives at linear response.
float linear_gain(uint16_t level) { return reel::vca_gain(level / 65535.0, 0.0); }

// --- editing ---------------------------------------------------------------

void test_editing(const LV2_Descriptor* d)
{
    Instance in(d, kRate);
    in.render(in.block);
    require(telemetry(in, Port::t_count) == 0 && telemetry(in, Port::t_active) == -1, "starts empty");

    // Add stores one at the Frame control, not Frame plus CV, with the four
    // Level values.
    in.control(Port::frame_cv_amount) = 1;
    in.render(in.block, [](Instance& x, uint32_t i, uint64_t) { x.signal(Port::frame_cv, i) = 0.3f; });
    const Levels first = {0.1f, 0.2f, 0.3f, 0.4f};
    in.control(Port::frame) = 0.2f;
    for (uint32_t c = 0; c < 4; ++c) { in.control(nth(Port::ch_1, c)) = first[c]; }
    in.render(in.block);
    press(in, Port::add);
    KeyframeList list = saved_list(in);
    require(list.count == 1 && list.keyframes[0].timestamp == code(0.2f), "Add stores at the Frame control");
    for (uint32_t c = 0; c < 4; ++c) {
        require(list.keyframes[0].values[c] == code(first[c]), "Add stores the Level values");
    }
    require(telemetry(in, Port::t_count) == 1 && telemetry(in, Port::t_active) == 0, "the new keyframe is active");

    // Add within 1/32 of a keyframe does nothing.
    in.control(Port::frame) = 0.22f;
    in.render(in.block);
    press(in, Port::add);
    require(saved_list(in).count == 1, "Add within 1/32 does nothing");
    require(telemetry(in, Port::t_active) == 0, "still active within 1/32");

    // A Level change edits the active keyframe; under 1/1024 it does nothing.
    in.control(Port::ch_2) = 0.9f;
    in.render(in.block);
    require(saved_list(in).keyframes[0].values[1] == code(0.9f), "a Level change edits the active keyframe");
    in.control(Port::ch_2) = 0.9f + 63.0f / 65535;
    in.render(in.block);
    require(saved_list(in).keyframes[0].values[1] == code(0.9f), "a change under 1/1024 does nothing");
    in.control(Port::ch_2) = 0.9f + 70.0f / 65535;
    in.render(in.block);
    require(saved_list(in).keyframes[0].values[1] == code(0.9f + 70.0f / 65535),
            "a change of 1/1024 from the last move counts");

    // Frame CV never selects a keyframe: Frame far away, the CV bringing the
    // effective Frame onto the keyframe.
    in.control(Port::frame) = 0.9f;
    in.render(in.block);
    in.render(4 * in.block, [](Instance& x, uint32_t i, uint64_t) { x.signal(Port::frame_cv, i) = -0.7f; });
    require(telemetry(in, Port::t_active) == -1, "Frame CV selects nothing");
    require(telemetry(in, Port::t_nearest) == 0, "but the nearest keyframe follows the effective Frame");
    in.control(Port::ch_1) = 0.8f;
    in.render(in.block);
    list = saved_list(in);
    require(list.count == 1 && list.keyframes[0].values[0] == code(0.1f),
            "with no active keyframe a Level change edits no keyframe");

    // A second keyframe, then Delete removes the active one only.
    add_keyframe(in, 0.6f, {0.5f, 0.6f, 0.7f, 0.8f});
    list = saved_list(in);
    require(list.count == 2 && list.keyframes[1].timestamp == code(0.6f) &&
                list.keyframes[1].values[3] == code(0.8f) && list.keyframes[1].id == 1,
            "a second keyframe, id 1");
    in.control(Port::frame) = 0.4f;
    in.render(in.block);
    require(telemetry(in, Port::t_active) == -1, "nothing active between them");
    press(in, Port::delete_);
    require(saved_list(in).count == 2, "Delete with nothing active does nothing");
    in.control(Port::frame) = 0.2f;
    in.render(in.block);
    require(telemetry(in, Port::t_active) == 0, "the first is active again");
    press(in, Port::delete_);
    list = saved_list(in);
    require(list.count == 1 && list.keyframes[0].timestamp == code(0.6f), "Delete removes the active keyframe");
    require(telemetry(in, Port::t_active) == -1 && telemetry(in, Port::t_count) == 1, "telemetry after Delete");

    // Clear all; then the levels follow the Level controls again.
    press(in, Port::clear_all);
    require(saved_list(in).count == 0 && telemetry(in, Port::t_count) == 0, "Clear all");
    const Levels now = {0.25f, 0.5f, 0.75f, 1.0f};
    for (uint32_t c = 0; c < 4; ++c) { in.control(nth(Port::ch_1, c)) = now[c]; }
    const auto o = in.render(4 * in.block);
    for (uint32_t c = 0; c < 4; ++c) {
        require(o[index(nth(Port::level_1, c))].back() == linear_gain(code(now[c])),
                "with no keyframes the levels are the Level controls");
    }

    // A press shorter than a grid step is not lost: Add high for frames 4-6
    // of a period only (grid frames at 0 and 8), as a host slicing there runs
    // it. The ports stay connected at the start of the period.
    Instance short_press(d, kRate, 128);
    short_press.random_slices = false;
    short_press.control(Port::frame) = 0.5f;
    short_press.render(128);
    const LV2_Descriptor* desc = short_press.descriptor;
    desc->run(short_press.handle, 4);
    short_press.control(Port::add) = 1;
    desc->run(short_press.handle, 3);
    short_press.control(Port::add) = 0;
    desc->run(short_press.handle, 121);
    short_press.render(128);
    require(saved_list(short_press).count == 1, "a press shorter than a grid step adds a keyframe");
    // Held when the plugin starts: not a press.
    Instance held(d, kRate);
    held.control(Port::add) = 1;
    held.render(4 * held.block);
    require(saved_list(held).count == 0, "Add held from the start is not a press");

    // 64 is the limit: 64 restored (Add can't place them closer than 1/32),
    // then Add away from all of them stores nothing.
    Instance full(d, kRate);
    KeyframeList sixty_four;
    sixty_four.count = 64;
    for (uint16_t k = 0; k < 64; ++k) {
        sixty_four.keyframes[k] = {static_cast<uint16_t>(k * 1000), k, {1000, 2000, 3000, 4000}};
    }
    test::State state;
    full.save(state);
    const reel::StateKeys keys = keys_of(full);
    uint8_t buffer[reel::kVectorBytes];
    const size_t size = reel::serialise_keyframes(keys, sixty_four, buffer);
    state.values[keys.keyframes] = {std::vector<uint8_t>(buffer, buffer + size), keys.atom_vector,
                                    LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE};
    require(full.restore(state) == LV2_STATE_SUCCESS, "restore 64");
    full.control(Port::frame) = 1.0f; // 2535 from the last keyframe: none active
    full.render(full.block);
    require(telemetry(full, Port::t_count) == 64 && telemetry(full, Port::t_active) == -1, "64 restored");
    press(full, Port::add);
    require(same(saved_list(full), sixty_four), "Add with 64 stored does nothing");
    std::cout << "PASS editing: Add at the Frame control with the Level values, none within 1/32; Level moves of "
                 "1/1024 edit the active keyframe; Frame CV selects nothing; Delete, Clear all; short presses; "
                 "64 is the limit\n";
}

// --- easing ----------------------------------------------------------------

void test_easing(const LV2_Descriptor* d)
{
    const Levels a = {0.0f, 0.2f, 1.0f, 0.7f};
    const Levels b = {1.0f, 0.9f, 0.0f, 0.1f};
    const auto positions = sweep(1000);
    for (int curve = frames::EASING_CURVE_STEP; curve <= frames::EASING_CURVE_BOUNCE; ++curve) {
        Instance in(d, kRate);
        for (uint32_t c = 0; c < 4; ++c) { in.control(nth(Port::easing_1, c)) = static_cast<float>(curve); }
        add_keyframe(in, 0.25f, a);
        add_keyframe(in, 0.75f, b);
        const auto got = levels_at(in, positions);

        frames::Keyframer reference;
        reference.Clear();
        uint16_t va[4], vb[4];
        for (uint32_t c = 0; c < 4; ++c) {
            reference.mutable_settings(c)->easing_curve = static_cast<frames::EasingCurve>(curve);
            reference.mutable_settings(c)->response = 0;
            va[c] = code(a[c]);
            vb[c] = code(b[c]);
        }
        reference.AddKeyframe(code(0.25f), va);
        reference.AddKeyframe(code(0.75f), vb);
        for (size_t k = 0; k < positions.size(); ++k) {
            reference.Evaluate(code(positions[k]));
            for (uint32_t c = 0; c < 4; ++c) {
                require(got[k][c] == linear_gain(reference.level(c)),
                        "curve " + std::to_string(curve) + " channel " + std::to_string(c + 1) + " at " +
                            std::to_string(positions[k]) + ": " + std::to_string(got[k][c]) + " for " +
                            std::to_string(linear_gain(reference.level(c))));
            }
        }
        // Every curve stays between the two levels and, but for the step,
        // moves smoothly. Upstream's Easing jumped by half of full scale
        // where Bounce falls (tools/patches/keyframer-easing-bounds.patch).
        for (size_t k = 0; k < positions.size(); ++k) {
            for (uint32_t c = 0; c < 4; ++c) {
                const float lo = linear_gain(code(std::min(a[c], b[c])));
                const float hi = linear_gain(code(std::max(a[c], b[c])));
                require(got[k][c] >= lo && got[k][c] <= hi,
                        "curve " + std::to_string(curve) + " stays between the levels");
                if (k && curve != frames::EASING_CURVE_STEP) {
                    require(std::fabs(got[k][c] - got[k - 1][c]) < 0.03f,
                            "curve " + std::to_string(curve) + " channel " + std::to_string(c + 1) +
                                " moves smoothly at " + std::to_string(positions[k]));
                }
            }
        }
        // Held before the first and after the last keyframe.
        const auto ends = levels_at(in, {0.0f, 0.1f, 0.25f, 0.76f, 0.9f, 1.0f});
        for (uint32_t c = 0; c < 4; ++c) {
            for (int k : {0, 1, 2}) { require(ends[k][c] == linear_gain(code(a[c])), "held before the first"); }
            for (int k : {3, 4, 5}) { require(ends[k][c] == linear_gain(code(b[c])), "held after the last"); }
        }
        if (curve == frames::EASING_CURVE_STEP) {
            // Upstream's step lands (to - from) / 32768 short of the right
            // keyframe's level (Easing scales by 65535 >> 1, then >> 15).
            const auto step = levels_at(in, {0.30f, 0.49f, 0.51f, 0.70f});
            const auto near_b = [&](float x, uint32_t c) { return std::fabs(x - linear_gain(code(b[c]))) <= 3.0f / 65535; };
            for (uint32_t c = 0; c < 4; ++c) {
                require(step[0][c] == linear_gain(code(a[c])) && step[1][c] == linear_gain(code(a[c])) &&
                            near_b(step[2][c], c) && near_b(step[3][c], c),
                        "step jumps at the midpoint: channel " + std::to_string(c + 1) + ": " +
                            std::to_string(step[0][c]) + " " + std::to_string(step[1][c]) + " " +
                            std::to_string(step[2][c]) + " " + std::to_string(step[3][c]));
            }
        }
    }
    std::cout << "PASS easing: the six curves equal Keyframer::Evaluate at 1000 positions, stay between the levels "
                 "and move smoothly (Bounce too); step at the midpoint; "
                 "held outside the keyframes\n";
}

// --- vca -------------------------------------------------------------------

// The hardware's gain: ConvertToDacCode's 12-bit code as a control voltage
// (65535 is 2.5 V), through the 2164 (-30 dB per volt).
double hardware_db(uint16_t level, uint8_t response)
{
    const uint16_t dac = frames::Keyframer::ConvertToDacCode(level, response);
    const double volts = dac * 16.0 / 65535.0 * 2.5;
    return -30.0 * volts;
}

void test_vca(const LV2_Descriptor* d)
{
    const auto gain_at = [&](float level, float response) {
        Instance in(d, kRate);
        in.control(Port::ch_1) = level;
        in.control(Port::response_1) = response;
        return in.render(2 * in.block)[index(Port::level_1)].back();
    };
    double worst = 0, worst_low = 0;
    for (int k = 0; k <= 64; ++k) {
        const float x = k / 64.0f;
        const uint16_t level = code(x);
        // Response 0: the gain is the level.
        const float linear = gain_at(x, 0.0f);
        require(k == 0 ? linear == 0.0f : std::fabs(linear - level / 65535.0f) <= 1e-7f,
                "response 0: gain = level at " + std::to_string(x));
        // Response 1: -75 dB per unit of level below 1.
        const float exponential = gain_at(x, 1.0f);
        if (k > 0) {
            const double db = 20 * std::log10(exponential);
            require(std::fabs(db - (-75.0 * (1.0 - level / 65535.0))) < 0.01,
                    "response 1: -75 dB per unit, at " + std::to_string(x) + ": " + std::to_string(db));
        } else {
            require(exponential == 0.0f, "level 0 is silent at response 1");
        }
        // Three middle responses against the hardware's code path.
        for (float response : {0.25f, 0.5f, 0.75f}) {
            const float gain = gain_at(x, response);
            if (k == 0) {
                require(gain == 0.0f, "level 0 is silent");
                continue;
            }
            const double error = std::fabs(20 * std::log10(gain) - hardware_db(level, reel::response_code(response)));
            if (k >= 4) {
                worst = std::max(worst, error);
            } else {
                worst_low = std::max(worst_low, error);
            }
        }
    }
    require(worst <= 0.1, "middle responses within 0.1 dB of the hardware from level 1/16: " + std::to_string(worst));
    for (float response : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
        require(gain_at(1.0f, response) == 1.0f, "level 1 is unity at every response");
        require(gain_at(0.0f, response) == 0.0f, "level 0 is silent at every response");
    }
    // Out is In times the gain.
    Instance in(d, kRate);
    in.control(Port::ch_3) = 0.5f;
    in.control(Port::response_3) = 0.5f;
    const auto o = in.render(1024, inputs);
    for (uint32_t i = 512; i < 1024; ++i) {
        require(o[index(Port::out_3)][i] == noise(i, 2) * o[index(Port::level_3)][i], "Out 3 = In 3 times the gain");
    }
    std::cout << "PASS vca: response 0 linear, 1 at -75 dB per unit; responses 0.25, 0.5, 0.75 within " << worst
              << " dB of the hardware's code path from level 1/16 (" << worst_low
              << " dB below, where its table is coarse); level 0 silent, 1 unity\n";
}

// --- trigger ---------------------------------------------------------------

struct Pulses {
    int count = 0;
    bool lengths_ok = true;
};

Pulses pulses(const std::vector<float>& trigger, uint32_t length)
{
    Pulses p;
    uint32_t run = 0;
    for (size_t i = 0; i <= trigger.size(); ++i) {
        const float x = i < trigger.size() ? trigger[i] : 0.0f;
        require(x == 0.0f || x == 1.0f, "trigger is 0 or 1");
        if (x == 1.0f) {
            ++run;
        } else if (run) {
            ++p.count;
            p.lengths_ok &= run == length;
            run = 0;
        }
    }
    return p;
}

// Frame through Frame CV, as a function of time in seconds.
Pulses frame_path(Instance& in, double seconds, const std::function<float(double)>& path)
{
    const uint64_t start = in.frame;
    const auto o = in.render(static_cast<uint64_t>(seconds * kRate), [&](Instance& x, uint32_t i, uint64_t frame) {
        x.signal(Port::frame_cv, i) = path((frame - start) / kRate);
    });
    return pulses(o[index(Port::trigger)], static_cast<uint32_t>(std::lround(0.010 * kRate)));
}

void test_trigger(const LV2_Descriptor* d)
{
    Instance in(d, kRate);
    add_keyframe(in, 0.25f, {1, 0, 0, 0});
    add_keyframe(in, 0.5f, {0, 1, 0, 0});
    add_keyframe(in, 0.75f, {0, 0, 1, 0});
    in.control(Port::frame) = 0;
    in.control(Port::frame_cv_amount) = 1;
    frame_path(in, 0.5, [](double) { return 0.0f; });
    // t_nearest: the keyframe nearest the effective Frame, counted from 0.
    for (const auto& [position, nearest] : std::vector<std::pair<float, int>>{
             {0.0f, 0}, {0.3f, 0}, {0.37f, 0}, {0.38f, 1}, {0.5f, 1}, {0.7f, 2}, {1.0f, 2}}) {
        frame_path(in, 0.01, [position = position](double) { return position; });
        require(telemetry(in, Port::t_nearest) == nearest, "t_nearest at " + std::to_string(position) + ": " +
                                                               std::to_string(telemetry(in, Port::t_nearest)));
    }
    frame_path(in, 0.1, [](double) { return 0.0f; });

    const Pulses up = frame_path(in, 2.0, [](double t) { return static_cast<float>(t / 2.0); });
    require(up.count == 3 && up.lengths_ok, "one 10 ms pulse per keyframe passed going up: " +
                                                std::to_string(up.count));
    const Pulses down = frame_path(in, 2.0, [](double t) { return static_cast<float>(1.0 - t / 2.0); });
    require(down.count == 3 && down.lengths_ok, "and going down: " + std::to_string(down.count));
    // Wobbling inside one interval (across its midpoint): none, once there.
    frame_path(in, 0.2, [](double) { return 0.375f; });
    const Pulses inside = frame_path(in, 2.0, [](double t) {
        return static_cast<float>(0.375 + 0.07 * std::sin(2 * kPi * 3 * t));
    });
    require(inside.count == 0, "none while Frame wobbles inside an interval: " + std::to_string(inside.count));
    // Wobbling across a keyframe: one, when it is first crossed.
    const Pulses across = frame_path(in, 2.0, [](double t) {
        return static_cast<float>(0.5 + 0.02 * std::sin(2 * kPi * 3 * t));
    });
    require(across.count == 1, "one while Frame wobbles around a keyframe: " + std::to_string(across.count));
    // Bypassed: silent.
    in.control(Port::enabled) = 0;
    const Pulses bypassed = frame_path(in, 2.0, [](double t) { return static_cast<float>(t / 2.0); });
    require(bypassed.count == 0, "no pulse while bypassed");
    in.control(Port::enabled) = 1;

    press(in, Port::clear_all);
    frame_path(in, 0.1, [](double) { return 0.0f; });
    const Pulses none = frame_path(in, 2.0, [](double t) { return static_cast<float>(std::fmod(t, 1.0)); });
    require(none.count == 0, "none with no keyframes");
    std::cout << "PASS trigger: t_nearest counts from 0; one 10 ms pulse per keyframe passed, each way; none inside an "
                 "interval; one around a "
                 "keyframe; none bypassed or with no keyframes\n";
}

void build(Instance& in);

// --- sequencer -------------------------------------------------------------

// Frame CV held at `cv` for `seconds`; the pulses on the trigger, and the
// Level outs at the end.
struct Held {
    Pulses pulses;
    Levels levels;
};

Held hold(Instance& in, float cv, double seconds = 0.05)
{
    const auto o = in.render(static_cast<uint64_t>(seconds * kRate),
                             [cv](Instance& x, uint32_t i, uint64_t) { x.signal(Port::frame_cv, i) = cv; });
    Held held{pulses(o[index(Port::trigger)], static_cast<uint32_t>(std::lround(0.01 * kRate))), {}};
    for (uint32_t c = 0; c < 4; ++c) { held.levels[c] = o[index(nth(Port::level_1, c))].back(); }
    return held;
}

void test_sequencer(const LV2_Descriptor* d)
{
    const std::array<std::pair<float, Levels>, 3> keyframes{{{0.2f, {0.9f, 0.1f, 0.5f, 0.3f}},
                                                             {0.5f, {0.2f, 0.8f, 0.4f, 0.6f}},
                                                             {0.8f, {0.6f, 0.3f, 0.7f, 0.0f}}}};
    Instance in(d, kRate);
    for (const auto& [position, levels] : keyframes) { add_keyframe(in, position, levels); }
    // Frame between keyframes (none active, so nothing is edited) and Frame
    // CV amount odd: the sequencer ignores both.
    in.control(Port::frame) = 0.65f;
    in.control(Port::frame_cv_amount) = -0.3f;
    in.control(Port::mode) = 1;
    const auto at = [&](const Held& held, int step, const char* what) {
        require(telemetry(in, Port::t_step) == step,
                std::string(what) + ": step " + std::to_string(telemetry(in, Port::t_step)));
        require(in.control(Port::t_frame) == code(keyframes[step].first) / 65535.0f, std::string(what) + ": t_frame");
        for (uint32_t c = 0; c < 4; ++c) {
            require(held.levels[c] == linear_gain(code(keyframes[step].second[c])),
                    std::string(what) + ": the keyframe's levels, exactly");
        }
    };
    at(hold(in, 0.0f), 0, "Sequencer on, Frame CV low");
    // Each rise above 2/3 after a fall below 1/3 steps once, with one pulse.
    Held h = hold(in, 1.0f);
    at(h, 1, "first rise");
    require(h.pulses.count == 1 && h.pulses.lengths_ok, "a step fires the trigger");
    // Not below 1/3 in between: no step.
    h = hold(in, 0.5f);
    require(h.pulses.count == 0, "a fall to 1/2 doesn't step");
    h = hold(in, 0.9f);
    at(h, 1, "a rise without a fall below 1/3");
    require(h.pulses.count == 0, "and fires nothing");
    hold(in, 0.3f);
    at(hold(in, 0.7f), 2, "1/3 and 2/3 are the thresholds");
    hold(in, -1.0f);
    h = hold(in, 1.0f);
    at(h, 0, "the step wraps after the last keyframe");
    require(h.pulses.count == 1, "the wrap fires the trigger");
    hold(in, 0.0f);
    hold(in, 1.0f);
    // Mode off: Frame and Frame CV again; the step is kept.
    in.control(Port::mode) = 0;
    hold(in, 0.0f, 0.2);
    require(telemetry(in, Port::t_step) == 1 && std::abs(in.control(Port::t_frame) - 0.65f) < 1e-4f,
            "Keyframes mode: Frame again, the step kept");
    hold(in, 1.0f);
    in.control(Port::mode) = 1;
    at(hold(in, 1.0f), 1, "Sequencer again resumes at the kept step");
    // No keyframes: the levels follow the Level controls (immediate).
    press(in, Port::clear_all);
    in.control(Port::ch_1) = 0.3f;
    h = hold(in, 0.0f);
    h = hold(in, 1.0f);
    require(telemetry(in, Port::t_step) == 0 && h.levels[0] == linear_gain(code(0.3f)),
            "no keyframes: the Level controls");
    std::cout << "PASS sequencer: each Frame CV rise above 2/3 after a fall below 1/3 steps once, with a pulse, and "
                 "wraps; Frame and Frame CV amount ignored; levels exact; the step kept across modes; no keyframes "
                 "follow the Level controls\n";
}

// --- telemetry -------------------------------------------------------------

// t_keyframes' text (src/telemetry.hpp) as a list.
KeyframeList decode(const std::string& text)
{
    require(text.rfind("1:", 0) == 0 && (text.size() - 2) % 24 == 0, "list format: " + text.substr(0, 40));
    KeyframeList list;
    list.count = static_cast<uint32_t>((text.size() - 2) / 24);
    require(list.count <= 64, "at most 64");
    for (uint32_t k = 0; k < list.count; ++k) {
        uint16_t words[6];
        for (uint32_t w = 0; w < 6; ++w) {
            const std::string digits = text.substr(2 + k * 24 + w * 4, 4);
            require(digits.find_first_not_of("0123456789abcdef") == std::string::npos, "lowercase hex");
            words[w] = static_cast<uint16_t>(std::stoul(digits, nullptr, 16));
        }
        list.keyframes[k] = {words[0], words[1], {words[2], words[3], words[4], words[5]}};
    }
    return list;
}

void test_telemetry(const LV2_Descriptor* d)
{
    // The empty list at the first run().
    Instance in(d, kRate);
    in.render(in.block);
    require(in.lists.size() == 1 && in.lists[0].text == "1:", "the empty list at the first run()");

    // A change goes out within 40 ms (plus the period that sees it).
    build(in);
    in.lists.clear();
    in.control(Port::frame) = 0.3f;
    in.render(in.block);
    add_keyframe(in, 0.3f, {0.1f, 0.2f, 0.3f, 0.4f});
    const uint64_t added = in.frame;
    in.render(static_cast<uint64_t>(0.1 * kRate));
    const auto fresh = std::find_if(in.lists.begin(), in.lists.end(),
                                    [](const auto& l) { return decode(l.text).count == 4; });
    require(fresh != in.lists.end() && fresh->frame <= added + 0.04 * kRate, "a change goes out within 40 ms");
    require(same(decode(in.lists.back().text), saved_list(in)), "the list is the saved one: positions, ids, levels");

    // Unchanged: again every 0.5 s.
    in.lists.clear();
    in.render(static_cast<uint64_t>(3 * kRate));
    require(in.lists.size() >= 5 && in.lists.size() <= 7, "every 0.5 s: " + std::to_string(in.lists.size()));
    for (size_t k = 1; k < in.lists.size(); ++k) {
        const uint64_t gap = in.lists[k].frame - in.lists[k - 1].frame;
        require(gap >= 0.5 * kRate - in.block && gap <= 0.5 * kRate + in.block, "0.5 s apart");
    }

    // A Level dragged on the active keyframe: at most 25 lists a second, the
    // last one right.
    in.lists.clear();
    in.render(static_cast<uint64_t>(2 * kRate), [](Instance& x, uint32_t, uint64_t frame) {
        x.control(Port::ch_2) = static_cast<float>(0.5 + 0.4 * std::sin(frame * 0.0003));
    });
    require(in.lists.size() >= 40 && in.lists.size() <= 51, "25 a second: " + std::to_string(in.lists.size()));
    in.render(static_cast<uint64_t>(0.1 * kRate));
    require(same(decode(in.lists.back().text), saved_list(in)), "the last list after a drag");

    // jalv prepares the buffer before every run(), Ingen once per period
    // (above, with random slices): neither loses a list.
    Instance jalv(d, kRate);
    jalv.atom_per_slice = true;
    build(jalv);
    jalv.render(static_cast<uint64_t>(0.1 * kRate));
    require(same(decode(jalv.lists.back().text), saved_list(jalv)), "a buffer prepared per run()");

    // 64 keyframes: 1538 characters.
    KeyframeList sixty_four;
    sixty_four.count = 64;
    for (uint16_t k = 0; k < 64; ++k) {
        sixty_four.keyframes[k] = {static_cast<uint16_t>(k * 1000 + 7), static_cast<uint16_t>(k * 3),
                                   {static_cast<uint16_t>(k), 65535, 0, static_cast<uint16_t>(40000 + k)}};
    }
    Instance full(d, kRate);
    test::State state;
    full.save(state);
    const reel::StateKeys keys = keys_of(full);
    uint8_t buffer[reel::kVectorBytes];
    const size_t size = reel::serialise_keyframes(keys, sixty_four, buffer);
    state.values[keys.keyframes] = {std::vector<uint8_t>(buffer, buffer + size), keys.atom_vector,
                                    LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE};
    require(full.restore(state) == LV2_STATE_SUCCESS, "restore 64");
    full.render(static_cast<uint64_t>(0.1 * kRate));
    require(full.lists.back().text.size() == 2 + 64 * 24 && same(decode(full.lists.back().text), sixty_four),
            "64 keyframes");

    // Without urid:map nothing is written.
    Instance bare(d, kRate, 128, false);
    build(bare);
    bare.render(static_cast<uint64_t>(kRate));
    require(bare.lists.empty(), "no urid:map, no list");
    std::cout << "PASS telemetry: t_keyframes sends the list (\"1:\", 24 hex digits a keyframe) at the first run(), "
                 "within 40 ms of a change, at most 25 times a second, every 0.5 s unchanged; buffers prepared per "
                 "period or per run(); 64 keyframes; nothing without urid:map\n";
}

// --- state -----------------------------------------------------------------

void build(Instance& in)
{
    add_keyframe(in, 0.1f, {0.0f, 1.0f, 0.5f, 0.25f});
    add_keyframe(in, 0.45f, {1.0f, 0.0f, 0.3f, 0.6f});
    add_keyframe(in, 0.8f, {0.4f, 0.7f, 1.0f, 0.0f});
    // Delete the middle one and add it back: ids 0, 3, 2.
    in.control(Port::frame) = 0.45f;
    in.render(in.block);
    press(in, Port::delete_);
    add_keyframe(in, 0.5f, {0.9f, 0.1f, 0.2f, 0.8f});
    in.control(Port::easing_2) = frames::EASING_CURVE_SINE;
    in.control(Port::easing_3) = frames::EASING_CURVE_STEP;
    in.control(Port::response_4) = 0.6f;
}

void settings_like(Instance& in)
{
    in.control(Port::easing_2) = frames::EASING_CURVE_SINE;
    in.control(Port::easing_3) = frames::EASING_CURVE_STEP;
    in.control(Port::response_4) = 0.6f;
}

int state_saves = 0;

void test_state(const LV2_Descriptor* d)
{
    Instance a(d, kRate);
    build(a);
    const KeyframeList original = saved_list(a);
    require(original.count == 3 && original.keyframes[0].id == 0 && original.keyframes[1].id == 3 &&
                original.keyframes[2].id == 2,
            "ids 0, 3, 2 after a delete");
    test::State state;
    require(a.save(state) == LV2_STATE_SUCCESS, "save");

    // A fresh instance with other Level values, Frame on a keyframe: the
    // restored list, untouched by those values (loaded values are not edits).
    Instance b(d, kRate);
    settings_like(b);
    for (uint32_t c = 0; c < 4; ++c) { b.control(nth(Port::ch_1, c)) = 0.33f; }
    b.control(Port::frame) = 0.5f;
    b.render(b.block);
    require(b.restore(state) == LV2_STATE_SUCCESS, "restore");
    require(same(saved_list(b), original), "save before run() takes the restored list gives it");
    b.render(4 * b.block);
    require(same(saved_list(b), original), "restored list, same ids, not edited by the Level controls");
    require(telemetry(b, Port::t_count) == 3 && telemetry(b, Port::t_active) == 1, "restored telemetry");
    const auto positions = sweep(1000);
    require(levels_at(a, positions) == levels_at(b, positions), "identical levels at 1000 positions");
    // A Level move a second after the restore is an edit again.
    b.control(Port::frame) = 0.5f;
    b.render(static_cast<uint64_t>(kRate));
    b.control(Port::ch_1) = 0.6f;
    b.render(b.block);
    require(saved_list(b).keyframes[1].values[0] == code(0.6f), "a move after a restore edits");

    // Ingen's order: the state is restored when the block is created, the
    // block runs with default controls, then the patch's port values arrive
    // one by one: Frame onto a keyframe, Levels away from its values. They
    // are not edits.
    {
        Instance g(d, kRate);
        g.restore(state);
        g.render(4 * g.block);
        g.control(Port::frame) = 0.5f;
        g.render(g.block);
        for (uint32_t c = 0; c < 4; ++c) {
            g.control(nth(Port::ch_1, c)) = 0.3f;
            g.render(g.block);
        }
        g.render(static_cast<uint64_t>(0.9 * kRate));
        require(telemetry(g, Port::t_active) == 1, "Frame on a keyframe after loading");
        require(same(saved_list(g), original), "port values loaded after a restore are not edits");
        g.render(static_cast<uint64_t>(0.1 * kRate));
        g.control(Port::ch_1) = 0.35f;
        g.render(g.block);
        require(saved_list(g).keyframes[1].values[0] == code(0.35f), "a move a second after loading edits");
    }

    // New ids continue from the number of keyframes.
    Instance c(d, kRate);
    c.restore(state);
    add_keyframe(c, 0.95f, {1, 1, 1, 1});
    const KeyframeList with_new = saved_list(c);
    require(with_new.count == 4 && with_new.keyframes[3].id == 3, "new ids continue from the count");

    // activate keeps the keyframes.
    b.activate();
    b.render(b.block);
    require(saved_list(b).count == 3 && telemetry(b, Port::t_count) == 3, "activate keeps the keyframes");

    // Malformed lists and missing keys leave the instance empty.
    const reel::StateKeys keys = keys_of(a);
    const auto vector = [&](std::vector<int32_t> ints) {
        test::State s = state;
        LV2_Atom_Vector_Body body{4, keys.atom_int};
        std::vector<uint8_t> data(sizeof(body) + ints.size() * 4);
        std::memcpy(data.data(), &body, sizeof(body));
        if (!ints.empty()) { std::memcpy(data.data() + sizeof(body), ints.data(), ints.size() * 4); }
        s.values[keys.keyframes] = {data, keys.atom_vector, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE};
        return s;
    };
    std::vector<int32_t> sixty_five;
    for (int32_t k = 0; k < 65; ++k) { sixty_five.insert(sixty_five.end(), {k * 1000, k, 0, 0, 0, 0}); }
    const std::pair<test::State, const char*> rejected[] = {
        {vector({100, 0, 1, 2, 3}), "a length not a multiple of six"},
        {vector(sixty_five), "65 keyframes"},
        {vector({200, 0, 1, 2, 3, 4, 100, 1, 1, 2, 3, 4}), "unsorted positions"},
    };
    for (const auto& [bad, what] : rejected) {
        Instance e(d, kRate);
        e.restore(state);
        e.render(e.block);
        require(e.restore(bad) == LV2_STATE_ERR_BAD_TYPE, std::string("rejects ") + what);
        e.render(e.block);
        require(telemetry(e, Port::t_count) == 0 && saved_list(e).count == 0,
                std::string("a rejected list leaves no keyframes: ") + what);
    }
    {
        Instance e(d, kRate);
        e.restore(state);
        e.render(e.block);
        test::State only_version = state;
        only_version.values.erase(keys.keyframes);
        require(e.restore(only_version) == LV2_STATE_SUCCESS, "restore without the keyframes key");
        e.render(e.block);
        require(telemetry(e, Port::t_count) == 0, "a missing key means no keyframes");
        test::State saved;
        e.save(saved);
        require(!saved.values.count(keys.keyframes), "and none are saved");
    }

    // Restore while running: another thread restores the same state again and
    // again while run() goes on; the list ends up as saved.
    {
        Instance e(d, kRate);
        std::atomic<bool> done{false};
        std::thread restorer([&] {
            while (!done.load()) { e.restore(state); }
        });
        e.render(static_cast<uint64_t>(kRate), [](Instance& x, uint32_t i, uint64_t frame) {
            x.signal(Port::frame_cv, i) = static_cast<float>(0.5 + 0.5 * std::sin(frame * 0.0003));
        });
        done = true;
        restorer.join();
        e.render(e.block);
        require(same(saved_list(e), original), "restore while running");
    }

    // Thousands of saves on one thread while another edits: every save is a
    // whole list (the sequence lock), sorted, with the right count.
    {
        Instance e(d, kRate);
        e.restore(state);
        e.render(e.block);
        std::atomic<bool> done{false};
        std::atomic<int> saves{0};
        std::string failure;
        std::thread saver([&] {
            test::State s;
            KeyframeList list;
            while (!done.load()) {
                if (e.save(s) != LV2_STATE_SUCCESS) { failure = "save failed"; }
                const auto found = s.values.find(keys.keyframes);
                if (found == s.values.end()) {
                    failure = "a save lost the list";
                } else if (reel::parse_keyframes(keys, found->second.data.data(), found->second.data.size(),
                                                 found->second.type, list) != reel::ParseResult::ok) {
                    failure = "a torn save";
                } else if (list.count < 3 || list.count > 4) {
                    failure = "a save with " + std::to_string(list.count) + " keyframes";
                }
                saves.fetch_add(1);
            }
        });
        // Edits: the middle keyframe's levels move all the time; a fourth
        // keyframe comes and goes.
        e.render(static_cast<uint64_t>(2 * kRate), [](Instance& x, uint32_t i, uint64_t frame) {
            if (i != 0) { return; }
            const uint64_t n = frame / x.block;
            x.control(Port::frame) = (n / 40) % 2 ? 0.95f : 0.5f;
            for (uint32_t c = 0; c < 4; ++c) {
                x.control(nth(Port::ch_1, c)) = static_cast<float>(0.5 + 0.5 * std::sin(n * 0.37 + c));
            }
            x.control(Port::add) = n % 80 == 60 ? 1.0f : 0.0f;
            x.control(Port::delete_) = n % 80 == 70 ? 1.0f : 0.0f;
        });
        done = true;
        saver.join();
        require(failure.empty(), failure);
        require(saves.load() > 100, "saves while editing: " + std::to_string(saves.load()));
        state_saves = saves.load();
    }
    std::cout << "PASS state: save and restore into a fresh instance (identical levels at 1000 positions, same ids, "
                 "Level values not edits, nor Level values set within a second as Ingen loads them); activate keeps "
                 "them; malformed lists and missing keys give none; restore "
                 "while running; "
              << state_saves << " saves while editing, each a whole list\n";
}

// --- slicing ---------------------------------------------------------------

// Controls change only where a period starts for both 128- and 37-frame
// periods, so every host sees them change at the same frames.
void performance(Instance& in, uint32_t i, uint64_t frame)
{
    inputs(in, i, frame);
    // Frame CV crosses both of the sequencer's thresholds.
    in.signal(Port::frame_cv, i) = static_cast<float>(0.8 * std::sin(frame * 0.0007));
    constexpr uint64_t kChange = 128 * 37;
    if (i == 0 && frame % kChange == 0) {
        const uint64_t n = frame / kChange;
        in.control(Port::frame) = static_cast<float>((n * 7) % 11) / 10;
        in.control(Port::ch_1) = static_cast<float>(0.5 + 0.5 * std::sin(n * 0.7));
        in.control(Port::ch_2) = n % 2 ? 0.2f : 0.9f;
        in.control(Port::ch_4) = static_cast<float>(n % 5) / 4;
        in.control(Port::add) = n % 3 == 1 ? 1.0f : 0.0f;
        in.control(Port::delete_) = n % 7 == 5 ? 1.0f : 0.0f;
        in.control(Port::clear_all) = n % 23 == 22 ? 1.0f : 0.0f;
        in.control(Port::easing_1) = static_cast<float>(n % 6);
        in.control(Port::response_2) = static_cast<float>(n % 4) / 3;
        in.control(Port::output) = n % 3 ? -12.0f : 3.0f;
        in.control(Port::enabled) = n % 13 == 12 ? 0.0f : 1.0f;
        in.control(Port::mode) = n % 11 >= 8 ? 1.0f : 0.0f;
    }
}

void test_slicing(const LV2_Descriptor* d)
{
    for (double rate : {48000.0, 44100.0}) {
        Instance whole(d, rate, 128), sliced(d, rate, 128), small(d, rate, 37);
        whole.random_slices = false;
        const uint64_t frames = static_cast<uint64_t>(4 * rate);
        const auto a = whole.render(frames, performance);
        require_finite(a);
        require(saved_list(whole).count > 0, "the performance stores keyframes");
        require(pulses(a[index(Port::trigger)], static_cast<uint32_t>(std::lround(0.01 * rate))).count > 0,
                "the performance fires the trigger");
        require(a == sliced.render(frames, performance), "random slices change nothing");
        require(a == small.render(frames, performance), "a 37-frame period changes nothing");
        require(same(saved_list(whole), saved_list(sliced)) && same(saved_list(whole), saved_list(small)),
                "the same keyframes");
    }
    std::cout << "PASS slicing: whole periods, random slices and 37-frame periods agree with edits, presses, easing, "
                 "response, bypass and Frame CV moving\n";
}

// --- bypass ----------------------------------------------------------------

void test_bypass(const LV2_Descriptor* d)
{
    const uint32_t glide = static_cast<uint32_t>(std::lround(0.010 * kRate));
    const auto setup = [](Instance& in) {
        add_keyframe(in, 0.2f, {0.2f, 0.4f, 0.0f, 1.0f});
        add_keyframe(in, 0.8f, {0.9f, 0.1f, 0.5f, 0.3f});
        in.control(Port::frame) = 0.5f;
        in.control(Port::output) = -6;
        in.control(Port::response_2) = 0.5f;
        in.render(80 * in.block); // Output's 10 ms smoother settles
    };
    Instance in(d, kRate), twin(d, kRate);
    setup(in);
    setup(twin);
    const auto before = in.render(in.block, inputs);
    twin.render(twin.block, inputs);
    float held[4];
    for (uint32_t c = 0; c < 4; ++c) { held[c] = before[index(nth(Port::level_1, c))].back(); }

    in.control(Port::enabled) = 0;
    const auto off = in.render(4 * in.block, inputs);
    twin.render(4 * twin.block, inputs);
    const float output = std::pow(10.0f, -6.0f / 20.0f);
    for (uint32_t i = 0; i < 4 * in.block; ++i) {
        const uint64_t frame = in.frame - 4 * in.block + i;
        float sum = 0;
        for (uint32_t c = 0; c < 4; ++c) {
            const float x = noise(frame, c);
            const float y = off[index(nth(Port::out_1, c))][i];
            if (i + 1 >= glide) {
                require(y == x, "bypassed: Out " + std::to_string(c + 1) + " = In exactly");
            } else if (x != 0.0f) {
                const float gain = y / x;
                require(gain >= std::min(held[c], 1.0f) - 1e-6f && gain <= 1.0f + 1e-6f, "glides toward unity");
            }
            require(off[index(nth(Port::level_1, c))][i] == held[c], "Level outs hold");
            sum += y;
        }
        if (i + 1 >= glide) {
            require(std::fabs(off[index(Port::mix)][i] - sum * output) <= 1e-6f, "Mix = sum of the inputs times Output");
        }
    }

    // On again: back, exactly, to what an instance never bypassed gives.
    in.control(Port::enabled) = 1;
    const auto on = in.render(8 * in.block, inputs);
    const auto expected = twin.render(8 * twin.block, inputs);
    for (uint32_t p = 0; p < on.size(); ++p) {
        for (uint32_t i = glide; i < on[p].size(); ++i) {
            require(on[p][i] == expected[p][i], "on again returns exactly, port " + std::to_string(p));
        }
    }

    // Off from the start: exact passthrough from the first frame.
    Instance from_start(d, kRate);
    from_start.control(Port::enabled) = 0;
    from_start.control(Port::ch_1) = 0.1f;
    const auto o = from_start.render(1024, inputs);
    for (uint32_t i = 0; i < 1024; ++i) {
        require(o[index(Port::out_1)][i] == noise(i, 0), "off from the start");
        require(o[index(Port::level_1)][i] == linear_gain(code(0.1f)), "Level outs hold the first gains");
    }
    std::cout << "PASS bypass: off reaches unity in 10 ms (Mix the sum of the inputs times Output, Level outs held); "
                 "on returns exactly; off from the start passes through, Level outs at the first gains\n";
}

// --- instances -------------------------------------------------------------

void test_instances(const LV2_Descriptor* d)
{
    const auto one = [](Instance& in) { add_keyframe(in, 0.3f, {1, 0, 0.5f, 0}); add_keyframe(in, 0.7f, {0, 1, 0, 0.5f}); };
    const auto two = [](Instance& in) { add_keyframe(in, 0.1f, {0.2f, 0.2f, 1, 1}); };
    const auto play = [](Instance& in, uint32_t i, uint64_t frame) {
        inputs(in, i, frame);
        in.signal(Port::frame_cv, i) = static_cast<float>(0.5 + 0.5 * std::sin(frame * 0.0002));
    };
    Instance a_alone(d, kRate), b_alone(d, kRate), a(d, kRate), b(d, kRate);
    one(a_alone);
    one(a);
    two(b_alone);
    two(b);
    for (Instance* in : {&a_alone, &b_alone, &a, &b}) { in->control(Port::frame) = 0; }
    const auto expected_a = a_alone.render(96000, play);
    const auto expected_b = b_alone.render(96000, play);
    Outputs got_a, got_b;
    std::thread ta([&] { got_a = a.render(96000, play); });
    std::thread tb([&] { got_b = b.render(96000, play); });
    ta.join();
    tb.join();
    require(got_a == expected_a && got_b == expected_b, "instances share nothing");
    require(saved_list(a).count == 2 && saved_list(b).count == 1, "each keeps its own keyframes");
    std::cout << "PASS instances: two instances on two threads keep their own keyframes\n";
}

// --- allocation ------------------------------------------------------------

void test_allocation(const LV2_Descriptor* d)
{
#if REEL_COUNT_ALLOCATIONS
    // run() on a thread that has never run Reel, with edits and presses, and
    // save and restore called from another thread meanwhile.
    Instance instance(d, kRate, 128);
    instance.record_lists = false;
    build(instance);
    test::State state;
    instance.save(state);
    std::atomic<bool> done{false};
    std::thread saver([&] {
        test::State s;
        int n = 0;
        while (!done.load()) {
            instance.save(s);
            if (++n % 16 == 0) { instance.restore(state); }
        }
    });
    int during = 0;
    std::thread worker([&] {
        counting = true;
        instance.render(static_cast<uint64_t>(kRate), performance);
        counting = false;
        during = allocations.load();
    });
    worker.join();
    done = true;
    saver.join();
    // render() itself allocates its result vectors (one per output port, and
    // the outer vector); nothing else may.
    const int expected = 1 + 10;
    require(during <= expected, "run() allocates: " + std::to_string(during - expected) + " extra");
    std::cout << "PASS allocation: no allocation in run() with edits, presses, saves and restores\n";
#else
    (void)d;
    std::cout << "SKIP allocation: sanitizer build\n";
#endif
}

// --- rt_timing -------------------------------------------------------------

// 10 simulated minutes of random controls, edits and presses, with a save
// from another thread every 10 ms.
void test_timing(const LV2_Descriptor* d)
{
    Instance instance(d, kRate, 128);
    instance.measure = true;
    const double seconds = 600;
    uint32_t rng = 0x9E3779B9;
    const auto random = [&rng] {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        return (rng % 10000) / 10000.0f;
    };
    std::atomic<bool> done{false};
    std::thread saver([&] {
        test::State s;
        while (!done.load()) {
            instance.save(s);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });
    instance.render(static_cast<uint64_t>(seconds * kRate), [&](Instance& in, uint32_t i, uint64_t frame) {
        inputs(in, i, frame);
        in.signal(Port::frame_cv, i) = static_cast<float>(0.5 * std::sin(frame * 0.0001));
        if (i == 0 && frame % 12000 == 0) {
            in.control(Port::frame) = random();
            for (uint32_t c = 0; c < 4; ++c) {
                in.control(nth(Port::ch_1, c)) = random();
                in.control(nth(Port::easing_1, c)) = std::floor(6 * random());
                in.control(nth(Port::response_1, c)) = random();
            }
            const float r = random();
            in.control(Port::add) = r < 0.5f;
            in.control(Port::delete_) = r > 0.85f;
            in.control(Port::clear_all) = r > 0.995f;
            in.control(Port::enabled) = random() > 0.05f;
            in.control(Port::output) = 36 * random() - 24;
            in.control(Port::mode) = random() > 0.8f ? 1.0f : 0.0f;
        }
    });
    done = true;
    saver.join();
    auto times = instance.run_times;
    std::sort(times.begin(), times.end());
    double total = 0;
    for (double t : times) { total += t; }
    std::cout << "TIMING rt_timing, 48 kHz, 128 frames (" << 128 / kRate * 1e6
              << " us), 10 simulated minutes: mean " << total / times.size() << " us, 99.9% "
              << times[times.size() * 999 / 1000] << " us, worst " << times.back() << " us, load "
              << 100 * total / (seconds * 1e6) << "%\n";
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) { std::cerr << "usage: test_keyframes reel.so [--timing]\n"; return 1; }
    try {
        test::Library library(argv[1]);
        if (argc > 2 && std::strcmp(argv[2], "--timing") == 0) {
            test_timing(library.descriptor);
            return 0;
        }
        test_editing(library.descriptor);
        test_easing(library.descriptor);
        test_vca(library.descriptor);
        test_trigger(library.descriptor);
        test_sequencer(library.descriptor);
        test_telemetry(library.descriptor);
        test_state(library.descriptor);
        test_slicing(library.descriptor);
        test_bypass(library.descriptor);
        test_instances(library.descriptor);
        test_allocation(library.descriptor);
    } catch (const std::exception& e) {
        std::cerr << "FAIL keyframes: " << e.what() << '\n';
        return 1;
    }
    return 0;
}

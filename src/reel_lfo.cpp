// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alejandro Bustos
//
// Reel LFO (implementation plan, 4.5): frames::PolyLfo driven at its tables'
// own update rate, 24 kHz, whatever the host's. A phase accumulator renders
// on each whole tick; the outputs move linearly between the last two
// renders, so they are at most one tick (42 us) late.
#include "reel.hpp"

#include "vca.hpp"

#include "frames/poly_lfo.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <new>

namespace reel {
namespace {

// The rate lut_increments is made for (resources/lookup_tables.py).
constexpr double kUpdateRate = 24000.0;

// A control from 0 to 1 as the hardware's 16-bit knob value.
uint16_t knob(float x) { return static_cast<uint16_t>(std::lround(std::clamp(x, 0.0f, 1.0f) * 65535.0f)); }

// Tempo sync: Note's lengths in beats (the Turtle's enumeration, in order).
constexpr double kNoteBeats[] = {32, 16, 8, 4, 2, 1, 0.5, 0.25};
constexpr int kNotes = sizeof(kNoteBeats) / sizeof(kNoteBeats[0]);

// The Rate (0 to 1) that gives `hz`: PolyLfo's rate code climbs an octave
// every 5040 from the table's lowest rate (lut_increments; plan, 4.5).
float rate_for(double hz)
{
    const double lowest = frames::PolyLfo::FrequencyToPhaseIncrement(0) * kUpdateRate / 4294967296.0;
    return static_cast<float>(5040.0 * std::log2(hz / lowest) / 65535.0);
}

class ReelLfo {
public:
    explicit ReelLfo(double rate) noexcept : step_(kUpdateRate / rate) { activate(); }

    void connect(uint32_t index, void* data) noexcept
    {
        if (index < kLfoPortCount) { ports_[index] = static_cast<float*>(data); }
    }

    // As the hardware starts: PolyLfo is a static there, so its phases start
    // at 0 (Init doesn't set them), then Init.
    void activate() noexcept
    {
        std::memset(static_cast<void*>(&lfo_), 0, sizeof(lfo_));
        lfo_.Init();
        tick_ = 0;
        first_ = true;
    }

    void run(uint32_t frames) noexcept
    {
        lfo_.set_shape(knob(control(LfoPort::shape, 0.0f)));
        lfo_.set_shape_spread(knob(control(LfoPort::shape_spread, 0.5f)));
        lfo_.set_spread(knob(control(LfoPort::spread, 0.5f)));
        lfo_.set_coupling(knob(control(LfoPort::coupling, 0.5f)));
        // With Sync on, Rate is the Note's length at the BPM (Peepo sets BPM from
        // the patch tempo); Rate CV still adds.
        const bool sync = control(LfoPort::sync, 0.0f) > 0.5f;
        const int note = static_cast<int>(std::lround(control(LfoPort::note, 3.0f, 0.0f, kNotes - 1.0f)));
        const double bpm = control(LfoPort::bpm, 120.0f, 30.0f, 300.0f);
        const float rate = sync ? rate_for(bpm / 60.0 / kNoteBeats[note]) : control(LfoPort::rate, 0.5f);
        const float* rate_cv = ports_[index(LfoPort::rate_cv)];
        float* outs[kChannels];
        for (uint32_t c = 0; c < kChannels; ++c) { outs[c] = ports_[index(nth(LfoPort::out_1, c))]; }

        for (uint32_t i = 0; i < frames; ++i) {
            const float cv = rate_cv && std::isfinite(rate_cv[i]) ? rate_cv[i] : 0.0f;
            if (first_) {
                render(rate, cv);
                std::copy(std::begin(current_), std::end(current_), std::begin(previous_));
                first_ = false;
            }
            tick_ += step_;
            while (tick_ >= 1.0) {
                tick_ -= 1.0;
                std::copy(std::begin(current_), std::end(current_), std::begin(previous_));
                render(rate, cv);
            }
            const float t = static_cast<float>(tick_);
            for (uint32_t c = 0; c < kChannels; ++c) {
                if (outs[c]) { outs[c][i] = previous_[c] + (current_[c] - previous_[c]) * t; }
            }
        }
    }

private:
    float control(LfoPort port, float fallback, float minimum = 0.0f, float maximum = 1.0f) const noexcept
    {
        const float* p = ports_[index(port)];
        return p && std::isfinite(*p) ? std::clamp(*p, minimum, maximum) : fallback;
    }

    // Rate plus Rate CV, clamped to the knob's range (0.0134 to 110 Hz):
    // upstream doesn't clamp, and outside 0 to about 2 its phase increment
    // lookup reads outside lut_increments.
    void render(float rate, float cv) noexcept
    {
        lfo_.Render(knob(rate + cv));
        for (uint8_t c = 0; c < kChannels; ++c) { current_[c] = dac_gain(lfo_.dac_code(c)); }
    }

    float* ports_[kLfoPortCount] = {};
    frames::PolyLfo lfo_;
    double step_;     // ticks per frame
    double tick_ = 0; // the fraction of a tick since the last render
    bool first_ = true;
    float previous_[kChannels] = {};
    float current_[kChannels] = {};
};

LV2_Handle instantiate(const LV2_Descriptor*, double rate, const char*, const LV2_Feature* const*)
{
    if (!std::isfinite(rate) || rate < 8000 || rate > 768000) { return nullptr; }
    return new (std::nothrow) ReelLfo(rate);
}
void connect_port(LV2_Handle h, uint32_t index, void* data) { static_cast<ReelLfo*>(h)->connect(index, data); }
void activate(LV2_Handle h) { static_cast<ReelLfo*>(h)->activate(); }
void run(LV2_Handle h, uint32_t frames) { static_cast<ReelLfo*>(h)->run(frames); }
void cleanup(LV2_Handle h) { delete static_cast<ReelLfo*>(h); }
const void* extension_data(const char*) { return nullptr; }

} // namespace

const LV2_Descriptor lfo_descriptor{
    kLfoURI, instantiate, connect_port, activate, run, nullptr, cleanup, extension_data,
};

} // namespace reel

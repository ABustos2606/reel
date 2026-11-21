// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alejandro Bustos
#include "reel.hpp"

#include <lv2/urid/urid.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>

namespace reel {
namespace {

LV2_Handle instantiate(const LV2_Descriptor*, double rate, const char*, const LV2_Feature* const* features)
{
    if (!std::isfinite(rate) || rate < 8000 || rate > 768000) { return nullptr; }
    return Reel::create(rate, features);
}
void connect_port(LV2_Handle h, uint32_t index, void* data) { static_cast<Reel*>(h)->connect(index, data); }
void activate(LV2_Handle h) { static_cast<Reel*>(h)->activate(); }
void run(LV2_Handle h, uint32_t frames) { static_cast<Reel*>(h)->run(frames); }
void cleanup(LV2_Handle h) { delete static_cast<Reel*>(h); }

// LV2 state's threading class: save may run while run() does (Ingen saves
// from its own thread); neither touches the keyframer.
LV2_State_Status save(LV2_Handle h, LV2_State_Store_Function store, LV2_State_Handle state, uint32_t,
                      const LV2_Feature* const*)
{
    return static_cast<Reel*>(h)->save(store, state);
}
LV2_State_Status restore(LV2_Handle h, LV2_State_Retrieve_Function retrieve, LV2_State_Handle state, uint32_t,
                         const LV2_Feature* const*)
{
    return static_cast<Reel*>(h)->restore(retrieve, state);
}
const LV2_State_Interface state_interface{save, restore};

const void* extension_data(const char* uri)
{
    return std::strcmp(uri, LV2_STATE__interface) == 0 ? &state_interface : nullptr;
}

float smoothing(double seconds, double rate) { return static_cast<float>(1.0 - std::exp(-1.0 / (seconds * rate))); }

// One step of a one-pole smoother. In float a slow one stalls short of its
// target once each step rounds to nothing (about 1e-5 away for a 10 ms
// smoother at 48 kHz), so it lands on the target then.
void glide(float& value, float target, float coefficient)
{
    const float next = value + (target - value) * coefficient;
    value = next == value ? target : next;
}

} // namespace

const LV2_Descriptor descriptor{
    kURI, instantiate, connect_port, activate, run, nullptr, cleanup, extension_data,
};

Reel* Reel::create(double rate, const LV2_Feature* const* features) noexcept
{
    // urid:map is optional: without it Reel runs, and its state is not saved.
    const LV2_URID_Map* map = nullptr;
    for (auto f = features; f && *f; ++f) {
        if (std::strcmp((*f)->URI, LV2_URID__map) == 0) { map = static_cast<const LV2_URID_Map*>((*f)->data); }
    }
    auto* reel = new (std::nothrow) Reel(rate, StateKeys::map(map));
    if (reel) { reel->activate(); }
    return reel;
}

Reel::Reel(double rate, const StateKeys& keys) noexcept
    : keys_(keys), frame_smoothing_(smoothing(kFrameSmoothingSeconds, rate)),
      output_smoothing_(smoothing(kOutputSmoothingSeconds, rate))
{
}

void Reel::connect(uint32_t index, void* data) noexcept
{
    if (index < kPortCount) { ports_[index] = static_cast<float*>(data); }
}

// What the hardware's Keyframer::Init does on a fresh unit (it does nothing
// under -DTEST): every channel Linear with response 0, no keyframes. The
// immediate levels are set from the Level controls at the first grid frame.
void Reel::activate() noexcept
{
    keyframer_.Init();
    for (uint8_t c = 0; c < frames::kNumChannels; ++c) {
        keyframer_.mutable_settings(c)->easing_curve = frames::EASING_CURVE_LINEAR;
        keyframer_.mutable_settings(c)->response = 0;
        keyframer_.set_immediate(c, 0);
    }
    keyframer_.Clear();
    // Sets position() and nearest_keyframe() (-1 with no keyframes) before
    // the first run() reports them.
    keyframer_.Evaluate(0);
    grid_ = 0;
    first_ = true;
}

float Reel::control(Port port, float fallback, float minimum, float maximum) const noexcept
{
    const float* p = ports_[index(port)];
    if (!p || !std::isfinite(*p)) { return fallback; }
    return std::clamp(*p, minimum, maximum);
}

// A sample of an audio or CV input; unconnected ports and non-finite samples
// read 0.
float Reel::signal(Port port, uint32_t frame) const noexcept
{
    const float* p = ports_[index(port)];
    if (!p) { return 0.0f; }
    const float x = p[frame];
    return std::isfinite(x) ? x : 0.0f;
}

void Reel::write(Port port, float value) const noexcept
{
    if (float* p = ports_[index(port)]) { *p = value; }
}

// One grid frame (implementation plan, 4.1). Milestone 0: no keyframes, so
// the levels are the Level controls (SyncWithPots), and the VCAs pass each
// input times its level.
void Reel::evaluate(uint32_t frame) noexcept
{
    const float position = std::clamp(frame_ + signal(Port::frame_cv, frame) *
                                                   control(Port::frame_cv_amount, 1.0f, -1.0f, 1.0f),
                                      0.0f, 1.0f);
    effective_frame_ = static_cast<uint16_t>(std::lround(position * 65535.0f));
    for (uint8_t c = 0; c < frames::kNumChannels; ++c) {
        const float level = control(nth(Port::ch_1, c), 1.0f, 0.0f, 1.0f);
        keyframer_.set_immediate(c, static_cast<uint16_t>(std::lround(level * 65535.0f)));
    }
    keyframer_.Evaluate(effective_frame_);
    for (uint8_t c = 0; c < frames::kNumChannels; ++c) {
        // Response is milestone 1 (implementation plan, 4.3): the gain is the
        // level.
        const float gain = keyframer_.level(c) / 65535.0f;
        gain_from_[c] = first_ ? gain : gain_to_[c];
        gain_to_[c] = gain;
    }
    first_ = false;
}

void Reel::run(uint32_t frames) noexcept
{
    const float frame_target = control(Port::frame, 0.0f, 0.0f, 1.0f);
    const float output_target = std::pow(10.0f, control(Port::output, 0.0f, -24.0f, 12.0f) / 20.0f);
    if (first_) {
        frame_ = frame_target;
        output_ = output_target;
    }
    float* outs[kChannels];
    float* levels[kChannels];
    for (uint32_t c = 0; c < kChannels; ++c) {
        outs[c] = ports_[index(nth(Port::out_1, c))];
        levels[c] = ports_[index(nth(Port::level_1, c))];
    }
    float* mix = ports_[index(Port::mix)];
    float* trigger = ports_[index(Port::trigger)];

    for (uint32_t i = 0; i < frames; ++i) {
        glide(frame_, frame_target, frame_smoothing_);
        glide(output_, output_target, output_smoothing_);
        if (grid_ == 0) { evaluate(i); }
        const float t = static_cast<float>(grid_ + 1) / kGridFrames;
        float sum = 0;
        for (uint32_t c = 0; c < kChannels; ++c) {
            const float gain = gain_from_[c] + (gain_to_[c] - gain_from_[c]) * t;
            const float out = signal(nth(Port::in_1, c), i) * gain;
            sum += out;
            if (outs[c]) { outs[c][i] = out; }
            if (levels[c]) { levels[c][i] = gain; }
        }
        if (mix) { mix[i] = sum * output_; }
        if (trigger) { trigger[i] = 0.0f; }
        grid_ = (grid_ + 1) % kGridFrames;
    }

    write(Port::t_frame, effective_frame_ / 65535.0f);
    write(Port::t_count, static_cast<float>(keyframer_.num_keyframes()));
    write(Port::t_active, -1.0f);
    write(Port::t_nearest, static_cast<float>(keyframer_.nearest_keyframe()));
    write(Port::t_step, 0.0f);
}

LV2_State_Status Reel::save(LV2_State_Store_Function store, LV2_State_Handle handle) noexcept
{
    return save_keyframes(keys_, saved_, store, handle);
}

// Milestone 0: the saved list is checked and dropped; milestone 1 hands it to
// run() (implementation plan, section 5).
LV2_State_Status Reel::restore(LV2_State_Retrieve_Function retrieve, LV2_State_Handle handle) noexcept
{
    KeyframeList list;
    return restore_keyframes(keys_, retrieve, handle, list);
}

} // namespace reel

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alejandro Bustos
#include "reel.hpp"

#include "vca.hpp"

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
// from its own thread); neither touches the keyframer, which only run()
// uses (implementation plan, section 5).
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

// A control from 0 to 1 in the keyframer's units, 0 to 65535.
uint16_t to_code(float x) { return static_cast<uint16_t>(std::lround(x * 65535.0f)); }

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
      output_smoothing_(smoothing(kOutputSmoothingSeconds, rate)),
      trigger_frames_(static_cast<uint32_t>(std::lround(kTriggerSeconds * rate))),
      bypass_frames_(std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(kBypassSeconds * rate)))),
      load_steps_(static_cast<uint32_t>(std::lround(kLoadSeconds * rate / kGridFrames)))
{
    // Keyframer's constructor leaves its members unset, and activate keeps
    // the list it finds: start from an empty one.
    keyframer_.Clear();
}

void Reel::connect(uint32_t index, void* data) noexcept
{
    if (index < kPortCount) { ports_[index] = static_cast<float*>(data); }
}

// What the hardware's Keyframer::Init does on a fresh unit (it does nothing
// under -DTEST): every channel Linear with response 0. The ports overwrite
// those at each grid frame, and the immediate levels follow the Level
// controls from the first. The keyframes are kept: activate restarts the
// sound, not the patch.
void Reel::activate() noexcept
{
    copy_keyframes(keyframer_, scratch_);
    keyframer_.Init();
    for (uint8_t c = 0; c < frames::kNumChannels; ++c) {
        keyframer_.mutable_settings(c)->easing_curve = frames::EASING_CURVE_LINEAR;
        keyframer_.mutable_settings(c)->response = 0;
        keyframer_.set_immediate(c, 0);
    }
    load_keyframes(keyframer_, scratch_);
    published_.publish(keyframer_);
    // Sets position() and nearest_keyframe() before the first run() reports
    // them.
    keyframer_.Evaluate(0);
    editor_.reset();
    trigger_.reset(trigger_frames_);
    inputs_ = EditInputs{};
    gain_valid_ = false;
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

// The controls, once per run(). Button edges are latched until the next grid
// frame applies them, so a press shorter than a grid step is not lost.
void Reel::read_controls() noexcept
{
    inputs_.frame = to_code(control(Port::frame, 0.0f, 0.0f, 1.0f));
    for (uint8_t c = 0; c < frames::kNumChannels; ++c) {
        inputs_.levels[c] = to_code(control(nth(Port::ch_1, c), 1.0f, 0.0f, 1.0f));
        easing_[c] = static_cast<frames::EasingCurve>(std::lround(control(nth(Port::easing_1, c), 1.0f, 0.0f, 5.0f)));
        response_[c] = response_code(control(nth(Port::response_1, c), 0.0f, 0.0f, 1.0f));
    }
    const bool add = control(Port::add, 0.0f, 0.0f, 1.0f) > 0.5f;
    const bool remove = control(Port::delete_, 0.0f, 0.0f, 1.0f) > 0.5f;
    const bool clear = control(Port::clear_all, 0.0f, 0.0f, 1.0f) > 0.5f;
    // A button already held when the plugin starts is not a press.
    if (!first_) {
        inputs_.add |= add && !add_;
        inputs_.remove |= remove && !remove_;
        inputs_.clear |= clear && !clear_;
    }
    add_ = add;
    remove_ = remove;
    clear_ = clear;
}

// The keyframe nearest the effective Frame, counted from 0, or -1 with none
// (t_nearest). Upstream's nearest_keyframe() counts from 1 when Frame is
// nearer the keyframe after it, and can name the slot after the last one, so
// the page is told the index itself. Ties go left, as upstream's.
int16_t Reel::nearest_index() const noexcept
{
    const int16_t count = static_cast<int16_t>(keyframer_.num_keyframes());
    if (!count) { return -1; }
    const int16_t position = keyframer_.position();
    if (position <= 0) { return 0; }
    if (position >= count) { return static_cast<int16_t>(count - 1); }
    const int32_t left = effective_frame_ - keyframer_.keyframe(position - 1).timestamp;
    const int32_t right = keyframer_.keyframe(position).timestamp - effective_frame_;
    return right < left ? position : static_cast<int16_t>(position - 1);
}

// One grid frame (implementation plan, 4.1).
void Reel::evaluate(uint32_t frame) noexcept
{
    // 1. A restored list, then edits and button presses (plan, 4.2 and 5).
    // The list is published before the pending slot is emptied, so a save in
    // between finds one or the other.
    if (pending_.take(scratch_)) {
        load_keyframes(keyframer_, scratch_);
        published_.publish(keyframer_);
        pending_.release();
        editor_.reset(load_steps_);
    }
    const EditInputs in = inputs_;
    inputs_.add = inputs_.remove = inputs_.clear = false;
    if (editor_.apply(keyframer_, in)) { published_.publish(keyframer_); }

    // 2. Frame: the smoothed control plus Frame CV times its amount, clamped,
    // in the keyframer's units (frames.cc).
    const float position = std::clamp(frame_ + signal(Port::frame_cv, frame) *
                                                   control(Port::frame_cv_amount, 1.0f, -1.0f, 1.0f),
                                      0.0f, 1.0f);
    effective_frame_ = to_code(position);

    // 3. Easing and response from the ports, then Evaluate.
    for (uint8_t c = 0; c < frames::kNumChannels; ++c) {
        keyframer_.mutable_settings(c)->easing_curve = easing_[c];
        keyframer_.mutable_settings(c)->response = response_[c];
    }
    keyframer_.Evaluate(effective_frame_);

    // 4. The gains (plan, 4.3), recomputed only for a channel whose level or
    // response changed, and the trigger (4.4).
    for (uint8_t c = 0; c < frames::kNumChannels; ++c) {
        const uint16_t level = keyframer_.level(c);
        if (!gain_valid_ || level != gain_level_[c] || response_[c] != gain_response_[c]) {
            gain_level_[c] = level;
            gain_response_[c] = response_[c];
            gain_[c] = vca_gain(level / 65535.0, response_balance(response_[c]));
        }
        gain_from_[c] = first_ ? gain_[c] : gain_to_[c];
        gain_to_[c] = gain_[c];
        // Bypassed from the start, the Level outs hold the first gains.
        if (first_) { held_[c] = gain_[c]; }
    }
    gain_valid_ = true;
    trigger_.check(keyframer_.position(), keyframer_.nearest_keyframe());
    first_ = false;
}

void Reel::run(uint32_t frames) noexcept
{
    read_controls();
    const float frame_target = control(Port::frame, 0.0f, 0.0f, 1.0f);
    const float output_target = std::pow(10.0f, control(Port::output, 0.0f, -24.0f, 12.0f) / 20.0f);
    const bool enabled = control(Port::enabled, 1.0f, 0.0f, 1.0f) > 0.5f;
    if (first_) {
        frame_ = frame_target;
        output_ = output_target;
        bypass_ = enabled ? 0 : bypass_frames_;
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
        // Bypass (plan, 4.6): every gain glides to unity over 10 ms, and
        // back; exact at both ends.
        if (enabled) {
            if (bypass_ > 0) { --bypass_; }
        } else if (bypass_ < bypass_frames_) {
            ++bypass_;
        }
        const float bypass = static_cast<float>(bypass_) / static_cast<float>(bypass_frames_);
        if (grid_ == 0) { evaluate(i); }
        const float t = static_cast<float>(grid_ + 1) / kGridFrames;
        float sum = 0;
        for (uint32_t c = 0; c < kChannels; ++c) {
            const float g = gain_from_[c] + (gain_to_[c] - gain_from_[c]) * t;
            const float gain = bypass_ == 0 ? g : bypass_ == bypass_frames_ ? 1.0f : g + (1.0f - g) * bypass;
            const float out = signal(nth(Port::in_1, c), i) * gain;
            sum += out;
            if (outs[c]) { outs[c][i] = out; }
            // Level outs hold while bypassed.
            if (enabled) { held_[c] = g; }
            if (levels[c]) { levels[c][i] = held_[c]; }
        }
        if (mix) { mix[i] = sum * output_; }
        // The trigger keeps its state while bypassed, but stays low.
        const float pulse = trigger_.next();
        if (trigger) { trigger[i] = enabled ? pulse : 0.0f; }
        grid_ = (grid_ + 1) % kGridFrames;
    }

    write(Port::t_frame, effective_frame_ / 65535.0f);
    write(Port::t_count, static_cast<float>(keyframer_.num_keyframes()));
    write(Port::t_active, static_cast<float>(editor_.active()));
    write(Port::t_nearest, static_cast<float>(nearest_index()));
    write(Port::t_step, 0.0f);
}

// Save may run while run() does (Ingen saves from its own thread): it reads
// the published list, or a restored one run() has not taken yet.
LV2_State_Status Reel::save(LV2_State_Store_Function store, LV2_State_Handle handle) noexcept
{
    if (!keys_.valid()) { return LV2_STATE_ERR_NO_FEATURE; }
    KeyframeList list;
    if (!pending_.peek(list)) { published_.read(list); }
    return save_keyframes(keys_, list, store, handle);
}

// The saved list is checked and handed to run(), which takes it at its next
// grid frame. A malformed list is rejected and leaves Reel with none (plan,
// section 3); a missing one means none.
LV2_State_Status Reel::restore(LV2_State_Retrieve_Function retrieve, LV2_State_Handle handle) noexcept
{
    KeyframeList list;
    const LV2_State_Status status = restore_keyframes(keys_, retrieve, handle, list);
    if (keys_.valid()) { pending_.put(list); }
    return status;
}

} // namespace reel

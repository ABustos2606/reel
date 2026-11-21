// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alejandro Bustos
#include "keyframe_state.hpp"

#include <lv2/atom/atom.h>

#include <algorithm>
#include <cstring>
#include <thread>

namespace reel {

StateKeys StateKeys::map(const LV2_URID_Map* map)
{
    StateKeys keys;
    if (map && map->map) {
        keys.version = map->map(map->handle, kStateVersionURI);
        keys.keyframes = map->map(map->handle, kStateKeyframesURI);
        keys.atom_int = map->map(map->handle, LV2_ATOM__Int);
        keys.atom_vector = map->map(map->handle, LV2_ATOM__Vector);
    }
    return keys;
}

ParseResult parse_keyframes(const StateKeys& keys, const void* value, size_t size, uint32_t type, KeyframeList& list)
{
    list.count = 0;
    if (!value || type != keys.atom_vector || size < sizeof(LV2_Atom_Vector_Body)) { return ParseResult::bad_type; }
    LV2_Atom_Vector_Body body;
    std::memcpy(&body, value, sizeof(body));
    if (body.child_type != keys.atom_int || body.child_size != sizeof(int32_t)) { return ParseResult::bad_type; }
    const size_t bytes = size - sizeof(body);
    if (bytes % (kIntsPerKeyframe * sizeof(int32_t)) != 0) { return ParseResult::bad_size; }
    const size_t count = bytes / (kIntsPerKeyframe * sizeof(int32_t));
    if (count > kMaxKeyframes) { return ParseResult::too_many; }

    KeyframeList parsed;
    const auto* ints = static_cast<const uint8_t*>(value) + sizeof(body);
    for (size_t k = 0; k < count; ++k) {
        int32_t v[kIntsPerKeyframe];
        std::memcpy(v, ints + k * sizeof(v), sizeof(v));
        for (int32_t x : v) {
            if (x < 0 || x > 65535) { return ParseResult::out_of_range; }
        }
        frames::Keyframe& keyframe = parsed.keyframes[k];
        keyframe.timestamp = static_cast<uint16_t>(v[0]);
        keyframe.id = static_cast<uint16_t>(v[1]);
        for (uint32_t c = 0; c < frames::kNumChannels; ++c) { keyframe.values[c] = static_cast<uint16_t>(v[2 + c]); }
        if (k > 0 && keyframe.timestamp <= parsed.keyframes[k - 1].timestamp) { return ParseResult::unsorted; }
    }
    parsed.count = static_cast<uint32_t>(count);
    list = parsed;
    return ParseResult::ok;
}

size_t serialise_keyframes(const StateKeys& keys, const KeyframeList& list, uint8_t (&buffer)[kVectorBytes])
{
    const LV2_Atom_Vector_Body body{sizeof(int32_t), keys.atom_int};
    std::memcpy(buffer, &body, sizeof(body));
    size_t offset = sizeof(body);
    for (uint32_t k = 0; k < list.count && k < kMaxKeyframes; ++k) {
        const frames::Keyframe& keyframe = list.keyframes[k];
        int32_t v[kIntsPerKeyframe] = {keyframe.timestamp, keyframe.id};
        for (uint32_t c = 0; c < frames::kNumChannels; ++c) { v[2 + c] = keyframe.values[c]; }
        std::memcpy(buffer + offset, v, sizeof(v));
        offset += sizeof(v);
    }
    return offset;
}

LV2_State_Status save_keyframes(const StateKeys& keys, const KeyframeList& list, LV2_State_Store_Function store,
                                LV2_State_Handle handle)
{
    if (!keys.valid()) { return LV2_STATE_ERR_NO_FEATURE; }
    const uint32_t flags = LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE;
    const int32_t version = kStateVersion;
    LV2_State_Status status = store(handle, keys.version, &version, sizeof(version), keys.atom_int, flags);
    if (status != LV2_STATE_SUCCESS || list.count == 0) { return status; }
    uint8_t buffer[kVectorBytes];
    const size_t size = serialise_keyframes(keys, list, buffer);
    return store(handle, keys.keyframes, buffer, size, keys.atom_vector, flags);
}

LV2_State_Status restore_keyframes(const StateKeys& keys, LV2_State_Retrieve_Function retrieve, LV2_State_Handle handle,
                                   KeyframeList& list)
{
    list.count = 0;
    if (!keys.valid()) { return LV2_STATE_ERR_NO_FEATURE; }
    size_t size = 0;
    uint32_t type = 0;
    uint32_t flags = 0;
    if (const void* version = retrieve(handle, keys.version, &size, &type, &flags)) {
        int32_t v = 0;
        if (type != keys.atom_int || size != sizeof(v)) { return LV2_STATE_ERR_BAD_TYPE; }
        std::memcpy(&v, version, sizeof(v));
        if (v < 1 || v > kStateVersion) { return LV2_STATE_ERR_BAD_TYPE; }
    }
    const void* value = retrieve(handle, keys.keyframes, &size, &type, &flags);
    if (!value) { return LV2_STATE_SUCCESS; }
    return parse_keyframes(keys, value, size, type, list) == ParseResult::ok ? LV2_STATE_SUCCESS
                                                                              : LV2_STATE_ERR_BAD_TYPE;
}

void copy_keyframes(const frames::Keyframer& keyframer, KeyframeList& list) noexcept
{
    list.count = keyframer.num_keyframes();
    for (uint32_t k = 0; k < list.count; ++k) { list.keyframes[k] = keyframer.keyframe(k); }
}

void load_keyframes(frames::Keyframer& keyframer, const KeyframeList& list) noexcept
{
    keyframer.Clear();
    for (uint32_t k = 0; k < list.count && k < kMaxKeyframes; ++k) {
        uint16_t values[frames::kNumChannels];
        for (uint32_t c = 0; c < frames::kNumChannels; ++c) { values[c] = list.keyframes[k].values[c]; }
        keyframer.AddKeyframe(list.keyframes[k].timestamp, values);
    }
    // Positions are strictly increasing (parse_keyframes checks), so keyframe
    // k is at index k.
    for (uint32_t k = 0; k < keyframer.num_keyframes(); ++k) {
        keyframer.mutable_keyframe(k)->id = list.keyframes[k].id;
    }
}

void PublishedList::publish(const frames::Keyframer& keyframer) noexcept
{
    const uint32_t sequence = sequence_.load(std::memory_order_relaxed);
    sequence_.store(sequence + 1, std::memory_order_relaxed); // odd: writing
    // Each release store keeps the odd sequence before it: a reader that sees
    // any new word also sees an odd or newer sequence.
    const uint32_t count = keyframer.num_keyframes();
    count_.store(count, std::memory_order_release);
    for (uint32_t k = 0; k < count; ++k) {
        const frames::Keyframe& keyframe = keyframer.keyframe(k);
        const uint32_t base = k * kWordsPerKeyframe;
        words_[base].store(keyframe.timestamp | uint32_t{keyframe.id} << 16, std::memory_order_release);
        words_[base + 1].store(keyframe.values[0] | uint32_t{keyframe.values[1]} << 16, std::memory_order_release);
        words_[base + 2].store(keyframe.values[2] | uint32_t{keyframe.values[3]} << 16, std::memory_order_release);
    }
    sequence_.store(sequence + 2, std::memory_order_release);
}

void PublishedList::read(KeyframeList& list) const noexcept
{
    for (;;) {
        const uint32_t before = sequence_.load(std::memory_order_acquire);
        if (before & 1) {
            std::this_thread::yield();
            continue;
        }
        // Acquire loads: the second sequence load can't move above them.
        list.count = std::min<uint32_t>(count_.load(std::memory_order_acquire), kMaxKeyframes);
        for (uint32_t k = 0; k < list.count; ++k) {
            const uint32_t base = k * kWordsPerKeyframe;
            const uint32_t a = words_[base].load(std::memory_order_acquire);
            const uint32_t b = words_[base + 1].load(std::memory_order_acquire);
            const uint32_t c = words_[base + 2].load(std::memory_order_acquire);
            frames::Keyframe& keyframe = list.keyframes[k];
            keyframe.timestamp = static_cast<uint16_t>(a);
            keyframe.id = static_cast<uint16_t>(a >> 16);
            keyframe.values[0] = static_cast<uint16_t>(b);
            keyframe.values[1] = static_cast<uint16_t>(b >> 16);
            keyframe.values[2] = static_cast<uint16_t>(c);
            keyframe.values[3] = static_cast<uint16_t>(c >> 16);
        }
        if (sequence_.load(std::memory_order_relaxed) == before) { return; }
    }
}

void PendingList::put(const KeyframeList& list) noexcept
{
    for (;;) {
        uint32_t state = state_.load(std::memory_order_acquire);
        if ((state == kEmpty || state == kReady) &&
            state_.compare_exchange_weak(state, kBusy, std::memory_order_acquire)) {
            break;
        }
        std::this_thread::yield();
    }
    list_ = list;
    state_.store(kReady, std::memory_order_release);
}

bool PendingList::take(KeyframeList& list) noexcept
{
    uint32_t state = kReady;
    if (!state_.compare_exchange_strong(state, kTaking, std::memory_order_acquire)) { return false; }
    list = list_;
    return true;
}

void PendingList::release() noexcept { state_.store(kEmpty, std::memory_order_release); }

bool PendingList::peek(KeyframeList& list) noexcept
{
    for (;;) {
        uint32_t state = state_.load(std::memory_order_acquire);
        if (state == kEmpty) { return false; }
        if (state == kReady && state_.compare_exchange_weak(state, kBusy, std::memory_order_acquire)) {
            list = list_;
            state_.store(kReady, std::memory_order_release);
            return true;
        }
        std::this_thread::yield();
    }
}

} // namespace reel

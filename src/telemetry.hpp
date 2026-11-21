// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alejandro Bustos
//
// The keyframe list for Peepo's page (t_keyframes, implementation plan,
// 4.7): one atom:String event on an atom:Sequence output, written when the
// list changes (at most every kListMinSeconds) and again every
// kListRepeatSeconds while it doesn't, so a page that starts watching gets it
// soon. Ingen forwards a watched atom output's events to its clients as
// ingen:activity values, written as Turtle literals; Peepo's reader takes
// literals, not RDF lists, hence a string rather than an atom:Vector.
//
// The text: "1:" (the format), then 24 lowercase hex digits per keyframe in
// position order: position, id, level 1, level 2, level 3, level 4, four
// digits each (0000-ffff). No keyframes: "1:".
#pragma once

#include "frames/keyframer.h"

#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/urid/urid.h>

#include <cstdint>
#include <cstring>

namespace reel {

inline constexpr char kListFormat[] = "1:";
inline constexpr uint32_t kListDigitsPerKeyframe = 24;
inline constexpr uint32_t kListTextBytes =
    sizeof(kListFormat) - 1 + frames::kMaxNumKeyframe * kListDigitsPerKeyframe + 1; // with the NUL
// Turtle's rsz:minimumSize for t_keyframes; the largest list takes 1576.
inline constexpr uint32_t kListPortBytes = 4096;
inline constexpr double kListMinSeconds = 0.04;   // tune: Ingen's own monitor rate is 25 Hz
inline constexpr double kListRepeatSeconds = 0.5; // tune

// The list as text, into `text` (kListTextBytes); returns its length without
// the NUL.
inline uint32_t list_text(const frames::Keyframer& keyframer, char (&text)[kListTextBytes]) noexcept
{
    static constexpr char kHex[] = "0123456789abcdef";
    uint32_t n = sizeof(kListFormat) - 1;
    std::memcpy(text, kListFormat, n);
    const uint16_t count = keyframer.num_keyframes();
    for (uint16_t k = 0; k < count && k < frames::kMaxNumKeyframe; ++k) {
        const frames::Keyframe& keyframe = keyframer.keyframe(k);
        const uint16_t words[6] = {keyframe.timestamp, keyframe.id,        keyframe.values[0],
                                   keyframe.values[1], keyframe.values[2], keyframe.values[3]};
        for (uint16_t word : words) {
            for (int shift = 12; shift >= 0; shift -= 4) { text[n++] = kHex[(word >> shift) & 0xf]; }
        }
    }
    text[n] = '\0';
    return n;
}

class ListTelemetry {
public:
    // Without urid:map nothing is written.
    void map(const LV2_URID_Map* map) noexcept
    {
        if (map && map->map) {
            sequence_ = map->map(map->handle, LV2_ATOM__Sequence);
            string_ = map->map(map->handle, LV2_ATOM__String);
        }
    }
    void connect(void* data) noexcept { port_ = static_cast<LV2_Atom_Sequence*>(data); }
    // At activate: the list goes out at the first run().
    void reset(double rate) noexcept
    {
        min_frames_ = static_cast<uint64_t>(kListMinSeconds * rate);
        repeat_frames_ = static_cast<uint64_t>(kListRepeatSeconds * rate);
        since_ = repeat_frames_;
        changed_ = true;
    }
    void changed() noexcept { changed_ = true; }

    // At the end of every run(). Ingen prepares an output sequence once per
    // period (an atom:Chunk the size of its capacity, as LV2 asks), then
    // calls run() once per slice with the same buffer: a sequence already
    // there was written by an earlier slice of this period, and is kept.
    void run(const frames::Keyframer& keyframer, uint32_t frames) noexcept
    {
        if (!port_ || !sequence_ || !string_) { return; }
        LV2_Atom& atom = port_->atom;
        if (atom.type != sequence_) {
            capacity_ = atom.size;
            if (capacity_ < sizeof(LV2_Atom_Sequence_Body)) { return; }
            clear();
        }
        since_ += frames;
        if (!(changed_ ? since_ >= min_frames_ : since_ >= repeat_frames_)) { return; }
        const uint32_t length = list_text(keyframer, text_);
        const uint32_t body = length + 1;
        const uint32_t event = sizeof(LV2_Atom_Event) + lv2_atom_pad_size(body);
        // A newer list supersedes one written by an earlier slice.
        if (sizeof(LV2_Atom_Sequence_Body) + event > capacity_) { return; }
        clear();
        auto* ev = reinterpret_cast<LV2_Atom_Event*>(port_ + 1);
        ev->time.frames = 0;
        ev->body.size = body;
        ev->body.type = string_;
        std::memcpy(ev + 1, text_, body);
        atom.size += event;
        changed_ = false;
        since_ = 0;
    }

private:
    void clear() noexcept
    {
        port_->atom.type = sequence_;
        port_->atom.size = sizeof(LV2_Atom_Sequence_Body);
        port_->body.unit = 0;
        port_->body.pad = 0;
    }

    LV2_Atom_Sequence* port_ = nullptr;
    LV2_URID sequence_ = 0;
    LV2_URID string_ = 0;
    uint32_t capacity_ = 0; // the body's capacity, from the host's last atom:Chunk
    uint64_t min_frames_ = 0;
    uint64_t repeat_frames_ = 0;
    uint64_t since_ = 0;
    bool changed_ = true;
    char text_[kListTextBytes] = {};
};

} // namespace reel

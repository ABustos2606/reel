// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alejandro Bustos
//
// Edits from Peepo's page on the `commands` input (port 41, implementation
// plan, 4.8): atom:Int events in an atom:Sequence, as Ingen makes them from a
// typed xsd:int set on the port.
//
//     value = op << 24 | keyframe << 16 | position
//     op 0: nothing (the page sends it when a gesture ends, so the value Ingen
//           saves with the patch, and sets again when it loads, does nothing)
//     op 1: move keyframe `keyframe` (0-63, in position order) to `position`
//           (0-65535)
//
// Other ops are ignored. A move is absolute, so applying it twice changes
// nothing: Ingen hands every slice of a period the same sequence (its times
// count from the period's start), and a buffer identical to the last run()'s
// is skipped as already read. Valid events are applied at the next grid
// frame, in order, whatever their time stamps.
#pragma once

#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/urid/urid.h>

#include <cstdint>
#include <cstring>

namespace reel {

inline constexpr uint32_t kCommandNone = 0;
inline constexpr uint32_t kCommandMove = 1;
// rsz:minimumSize of the port, and the most of it read per run().
inline constexpr uint32_t kCommandPortBytes = 4096;

struct Move {
    uint8_t keyframe = 0;
    uint16_t position = 0;
};

// Decodes one command; true for a well-formed move.
inline bool decode_move(int32_t value, Move& move) noexcept
{
    if (value < 0) { return false; }
    const uint32_t v = static_cast<uint32_t>(value);
    if ((v >> 24) != kCommandMove) { return false; }
    const uint32_t keyframe = (v >> 16) & 0xff;
    if (keyframe >= 64) { return false; }
    move.keyframe = static_cast<uint8_t>(keyframe);
    move.position = static_cast<uint16_t>(v & 0xffff);
    return true;
}

inline constexpr int32_t encode_move(uint32_t keyframe, uint32_t position)
{
    return static_cast<int32_t>(kCommandMove << 24 | (keyframe & 0xff) << 16 | (position & 0xffff));
}

class Commands {
public:
    static constexpr uint32_t kQueue = 16;

    // Without urid:map nothing is read.
    void map(const LV2_URID_Map* map) noexcept
    {
        if (map && map->map) {
            sequence_ = map->map(map->handle, LV2_ATOM__Sequence);
            int_ = map->map(map->handle, LV2_ATOM__Int);
        }
    }
    void connect(void* data) noexcept { port_ = static_cast<const LV2_Atom_Sequence*>(data); }
    void reset() noexcept
    {
        count_ = 0;
        last_size_ = 0;
        last_hash_ = 0;
        last_port_ = nullptr;
    }

    // At the start of every run(): queues the moves of a sequence not read
    // before. A full queue keeps the newest move in its last slot.
    void read() noexcept
    {
        if (!port_ || !sequence_ || !int_ || port_->atom.type != sequence_) { return; }
        const uint32_t size = port_->atom.size;
        if (size < sizeof(LV2_Atom_Sequence_Body) || size > kCommandPortBytes - sizeof(LV2_Atom)) { return; }
        if (size == sizeof(LV2_Atom_Sequence_Body)) {
            // Empty: the next sequence is new, even if it repeats the last.
            last_port_ = nullptr;
            return;
        }
        const uint64_t hash = fnv(port_, sizeof(LV2_Atom) + size);
        if (port_ == last_port_ && size == last_size_ && hash == last_hash_) { return; }
        last_port_ = port_;
        last_size_ = size;
        last_hash_ = hash;
        LV2_ATOM_SEQUENCE_FOREACH(port_, ev)
        {
            if (ev->body.type != int_ || ev->body.size != sizeof(int32_t)) { continue; }
            int32_t value;
            std::memcpy(&value, ev + 1, sizeof(value));
            Move move;
            if (!decode_move(value, move)) { continue; }
            moves_[count_ < kQueue ? count_++ : kQueue - 1] = move;
        }
    }

    // At a grid frame: the queued moves, in order; then empty.
    uint32_t count() const noexcept { return count_; }
    const Move& operator[](uint32_t i) const noexcept { return moves_[i]; }
    void clear() noexcept { count_ = 0; }

private:
    static uint64_t fnv(const void* data, uint32_t bytes) noexcept
    {
        const auto* p = static_cast<const uint8_t*>(data);
        uint64_t h = 1469598103934665603ull;
        for (uint32_t i = 0; i < bytes; ++i) { h = (h ^ p[i]) * 1099511628211ull; }
        return h;
    }

    const LV2_Atom_Sequence* port_ = nullptr;
    LV2_URID sequence_ = 0;
    LV2_URID int_ = 0;
    const LV2_Atom_Sequence* last_port_ = nullptr;
    uint32_t last_size_ = 0;
    uint64_t last_hash_ = 0;
    Move moves_[kQueue] = {};
    uint32_t count_ = 0;
};

} // namespace reel

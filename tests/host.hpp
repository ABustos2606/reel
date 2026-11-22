// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alejandro Bustos
#pragma once
#include "../src/ports.hpp"

#include <lv2/core/lv2.h>
#include <lv2/state/state.h>
#include <lv2/urid/urid.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace test {

void require(bool condition, const std::string& message);

class Library {
public:
    explicit Library(const char* path);
    ~Library();
    Library(const Library&) = delete;
    Library& operator=(const Library&) = delete;
    const LV2_Descriptor* descriptor = nullptr; // Reel
    const LV2_Descriptor* lfo = nullptr;        // Reel LFO
    const LV2_Descriptor* (*get)(uint32_t) = nullptr;

private:
    void* library_;
};

// urid:map, as a host gives it. Not real-time: plugins map in instantiate.
class UridMap {
public:
    UridMap();
    UridMap(const UridMap&) = delete;
    UridMap& operator=(const UridMap&) = delete;
    LV2_URID map(const char* uri);
    std::string unmap(LV2_URID urid) const { return urid && urid <= uris_.size() ? uris_[urid - 1] : ""; }
    LV2_URID_Map feature_data;
    LV2_Feature feature;

private:
    std::vector<std::string> uris_;
};

// A saved LV2 state: what save stored, key by key.
struct State {
    struct Value {
        std::vector<uint8_t> data;
        uint32_t type = 0;
        uint32_t flags = 0;
    };
    std::map<LV2_URID, Value> values;
};

inline constexpr uint32_t kMaxPorts = std::max(reel::kPortCount, reel::kLfoPortCount);

// One plugin instance, run period by period, each period cut into random
// slices as Ingen does at control changes. Every control starts at its
// Turtle default (tests/ttl_check.py checks the list in host.cpp).
class Instance {
public:
    static constexpr float kGuard = 12345.0f;
    Instance(const LV2_Descriptor* descriptor, double rate, uint32_t block = 128, bool with_map = true);
    ~Instance();
    Instance(const Instance&) = delete;
    Instance& operator=(const Instance&) = delete;

    float& control(reel::Port port) { return controls_[reel::index(port)]; }
    float& control(reel::LfoPort port) { return controls_[reel::index(port)]; }
    float& signal(reel::Port port, uint32_t i) { return buffers_[reel::index(port)][i]; }
    float& signal(reel::LfoPort port, uint32_t i) { return buffers_[reel::index(port)][i]; }
    void activate();

    // Called for every frame of a period before it runs, to write audio and
    // CV inputs (and controls, which then hold for the whole period).
    using Input = std::function<void(Instance&, uint32_t index, uint64_t frame)>;
    // Runs `frames` frames; returns every signal output port, frame by frame:
    // result[port][frame] (input ports and controls are left empty).
    std::vector<std::vector<float>> render(uint64_t frames, const Input& input = {});

    const LV2_State_Interface* state_interface() const;
    LV2_State_Status save(State& state);
    LV2_State_Status restore(const State& state);

    UridMap urids;
    const LV2_Descriptor* descriptor;
    LV2_Handle handle = nullptr;
    double rate;
    uint32_t block;
    bool lfo;
    uint32_t port_count;
    uint64_t frame = 0;
    bool random_slices = true;
    bool measure = false;
    std::vector<double> run_times; // microseconds per period, when measuring
    // t_keyframes: the strings Reel wrote, with the first frame of their
    // period. Ingen prepares the atom output once per period (an atom:Chunk of
    // its capacity) and runs every slice on it; jalv prepares it before every
    // run(): `atom_per_slice`.
    struct List {
        uint64_t frame;
        std::string text;
    };
    std::vector<List> lists;
    bool atom_per_slice = false;
    // commands: values sent before the next period. Ingen writes them into
    // one sequence per period (times from the period's start) and hands it to
    // every slice; jalv, with `atom_per_slice`, prepares the input before
    // every run(), so later slices of a period see an empty sequence.
    std::vector<int32_t> commands;
    bool record_lists = true; // off where the test counts allocations
    static constexpr uint32_t kAtomBytes = 4096; // rsz:minimumSize

private:
    bool is_signal(uint32_t p) const { return lfo ? reel::is_lfo_signal(p) : reel::is_signal(p); }
    bool is_output(uint32_t p) const;
    void connect(uint32_t offset);
    void period();
    std::array<float, kMaxPorts> controls_{};
    std::array<std::vector<float>, kMaxPorts> buffers_;
    std::vector<uint64_t> atom_; // 8-byte aligned
    std::vector<uint64_t> commands_atom_;
    LV2_URID chunk_ = 0, sequence_ = 0, string_ = 0, int_ = 0;
    void prepare_commands(bool with_events);
    void prepare_atom();
    void read_atom();
    uint32_t rng_ = 0x6A09E667;
};

} // namespace test

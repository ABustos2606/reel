// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alejandro Bustos
#include "host.hpp"

#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>

#include <chrono>
#include <cstring>
#include <dlfcn.h>
#include <stdexcept>

namespace test {

using reel::LfoPort;
using reel::Port;

void require(bool condition, const std::string& message)
{
    if (!condition) { throw std::runtime_error(message); }
}

Library::Library(const char* path) : library_(dlopen(path, RTLD_NOW | RTLD_LOCAL))
{
    if (!library_) { throw std::runtime_error(dlerror()); }
    get = reinterpret_cast<const LV2_Descriptor* (*)(uint32_t)>(dlsym(library_, "lv2_descriptor"));
    require(get, "lv2_descriptor export");
    descriptor = get(0);
    lfo = get(1);
    require(descriptor && lfo, "descriptors 0 and 1");
}

Library::~Library() { dlclose(library_); }

UridMap::UridMap()
    : feature_data{this, [](LV2_URID_Map_Handle h, const char* uri) { return static_cast<UridMap*>(h)->map(uri); }},
      feature{LV2_URID__map, &feature_data}
{
}

LV2_URID UridMap::map(const char* uri)
{
    for (size_t i = 0; i < uris_.size(); ++i) {
        if (uris_[i] == uri) { return static_cast<LV2_URID>(i + 1); }
    }
    uris_.emplace_back(uri);
    return static_cast<LV2_URID>(uris_.size());
}

Instance::Instance(const LV2_Descriptor* d, double sample_rate, uint32_t size, bool with_map)
    : descriptor(d), rate(sample_rate), block(size), lfo(std::strcmp(d->URI, reel::kLfoURI) == 0),
      port_count(lfo ? reel::kLfoPortCount : reel::kPortCount)
{
    const LV2_Feature* with[] = {&urids.feature, nullptr};
    const LV2_Feature* without[] = {nullptr};
    handle = d->instantiate(d, rate, ".", with_map ? with : without);
    require(handle, "instantiate at " + std::to_string(rate) + " Hz");
    for (auto& buffer : buffers_) { buffer.assign(size + 1, 0.0f); }
    atom_.assign(kAtomBytes / sizeof(uint64_t), 0);
    chunk_ = urids.map(LV2_ATOM__Chunk);
    sequence_ = urids.map(LV2_ATOM__Sequence);
    string_ = urids.map(LV2_ATOM__String);
    int_ = urids.map(LV2_ATOM__Int);
    commands_atom_.assign(kAtomBytes / sizeof(uint64_t), 0);
    prepare_commands(false);
    // The Turtle defaults that are not 0, one assignment per line: ttl_check
    // reads them.
    if (lfo) {
        // lfo-defaults-begin
        control(LfoPort::rate) = 0.5f;
        control(LfoPort::shape_spread) = 0.5f;
        control(LfoPort::spread) = 0.5f;
        control(LfoPort::coupling) = 0.5f;
        control(LfoPort::bpm) = 120.0f;
        control(LfoPort::note) = 3.0f;
        // lfo-defaults-end
    } else {
        // reel-defaults-begin
        control(Port::frame_cv_amount) = 1.0f;
        control(Port::ch_1) = 1.0f;
        control(Port::ch_2) = 1.0f;
        control(Port::ch_3) = 1.0f;
        control(Port::ch_4) = 1.0f;
        control(Port::easing_1) = 1.0f;
        control(Port::easing_2) = 1.0f;
        control(Port::easing_3) = 1.0f;
        control(Port::easing_4) = 1.0f;
        control(Port::enabled) = 1.0f;
        // reel-defaults-end
    }
    // Hosts connect every port before activating (Ingen does).
    connect(0);
    activate();
}

Instance::~Instance()
{
    if (descriptor->deactivate) { descriptor->deactivate(handle); }
    descriptor->cleanup(handle);
}

void Instance::activate()
{
    if (descriptor->activate) { descriptor->activate(handle); }
}

bool Instance::is_output(uint32_t p) const
{
    if (lfo) { return p >= reel::index(LfoPort::out_1) && p <= reel::index(LfoPort::out_4); }
    return (p >= reel::index(Port::out_1) && p <= reel::index(Port::mix)) ||
           (p >= reel::index(Port::level_1) && p <= reel::index(Port::trigger));
}

void Instance::connect(uint32_t offset)
{
    for (uint32_t p = 0; p < port_count; ++p) {
        if (!lfo && p == reel::index(Port::t_keyframes)) {
            descriptor->connect_port(handle, p, atom_.data());
            continue;
        }
        if (!lfo && p == reel::index(Port::commands)) {
            descriptor->connect_port(handle, p, commands_atom_.data());
            continue;
        }
        descriptor->connect_port(handle, p, is_signal(p) ? buffers_[p].data() + offset : &controls_[p]);
    }
}

void Instance::period()
{
    for (uint32_t p = 0; p < port_count; ++p) { buffers_[p][block] = kGuard; }
    prepare_atom();
    prepare_commands(true);
    uint32_t offset = 0;
    double duration = 0;
    while (offset < block) {
        rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5;
        const uint32_t count = random_slices ? std::min(1 + rng_ % 97, block - offset) : block;
        connect(offset);
        if (atom_per_slice && offset) {
            read_atom();
            prepare_atom();
            prepare_commands(false);
        }
        const auto start = std::chrono::steady_clock::now();
        descriptor->run(handle, count);
        duration += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
        offset += count;
    }
    read_atom();
    if (measure) { run_times.push_back(duration); }
    for (uint32_t p = 0; p < port_count; ++p) {
        if (buffers_[p][block] != kGuard) {
            throw std::runtime_error("write past the end of a period, port " + std::to_string(p));
        }
    }
}

void Instance::prepare_atom()
{
    auto* atom = reinterpret_cast<LV2_Atom*>(atom_.data());
    atom->type = chunk_;
    atom->size = kAtomBytes - sizeof(LV2_Atom);
}

// The commands input: the values sent since the last period, one event each
// at its index's frame (or an empty sequence).
void Instance::prepare_commands(bool with_events)
{
    auto* seq = reinterpret_cast<LV2_Atom_Sequence*>(commands_atom_.data());
    seq->atom.type = sequence_;
    seq->atom.size = sizeof(LV2_Atom_Sequence_Body);
    seq->body.unit = 0;
    seq->body.pad = 0;
    if (!with_events) { return; }
    uint32_t n = 0;
    for (int32_t value : commands) {
        const uint32_t event = sizeof(LV2_Atom_Event) + lv2_atom_pad_size(sizeof(int32_t));
        require(sizeof(LV2_Atom) + seq->atom.size + event <= kAtomBytes, "commands: room");
        auto* bytes = reinterpret_cast<uint8_t*>(commands_atom_.data());
        auto* ev = reinterpret_cast<LV2_Atom_Event*>(bytes + sizeof(LV2_Atom) + seq->atom.size);
        ev->time.frames = std::min<uint32_t>(n++, block - 1);
        ev->body.size = sizeof(int32_t);
        ev->body.type = int_;
        std::memcpy(ev + 1, &value, sizeof(value));
        seq->atom.size += event;
    }
    commands.clear();
}

// The strings in the sequence Reel wrote (nothing if it wrote none).
void Instance::read_atom()
{
    if (lfo || !record_lists) { return; }
    const auto* seq = reinterpret_cast<const LV2_Atom_Sequence*>(atom_.data());
    if (seq->atom.type != sequence_) { return; }
    require(seq->atom.size >= sizeof(LV2_Atom_Sequence_Body) && seq->atom.size <= kAtomBytes - sizeof(LV2_Atom),
            "t_keyframes: sequence size");
    LV2_ATOM_SEQUENCE_FOREACH(seq, ev)
    {
        require(ev->body.type == string_ && ev->body.size > 0, "t_keyframes: atom:String events");
        const char* text = reinterpret_cast<const char*>(ev + 1);
        require(text[ev->body.size - 1] == 0, "t_keyframes: NUL-terminated");
        lists.push_back({frame, std::string(text)});
    }
}

std::vector<std::vector<float>> Instance::render(uint64_t frames, const Input& input)
{
    std::vector<std::vector<float>> output(port_count);
    for (uint32_t p = 0; p < port_count; ++p) {
        if (is_output(p)) { output[p].reserve(frames); }
    }
    uint64_t done = 0;
    while (done < frames) {
        if (input) {
            for (uint32_t i = 0; i < block; ++i) { input(*this, i, frame + i); }
        }
        period();
        const uint64_t count = std::min<uint64_t>(block, frames - done);
        for (uint32_t p = 0; p < port_count; ++p) {
            if (is_output(p)) { output[p].insert(output[p].end(), buffers_[p].begin(), buffers_[p].begin() + count); }
        }
        done += count;
        frame += block;
    }
    return output;
}

const LV2_State_Interface* Instance::state_interface() const
{
    if (!descriptor->extension_data) { return nullptr; }
    return static_cast<const LV2_State_Interface*>(descriptor->extension_data(LV2_STATE__interface));
}

LV2_State_Status Instance::save(State& state)
{
    const auto* iface = state_interface();
    require(iface, "state:interface");
    state.values.clear();
    const LV2_State_Store_Function store = [](LV2_State_Handle h, uint32_t key, const void* value, size_t size,
                                              uint32_t type, uint32_t flags) -> LV2_State_Status {
        auto& values = static_cast<State*>(h)->values;
        if (values.count(key)) { return LV2_STATE_ERR_UNKNOWN; }
        const auto* bytes = static_cast<const uint8_t*>(value);
        values[key] = {std::vector<uint8_t>(bytes, bytes + size), type, flags};
        return LV2_STATE_SUCCESS;
    };
    return iface->save(handle, store, &state, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE, nullptr);
}

LV2_State_Status Instance::restore(const State& state)
{
    const auto* iface = state_interface();
    require(iface, "state:interface");
    const LV2_State_Retrieve_Function retrieve = [](LV2_State_Handle h, uint32_t key, size_t* size, uint32_t* type,
                                                    uint32_t* flags) -> const void* {
        const auto& values = static_cast<const State*>(h)->values;
        const auto found = values.find(key);
        if (found == values.end()) { return nullptr; }
        *size = found->second.data.size();
        *type = found->second.type;
        *flags = found->second.flags;
        return found->second.data.data();
    };
    return iface->restore(handle, retrieve, const_cast<State*>(&state), LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE,
                          nullptr);
}

} // namespace test

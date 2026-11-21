// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alejandro Bustos
//
// Milestone 0: scaffold and allocation (implementation plan, 6.1).
#include "host.hpp"

#include "../src/keyframe_state.hpp"

#include <lv2/atom/atom.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
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

using reel::LfoPort;
using reel::Port;
using reel::index;
using reel::nth;
using test::Instance;
using test::require;
using Outputs = std::vector<std::vector<float>>;

namespace {

constexpr uint32_t kGrid = 8;

float noise(uint64_t frame, uint32_t salt)
{
    uint32_t x = static_cast<uint32_t>(frame) * 2654435761u + salt * 40503u;
    x ^= x >> 15; x *= 2246822519u; x ^= x >> 13;
    return static_cast<float>(x % 2001) / 2000.0f - 0.5f;
}

// Different noise on each of the four inputs.
void inputs(Instance& instance, uint32_t i, uint64_t frame)
{
    for (uint32_t c = 0; c < reel::kChannels; ++c) { instance.signal(nth(Port::in_1, c), i) = noise(frame, c); }
}

const std::vector<float>& out(const Outputs& o, Port port) { return o[index(port)]; }

void require_finite(const Outputs& outputs)
{
    for (const auto& port : outputs) {
        for (float x : port) { require(std::isfinite(x), "finite output"); }
    }
}

bool close(float a, float b, float tolerance = 1e-6f) { return std::fabs(a - b) <= tolerance; }

// A Level control as the keyframer holds it: 16 bits, 0 to 65535.
float quantised(float level) { return std::lround(level * 65535.0f) / 65535.0f; }

void test_descriptor(const test::Library& library)
{
    require(std::strcmp(library.descriptor->URI, reel::kURI) == 0, "Reel URI");
    require(std::strcmp(library.lfo->URI, reel::kLfoURI) == 0, "Reel LFO URI");
    require(!library.get(2) && !library.get(UINT32_MAX), "two descriptors");
    for (const auto* d : {library.descriptor, library.lfo}) {
        for (double rate : {0.0, -1.0, 100.0, 1e7, std::numeric_limits<double>::quiet_NaN(),
                            std::numeric_limits<double>::infinity()}) {
            require(!d->instantiate(d, rate, ".", nullptr), "reject rate " + std::to_string(rate));
        }
        for (double rate : {8000.0, 22050.0, 44100.0, 48000.0, 96000.0, 192000.0}) {
            auto h = d->instantiate(d, rate, ".", nullptr);
            require(h, "instantiate at " + std::to_string(rate));
            // Unconnected ports and unknown indices must not crash.
            d->connect_port(h, 999, nullptr);
            d->run(h, 0);
            d->run(h, 128);
            if (d->activate) { d->activate(h); }
            d->run(h, 37);
            if (d->deactivate) { d->deactivate(h); }
            d->cleanup(h);
        }
    }
    require(library.descriptor->extension_data(LV2_STATE__interface), "Reel has state:interface");
    require(!library.descriptor->extension_data("urn:unknown"), "unknown extension");
    require(!library.lfo->extension_data(LV2_STATE__interface), "Reel LFO has no state");
    std::cout << "PASS descriptor: two plugins, rates, unconnected ports, unknown indices, zero frames, "
                 "no urid:map\n";
}

// With no keyframes each Out is its input times its Level control (response
// is milestone 1: the gain is the level), Level out n is that gain, Mix is
// the sum of the Outs times Output, Trigger is 0, and the telemetry says
// there are no keyframes.
void test_levels(const LV2_Descriptor* d)
{
    const float levels[4] = {1.0f, 0.5f, 0.25f, 0.0f};
    Instance instance(d, 48000);
    for (uint32_t c = 0; c < 4; ++c) { instance.control(nth(Port::ch_1, c)) = levels[c]; }
    instance.control(Port::frame) = 0.3f;
    const auto o = instance.render(48000, inputs);
    require_finite(o);
    for (uint32_t i = 0; i < 48000; ++i) {
        float sum = 0;
        for (uint32_t c = 0; c < 4; ++c) {
            const float expected = noise(i, c) * quantised(levels[c]);
            require(out(o, nth(Port::out_1, c))[i] == expected, "Out " + std::to_string(c + 1) + " = In times Level");
            require(out(o, nth(Port::level_1, c))[i] == quantised(levels[c]), "Level out " + std::to_string(c + 1));
            sum += expected;
        }
        require(close(out(o, Port::mix)[i], sum), "Mix is the sum of the Outs");
        require(out(o, Port::trigger)[i] == 0.0f, "Trigger silent");
    }
    require(instance.control(Port::t_count) == 0 && instance.control(Port::t_active) == -1 &&
                instance.control(Port::t_nearest) == -1 && instance.control(Port::t_step) == 0,
            "telemetry: no keyframes");
    require(close(instance.control(Port::t_frame), std::lround(0.3 * 65535) / 65535.0f), "t_frame is Frame");

    // Output -6 dB, after its 10 ms smoothing: Mix only.
    instance.control(Port::output) = -6;
    const auto quiet = instance.render(9600, inputs);
    const float gain = std::pow(10.0f, -6.0f / 20.0f);
    // 15 time constants in: settled (a float one-pole that stalls short of
    // its target is about 1e-5 off). The plugin's powf and the compiler's
    // may differ in the last bit.
    for (uint32_t i = 7200; i < 9600; ++i) {
        float sum = 0;
        for (uint32_t c = 0; c < 4; ++c) { sum += out(quiet, nth(Port::out_1, c))[i]; }
        require(std::fabs(out(quiet, Port::mix)[i] - sum * gain) <= 1e-6f * std::fabs(sum),
                "Output scales the Mix: frame " + std::to_string(i) + ", " + std::to_string(out(quiet, Port::mix)[i]) +
                    " for " + std::to_string(sum * gain));
    }
    // 1 ms in, the Mix gain is still on its way down (10 ms smoothing).
    float sum = 0;
    for (uint32_t c = 0; c < 4; ++c) { sum += out(quiet, nth(Port::out_1, c))[48]; }
    const float ratio = out(quiet, Port::mix)[48] / sum;
    require(ratio > 0.9f && ratio < 0.99f, "Output glides: gain " + std::to_string(ratio) + " after 1 ms");
    std::cout << "PASS levels: Out n = In n times Level n, Level outs, Mix and Output, Trigger 0, telemetry\n";
}

// A Level change glides to the new gain within one 8-frame grid step, in
// equal steps, whatever the slicing.
void test_level_change(const LV2_Descriptor* d)
{
    Instance instance(d, 48000);
    instance.render(1000);
    instance.control(Port::ch_2) = 0.0f;
    const auto o = instance.render(1000);
    const auto& level = out(o, Port::level_2);
    const uint32_t start = (kGrid - instance.frame % kGrid) % kGrid; // frames to the next grid frame
    for (uint32_t i = 0; i < 1000; ++i) {
        float expected = 0;
        if (i < start) {
            expected = 1;
        } else if (i < start + kGrid) {
            expected = 1.0f - static_cast<float>(i - start + 1) / kGrid;
        }
        require(close(level[i], expected), "Level out 2 glides over one grid step at frame " + std::to_string(i));
    }
    std::cout << "PASS level change: one 8-frame linear glide, on the grid counted from activate\n";
}

// Frame control plus Frame CV times Frame CV amount, clamped: shown on t_frame.
void test_frame(const LV2_Descriptor* d)
{
    const auto effective = [&](float frame, float cv, float amount) {
        Instance instance(d, 48000);
        instance.control(Port::frame) = frame;
        instance.control(Port::frame_cv_amount) = amount;
        instance.render(4800, [cv](Instance& in, uint32_t i, uint64_t) { in.signal(Port::frame_cv, i) = cv; });
        return instance.control(Port::t_frame);
    };
    require(close(effective(0.2f, 0.5f, 1.0f), 0.7f, 1e-4f), "Frame + Frame CV");
    require(close(effective(0.2f, 0.5f, -1.0f), 0.0f), "Frame CV amount -1, clamped at 0");
    require(close(effective(0.8f, 0.5f, 1.0f), 1.0f), "clamped at 1");
    require(close(effective(0.2f, 0.5f, 0.0f), 0.2f, 1e-4f), "Frame CV amount 0");
    std::cout << "PASS frame: Frame + Frame CV times its amount, clamped to [0, 1], on t_frame\n";
}

// Level changes, Frame CV moving: identical output for whole periods, random
// slices and a 37-frame period; activate restarts exactly.
void performance(Instance& in, uint32_t i, uint64_t frame)
{
    inputs(in, i, frame);
    in.signal(Port::frame_cv, i) = static_cast<float>(0.4 * std::sin(frame * 0.001));
    // Controls change only where a period starts for both 128- and 37-frame
    // periods, so every host sees them change at the same frames.
    constexpr uint64_t kChange = 128 * 37;
    if (i == 0 && frame % kChange == 0) {
        const uint64_t n = frame / kChange;
        in.control(Port::ch_1) = static_cast<float>(0.5 + 0.5 * std::sin(n * 0.7));
        in.control(Port::ch_3) = n % 2 ? 0.2f : 0.9f;
        in.control(Port::output) = n % 3 ? -12.0f : 3.0f;
        in.control(Port::frame) = static_cast<float>(n % 5) / 4;
    }
}

void test_slicing(const LV2_Descriptor* d)
{
    for (double rate : {48000.0, 44100.0}) {
        Instance whole(d, rate, 128), sliced(d, rate, 128), small(d, rate, 37);
        whole.random_slices = false;
        const uint64_t frames = static_cast<uint64_t>(rate);
        const auto a = whole.render(frames, performance);
        require_finite(a);
        require(a == sliced.render(frames, performance), "random slices change nothing");
        require(a == small.render(frames, performance), "a 37-frame period changes nothing");
        whole.activate();
        whole.frame = 0;
        require(whole.render(frames, performance) == a, "activate restarts exactly");
    }
    std::cout << "PASS slicing: whole periods, random slices and 37-frame periods agree; activate restarts\n";
}

// Two instances with different settings, on two threads at once, each give
// what they give alone.
void test_instances(const LV2_Descriptor* d)
{
    const auto settings = [](Instance& in, float level) {
        for (uint32_t c = 0; c < 4; ++c) { in.control(nth(Port::ch_1, c)) = level * (c + 1) / 4; }
    };
    Instance a_alone(d, 48000), b_alone(d, 48000), a(d, 48000), b(d, 48000);
    settings(a_alone, 0.3f);
    settings(a, 0.3f);
    settings(b_alone, 0.9f);
    settings(b, 0.9f);
    const auto expected_a = a_alone.render(96000, performance);
    const auto expected_b = b_alone.render(96000, performance);
    Outputs got_a, got_b;
    std::thread ta([&] { got_a = a.render(96000, performance); });
    std::thread tb([&] { got_b = b.render(96000, performance); });
    ta.join();
    tb.join();
    require(got_a == expected_a && got_b == expected_b, "instances share nothing");
    std::cout << "PASS instances: two instances on two threads keep their own settings\n";
}

void test_nonfinite(const LV2_Descriptor* d)
{
    Instance instance(d, 48000);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    for (Port p : {Port::frame, Port::frame_cv_amount, Port::ch_1, Port::ch_2, Port::output, Port::easing_1,
                   Port::response_1, Port::mode, Port::enabled, Port::add}) {
        instance.control(p) = nan;
    }
    instance.control(Port::ch_3) = inf;
    instance.control(Port::ch_4) = -inf;
    const auto o = instance.render(48000, [&](Instance& in, uint32_t i, uint64_t frame) {
        in.signal(Port::in_1, i) = frame % 3 ? nan : inf;
        in.signal(Port::in_2, i) = -inf;
        in.signal(Port::in_3, i) = 1e30f;
        in.signal(Port::frame_cv, i) = frame % 2 ? inf : nan;
    });
    require_finite(o);
    require(close(instance.control(Port::t_frame), 0.0f), "non-finite Frame falls back to 0");
    // A non-finite Level falls back to its default, 1.
    for (uint32_t c = 0; c < 4; ++c) {
        require(out(o, nth(Port::level_1, c)).back() == 1.0f, "non-finite Level reads its default, 1");
    }
    std::cout << "PASS nonfinite: NaN, infinite and huge inputs and controls leave every output finite\n";
}

// The state stub: save writes reel#version and no list (there are no
// keyframes yet); restore checks a list and rejects malformed ones.
void test_state(const LV2_Descriptor* d)
{
    Instance instance(d, 48000);
    test::State state;
    require(instance.save(state) == LV2_STATE_SUCCESS, "save");
    const LV2_URID version = instance.urids.map(reel::kStateVersionURI);
    const LV2_URID keyframes = instance.urids.map(reel::kStateKeyframesURI);
    const LV2_URID atom_int = instance.urids.map(LV2_ATOM__Int);
    const LV2_URID atom_vector = instance.urids.map(LV2_ATOM__Vector);
    require(state.values.size() == 1 && state.values.count(version), "save stores only the version");
    const auto& v = state.values[version];
    int32_t saved_version = 0;
    require(v.type == atom_int && v.data.size() == 4, "version is an atom:Int");
    std::memcpy(&saved_version, v.data.data(), 4);
    require(saved_version == 1, "version 1");
    require(v.flags & LV2_STATE_IS_POD && v.flags & LV2_STATE_IS_PORTABLE, "POD and portable");
    require(instance.restore(state) == LV2_STATE_SUCCESS, "restore what was saved");
    require(instance.restore(test::State{}) == LV2_STATE_SUCCESS, "restore with no keys");

    const auto vector = [&](std::vector<int32_t> ints, LV2_URID child = 0) {
        test::State s = state;
        LV2_Atom_Vector_Body body{4, child ? child : atom_int};
        std::vector<uint8_t> data(sizeof(body) + ints.size() * 4);
        std::memcpy(data.data(), &body, sizeof(body));
        if (!ints.empty()) { std::memcpy(data.data() + sizeof(body), ints.data(), ints.size() * 4); }
        s.values[keyframes] = {data, atom_vector, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE};
        return s;
    };
    std::vector<int32_t> two = {100, 0, 1, 2, 3, 4, 60000, 1, 65535, 0, 7, 8};
    require(instance.restore(vector(two)) == LV2_STATE_SUCCESS, "restore a valid list");
    require(instance.restore(vector({})) == LV2_STATE_SUCCESS, "restore an empty list");
    std::vector<int32_t> sixty_five;
    for (int32_t k = 0; k < 65; ++k) { sixty_five.insert(sixty_five.end(), {k * 1000, k, 0, 0, 0, 0}); }
    std::vector<int32_t> sixty_four(sixty_five.begin(), sixty_five.end() - 6);
    require(instance.restore(vector(sixty_four)) == LV2_STATE_SUCCESS, "64 keyframes");
    const std::pair<std::vector<int32_t>, const char*> bad[] = {
        {{100, 0, 1, 2, 3}, "length not a multiple of six"},
        {sixty_five, "65 keyframes"},
        {{100, 0, 1, 2, 3, 4, 100, 1, 1, 2, 3, 4}, "equal positions"},
        {{200, 0, 1, 2, 3, 4, 100, 1, 1, 2, 3, 4}, "positions decreasing"},
        {{100, 0, 1, 2, 3, 65536}, "level above 65535"},
        {{-1, 0, 1, 2, 3, 4}, "negative position"},
    };
    for (const auto& [ints, what] : bad) {
        require(instance.restore(vector(ints)) == LV2_STATE_ERR_BAD_TYPE, std::string("rejects ") + what);
    }
    require(instance.restore(vector(two, atom_vector)) == LV2_STATE_ERR_BAD_TYPE, "rejects a vector of non-Ints");
    test::State wrong_type = vector(two);
    wrong_type.values[keyframes].type = atom_int;
    require(instance.restore(wrong_type) == LV2_STATE_ERR_BAD_TYPE, "rejects a keyframes value that is no vector");
    test::State newer = state;
    const int32_t two_version = 2;
    std::memcpy(newer.values[version].data.data(), &two_version, 4);
    require(instance.restore(newer) == LV2_STATE_ERR_BAD_TYPE, "rejects a newer layout");

    // The parser and serialiser agree, through the state functions directly.
    reel::StateKeys keys{version, keyframes, atom_int, atom_vector};
    reel::KeyframeList list;
    const auto parsed_two = vector(two).values[keyframes];
    require(reel::parse_keyframes(keys, parsed_two.data.data(), parsed_two.data.size(), atom_vector, list) ==
                    reel::ParseResult::ok && list.count == 2 && list.keyframes[1].timestamp == 60000 &&
                list.keyframes[1].values[1] == 0 && list.keyframes[1].values[3] == 8,
            "parse_keyframes");
    uint8_t buffer[reel::kVectorBytes];
    const size_t size = reel::serialise_keyframes(keys, list, buffer);
    require(size == parsed_two.data.size() && std::memcmp(buffer, parsed_two.data.data(), size) == 0,
            "serialise_keyframes gives back what was parsed");
    test::State saved;
    const auto store = [](LV2_State_Handle h, uint32_t key, const void* value, size_t n, uint32_t type,
                          uint32_t flags) -> LV2_State_Status {
        const auto* bytes = static_cast<const uint8_t*>(value);
        static_cast<test::State*>(h)->values[key] = {std::vector<uint8_t>(bytes, bytes + n), type, flags};
        return LV2_STATE_SUCCESS;
    };
    require(reel::save_keyframes(keys, list, store, &saved) == LV2_STATE_SUCCESS && saved.values.size() == 2 &&
                saved.values[keyframes].data == parsed_two.data,
            "save_keyframes stores a list when there is one");

    // Without urid:map, Reel runs and declines to save.
    Instance unmapped(d, 48000, 128, false);
    require_finite(unmapped.render(4800, inputs));
    test::State none;
    require(unmapped.save(none) == LV2_STATE_ERR_NO_FEATURE && none.values.empty(), "no urid:map: no state");
    std::cout << "PASS state stub: version 1 saved, no list; valid lists accepted, malformed ones rejected; "
                 "serialiser round trip; no urid:map\n";
}

void test_lfo(const LV2_Descriptor* d)
{
    Instance lfo(d, 48000);
    const auto o = lfo.render(48000, [](Instance& in, uint32_t i, uint64_t frame) {
        in.signal(LfoPort::rate_cv, i) = static_cast<float>(std::sin(frame * 0.001));
    });
    for (uint32_t c = 0; c < 4; ++c) {
        for (float x : o[index(nth(LfoPort::out_1, c))]) { require(x == 0.0f, "Reel LFO outputs 0 until milestone 2"); }
    }
    std::cout << "PASS lfo: Reel LFO loads and runs; outputs 0 (milestone 2 drives them)\n";
}

void test_allocation(const LV2_Descriptor* d, const LV2_Descriptor* lfo_d)
{
#if REEL_COUNT_ALLOCATIONS
    // run() on a thread that has never run Reel, with controls changing, and
    // save and restore called from another thread meanwhile.
    Instance instance(d, 48000, 128);
    Instance lfo(lfo_d, 48000, 128);
    test::State state;
    instance.save(state);
    std::atomic<bool> done{false};
    std::thread saver([&] {
        test::State s;
        while (!done.load()) {
            instance.save(s);
            instance.restore(state);
        }
    });
    int during = 0;
    std::thread worker([&] {
        counting = true;
        instance.render(48000, performance);
        lfo.render(48000);
        counting = false;
        during = allocations.load();
    });
    worker.join();
    done = true;
    saver.join();
    // render() itself allocates its result vectors (one per output port of
    // each plugin, and the outer vectors); nothing else may.
    const int expected = 2 + 10 + 4;
    require(during <= expected, "run() allocates: " + std::to_string(during - expected) + " extra");
    std::cout << "PASS allocation: no allocation in run() on a new thread, with saves and restores alongside; "
                 "Reel LFO too\n";
#else
    (void)d;
    (void)lfo_d;
    std::cout << "SKIP allocation: sanitizer build\n";
#endif
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) { std::cerr << "usage: test_scaffold reel.so\n"; return 1; }
    try {
        test::Library library(argv[1]);
        test_descriptor(library);
        test_levels(library.descriptor);
        test_level_change(library.descriptor);
        test_frame(library.descriptor);
        test_slicing(library.descriptor);
        test_instances(library.descriptor);
        test_nonfinite(library.descriptor);
        test_state(library.descriptor);
        test_lfo(library.lfo);
        test_allocation(library.descriptor, library.lfo);
    } catch (const std::exception& e) {
        std::cerr << "FAIL scaffold: " << e.what() << '\n';
        return 1;
    }
    return 0;
}

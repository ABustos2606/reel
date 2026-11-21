// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alejandro Bustos
//
// Milestone 2: Reel LFO (implementation plan, 4.5 and 6.1). The plugin is
// compared with the vendored PolyLfo driven directly at 24 kHz.
#include "host.hpp"

#include "../src/vca.hpp"

#include "frames/poly_lfo.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>

using reel::index;
using reel::LfoPort;
using reel::nth;
using test::Instance;
using test::require;
using Outputs = std::vector<std::vector<float>>;

namespace {

uint16_t knob(float x) { return static_cast<uint16_t>(std::lround(std::clamp(x, 0.0f, 1.0f) * 65535.0f)); }

// The controls of one test, as functions of the frame: they change once per
// period, as a host's controls do, and Rate CV moves every frame.
struct Controls {
    float rate, shape, shape_spread, spread, coupling, rate_cv;
};
using Script = Controls (*)(uint64_t period, uint64_t frame);

// PolyLfo driven directly: rendered at every 24 kHz tick, counted here from
// the frame number rather than accumulated, and sampled at the host rate
// between the last two renders. The first render is at the first frame.
Outputs reference(double rate, uint32_t block, uint64_t frames, Script script)
{
    frames::PolyLfo lfo;
    std::memset(static_cast<void*>(&lfo), 0, sizeof(lfo));
    lfo.Init();
    const double step = 24000.0 / rate;
    Outputs out(4);
    float previous[4] = {}, current[4] = {};
    const auto render = [&](const Controls& c) {
        lfo.set_shape(knob(c.shape));
        lfo.set_shape_spread(knob(c.shape_spread));
        lfo.set_spread(knob(c.spread));
        lfo.set_coupling(knob(c.coupling));
        lfo.Render(knob(c.rate + c.rate_cv));
        for (uint8_t ch = 0; ch < 4; ++ch) { current[ch] = reel::dac_gain(lfo.dac_code(ch)); }
    };
    for (uint64_t n = 0; n < frames; ++n) {
        const Controls c = script(n / block, n);
        if (n == 0) {
            render(c);
            std::copy(current, current + 4, previous);
        }
        const double before = n * step, after = (n + 1) * step;
        for (auto ticks = static_cast<uint64_t>(std::floor(after)) - static_cast<uint64_t>(std::floor(before));
             ticks > 0; --ticks) {
            std::copy(current, current + 4, previous);
            render(c);
        }
        const float t = static_cast<float>(after - std::floor(after));
        for (uint8_t ch = 0; ch < 4; ++ch) { out[ch].push_back(previous[ch] + (current[ch] - previous[ch]) * t); }
    }
    return out;
}

Outputs plugin(const LV2_Descriptor* d, double rate, uint32_t block, uint64_t frames, Script script,
               bool slices = true)
{
    Instance in(d, rate, block);
    in.random_slices = slices;
    const auto all = in.render(frames, [script](Instance& i, uint32_t k, uint64_t frame) {
        const Controls c = script(frame / i.block, frame);
        i.control(LfoPort::rate) = c.rate;
        i.control(LfoPort::shape) = c.shape;
        i.control(LfoPort::shape_spread) = c.shape_spread;
        i.control(LfoPort::spread) = c.spread;
        i.control(LfoPort::coupling) = c.coupling;
        i.signal(LfoPort::rate_cv, k) = c.rate_cv;
    });
    Outputs out;
    for (uint32_t c = 0; c < 4; ++c) { out.push_back(all[index(nth(LfoPort::out_1, c))]); }
    return out;
}

double max_difference(const Outputs& a, const Outputs& b)
{
    double worst = 0;
    for (size_t c = 0; c < a.size(); ++c) {
        require(a[c].size() == b[c].size(), "lengths");
        for (size_t i = 0; i < a[c].size(); ++i) { worst = std::max(worst, std::abs(double(a[c][i]) - b[c][i])); }
    }
    return worst;
}

// Every control moving, Rate CV a slow sine reaching past both ends.
Controls moving(uint64_t period, uint64_t frame)
{
    const double p = static_cast<double>(period);
    return {static_cast<float>(0.55 + 0.35 * std::sin(p * 0.05)), static_cast<float>(0.5 + 0.5 * std::sin(p * 0.031)),
            static_cast<float>(0.5 + 0.5 * std::sin(p * 0.017)), static_cast<float>(0.5 + 0.5 * std::sin(p * 0.023)),
            static_cast<float>(0.5 + 0.5 * std::sin(p * 0.041)), static_cast<float>(0.6 * std::sin(frame * 2e-5))};
}

// The rate of an output from its rising crossings of the middle of its range,
// interpolated between frames; 0 with fewer than two.
double frequency(const std::vector<float>& x, double rate)
{
    const auto [lo, hi] = std::minmax_element(x.begin(), x.end());
    const float middle = (*lo + *hi) / 2;
    double first = -1, last = -1;
    int crossings = 0;
    for (size_t i = 1; i < x.size(); ++i) {
        if (x[i - 1] < middle && x[i] >= middle) {
            const double at = i - 1 + (middle - x[i - 1]) / (x[i] - x[i - 1]);
            if (first < 0) { first = at; }
            last = at;
            ++crossings;
        }
    }
    return crossings < 2 ? 0 : (crossings - 1) * rate / (last - first);
}

double table_frequency(uint16_t code)
{
    return frames::PolyLfo::FrequencyToPhaseIncrement(code) * 24000.0 / 4294967296.0;
}

void test_reference(const LV2_Descriptor* d)
{
    for (double rate : {48000.0, 44100.0, 96000.0, 16000.0}) {
        const uint64_t frames = static_cast<uint64_t>(3 * rate);
        const Outputs expected = reference(rate, 128, frames, moving);
        const Outputs whole = plugin(d, rate, 128, frames, moving, false);
        const double worst = max_difference(whole, expected);
        require(worst < 1e-5, "at " + std::to_string(rate) + " Hz the outputs differ from PolyLfo's by " +
                                  std::to_string(worst));
        require(whole == plugin(d, rate, 128, frames, moving), "random slices change nothing");
        for (const auto& out : whole) {
            for (float x : out) { require(std::isfinite(x) && x > 0 && x <= 1, "outputs from 1/4096 to 1"); }
        }
    }
    std::cout << "PASS lfo: the four outputs equal PolyLfo driven directly at 24 kHz, sampled at 48, 44.1, 96 and "
                 "16 kHz, with every control and Rate CV moving; slicing changes nothing; 1/4096 to 1\n";
}

void test_rates(const LV2_Descriptor* d)
{
    // Spread at its centre: four outputs at one rate and phase.
    static float fixed_rate;
    const Script fixed = [](uint64_t, uint64_t) { return Controls{fixed_rate, 0, 0.5f, 0.5f, 0.5f, 0}; };
    struct Case {
        float rate;
        double hz, host, seconds;
    };
    for (const Case c : {Case{0, 0.0134, 8000, 160}, Case{1, 110, 48000, 2}}) {
        fixed_rate = c.rate;
        const Outputs out = plugin(d, c.host, 128, static_cast<uint64_t>(c.seconds * c.host), fixed, false);
        const double measured = frequency(out[0], c.host);
        const double table = table_frequency(knob(c.rate));
        require(std::abs(measured / table - 1) < 0.002,
                "Rate " + std::to_string(c.rate) + ": " + std::to_string(measured) + " Hz, the table gives " +
                    std::to_string(table));
        require(std::abs(measured / c.hz - 1) < 0.005,
                "Rate " + std::to_string(c.rate) + ": " + std::to_string(measured) + " Hz, not " + std::to_string(c.hz));
        require(out[1] == out[0] && out[3] == out[0], "Spread at its centre: one rate and phase");
    }
    std::cout << "PASS lfo rates: Rate 0 gives " << table_frequency(0) << " Hz and Rate 1 " << table_frequency(65535)
              << " Hz (0.0134 and 110 within 0.5%), measured on the output\n";
}

void test_clamp(const LV2_Descriptor* d)
{
    const auto run = [d](float rate, float cv) {
        static float r, v;
        r = rate;
        v = cv;
        return plugin(d, 48000, 128, 48000, [](uint64_t, uint64_t) { return Controls{r, 0.3f, 0.6f, 0.7f, 0.4f, v}; });
    };
    require(run(1, 1) == run(1, 0) && run(0.5f, 5) == run(1, 0), "Rate CV past the top is Rate 1");
    require(run(0, -1) == run(0, 0) && run(0.5f, -7) == run(0, 0), "Rate CV past the bottom is Rate 0");
    require(run(0.25f, 0.25f) == run(0.5f, 0), "Rate CV adds to Rate");
    std::cout << "PASS lfo clamp: Rate plus Rate CV clamped to the knob's range, 0.0134 to 110 Hz\n";
}

void test_spread(const LV2_Descriptor* d)
{
    // Below its centre Spread gives each next output a higher rate: at 0, an
    // octave each (PolyLfo::Render adds 5040, an octave, per output).
    const Outputs out = plugin(d, 48000, 128, 4 * 48000,
                               [](uint64_t, uint64_t) { return Controls{0.6f, 0, 0.5f, 0, 0.5f, 0}; });
    const double f1 = frequency(out[0], 48000), f2 = frequency(out[1], 48000), f4 = frequency(out[3], 48000);
    require(std::abs(f2 / f1 - 2) < 0.01 && std::abs(f4 / f1 - 8) < 0.05,
            "Spread 0: rates " + std::to_string(f1) + ", " + std::to_string(f2) + ", " + std::to_string(f4));
    std::cout << "PASS lfo spread: below the centre each next output runs faster (an octave each at 0: " << f1 << ", "
              << f2 << " ... " << f4 << " Hz)\n";
}

void test_activate(const LV2_Descriptor* d)
{
    Instance in(d, 48000);
    const auto a = in.render(10000);
    in.render(7777);
    in.activate();
    in.frame = 0;
    require(in.render(10000) == a, "activate restarts the LFOs");
    std::cout << "PASS lfo activate: restarts from phase 0\n";
}

// Tempo sync: with Sync on, a cycle lasts the Note at the BPM; Rate is
// ignored and Rate CV still adds (one octave per 5040/65535).
void test_sync(const LV2_Descriptor* d)
{
    const auto measure = [d](float sync, float bpm, float note, float rate, float cv, double seconds) {
        Instance in(d, 48000, 128);
        in.random_slices = false;
        in.control(LfoPort::sync) = sync;
        in.control(LfoPort::bpm) = bpm;
        in.control(LfoPort::note) = note;
        in.control(LfoPort::rate) = rate;
        const auto all = in.render(static_cast<uint64_t>(seconds * 48000), [cv](Instance& i, uint32_t k, uint64_t) {
            i.signal(LfoPort::rate_cv, k) = cv;
        });
        return std::make_pair(frequency(all[index(LfoPort::out_1)], 48000), all[index(LfoPort::out_1)]);
    };
    struct Case {
        float bpm, note;
        double hz;
    };
    for (const Case c : {Case{120, 5, 2.0}, Case{90, 3, 0.375}, Case{300, 7, 20.0}, Case{30, 0, 0.015625}}) {
        const double seconds = std::max(2.0, 2.5 / c.hz);
        const double hz = measure(1, c.bpm, c.note, 0.9f, 0, seconds).first;
        require(std::abs(hz / c.hz - 1) < 0.005, "sync at " + std::to_string(c.bpm) + " BPM, note " +
                                                      std::to_string(c.note) + ": " + std::to_string(hz) + " Hz");
    }
    require(measure(1, 120, 5, 0, 0, 2).second == measure(1, 120, 5, 1, 0, 2).second, "Sync on: Rate ignored");
    const double octave = measure(1, 120, 5, 0.5f, 5040.0f / 65535.0f, 2).first;
    require(std::abs(octave / 4.0 - 1) < 0.005, "Sync on: Rate CV adds, an octave: " + std::to_string(octave));
    require(measure(0, 60, 0, 0.6f, 0, 2).second == measure(0, 200, 7, 0.6f, 0, 2).second,
            "Sync off: BPM and Note ignored");
    std::cout << "PASS lfo sync: Tempo sync gives the Note at the BPM (1/4 at 120 = 2 Hz, 1 bar at 90, 1/16 at 300, "
                 "8 bars at 30) within 0.5%; Rate ignored, Rate CV adds; off, BPM and Note ignored\n";
}

// 10 simulated minutes at 48 kHz, 128 frames, controls and Rate CV moving
// (make test-timing).
void test_timing(const LV2_Descriptor* d)
{
    Instance in(d, 48000, 128);
    in.measure = true;
    const double seconds = 600;
    in.render(static_cast<uint64_t>(seconds * 48000), [](Instance& i, uint32_t k, uint64_t frame) {
        const Controls c = moving(frame / i.block, frame);
        i.control(LfoPort::rate) = c.rate;
        i.control(LfoPort::shape) = c.shape;
        i.control(LfoPort::shape_spread) = c.shape_spread;
        i.control(LfoPort::spread) = c.spread;
        i.control(LfoPort::coupling) = c.coupling;
        i.signal(LfoPort::rate_cv, k) = c.rate_cv;
    });
    auto times = in.run_times;
    std::sort(times.begin(), times.end());
    double total = 0;
    for (double t : times) { total += t; }
    std::cout << "TIMING lfo, 48 kHz, 128 frames, 10 simulated minutes: mean " << total / times.size()
              << " us, 99.9% " << times[times.size() * 999 / 1000] << " us, worst " << times.back() << " us, load "
              << 100 * total / (seconds * 1e6) << "%\n";
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) { std::cerr << "usage: test_lfo reel.so\n"; return 1; }
    try {
        test::Library library(argv[1]);
        if (argc > 2 && std::strcmp(argv[2], "--timing") == 0) {
            test_timing(library.lfo);
            return 0;
        }
        test_reference(library.lfo);
        test_rates(library.lfo);
        test_clamp(library.lfo);
        test_spread(library.lfo);
        test_activate(library.lfo);
        test_sync(library.lfo);
    } catch (const std::exception& e) {
        std::cerr << "FAIL lfo: " << e.what() << '\n';
        return 1;
    }
    return 0;
}

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alejandro Bustos
//
// Reel LFO. Milestone 0: the plugin and its ports; the four outputs are 0.
// Milestone 2 drives frames::PolyLfo at 24 kHz (implementation plan, 4.5).
#include "reel.hpp"

#include <cmath>
#include <new>

namespace reel {
namespace {

class ReelLfo {
public:
    void connect(uint32_t index, void* data) noexcept
    {
        if (index < kLfoPortCount) { ports_[index] = static_cast<float*>(data); }
    }

    void run(uint32_t frames) noexcept
    {
        for (uint32_t c = 0; c < kChannels; ++c) {
            if (float* out = ports_[index(nth(LfoPort::out_1, c))]) {
                for (uint32_t i = 0; i < frames; ++i) { out[i] = 0.0f; }
            }
        }
    }

private:
    float* ports_[kLfoPortCount] = {};
};

LV2_Handle instantiate(const LV2_Descriptor*, double rate, const char*, const LV2_Feature* const*)
{
    if (!std::isfinite(rate) || rate < 8000 || rate > 768000) { return nullptr; }
    return new (std::nothrow) ReelLfo();
}
void connect_port(LV2_Handle h, uint32_t index, void* data) { static_cast<ReelLfo*>(h)->connect(index, data); }
void run(LV2_Handle h, uint32_t frames) { static_cast<ReelLfo*>(h)->run(frames); }
void cleanup(LV2_Handle h) { delete static_cast<ReelLfo*>(h); }
const void* extension_data(const char*) { return nullptr; }

} // namespace

const LV2_Descriptor lfo_descriptor{
    kLfoURI, instantiate, connect_port, nullptr, run, nullptr, cleanup, extension_data,
};

} // namespace reel

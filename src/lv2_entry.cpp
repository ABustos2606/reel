// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alejandro Bustos
#include "reel.hpp"

extern "C" LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index)
{
    switch (index) {
    case 0: return &reel::descriptor;
    case 1: return &reel::lfo_descriptor;
    default: return nullptr;
    }
}

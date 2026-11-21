<!-- SPDX-License-Identifier: MIT -->
# Milestone 0 validation

Recorded 21 November 2026. The scaffold is built: vendored code, Makefile, both descriptors in one `reel.so`, Turtle with the full port lists (Reel 40 ports, Reel LFO 10), `ports.hpp`, `ttl_check`, the state stub and the in-process test host. Reel evaluates the keyframer on its 8-frame grid and, with no keyframes, passes four audio inputs through four VCAs at the Level controls (`set_immediate`, as `SyncWithPots`), with the Mix, Output, the Level out CVs, Frame plus Frame CV on `t_frame`, and the other telemetry ports reporting no keyframes. Response, editing, the trigger, bypass and the keyframe list are milestone 1: those ports are declared and inert. Reel LFO loads and outputs 0 until milestone 2. The checks cover the build, the port ABI, the scaffold behaviour and loading in real hosts; they don't cover the Pi (no Pi check is asked for in milestone 0).

| Check | Result |
| --- | --- |
| `tools/vendor.sh` | eurorack `f4cfa769`, stmlib `e3bd7c9c` (eurorack's submodule at that commit); 10 files; no local patches; LF; a second run gives identical files |
| x86-64 `make -j8 test`, Debian 13 under WSL | PASS, 11 checks; GCC 14.2, LV2 1.18.10; no warnings from Reel's code |
| `make test SAN=1` | PASS; AddressSanitizer and UndefinedBehaviorSanitizer (allocation check skipped, as usual under sanitizers) |
| `make test SAN=thread` | PASS (run under `setarch -R`: TSan cannot map memory with WSL's default ASLR) |
| arm64 `make -j8 test BUILD_DIR=build/arm64 OPT="-O2 -mcpu=cortex-a76"` | PASS, 11 checks; Debian 12 root `/opt/peepo-arm64`, GCC 12.2, QEMU user emulation, from a temporary copy removed after the check |
| `make install DESTDIR=build/stage PREFIX=/opt/peepo` | PASS; `reel.so`, the three Turtle files, `LICENSE`, `THIRD_PARTY_NOTICES` |
| `lv2info` on the staged bundle | PASS; Reel 40 ports (Mixer Plugin, `urid:map` optional, `state:interface`), Reel LFO 10 (Oscillator Plugin) |
| `nm -D --defined-only reel.so`, x86-64 and arm64 | Only `lv2_descriptor` |
| `lv2lint` 0.17.151 (`~/src/lv2lint`, as built for Chamber), both plugins, `LV2_PATH` with the staged bundle and `/usr/lib/lv2` | No errors; one warning, below |
| `make test-hosts`, jalv | Reel loads; with Level 2 at 0.5 and Levels 3-4 at 0, a 0.5 tone gives Out 2 0.250, Mix 0.750, Level out 2 0.500; Reel LFO loads |
| `make test-hosts`, Peepo's Ingen, 1 and 4 threads, 1024- and 128-frame periods | Two Reels and a Reel LFO in one graph; Out 2 at Level 0.5 gives 0.250, the other Reel's Mix 2.000, Level out 2 (to a CV port) 0.500; Frame, Frame CV amount, Level, Easing, Response, Mode, Output and On save and restore; `reel#version` 1 is saved in the block's `state.ttl` and no `reel#keyframes`; the patch reloads |

`lv2lint` warns **Plugin Author Email**: no `foaf:mbox` is given. The name and GitHub homepage are there; an email address is not invented or published (as Discipline, Chamber and Drizzle).

## What the offline tests cover

- `ttl_check`: for both plugins, indices, symbols and kinds match `src/ports.hpp`; every symbol is frozen in order; names ASCII and at most 16 characters, with no upstream names; bounds and defaults; Easing labels in `EasingCurve` order with Linear the default, Mode labels; toggles; the `enabled` designation; Output in dB; integer telemetry; CV inputs -1 to 1 and CV outputs 0 to 1; license; no required features, `urid:map` optional and `state:interface` on Reel only. The C++ fallbacks for an unconnected or non-finite control and the test host's starting values agree with the Turtle defaults.
- `scaffold`: two descriptors; invalid rates rejected; 8 to 192 kHz accepted; unconnected ports, unknown indices and zero-frame runs; no `urid:map`. With no keyframes Out n is In n times Level n (at the keyframer's 16-bit resolution), Level out n is the gain, Mix the sum of the Outs times Output (smoothed 10 ms, settling exactly), Trigger 0, telemetry "no keyframes". A Level change glides linearly over one 8-frame grid step counted from `activate`. `t_frame` is Frame plus Frame CV times its amount, clamped. Identical output for whole periods, random slices and a 37-frame period with controls and Frame CV moving, at 48 and 44.1 kHz; `activate` restarts exactly; two instances on two threads keep their own settings; non-finite inputs and controls give finite outputs. The state stub: `save` stores `reel#version` 1 (POD, portable) and no list; `restore` accepts no keys, an empty list, two and 64 keyframes, and rejects a length not a multiple of six, 65 keyframes, equal or decreasing positions, values outside 0-65535, a vector of non-Ints, a non-vector and a newer version; parser and serialiser round trip. Reel LFO runs and outputs 0.
- `allocation`: no `malloc` or `calloc` inside `run()` on a new thread, with controls changing and `save` and `restore` called from another thread meanwhile; Reel LFO too.

## Found and fixed during the milestone

- A one-pole smoother in float stalls short of its target once each step rounds to nothing (about 1e-5 away for 10 ms at 48 kHz), so the Mix never reached the exact Output gain. The smoother now lands on its target when a step no longer changes it.
- `Keyframer::nearest_keyframe()` is unset until the first `Evaluate`; `activate` evaluates once, so the first `run()` reports -1.
- `nearest_keyframe()` can be one past the last keyframe; `t_nearest` is declared -1 to 64 (plan and page contract updated).

## Decisions taken in this milestone

Recorded in the spec (Decisions, 21 November 2026): the bypass question settled as recommended, with the `enabled` port at index 34, built in milestone 1. In the plan's "Updated" line: `urid:map` optional; no `reel#keyframes` key for an empty list; the C++ name `delete_` for the `delete` port.

## Not in this milestone

Keyframe editing (Add, Delete, Clear all, level edits), easing, response, the trigger, bypass, the state handover and keyframe restore, the sequencer, Reel LFO's outputs, `tools/render`, the `rt_timing` test, Carla, and any Pi measurement.

## Release

To tag `v0.0.1` on the commit that adds this record, once the work is committed. Not pushed.

# Reel implementation plan

**Written:** 20 October 2026. **Rewritten 20 November 2026** against the source, before any code: no vendored changes are needed (flash storage is compiled out by `-DTEST`), port indices (3), state with keyframe ids (3), the evaluation grid and the VCA's response curve in floating point (4.1, 4.3), editing as `ui.cc` really does it (4.2), the trigger's arming rule and the sequencer's clock (4.4), the LFO's update clock and range guard (4.5), state threading (5), the tests and the records. **Follows:** [design.md](design.md), the spec, and [peepo-page.md](peepo-page.md), the page contract. Where this plan and the spec disagree, the spec wins and this plan is fixed. **Checked against:** Frames' `keyframer.h/.cc`, `poly_lfo.h/.cc`, `ui.h/.cc`, `frames.cc`, `drivers/{adc.h,dac.h,system.cc}`, `resources.h` and `resources/lookup_tables.py` at upstream commit `f4cfa7697095a28d292bff758beb51deb49b5a86` (a checkout is in `C:\LV2-own-plugins\chamber\tmp\eurorack-src`); Ingen's `ArcImpl::can_connect` and Peepo's `src/core/model.cpp`; Loo-pi's plan and records for LV2 state under Ingen; Chamber's and Drizzle's for the build, tests and records.

**Updated 21 November 2026, milestone 0** ([record](milestone-0-validation.md)): the bypass question settled (`enabled` declared); the C++ enum names port 16 `delete_` (a keyword) for the symbol `delete`; `t_nearest` ranges -1 to 64, since `nearest_keyframe()` can name the boundary after the last keyframe; the Reel LFO's descriptor is in `reel_lfo.cpp` with no header of its own; `save` stores no `reel#keyframes` key for an empty list (section 3: a missing key means no keyframes); `urid:map` is an optional feature (without it Reel runs and saves nothing).

This plan says how to build Reel: code layout, ports, state, the VCAs, real-time rules, tests, milestones and records. Values that only use can settle are given a starting value and marked **tune**.

## 1. What Ingen's source changes

From Discipline's and Loo-pi's plans, unchanged:

1. **Engine bypass does not run the plugin.** It copies audio input *n* to audio output *n*: In 1-4 to Out 1-4 at unity, Mix silent. Hence the `enabled` port (spec, Bypass).
2. **`run()` is called in slices.** Ingen splits a period at every control change. The keyframer is evaluated on a fixed grid counted in samples from `activate`, not per call (section 4.1).
3. **Blocks run on several threads.** Instances share nothing. The vendored code has no mutable statics (checked: `palette_` and `rainbow_` are const tables).
4. **Block length** from `bufsz:nominalBlockLength` / `maxBlockLength`: not needed.
5. **LV2 state is saved and restored by Ingen** with the patch (Peepo `docs/17-patch-storage.md`; checked on the Pi on 16 October 2026). **Save runs while `run()` does** (Loo-pi's finding: Ingen saves from its own thread); restore is handed over without locks too (section 5).
6. **Ingen connects audio to CV; Peepo's interface doesn't** (`ArcImpl::can_connect` against `model.cpp`). Port types are chosen for Peepo (spec, Audio or CV).

## 2. Technology and build

- **Plain LV2 C API in C++17**, no DPF. Both plugins in one shared object, `reel.so`, in `reel.lv2/` beside `manifest.ttl`, `reel.ttl` and `reel_lfo.ttl`.
- **License:** MIT. **Dependencies:** `lv2-dev` headers only.
- **Makefile**, no configure step, no network: `make`, `make install` (`DESTDIR`, `PREFIX`, `LV2DIR`), `make test`, `make test-hosts`, `make test-timing`, `make lint`, `OPT`, `SAN=1` and `SAN=thread`, as Chamber's. Arch flags are never hard-coded; the Pi build passes `OPT="-O2 -mcpu=cortex-a76"`.
- **Flags:** `-std=c++17 -fPIC -fvisibility=hidden -Wall -Wextra`, `-DTEST` (compiles out `Keyframer`'s flash storage, as upstream's desktop builds), `-fno-fast-math` after `OPT`. The vendored code's warnings are silenced for `third_party/` only. Only `lv2_descriptor` is exported (`src/exports.map`).
- **Releases** are git tags (`v0.0.1` ...). Peepo pins a commit. The user commits and tags; records say "tag ready" until then.

### Repository layout

```
Makefile
LICENSE                      MIT
README.md                    what it is, status, build, install, credit to Frames and Emilie Gillet
reel.lv2/
    manifest.ttl
    reel.ttl
    reel_lfo.ttl
third_party/eurorack/        vendored at f4cfa769 by tools/vendor.sh, unchanged
    README.md                upstream commits, file list, "no local changes"
    frames/keyframer.{h,cc}, poly_lfo.{h,cc}, resources.{h,cc}, resources/lookup_tables.py
    stmlib/                  LICENSE, stmlib.h, utils/dsp.h
src/
    lv2_entry.cpp            lv2_descriptor(): index 0 Reel, 1 Reel LFO
    ports.hpp                port index enums, the single source in C++
    reel.hpp/.cpp            instantiate, activate, run, cleanup; the evaluation grid, bypass
    editing.hpp/.cpp         Add, Delete, Clear all, level edits, active keyframe (from ui.cc)
    vca.hpp                  the response curve in floating point, smoothed gains, mix
    trigger.hpp              the trigger's arming and pulse (from frames.cc)
    keyframe_state.hpp/.cpp  LV2 state: serialise, restore, handover (section 5)
    reel_lfo.hpp/.cpp        the second plugin
    exports.map
tools/
    vendor.sh                copies the listed upstream files at the pinned commit
    render.cpp               keyframes and a Frame sweep in, WAV of outputs and levels out
tests/
    host.hpp/.cpp            in-process LV2 host with random slicing, state and an allocation hook (from Chamber and Loo-pi)
    test_*.cpp
    ttl_check.py
    host_smoke.py            jalv and Ingen with a private dummy JACK server (make test-hosts)
docs/
    design.md, implementation-plan.md, peepo-page.md, milestone-N-validation.md
```

## 3. Ports

Port **symbols, URIs and state keys are permanent** once a patch has been saved with them. All ports are declared in milestone 0; until a port does something it is inert. Ports added later are only appended, with `lv2:minorVersion` raised. Names are short plain ASCII, at most 16 characters, and say what the control does.

### Reel: `https://github.com/ABustos2606/reel#reel`

| # | Symbol | Name | Kind | Range, default |
| --- | --- | --- | --- | --- |
| 0-3 | `in_1` ... `in_4` | In 1 ... In 4 | audio in | |
| 4-7 | `out_1` ... `out_4` | Out 1 ... Out 4 | audio out | input times gain |
| 8 | `mix` | Mix | audio out | sum of Out 1-4, times Output |
| 9 | `frame` | Frame | control | 0-1, 0 |
| 10 | `frame_cv_amount` | Frame CV amount | control | -1 to 1, 1 |
| 11-14 | `ch_1` ... `ch_4` | Level 1 ... Level 4 | control | 0-1, 1 **tune** |
| 15 | `add` | Add | toggle (momentary, as Drizzle's Seed) | 0 |
| 16 | `delete` | Delete | toggle (momentary) | 0 |
| 17 | `clear_all` | Clear all | toggle (momentary) | 0 |
| 18-21 | `easing_1` ... `easing_4` | Easing 1 ... | enumeration, integer | Step 0, Linear 1, In quartic 2, Out quartic 3, Sine 4, Bounce 5; 1 (the hardware's default) |
| 22-25 | `response_1` ... `response_4` | Response 1 ... | control | 0-1, 0 (linear, the hardware's default) |
| 26 | `mode` | Mode | enumeration, integer | Keyframes 0, Sequencer 1; 0 |
| 27 | `output` | Output | control | dB, -24 to 12, 0 |
| 28 | `frame_cv` | Frame CV | CV in | 1.0 moves Frame across its whole range |
| 29-32 | `level_1` ... `level_4` | Level out 1 ... | CV out | the gains, 0 to 1 |
| 33 | `trigger` | Trigger | CV out | 1.0 for 10 ms **tune** |
| 34 | `enabled` | On | toggle, `lv2:enabled` | 1 (the spec's bypass question, settled in milestone 0) |
| 35 | `t_frame` | Frame now | control out | effective Frame, 0-1 |
| 36 | `t_count` | Keyframes | control out | 0-64 |
| 37 | `t_active` | Active keyframe | control out | index of the active keyframe (editing target), or -1 |
| 38 | `t_nearest` | Nearest keyframe | control out | `Keyframer::nearest_keyframe()` at the effective Frame, -1 to 64 |
| 39 | `t_step` | Step | control out | sequencer step, 0-63 |

Reel's enumerations use upstream's numbers (`EasingCurve`), so the glue passes them through. Telemetry ports are read by Peepo's page only while it is open ([peepo-page.md](peepo-page.md)). The keyframe list itself (positions, ids, levels) is the open telemetry question: an atom output appended in milestone 2.

### Reel LFO: `https://github.com/ABustos2606/reel#lfo`

| # | Symbol | Name | Kind | Range, default |
| --- | --- | --- | --- | --- |
| 0 | `rate` | Rate | control | 0-1, 0.5 (1.2 Hz) **tune** |
| 1 | `shape` | Shape | control | 0-1, 0 |
| 2 | `shape_spread` | Shape spread | control | 0-1, 0.5 |
| 3 | `spread` | Spread | control | 0-1, 0.5 |
| 4 | `coupling` | Coupling | control | 0-1, 0.5 |
| 5 | `rate_cv` | Rate CV | CV in | 1.0 moves Rate across its whole range |
| 6-9 | `out_1` ... `out_4` | Out 1 ... Out 4 | CV out | 0 to 1 |

The draft's `frequency` and `phase_spread` became `rate` and `spread` before any patch existed: below its centre Spread spreads rates, not phases.

### State

| Key | Type | Content |
| --- | --- | --- |
| `reel#version` | `atom:Int` | 1; lets a later layout read old patches |
| `reel#keyframes` | `atom:Vector` of `atom:Int` | six per keyframe, in position order: position, id, level 1-4, all 0-65535 |

Restore rejects (and leaves the instance empty) a vector whose length is not a multiple of six, more than 64 keyframes, or positions not strictly increasing; a missing key means no keyframes. Easing, response, mode and the Level controls are control ports, so they are not in state.

## 4. The glue

### 4.1 Instantiate, activate and the evaluation grid

`instantiate` accepts any rate from 8 to 768 kHz and holds a `frames::Keyframer` by value. Because `Keyframer::Init` does nothing under `-DTEST`, `activate` does what the hardware's fresh `Init` does: easing Linear and response 0 for every channel (then overwritten by the ports), `Clear()`, and the saved keyframes put back if state was restored.

The keyframer is evaluated on a grid: every 8 frames counted from `activate` (6 kHz at 48 kHz; the hardware refreshes at up to 32 kHz). At each grid frame:

1. pending state, edits and button edges are applied (4.2);
2. Frame = smoothed `frame` + `frame_cv` (that frame's sample) times `frame_cv_amount`, clamped to [0, 1] and scaled to 0-65535 as `frames.cc`; in Sequencer mode Frame is the current step's position instead (4.4);
3. easing and response settings are copied from the ports into `mutable_settings(n)`; `Evaluate(frame)`;
4. the four target gains are computed (4.3), and the trigger checked (4.4).

Gains move linearly from the previous grid frame's to the new over the 8 frames. `frame` is smoothed per sample by a one-pole of 5 ms (**tune**; the hardware's Frame is unfiltered, but Peepo's encoders move in steps). Ingen splits periods only where controls change, so evaluating at fixed sample positions gives the same output however a period is sliced (the `slicing` test), with no added latency.

### 4.2 Editing

`editing.cpp` copies `ui.cc`, applied at grid frames:

- **Active keyframe:** `FindNearestKeyframe(frame_knob, 2048)` where `frame_knob` is the `frame` control alone (unsmoothed, no CV), recomputed when it changes by at least 1/1024 (the hardware's ADC event step), and after Add, Delete, Clear all or a restore.
- **Level edits:** a change of `ch_n` by at least 1/1024 from the last value seen sets the active keyframe's level `n`, or, with no active keyframe, `set_immediate(n)`. The first `run()` after instantiate or restore takes the port values as the baseline: they are not edits. With no keyframes, `set_immediate` follows the Level controls at every grid frame (`SyncWithPots`).
- **Add**, on a rising edge: with no active keyframe, `AddKeyframe(frame_knob, levels)` with the four Level controls; otherwise nothing. Full at 64: nothing.
- **Delete**, on a rising edge: `RemoveKeyframe` of the active keyframe; then `SyncWithPots`.
- **Clear all**, on a rising edge: `Clear()`, `SyncWithPots`.
- Every change to the list is published for `save` and telemetry (section 5).

The hidden gestures (five Adds, ten Deletes), the long-press edit modes, flash save and the "abandoned" keyframe lock are not ported.

### 4.3 VCAs and response

`vca.hpp` computes in floating point what `ConvertToDacCode` and the 2164 do, without the 12-bit DAC's steps. For level `x` (0 to 1) and response `r` (`response_n` times 255, rounded):

- control voltage, linear: `v_lin = -(2/3) log10(max(x, 1/4096))`;
- control voltage, exponential: `v_exp = 2.5 (1 - x)`;
- `b = round(32767 (r / 255)^1.5) / 32768` (`lut_response_balance`);
- gain = `10^(-1.5 (v_lin + b (v_exp - v_lin)))`, and 0 when `x` is 0 (spec, Levels; the hardware's floor is -72 to -75 dB).

Out n = In n times the gain, interpolated per sample. Level out n = the gain. Mix = the sum of Out 1-4 times the Output gain (smoothed 10 ms). Non-finite input samples are zeroed per channel.

### 4.4 Trigger and sequencer

From `frames.cc`, at grid frames:

- **Trigger:** if `position()` differs from the last grid frame's and the trigger is armed, start a 10 ms pulse and disarm; if `nearest_keyframe()` differs from the last grid frame's, arm. The order (position first, then nearest) is upstream's. The pulse length is counted in samples.
- **Sequencer:** with `mode` on, the clock is `frame_cv` alone (not scaled by Frame CV amount): below 1/3 it arms, above 2/3 when armed it disarms and advances the step; the step wraps to 0 when it reaches the number of keyframes; Frame is that keyframe's position. With no keyframes, the levels are the immediate ones, as upstream. Peepo's gates (0 and 1) cross both thresholds. The step is kept across mode changes, as upstream.

### 4.5 Reel LFO

`frames::PolyLfo` is driven at its table's own update rate, 24 kHz, whatever the host rate: a phase accumulator adds `24000 / rate` per sample and calls `Render` on each whole tick; outputs are interpolated linearly between the last two renders (at most one tick, 42 us, late). Controls map as the hardware's knobs: `set_shape(shape * 65535)`, `set_shape_spread`, `set_spread`, `set_coupling` likewise, set at each tick.

Rate: `rate + rate_cv`, clamped to [0, 1], times 65535, passed to `Render`. Upstream doesn't clamp, and outside 0 to about 2 its phase increment lookup reads outside `lut_increments`; Reel clamps to the knob's range, 0.0134 to 110 Hz. Outputs: `(value + 32768) / 65535` from each channel's DAC value at linear response, 0 to 1, as the hardware's VCAs give them.

### 4.6 Bypass

`enabled` off: every gain glides to unity over 10 ms (Out n = In n, Mix = the sum of the inputs times Output) and Level outs hold their last values; the keyframer keeps evaluating, so turning on again glides back to where Frame is. Exact passthrough when settled. Declared in milestone 0, built in milestone 1.

## 5. Real-time rules and threads, checked

- No allocation, lock or I/O in `run()` (the in-process host's allocation hook, on a new thread).
- **The keyframer lives on the audio thread.** Only `run()` calls it.
- **Save while running:** after every change to the list, `run()` copies it (at most 64 x 12 bytes) into a published snapshot under a sequence lock. `save` reads the snapshot, retries if the sequence changed, and serialises it. `run()` never waits.
- **Restore:** `restore` parses and checks the state into a pending buffer, then publishes it with a release store; `run()` takes it at the next grid frame (acquire), rebuilds the list with `Clear()` and one `AddKeyframe` per saved keyframe, sets each saved id through `mutable_keyframe`, and recomputes the active keyframe. `Keyframer` has no setter for its id counter, so after a restore new ids continue from the number of keyframes; ids only pick colours (modulo 8), so a repeat is harmless and no vendored change is made for it. A restore that arrives while an older one is pending replaces it. Works whether or not Ingen calls `restore` during `run()`.
- `Evaluate` is a binary search over at most 64 keyframes and four easings: no bulk work.
- Outputs finite: gains are finite by construction; inputs are checked (4.3).

## 6. Tests

### 6.1 Offline (`make test`)

| Test | Check |
| --- | --- |
| `ttl_check` | every port of both plugins matches `ports.hpp`: index, symbol, kind, range, default, enumeration labels; symbols frozen in order; names ASCII and at most 16 characters with no upstream names; license; no required features |
| `scaffold` | two descriptors; unconnected ports, zero-frame runs, several rates; `activate` restarts exactly; with no keyframes, each Out follows its Level control |
| `editing` | the hardware's rules, from `ui.cc`: Add away from keyframes stores one at the Frame control (not the CV) with the Level values; Add within 1/32 does nothing; a Level change edits the active keyframe, else the immediate level; changes under 1/1024 do nothing; loaded values are not edits; Delete removes the active one; Clear all; 64 is the limit |
| `easing` | each curve between two keyframes equals `Keyframer::Evaluate` driven directly, at 1000 positions; step jumps at the midpoint; held before the first and after the last |
| `vca` | response 0 gives gain = level (to 1/4096); response 1 gives -75 dB per unit of level; three middle responses against the hardware's code path (`ConvertToDacCode`, converted to gain) within 0.1 dB over the range; level 0 silent; level 1 unity |
| `trigger` | one pulse each time Frame passes a keyframe, none while it wobbles inside one interval, none with no keyframes; 10 ms |
| `sequencer` | each Frame CV rise above 2/3 after a fall below 1/3 advances one keyframe and wraps; Frame and Frame CV amount ignored; levels exact |
| `state` | save, restore into a fresh instance: identical levels at 1000 positions, same ids; restore while running; malformed, oversized and unsorted vectors rejected; missing keys; thousands of saves on one thread while another edits (`SAN=thread`) |
| `slicing` | the same outputs for whole periods, random slices and a 37-frame period, with Frame, edits and Frame CV moving |
| `lfo` | Reel LFO's four outputs equal `PolyLfo` driven directly at 24 kHz, sampled at the host rate; rate 0 and 1 give 0.0134 and 110 Hz within 0.5%; out-of-range Rate CV clamped |
| `bypass` | `enabled` off reaches unity in 10 ms with Mix the sum of the inputs; on returns exactly |
| `allocation`, `instances` | no `malloc` or `calloc` in `run()` on a new thread; two instances keep their own keyframes, on two threads |
| `rt_timing` | 10 simulated minutes of random controls, edits and saves: worst and 99.9th-percentile `run()` time (`make test-timing`) |

Every test also runs under `SAN=1`, `SAN=thread`, and as an arm64 build under QEMU, as Chamber's milestone 0.

### 6.2 Desktop hosts

`lv2lint` (as built for Chamber), `lv2info`, `jalv` (state save and restore through presets), Carla (state with its project file). `tools/render` writes outputs and levels to a WAV for looking at curves.

### 6.3 Ingen under WSL

`make test-hosts` with Peepo's engine (`tools/ingen-spike/env.sh`): add keyframes through the controls, save the patch (`patch:Copy`), reload in a fresh engine: keyframes and controls come back, levels identical. One and four threads, 1024- and 128-frame periods. Audio through the four VCAs from a test signal.

### 6.4 On the Pi

Reel crossfading two effects in a typical patch for 30 minutes, Frame on an encoder and on an LFO, a save every minute; Reel LFO modulating two effects. Record JACK xruns (0 beyond the same patch without Reel), DSP load and RSS into Peepo's `docs/evidence/`; keyframes survive a reboot. Before running it, say that the check stops the instrument, for how long, and that it comes back.

## 7. Changes to vendored files

None. Under `-DTEST`, `Keyframer::Init`, `Save` and `Calibrate` touch no storage (the draft of this plan wanted them removed); the glue does `Init`'s defaults itself (4.1). `third_party/eurorack/README.md` says so. If a change is ever needed it goes in `tools/patches/`, applied by `tools/vendor.sh`, as Chamber's.

## 8. Milestones

Each has an end check. "Done" means the offline tests pass on x86-64 and arm64, plus the stated host checks. Each milestone ends with a record (section 10).

- **Milestone 0, scaffolding. Done 21 November 2026** (tag ready; see the [record](milestone-0-validation.md)). The spec's bypass question settled. `tools/vendor.sh`, Makefile, both descriptors, both `.ttl` files with the full port lists, `ports.hpp`, state stubs, `ttl_check`, `scaffold`, `allocation`. Four VCAs at the Level controls. **Done when** `make`, `make install DESTDIR=...`, `lv2info` and `lv2lint` are clean, only `lv2_descriptor` is exported, and both plugins load in jalv and in Ingen under WSL. Tag `v0.0.1`.
- **Milestone 1, keyframes.** Editing, easing, response, Frame CV, trigger, bypass, state; `editing`, `easing`, `vca`, `trigger`, `state`, `slicing`, `bypass`, `instances`, `rt_timing`. Save and reload through jalv, Carla and Ingen. The spec's silence and default questions settled. Pi xrun check. Tag `v0.1.0`; Peepo adds its overlay entry and pinned build (generic page).
- **Milestone 2, sequencer, LFO, telemetry, first page.** `sequencer`, `lfo`; the keyframe telemetry decided and appended. Peepo's Reel page, first version, from a mockup approved by the user before it is built. Tag `v0.2.0`.
- **Milestone 3, the page.** Touch editing; moving keyframes if the user wants it (a `commands` input, appended); stomp bindings; listening on the Pi. `v0.2.x` or `v0.3.0`.

## 9. Peepo-side work

In Peepo's repository, not here:

1. Build step in `tools/ingen-spike/14-more-plugins.sh`: `third reel https://github.com/ABustos2606/reel.git <commit> "make" make -j"$JOBS" OPT=... LV2DIR="$LV2DIR" install`, in the build root (Pi-only installs are wiped by the next package).
2. URIs in `config/catalogue/inventory.txt` and `tools/catalogue-inventory.py`; check that `catalogue.json` offers both before saying they are.
3. Overlay entries: "Reel" (**check** which category shows audio utilities) and "Reel LFO" (Control), descriptions that say what they do (no maker or module names), `"output": "output"` and `"bypass": "port"` on Reel, groups for the generic page until the Reel page exists.
4. The Reel page ([peepo-page.md](peepo-page.md)), from an approved mockup.
5. `docs/37-module-names.md` already names Reel; `docs/33-module-list.md` regenerated after the Pi catalogue is exported; the roadmap's Reel checklist ticked.

## 10. Records

Each milestone ends with `docs/milestone-N-validation.md` (`<!-- SPDX-License-Identifier: MIT -->` on its first line), written as Chamber's and Drizzle's:

- **Recorded** date and one paragraph: what is built, what the checks cover and what they don't.
- **A table of checks** with results: vendor run, each build (x86-64, `SAN=1`, `SAN=thread`, arm64), install, `lv2info`, `nm -D`, `test-hosts`, `lv2lint`, state round trips per host, Pi measurements with their numbers.
- **What the offline tests cover**, one bullet per test.
- **Found and fixed during the milestone**, with what changed in the plan, the spec or the page contract.
- **Not in this milestone.**
- **Release:** the tag, and that it is local until the user pushes.

When a milestone is done, this plan gets an "Updated" line under its header naming what changed and linking the record, the milestone in section 8 says **Done** with the date, the spec's milestone box is ticked, `peepo-page.md` is brought in step with any port added, and the README's status paragraph is rewritten.

## 11. Risks

| Item | Why it matters |
| --- | --- |
| State and editing on different threads | The handover must be correct; the `state` test under TSan, and Ingen saves under load |
| Editing by touch | Accidental edits when Frame sits on a keyframe and a Level is moved; the page must show the active keyframe |
| Level controls versus keyframe values | Plugins can't move their own inputs; the generic page shows stale Level values over keyframes. The Reel page is the real editor |
| Audio-only VCAs | Peepo won't cable CV into them; CV users get the Level outputs only |
| Telemetry size | 64 keyframes do not fit in control outputs; the atom output needs Peepo's adapter to watch it |

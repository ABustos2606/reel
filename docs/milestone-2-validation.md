<!-- SPDX-License-Identifier: MIT -->
# Milestone 2 validation

**Reel side done offline and in jalv and Ingen; the Reel page waits for its mockup's approval; the Pi checks are to do.** Recorded 21 November 2026. Reel's Sequencer mode is built: Frame CV clocks it through the keyframes, as `frames.cc` does. Reel LFO's four outputs run: the vendored `PolyLfo` driven at its tables' own 24 kHz, its DAC codes taken through the hardware's VCA. The keyframe telemetry is settled and appended as port 40, `t_keyframes`: the list as one string on an atom output, sent when it changes and every half second. The checks cover the sequencer's clock against upstream's thresholds, Reel LFO against `PolyLfo` driven directly at four host rates, the telemetry's text, timing and slicing, and, in WSL's Ingen, that a client watching `t_keyframes` receives the list. Also covered: threads (TSan), ASan and UBSan, slicing, allocation, x86-64 and arm64. Not covered: Carla (not installed) and the Pi. The page mockup is drawn (`docs/images/mockup-reel-*.png`); Peepo's page is built only after the user approves it.

| Check | Result |
| --- | --- |
| x86-64 `make -j8 test`, Debian 13 under WSL | PASS, 27 checks (`ttl_check`, 10 `scaffold`, 11 `keyframes`, 5 `lfo`); GCC 14.2; no warnings from Reel's code |
| `make test SAN=1` | PASS, 25 checks (both allocation checks skipped under sanitizers). Before `-fwrapv` it reported signed overflow in stmlib's `utils/dsp.h` (below) |
| `make test SAN=thread` (under `setarch -R`) | PASS, no reports |
| arm64 `make -j8 test BUILD_DIR=build/arm64 OPT="-O2 -mcpu=cortex-a76"` | PASS, 27 checks; Debian 12 root `/opt/peepo-arm64`, GCC 12.2, QEMU user emulation, from a temporary copy removed after the check |
| `make install DESTDIR=build/stage PREFIX=/opt/peepo` | PASS; `reel.so`, three Turtle files, `LICENSE`, `THIRD_PARTY_NOTICES` |
| `lv2info` on the staged bundle | Reel: 41 ports, port 40 `t_keyframes` an `atom:AtomPort` output; `state:interface`; Reel LFO: 10 ports |
| `nm -D --defined-only reel.so`, x86-64 and arm64 | Only `lv2_descriptor` |
| `lv2lint` 0.17.151, both plugins | No errors; the author-email warning of milestone 0 |
| `make test-timing`, x86-64 under WSL, 48 kHz, 128 frames, 10 simulated minutes | Reel (`rt_timing`, Mode switching too, a save every 10 ms from another thread): mean 2.9 us per period (0.11% load), 99.9% 24 to 33 us, worst 0.66 to 1.8 ms over three runs. Reel LFO, every control and Rate CV moving: mean 6.4 us (0.24% load), 99.9% 43 us, worst 3.0 ms. The worst cases are WSL scheduling (as Drizzle's); they say nothing about the Pi |
| `make test-hosts`, jalv, 1024- and 128-frame periods | Reel as in milestone 0 (Out 2 0.250, Mix 0.750, Level out 2 0.500). **Reel LFO runs**: at Rate 0.6, Spread 0.75, outputs 1, 2 and 4 peak at 0.998 |
| `make test-hosts`, Peepo's Ingen, 1 and 4 threads, 1024- and 128-frame periods | Milestone 1's checks pass unchanged (two keyframes added through the controls; saved; a fresh engine reloads them; Level out 2 0.1999 and 0.0690 at Response 0.4). **Telemetry:** a client that sets `ingen:broadcast` true on `ingen:/main/reel/t_keyframes` gets `patch:property ingen:activity ; patch:value "1:3333...bfff"`, 50 characters, whose positions and levels are the two keyframes'. The saved Mode (Sequencer) restores; the reload sets it back to Keyframes before checking levels |
| jalv, the state Ingen saved | As milestone 1: Level out 2 0.500 and 0.250 at the two keyframes |
| Carla | **Not done**: not installed in WSL, and Debian 13 has no `carla` package |
| **Pi 5: xruns, DSP load, RSS, keyframes after a reboot** | **Not done** (milestone 1's check, now with Reel LFO in the patch; below) |
| **Peepo's Reel page** | **Not built**: the mockup waits for the user's approval |

## What the offline tests cover

New in this milestone, through the in-process host (random slices, as Ingen):

- `sequencer` (`tests/test_keyframes.cpp`): three keyframes, Frame between them and Frame CV amount at -0.3, which the sequencer ignores. Mode on: step 0, with `t_frame` at the first keyframe's position and the Level outs exactly its levels. A rise above 2/3 steps once, with one 10 ms pulse. A fall to 1/2 and a rise again do nothing, since 1/3 was not crossed. 1/3 and 2/3 are the thresholds. The step wraps after the last keyframe, with a pulse. Mode off: Frame again, the step kept; Mode on again resumes at it. With no keyframes, the Level controls.
- `telemetry` (`tests/test_keyframes.cpp`): the empty list (`1:`) at the first `run()`. After Add, a list with the new keyframe within 40 ms, equal to the saved list (positions, ids, levels). Unchanged, one list every 0.5 s, each 0.5 s apart within a period. While a Level is dragged on the editing target, 40 to 51 lists in 2 s (25 a second at most), the last one right. A buffer prepared before every `run()` (as jalv does) as well as once per period (as Ingen does). 64 keyframes: 1538 characters that decode to the list. No `urid:map`: nothing written.
- `lfo` (`tests/test_lfo.cpp`, new): Reel LFO's outputs equal `PolyLfo` driven directly, rendered at every 24 kHz tick counted from the frame number and sampled between the last two renders, within 1e-5. Checked at 48, 44.1, 96 and 16 kHz, with every control and Rate CV moving, and identical with random slices. Every output is finite, from 1/4096 to 1. Rate 0 gives 0.0134278 Hz and Rate 1 110.229 Hz, measured on the output and matching `lut_increments` within 0.2%. Rate plus Rate CV is clamped at both ends. Spread 0 gives each next output an octave more (Out 4 eight times Out 1). `activate` restarts the phases.
- The milestone 1 tests now cover Mode too: `slicing`'s performance switches it, with a Frame CV that crosses both thresholds, and `rt_timing` switches it at random. `scaffold`'s Reel LFO check now asks for outputs from 1/4096 to 1 instead of 0, and both `allocation` checks still pass with the telemetry writing (the test host's own record of the lists is off there).
- `ttl_check`: 41 ports; `t_keyframes` is an atom output, a sequence of strings with `rsz:minimumSize` of at least 4096, and its symbol is frozen in order after the other 40.

## Found and fixed during the milestone

- **Spread below its centre speeds the LFOs up.** The spec, the plan and Reel LFO's description said each next LFO slows down. `PolyLfo::Render` adds `-5040 spread_ / 32768` to each next LFO's frequency, and `spread_` is negative below the centre, so each runs faster: an octave each at Spread 0. Measured by the `lfo` test. The spec (Poly LFO mode, Reel LFO's controls), the plan (4.5) and the Turtle description say so now.
- **stmlib's 8-bit interpolations overflow `int`.** `make test SAN=1` reported `Interpolate824` and `Crossfade` for `uint8_t` tables (`utils/dsp.h:110` and `:154`, from `PolyLfo::Render`). They multiply a table step by a fraction in `int`, which overflows: undefined behaviour. Wrapped, as on the hardware, the error is a multiple of 2^32, so a multiple of 65536 after `>> 16`, which the `int16_t` result drops: the result is exact. Vendored code is now compiled with `-fwrapv`, which makes that wrapping defined; no vendored file is changed (plan, section 7; `third_party/eurorack/README.md`).
- **Ingen runs every slice of a period on one atom output buffer.** It prepares the sequence once per period (`PortImpl::pre_process`), so writing an empty sequence at every `run()` would erase a list written by an earlier slice. The telemetry keeps a sequence it finds, and replaces it only with a newer list (plan, 4.7). The host test prepares the buffer as Ingen does, and also as jalv does.
- **`PolyLfo`'s phases are not set by `Init`.** On the hardware it is a static, so they start at 0; Reel zeroes the object before `Init` at `activate`.
- **The host check saved Sequencer mode,** which now works: on reload, Frame no longer moves the levels. The reload checks that Mode restores, then sets it back to Keyframes.

## Decisions taken in this milestone

Recorded in the spec (Decisions, 21 November 2026). The keyframe telemetry is one atom output, `t_keyframes`, carrying the list as one string, as recommended there; it is a string because Peepo's reader takes Turtle literals and not RDF lists (plan, 4.7). Reel LFO's outputs are the hardware's VCA gain at its DAC codes. Both plugins are `lv2:minorVersion` 2. The 40 ms and 0.5 s telemetry timings are **tune**.

**Still open, for the user:** the page mockup; whether encoder 1 moves Frame on the page (the accepted layout) or picks bars as on every module page (the mockup); Reel's catalogue category (the mockup uses CV + UTIL's colours); and a BPM control for Reel LFO (spec, Open questions).

## The page mockup

`docs/images/mockup-reel-1-on-keyframe.png` to `-5-clear.png`, rendered from `tools/mockup/reel-page.html` by `tools/mockup/render.sh` (headless Chrome, 800 x 480). It shows Frame on a keyframe, Frame between keyframes, the curves, Sequencer mode and Clear all's confirmation. The layout is described in [peepo-page.md](peepo-page.md).

## Pi check (to do)

Milestone 1's check, which stops the instrument while it runs (about 40 minutes, including the reboot), with Reel LFO modulating two effects in the same patch (plan, 6.4), and the page's telemetry watched once the page exists. It needs the user's go-ahead.

## Not in this milestone

The Reel page in Peepo (waits for the mockup's approval), Peepo's overlay entries, inventory and pinned build (plan, section 9), `tools/render`, Carla, the Pi.

## Release

`v0.2.0` (Turtle `lv2:minorVersion 2`, `lv2:microVersion 0`, both plugins) is ready to tag on the commit that adds this record, once the page and the Pi checks pass; nothing is committed, tagged or pushed.

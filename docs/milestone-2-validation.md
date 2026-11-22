<!-- SPDX-License-Identifier: MIT -->
# Milestone 2 validation

**Done: offline, in jalv and Ingen, on the Pi (30 minutes, 0 xruns; the keyframes back after a reboot); the Reel page approved and built in Peepo (checked in its tests).** Recorded 21 November 2026; the Pi checks 21-22 November. Reel's Sequencer mode is built: Frame CV clocks it through the keyframes, as `frames.cc` does. Reel LFO's four outputs run: the vendored `PolyLfo` driven at its tables' own 24 kHz, its DAC codes taken through the hardware's VCA. The keyframe telemetry is settled and appended as port 40, `t_keyframes`: the list as one string on an atom output, sent when it changes and every half second. The checks cover the sequencer's clock against upstream's thresholds, Reel LFO against `PolyLfo` driven directly at four host rates, the telemetry's text, timing and slicing, and, in WSL's Ingen, that a client watching `t_keyframes` receives the list. Also covered: threads (TSan), ASan and UBSan, slicing, allocation, x86-64 and arm64. Not covered: Carla (not installed), and the page on the Pi (its installed interface predates the page). The page mockup (`docs/images/mockup-reel-*.png`) was approved as drawn and built in Peepo the same day. At the user's request Reel LFO also got a BPM control that Peepo sets from the patch tempo, with Tempo sync and Note (ports 10-12, appended).

| Check | Result |
| --- | --- |
| x86-64 `make -j8 test`, Debian 13 under WSL | PASS, 28 checks (`ttl_check`, 10 `scaffold`, 11 `keyframes`, 6 `lfo`); GCC 14.2; no warnings from Reel's code |
| `make test SAN=1` | PASS, 26 checks (both allocation checks skipped under sanitizers). Before `-fwrapv` it reported signed overflow in stmlib's `utils/dsp.h` (below) |
| `make test SAN=thread` (under `setarch -R`) | PASS, no reports |
| arm64 `make -j8 test BUILD_DIR=build/arm64 OPT="-O2 -mcpu=cortex-a76"` | PASS, 28 checks; Debian 12 root `/opt/peepo-arm64`, GCC 12.2, QEMU user emulation, from a temporary copy removed after the check |
| `make install DESTDIR=build/stage PREFIX=/opt/peepo` | PASS; `reel.so`, three Turtle files, `LICENSE`, `THIRD_PARTY_NOTICES` |
| `lv2info` on the staged bundle | Reel: 41 ports, port 40 `t_keyframes` an `atom:AtomPort` output; `state:interface`; Reel LFO: 13 ports |
| `nm -D --defined-only reel.so`, x86-64 and arm64 | Only `lv2_descriptor` |
| `lv2lint` 0.17.151, both plugins | No errors; the author-email warning of milestone 0 |
| `make test-timing`, x86-64 under WSL, 48 kHz, 128 frames, 10 simulated minutes | Reel (`rt_timing`, Mode switching too, a save every 10 ms from another thread): mean 2.9 us per period (0.11% load), 99.9% 24 to 33 us, worst 0.66 to 1.8 ms over three runs. Reel LFO, every control and Rate CV moving: mean 6.4 us (0.24% load), 99.9% 43 us, worst 3.0 ms. The worst cases are WSL scheduling (as Drizzle's); they say nothing about the Pi |
| `make test-hosts`, jalv, 1024- and 128-frame periods | Reel as in milestone 0 (Out 2 0.250, Mix 0.750, Level out 2 0.500). **Reel LFO runs**: at Rate 0.6, Spread 0.75, outputs 1, 2 and 4 peak at 0.998 |
| `make test-hosts`, Peepo's Ingen, 1 and 4 threads, 1024- and 128-frame periods | Milestone 1's checks pass unchanged (two keyframes added through the controls; saved; a fresh engine reloads them; Level out 2 0.1999 and 0.0690 at Response 0.4). **Telemetry:** a client that sets `ingen:broadcast` true on `ingen:/main/reel/t_keyframes` gets `patch:property ingen:activity ; patch:value "1:3333...bfff"`, 50 characters, whose positions and levels are the two keyframes'. The saved Mode (Sequencer) restores; the reload sets it back to Keyframes before checking levels |
| jalv, the state Ingen saved | As milestone 1: Level out 2 0.500 and 0.250 at the two keyframes |
| Carla | **Not done**: not installed in WSL, and Debian 13 has no `carla` package |
| Pi 5, `make -j3 OPT="-O2 -mcpu=cortex-a76"` and `make test`, native arm64 (Debian 12, GCC 12.2) | No warnings; PASS, 28 checks |
| Pi 5, `make test-timing`, 48 kHz, 128 frames, 10 simulated minutes | Reel: mean 6.1 us (0.23% load), 99.9% 35 us, worst 94 us. Reel LFO: mean 12.9 us (0.49%), 99.9% 24 us, worst 74 us |
| **Pi 5: xruns, DSP load, RSS** (`tools/pi_check.py`, 48 kHz, 128 frames, 30 minutes) | **0 xruns**; JACK DSP load 4.4% mean, 9.2% max (the same patch without Reel and Reel LFO, 5 minutes: 2.5%, 2.8%); engine RSS 144.6 -> 150.8 MB (it grows without Reel too); 29 saves; power flags 0x0, 49-55 C |
| **Pi 5: keyframes after a reboot** (`tools/reboot_check.py`) | **PASS**: the bundle saved during the run, loaded into the engine after a reboot, gives the same three keyframes on `t_keyframes` |
| Peepo catalogue on the Pi | Overlay and inventory differed from Peepo's only by Reel's lines; re-exported: 256 -> 258 modules, only Reel (19 controls, 16 ports) and Reel LFO (8 controls, 5 ports) added. Peepo's `docs/evidence/catalogue-pi-20261121-reel.json` |
| **Peepo's Reel page** | Built in Peepo from the approved mockup: `ingen_tests` (`reel_monitor`: the list taken from `ingen:activity` as Ingen sends it, control outputs read, a malformed list ignored, subscriptions dropped on leaving, the patch never marked modified) and `ui_check` (captures `99-reel-on-keyframe`, `-between`, `-clear`, `-sequencer`: the editing target named, Level bars showing its levels, Add pulsed 1 then 0, a tap near a keyframe landing on it exactly, Clear all asking in the header, the sequencer's step in the footer) pass on Windows |

## What the offline tests cover

New in this milestone, through the in-process host (random slices, as Ingen):

- `sequencer` (`tests/test_keyframes.cpp`): three keyframes, Frame between them and Frame CV amount at -0.3, which the sequencer ignores. Mode on: step 0, with `t_frame` at the first keyframe's position and the Level outs exactly its levels. A rise above 2/3 steps once, with one 10 ms pulse. A fall to 1/2 and a rise again do nothing, since 1/3 was not crossed. 1/3 and 2/3 are the thresholds. The step wraps after the last keyframe, with a pulse. Mode off: Frame again, the step kept; Mode on again resumes at it. With no keyframes, the Level controls.
- `telemetry` (`tests/test_keyframes.cpp`): the empty list (`1:`) at the first `run()`. After Add, a list with the new keyframe within 40 ms, equal to the saved list (positions, ids, levels). Unchanged, one list every 0.5 s, each 0.5 s apart within a period. While a Level is dragged on the editing target, 40 to 51 lists in 2 s (25 a second at most), the last one right. A buffer prepared before every `run()` (as jalv does) as well as once per period (as Ingen does). 64 keyframes: 1538 characters that decode to the list. No `urid:map`: nothing written.
- `lfo` (`tests/test_lfo.cpp`, new): Reel LFO's outputs equal `PolyLfo` driven directly, rendered at every 24 kHz tick counted from the frame number and sampled between the last two renders, within 1e-5. Checked at 48, 44.1, 96 and 16 kHz, with every control and Rate CV moving, and identical with random slices. Every output is finite, from 1/4096 to 1. Rate 0 gives 0.0134278 Hz and Rate 1 110.229 Hz, measured on the output and matching `lut_increments` within 0.2%. Rate plus Rate CV is clamped at both ends. Spread 0 gives each next output an octave more (Out 4 eight times Out 1). `activate` restarts the phases. With Tempo sync on, a cycle lasts the Note at the BPM within 0.5% (1/4 at 120 BPM is 2 Hz; 1 bar at 90, 1/16 at 300, 8 bars at 30); Rate is ignored and Rate CV adds an octave per 5040/65535; off, BPM and Note change nothing.
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

**Settled with the user (21 November 2026):** the mockup approved as drawn; encoders as on every module page (E1 picks a bar, E2 turns it; Frame is a bar); Reel LFO follows the patch tempo like other modules (`bpm`, `sync`, `note`); the Pi check may go ahead. Reel is in Utility (with Peepo's audio mixers), so the page has the CV + UTIL colours of the mockup.

## The page mockup

`docs/images/mockup-reel-1-on-keyframe.png` to `-5-clear.png`, rendered from `tools/mockup/reel-page.html` by `tools/mockup/render.sh` (headless Chrome, 800 x 480). It shows Frame on a keyframe, Frame between keyframes, the curves, Sequencer mode and Clear all's confirmation. The layout is described in [peepo-page.md](peepo-page.md).

## Pi check

Run on 21-22 November 2026 (Peepo's `docs/evidence/reel-pi-20261121.txt` has the logs). Reel `d2af563` (the plugin code of `0089184`) built and installed on the Pi; the engine, routing and interface restarted with "Clouds guitar". `tools/pi_check.py` (modelled on Slope's) built the patch on the running engine: Overdrive and a delay from IN into Reel's inputs 1 and 2, Reel's mix into Mist, Reel LFO on the delay's feedback and Mist's density. Three keyframes were added through the controls as the page adds them (0.1 all Overdrive, 0.5 both at 0.6, 0.9 all delay) and read back from `t_keyframes`. For 15 minutes Frame moved every 200 ms as an encoder would, then Reel LFO's Out 1 drove Frame CV for 15 more; the graph was saved every minute (29 saves, Reel's state in `reel/state.ttl` each time). Then 5 minutes of the same patch without Reel and Reel LFO, and "Clouds guitar" reloaded. Results in the table above: no xruns, and Reel and Reel LFO add about 2 points of DSP load. The highest load, 9.2% (JACK's load sampled every 5 seconds), came after Reel LFO took over Frame; the mean stayed at 4.3%.

Then the reboot: the last saved bundle, kept on the card, was loaded into the freshly booted engine by `tools/reboot_check.py` as Peepo loads a patch (the running blocks removed, then `patch:Copy` of the bundle into `ingen:/main`), and Reel listed the same three keyframes. "Clouds guitar" was reloaded afterwards, unmodified.

## Not in this milestone

`tools/render`, Carla, the Reel page on the Pi (it comes with Peepo's next interface install). Peepo's build is pinned at `0089184` and moves to `v0.2.0` once tagged.

## Release

`v0.2.0` (Turtle `lv2:minorVersion 2`, `lv2:microVersion 0`, both plugins) is ready to tag on the commit that adds this record: the page and the Pi checks pass. Nothing is committed, tagged or pushed.

<!-- SPDX-License-Identifier: MIT -->
# Milestone 1 validation

**Done offline and in jalv and Ingen; the Pi check and Carla are to do.** Recorded 21 November 2026. Reel's keyframes are built. Add, Delete and Clear all work on their rising edges, with the rules of `ui.cc`. The Level controls edit the active keyframe or the immediate levels. The six easing curves come from the vendored `Keyframer`, and the VCA's response curve is computed in floating point. Frame CV is added to Frame, and the trigger fires as Frame passes a keyframe (from `frames.cc`). Bypass glides to unity. LV2 state saves and restores the list while `run()` runs: a sequence lock for `save`, a lock-free handover for `restore`. The checks cover the editing rules against the hardware's, the curves against `Keyframer` driven directly, the response curve against the hardware's code path, threads (TSan), slicing, allocation, x86-64 and arm64, and saving and reloading in jalv and Peepo's Ingen. They don't cover Carla (not installed, no Debian package) or the Pi (below). The sequencer (Mode), Reel LFO's outputs and the keyframe telemetry are milestone 2.

| Check | Result |
| --- | --- |
| x86-64 `make -j8 test`, Debian 13 under WSL | PASS, 20 checks (`ttl_check`, 10 `scaffold`, 9 `keyframes`); GCC 14.2; no warnings from Reel's code |
| `make test SAN=1` | PASS, 18 checks (both allocation checks skipped under sanitizers). Before the vendored patch it reported three faults in `keyframer.cc` (below) |
| `make test SAN=thread` (under `setarch -R`) | PASS, no reports; includes about 20,000 saves on one thread while another edits, and restores while running |
| arm64 `make -j8 test BUILD_DIR=build/arm64 OPT="-O2 -mcpu=cortex-a76"` | PASS, 20 checks; Debian 12 root `/opt/peepo-arm64`, GCC 12.2, QEMU user emulation, from a temporary copy removed after the check |
| `tools/patches/keyframer-easing-bounds.patch` | Applies to upstream's `frames/keyframer.cc` at `f4cfa769` (LF) and gives the vendored file byte for byte |
| `make install DESTDIR=build/stage PREFIX=/opt/peepo` | PASS; `reel.so`, three Turtle files, `LICENSE`, `THIRD_PARTY_NOTICES` |
| `lv2info` on the staged bundle | Reel: Mixer Plugin, 40 ports, `urid:map` optional, `state:interface`; Reel LFO: 10 ports |
| `nm -D --defined-only reel.so`, x86-64 and arm64 | Only `lv2_descriptor` |
| `lv2lint` 0.17.151, both plugins | No errors; the author-email warning of milestone 0 |
| `make test-timing` (`rt_timing`), x86-64 under WSL, 48 kHz, 128 frames, 10 simulated minutes of random controls, edits and presses, a save every 10 ms from another thread | mean 3.2 us per period (0.12% load), 99.9% 38 us, worst 0.98 to 2.0 ms over two runs. The worst is WSL scheduling (as Drizzle's); says nothing about the Pi |
| `make test-hosts`, jalv | Reel and Reel LFO load; Out 2 0.250, Mix 0.750, Level out 2 0.500 (milestone 0's check) |
| `make test-hosts`, Peepo's Ingen, 1 and 4 threads, 1024- and 128-frame periods | Two keyframes added through Frame, Level and Add (as a stomp or the page would): Level out 2 0.500 and 0.250 at them. Saved with Frame on the second keyframe and Level 2 at another value: `state.ttl` holds the list (`reel#keyframes`, 12 ints), the other Reel none, controls restore. **A fresh engine reloads the patch**: Level out 2 0.1999 and 0.0690 at the two keyframes, the saved levels at the saved Response 0.4 (`vca_gain`), so the Level values loaded with the patch edited nothing. JACK logged 0 xruns |
| jalv, the state Ingen saved (`jalv -l <patch>/reel`) | Level out 2 0.500 and 0.250 at the two keyframes (Frame set through jalv's console; Response at its default, since Ingen keeps port values in `main.ttl`) |
| jalv, saving | Not checked: jalv's console has no save command. Saving is lilv's `lilv_state_new_from_instance`, as Ingen uses, checked above |
| Carla | **Not done**: not installed in WSL, and Debian 13 has no `carla` package |
| **Pi 5: xruns, DSP load, RSS, keyframes after a reboot** | **Not done** (below) |

## What the offline tests cover

`tests/test_keyframes.cpp`, new in this milestone, drives the plugin through the in-process host (random slices, as Ingen) and compares it with the vendored `Keyframer` linked in directly:

- `editing`: Add stores a keyframe at the Frame control, not Frame plus CV, with the four Level values, and the new keyframe is active. Add within 1/32 of a keyframe does nothing. A Level change edits the active keyframe; under 1/1024 it does nothing, and 1/1024 from the last move counts. Frame CV selects no keyframe, so a Level change then edits none. A second keyframe gets id 1. Delete with nothing active does nothing; Delete removes the active keyframe; Clear all, after which the levels follow the Level controls. A 3-frame press between two grid frames is not lost; Add held from the start is not a press. 64 keyframes (from state: Add can't place them closer than 1/32): Add stores nothing.
- `easing`: for each of the six curves, Level outs 1-4 at 1000 Frame positions equal `Keyframer::Evaluate` driven directly, with the gain at response 0. Every curve stays between the two levels and, except Step, moves smoothly; this fails on Bounce without the vendored patch. Levels are held before the first and after the last keyframe, and Step jumps at the midpoint.
- `vca`: at response 0 the gain is the level, and at response 1 it falls 75 dB per unit of level. Responses 0.25, 0.5 and 0.75 are within 0.037 dB of the hardware's code path (`ConvertToDacCode`, its 12-bit code taken as volts through the 2164) from level 1/16 up, and 0.12 dB below, where the hardware's table is coarse. Level 0 is silent and level 1 unity at every response, and Out is In times the gain.
- `trigger`: `t_nearest` counts from 0. Sweeping across three keyframes gives one 10 ms pulse per keyframe passed, each way. Wobbling inside an interval (across its midpoint) gives none; wobbling around a keyframe gives one. There are none while bypassed and none with no keyframes.
- `state`: save, then restore into a fresh instance whose Level values differ and whose Frame sits on a keyframe. The list comes back with the same ids, unedited, and the levels at 1000 positions are identical. Save before `run()` has taken a restored list gives that list. Ingen's order (state restored, defaults run, then Frame onto a keyframe and Levels set one by one) edits nothing, and a move a second later edits. New ids continue from the count, and `activate` keeps the keyframes. Lists that aren't a multiple of six, have 65 keyframes or are unsorted are rejected and leave none; a missing key means none. A restore repeated on another thread while `run()` runs leaves the list. Thousands of saves while another thread edits (Level moves, a keyframe added and deleted) each give a whole, sorted list with the right count.
- `slicing`: whole periods, random slices and a 37-frame period give identical outputs and keyframes at 48 and 44.1 kHz. Edits, Add, Delete, Clear all, Easing, Response, Output and bypass all change meanwhile, with Frame CV moving and the trigger firing.
- `bypass`: off reaches exact passthrough in 10 ms, gliding between the gain and unity. The Mix is the sum of the inputs times Output, and the Level outs hold. Turned on again, the outputs are exactly those of an instance never bypassed after 10 ms. Off from the start passes through, with the Level outs at the first gains.
- `instances`: two instances with their own keyframes, on two threads at once, give what each gives alone.
- `allocation`: no `malloc` or `calloc` in `run()` on a new thread, with edits, presses, and saves and restores from another thread.
- `rt_timing` (`make test-timing`): above.

`tests/test_scaffold.cpp` (milestone 0) still passes unchanged, and so does `ttl_check` (the C++ name `delete_` is now mapped to its symbol when the fallbacks are read).

## Found and fixed during the milestone

- **Upstream's Bounce easing is broken where its curve falls.** `Keyframer::Easing` multiplies the table step by `(scale << 10) & 0xffff`, an unsigned value, so a negative step turns unsigned. The shaped scale comes out 131072 too high and the next product overflows `int`, which is undefined behaviour. With wrapping arithmetic, as on the hardware, a fade from 0 to 0.75 jumps by half of full scale in Bounce's falling parts; 0 to 1 and 0 to 0.5 are off by only two codes, which may be why it went unnoticed. The fix is `tools/patches/keyframer-easing-bounds.patch`, the first local change to vendored code. It also stops `Easing` reading one entry past its table at a keyframe (with weight 0) and `Evaluate` reading `keyframes_[64]` past the 64th keyframe. The plan (section 7), the spec, `third_party/eurorack/README.md`, `tools/vendor.sh` and the project instructions now say so.
- **Fades to 0 stop at 1/65535.** `Easing` rounds a fall one code short, so the step curve's second half, and any fade to 0 until Frame reaches the keyframe, sat at the hardware's 1/4096 floor (-72 dB) rather than silence. Levels 0 and 1/65535 are both silence now (plan, 4.3).
- **Loading a patch in Ingen edited a keyframe** (128 frames, 4 threads; not at 1024). Ingen restores state when it creates a block, then sets the patch's port values one event per port while the block runs. Frame arrived on a keyframe, then Level 2 arrived as a move and edited it. For 1 s after a restore is taken, Level changes are now the loaded values, not edits (plan, 4.2; spec, "Editing in Reel"; page contract). The host check saves with Frame on a keyframe to catch it, and the `state` test reproduces Ingen's order.
- `t_nearest` passed upstream's `nearest_keyframe()` through, which counts from 1 when Frame is nearer the next keyframe and can name the slot after the last. It is now the index of the nearest keyframe, -1 to 63 (Turtle maximum 63; plan, section 3; page contract).
- A Reel that starts bypassed held its Level outs at 0; it holds the first gains now.
- The milestone 0 test host had no timing hook; it measures `run()` per period now, as Drizzle's.

## Decisions taken in this milestone

Recorded in the spec (Decisions, 21 November 2026), as recommended there: silence at level 0; Level defaults 1. Also: `t_nearest` counts from 0; Bounce fixed rather than copied; the 1 s load window (**tune**); the trigger stays low while bypassed.

## Pi check (to do)

The check stops the instrument while it runs (about 40 minutes, including the reboot), and the instrument comes back afterwards. It needs the user's go-ahead, then, on the Pi, at the commit to be tagged:

```sh
make OPT="-O2 -mcpu=cortex-a76" && make test OPT="-O2 -mcpu=cortex-a76"
make test-timing OPT="-O2 -mcpu=cortex-a76"
sudo make install LV2DIR=/opt/peepo/lib/lv2 OPT="-O2 -mcpu=cortex-a76"
```

Then, in Peepo: Reel crossfading two effects in a typical patch for 30 minutes, Frame on an encoder and then on an LFO, a save every minute. Record JACK xruns (0 beyond the same patch without Reel), DSP load and RSS in Peepo's `docs/evidence/`, reboot, and check that the keyframes come back (plan, 6.4). Reel LFO is milestone 2, so it isn't in this patch.

## Not in this milestone

The sequencer (Mode is declared and inert; `t_step` is 0), Reel LFO's outputs, the keyframe telemetry (atom output), `tools/render`, the Reel page, Carla, the Pi.

## Release

`v0.1.0` (Turtle `lv2:minorVersion 1`, `lv2:microVersion 0`) is ready to tag on the commit that adds this record, once the Pi check passes; nothing is committed, tagged or pushed. Then Peepo adds its overlay entry and pinned build (plan, section 9).

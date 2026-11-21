# Reel

Reel is an LV2 keyframe mixer: four channels whose levels morph between stored snapshots (keyframes) as one Frame control or CV moves across them, with four VCAs, a mix output and level CV outputs. Reel LFO, a second plugin in the bundle, is a four-output LFO. It is ported from the open-source code of Frames, a eurorack module by Emilie Gillet. It is a plugin written for **Peepo**, and Peepo is the host that matters. Peepo shows it on a page of its own. Read `docs/design.md` before changing behaviour or ports: it is the spec. `docs/implementation-plan.md` says how to build it; `docs/peepo-page.md` is the contract with Peepo's Reel page.

## Peepo

Peepo is a guitar pedal built on a Raspberry Pi 5 (2 GB, Debian 12 arm64) running the **Ingen** modular host on JACK, with an LVGL touch UI. Users patch LV2 modules together, including CV modules (LFOs, envelopes, random sources). It has two encoders and three momentary stomps. It was called "Pedal PI" before; old names (`pedalpi`, `urn:pedalpi:ns#`) still turn up.

- Repository: `C:\Pedal PI` (https://github.com/ABustos2606/Peepo). It is readable from here (see `.Codex/settings.json`).
- Docs worth reading there:
    - `docs/38-eurorack-ports-roadmap.md`: why Drizzle, Chamber, Slope and Reel exist, their order and the Peepo-side steps;
    - `docs/37-module-names.md`: why the original module names are not used;
    - `docs/03-interface-and-controls.md` (Visual style) and `docs/25-effects-and-tuner.md`: Peepo's look, and how a module gets a page of its own (Tuner, Euclidean);
    - `docs/18-physical-controls.md`: what a stomp or encoder can do to a control;
    - `docs/22-plugin-catalogue.md` and `config/catalogue/overlay.json`: how plugins are named, grouped and described in the UI;
    - `docs/17-patch-storage.md`: LV2 state is saved with patches by Ingen (checked on the Pi, 16 October 2026);
    - `reference/upstream/ingen/`: Ingen's source.
- `C:\LV2-own-plugins\discipline` and `C:\LV2-own-plugins\loo-pi` are sister projects: same host limits, same kind of build, tests and milestones. Loo-pi also has a Peepo page; follow its `docs/peepo-page.md` for the contract. Copy their patterns (LV2 glue, `ttl_check`, in-process test host, timing test) rather than reinventing them. `drizzle`, `chamber` and `slope` are the other three Gillet ports, built the same way.
- Peepo builds every third-party plugin from its own repository at a pinned commit (`tools/ingen-spike/14-more-plugins.sh`, `reference/manifest.json`). Reel will be built the same way, so tag releases and keep the build plain (`make`, no network access, works natively on arm64).

## What is kept and what is not

- **Kept, unchanged but for one patch:** Frames' `keyframer.*`, `poly_lfo.*`, `resources.*` and the `stmlib` files they need (`stmlib.h`, `utils/dsp.h`), vendored from `pichenettes/eurorack` at commit `f4cfa7697095a28d292bff758beb51deb49b5a86` (12 October 2026) into `third_party/eurorack/`, with their MIT notices. Built with `-DTEST`, which compiles out the keyframer's flash storage; the glue sets the defaults `Keyframer::Init` leaves unset. One local change, `tools/patches/keyframer-easing-bounds.patch` (Bounce easing in signed arithmetic, no reads past two arrays), is listed in `third_party/eurorack/README.md` (plan, section 7).
- **Ported, not vendored:** the editing rules of `ui.cc` (`OnSwitchReleased`, `OnPotChanged`, `FindNearestKeyframe`, `SyncWithPots`), the sequencer clock and trigger arming of `frames.cc`, and the 2164's response curve (`ConvertToDacCode`, computed in floating point). Copy their rules exactly, naming the source function; don't reinvent them.
- **Not used:** flash storage (compiled out by `-DTEST`; LV2 state replaces it), the hidden gestures and long-press modes (controls replace them), drivers, calibration, LEDs, factory test, the hardware design.
- **Built new:** the four VCAs and the mix. On the hardware they are analog circuits driven by the keyframer's DAC outputs; Reel computes them.
- New code lives in `src/` (LV2 glue, VCAs, state, port enums), `tools/` and `reel.lv2/` (Turtle). Change vendored files only where the plan says, and keep those changes small and commented.
- **The Reel page is Peepo code** (LVGL, in Peepo's own style), not an LV2 UI. Reel gives it telemetry output ports and edit controls; keep `docs/peepo-page.md` in step with the ports.

## Host limits that shape the code

- **The keyframes are LV2 state.** Up to 64 keyframes (position, id and four levels each) are saved through `state:interface` and restored by Ingen with the patch; easing and response are control ports. Ingen calls `save` while `run()` runs; the keyframer lives on the audio thread, which publishes its list through a sequence lock and takes restored lists through a lock-free handover (plan, section 5).
- **xruns are the limit, not average CPU.** In `run()`: no allocation, no locks, no file or console I/O. Reel is cheap.
- **`run()` arrives in slices.** Ingen splits a period at every control change, so `n_samples` varies. The keyframer is evaluated every 8 samples counted from `activate`, so slicing changes nothing; gains are interpolated per sample.
- **Blocks run on several threads.** Instances share nothing.
- **Unpatched inputs read silence.** On the hardware an unpatched channel input is normalled to a DC level, so its output is the level as CV. A plugin can't tell, so Reel has separate level CV outputs. Never guess.
- **Peepo joins audio to audio and CV to CV only** (`src/core/model.cpp`), though Ingen would mix them. Reel's VCAs are audio ports; levels go out as CV.
- **Buttons become momentary controls.** Add and Delete, short and long presses, are control ports or page commands, so a stomp or the page can drive them.
- Everything else a user sets (mode, LFO settings) is a control port, so patches save it.

## Rules

- Plugin URIs: `https://github.com/ABustos2606/reel#...`. Never use Peepo's namespace. Don't change a URI or port symbol once a patch has been saved with it. The LV2 state keys are permanent too.
- Never use "Mutable Instruments" or "Frames" in plugin names, port names, descriptions, URIs or new code identifiers. Vendored upstream code keeps its own names (`namespace frames`). Credit Emilie Gillet and Frames in the README and `doap:` metadata only. ("Frame" as the name of the position control is an ordinary word and stays.)
- Port names and descriptions appear on Peepo's small screen: short, plain ASCII (its font draws other characters as boxes), and they say what the control does, never who made it.
- License: MIT, `Copyright (c) 2026 Alejandro Bustos`. Every new source file, header, Makefile and `.ttl` file starts with `SPDX-License-Identifier: MIT`; plugins declare `doap:license <https://spdx.org/licenses/MIT>`. Vendored files keep Emilie Gillet's copyright and MIT notice unchanged.
- Line endings are LF. Files go to Linux and the Pi.
- Use Peepo's name, never "Pedal PI", in new text.

## Testing

- Desktop: any LV2 host (Carla, Ardour, `jalv`), and `lv2lint` for the `.ttl` files. Check state save and restore in jalv and Carla too.
- Peepo's development engine runs under WSL (Debian) with Ingen in `/opt/peepo`; Peepo's `tools/ingen-spike/env.sh` starts it.
- On the Pi: measure xruns with Reel in a typical patch before calling a milestone done, and check that keyframes survive a reboot.

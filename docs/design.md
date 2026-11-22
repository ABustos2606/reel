# Reel design

Reel is a keyframe mixer for Peepo, a pedal built on a Raspberry Pi 5 running the Ingen modular host. It stores up to 64 snapshots (keyframes) of four channel levels along a Frame axis; moving Frame, by hand or by CV, morphs the levels between them, with an easing curve per channel. The levels drive four VCAs and a mix, and come out as CV. Reel LFO, a second plugin in the bundle, is a four-output LFO with related shapes, phases and rates. It is an LV2 plugin made from the open-source code of Frames, a eurorack module by Emilie Gillet. Peepo shows Reel on a page of its own ([peepo-page.md](peepo-page.md)). It must also work as a normal LV2 plugin in Carla or Ardour, without that page.

**Written:** 20 October 2026, from Peepo's `docs/38-eurorack-ports-roadmap.md` and a reading of Frames' source at `pichenettes/eurorack` commit `f4cfa7697095a28d292bff758beb51deb49b5a86` (12 October 2026). **Revised 20 November 2026** after reading every file Reel will use (`keyframer.*`, `poly_lfo.*`, `ui.*`, `frames.cc`, `drivers/dac.h`, `drivers/adc.h`, `drivers/system.cc`, `resources.h`, `resources/lookup_tables.py`) and Peepo's and Ingen's cable rules: how editing finds its keyframe, the step curve, the response curve and its floor, the trigger, the sequencer's clock, the LFO's controls and ranges, what flash storage does under `-DTEST`, and the audio-or-CV question were corrected or settled. **Status:** spec; milestone 0 built (21 November 2026); milestone 1 built and checked offline and in jalv and Ingen (21 November 2026); milestone 2 (the sequencer, Reel LFO, the keyframe telemetry, Peepo's page) built and checked offline, in jalv and Ingen and on the Pi (21-22 November 2026); milestone 3 (moving keyframes, touch editing, stomps) built and checked offline, in jalv and Ingen and in Peepo's tests (22 November 2026), listening on the Pi to do. [implementation-plan.md](implementation-plan.md) says how to build it.

## Why Frames

- Peepo has no way to store several mixes or modulation settings and morph between them with one control. On a pedal, Frame on an encoder or an expression CV turns one movement into a coordinated change of four things: a crossfade between effects, a build-up, a scene change.
- Its source is MIT licensed. No LV2 port exists; Poly's `lenticular_lv2` has none.
- It is small (about 400 lines for the keyframer and LFO, plus tables) and runs at a control rate.

## Where it lives

Reel has its own repository (https://github.com/ABustos2606/reel), outside Peepo:

- Peepo builds every plugin from another repository at a pinned commit. Reel is built the same way.
- It is a normal LV2 bundle, so it can be tested on a desktop without Ingen or the Pi.
- Its build is a plain Makefile.
- Its license is **MIT**, the same as Frames' code. Peepo (GPL-3.0-or-later) can build and ship it.
- Tagged releases let Peepo update to a new version on purpose.

What stays in Peepo:

- **the Reel page** (LVGL, in Peepo's own style), specified here in [peepo-page.md](peepo-page.md);
- its entries in `config/catalogue/overlay.json` and `config/catalogue/inventory.txt`;
- the build step that fetches and builds it at a pinned commit;
- stomp bindings, patches that use it, xrun measurements and test notes.

The plugin URIs are `https://github.com/ABustos2606/reel#...`, not Peepo's namespace. Fix them before the first saved patch, because changing them later breaks patches.

## Name

"Reel": a strip of frames. Mutable Instruments' guidelines ask derivative works not to use the trademark and not to keep the module's name (Peepo `docs/37-module-names.md`). "Frames" appears only in credits.

## What Frames does

Everything in this section is read from the source; the function that does it is named so the port can copy it. Positions and levels are 16-bit numbers on the hardware; here they are written as fractions of full scale.

### Inputs and outputs

Four channel knobs, the big Frame knob, a Frame CV input, and the Add and Delete buttons (`drivers/adc.h`: six ADC channels). Four VCAs whose inputs are normalled to a DC level, so an unpatched channel outputs its level as CV; a mix output; a trigger output.

**Frame** is the knob plus twice the CV's offset from its calibrated zero, clamped to full scale (`frames.cc`): the CV input can sweep Frame across its whole range in either direction.

### Keyframes

Up to 64 (`kMaxNumKeyframe`), kept sorted by position. Each holds a position, four levels and an id that picks its colour from an 8-entry palette for the RGB LED (`Keyframer::Keyframe`, `palette_`).

`Keyframer::Evaluate(frame)`:

- **No keyframes:** the levels are the knobs' values (`set_immediate`).
- **Before the first or after the last keyframe:** that keyframe's levels, held.
- **Between two keyframes:** each channel goes from the left keyframe's level to the right one's along its **easing curve**: step, linear, in quartic, out quartic, in-out sine, bounce (`Easing`). Step jumps at the **midpoint** between the two keyframes, not at a keyframe. `Easing` rounds a fall one code short of its target, so the step's second half, and a fade to 0, stop at 1/65535 until Frame reaches the keyframe. Upstream's bounce is broken where its curve falls: the interpolation turns unsigned and wraps, a jump of up to half of full scale on the hardware. Reel fixes it (plan, section 7).
- It also gives the **interval** Frame is in (`position()`) and the **nearest keyframe** on either side (`nearest_keyframe()`), used by the trigger.

### Response

Each channel's **response** (0 to 255) sets the VCA's curve (`ConvertToDacCode`). The VCA is a 2164 driven in its control-voltage domain, where gain in dB is linear in the code: 30 dB per volt over a 2.5 V span.

- Response 0, **linear:** the gain equals the level (`lut_vca_linear`), from 1/4096 (-72 dB) to 1.
- Response 255, **exponential:** the gain in dB is linear in the level, from -75 dB at 0 to 0 dB at 1.
- In between, the two control voltages are blended with weight `(response / 255)^1.5` (`lut_response_balance`).

Neither curve reaches silence: a level of 0 is -72 to -75 dB.

### Editing

From `Ui::OnSwitchReleased`, `OnPotChanged`, `FindNearestKeyframe` and `SyncWithPots`:

- The **active keyframe** is the one within 1/32 of full scale (`kKeyframeGridTolerance`, 2048) of the **Frame knob** (the CV is not counted). It is looked up again only when the Frame knob moves, or after Add or Delete.
- **Moving a channel knob** sets the active keyframe's level for that channel; with no active keyframe it sets the level used while there are no keyframes. Knob moves count only after a 1/32 one-pole filter, in steps of 1/1024 of full scale.
- **Add** (short press) with no active keyframe stores one at the Frame knob's position with the four knobs' values. With an active keyframe it stores nothing.
- **Delete** (short press) removes the active keyframe. If none are left, the levels follow the knobs again.
- Long Add: easing edit (each knob picks its channel's curve, in six equal zones). Long Delete: response edit (each knob sets its channel's response). Very long Add: save to flash. Very long Delete: erase all keyframes.
- Moving onto a keyframe does not move the knobs: the first touch of a knob jumps that channel to the knob's value.

### Trigger

When Frame crosses into another interval between keyframes, the trigger output fires a 1 ms pulse (128 ticks of the 128 kHz timer), but only if the nearest keyframe has changed since the last pulse (`frames.cc`, `can_fire_trigger`). In effect: one pulse each time Frame passes a keyframe, none while it wobbles around one. Nothing fires with no keyframes.

### Sequencer mode

Hidden: five Adds while a keyframe is active toggle it (`secret_handshake_counter_`, reset after 0.5 s idle). The Frame knob is ignored; the Frame CV becomes a clock: it arms below one third of full scale and fires above two thirds, and each firing moves to the next keyframe, wrapping (`frames.cc`). The levels are that keyframe's, exactly; nothing is eased.

### Poly LFO mode

Hidden: ten Deletes with the Frame knob at zero (`frames.cc`, `PolyLfo`). Four LFOs, read from a bank of 16 morphing wavetables (`wt_lfo_waveforms`):

- **Frame (knob plus CV):** the rate, 13 octaves from 0.0134 Hz (Frame 0) to 110 Hz (Frame 1), one octave per 5040/65536 of Frame (`lut_increments`, made for a 24 kHz update rate).
- **Knob 1, shape:** morphs through the wavetables.
- **Knob 2, shape spread:** each next LFO's shape is offset by this much (centre: all the same).
- **Knob 3, spread:** above the centre, the four LFOs keep one rate and are spread in phase; below it, each next LFO runs at a higher rate, up to an octave each at the far end (`Render` adds `-5040 spread_ / 32768` per LFO, and `spread_` is negative there; this spec said "lower" until milestone 2 measured it).
- **Knob 4, coupling:** each LFO's phase is modulated by its neighbour's sine, to the right above the centre, to the left below it.
- **Outputs:** through the VCAs with linear response, so 0 to full level: unipolar, and with audio in the inputs four tremolos.

Frame is not clamped in this mode on the hardware; below 0 or above about 2 the phase increment lookup reads outside its table.

### Storage

`Keyframer::Init`, `Save` and `Calibrate` use flash only when built without `-DTEST`. With `-DTEST` (as upstream's own desktop builds) they touch no storage, and `Init` sets nothing, not even defaults.

### Rates

The DAC is refreshed at up to 32 kHz (four channels on a 128 kHz timer); the main loop evaluates the keyframer once per refresh it sees. Knobs are polled every 1 ms.

### Not ported

Flash storage and calibration (LV2 state and the patch replace them), the LEDs and their animations, the factory test, the splash screen, the hidden gestures (controls replace them), the long-press edit modes (controls replace them), the hardware design.

## Plugins

Two plugins, one bundle, one binary:

| Plugin | URI | What it is |
| --- | --- | --- |
| Reel | `https://github.com/ABustos2606/reel#reel` | The keyframe mixer, with the sequencer as a mode |
| Reel LFO | `https://github.com/ABustos2606/reel#lfo` | The poly LFO: four related CV outputs |

The LFO mode shares nothing with the keyframer but the knobs, so it is its own plugin with its own controls (decided 20 October 2026). The sequencer uses the keyframes, so it stays a mode of Reel.

## Audio or CV

Ingen connects audio, control and CV ports to each other (`ArcImpl::can_connect`), but Peepo's interface does not: a cable joins audio to audio and CV to CV only (`src/core/model.cpp`). So a port type decides what can be plugged in.

**Settled here:** the four VCAs and the mix are **audio** ports, for the pedal's main use, crossfading and blending effects on the guitar. The four **Level out** CV outputs give the same levels as CV (the hardware's unpatched-input behaviour), for modulation. A version with CV VCAs is not planned.

## Controls

Symbols are fixed in milestone 0 and then never change; indices are in the plan.

### Reel

| Control | Kind | Replaces | Notes |
| --- | --- | --- | --- |
| Frame | 0 to 1 | the big knob | The position; Add, Delete and editing use this value |
| Frame CV amount | -1 to 1, default 1 | | Reel's own: scales Frame CV before it is added. At 1, a CV of 1.0 moves Frame across its whole range, as the hardware's twice-scaled CV |
| Level 1 ... 4 | 0 to 1 | the four knobs | The hardware's rules: levels with no keyframes, or edits of the active keyframe |
| Add, Delete | momentary | short presses | |
| Clear all | momentary | the very long Delete | The page asks for confirmation |
| Easing 1 ... 4 | Step, Linear, In quartic, Out quartic, Sine, Bounce | the long Add | Plain controls (decided 20 October 2026) |
| Response 1 ... 4 | 0 to 1 | the long Delete | Linear to exponential; 255 steps |
| Mode | Keyframes, Sequencer | five Adds | |
| Output | dB, -24 to 12 | | On the mix only |

Audio: In 1-4, Out 1-4 (input times gain), Mix. CV input: Frame CV. CV outputs: Level out 1-4 (the gain, 0 to 1), Trigger. Telemetry for the page: [peepo-page.md](peepo-page.md). The keyframe list goes to the page on an atom output, `t_keyframes`, as one string (settled in milestone 2; plan, 4.7). The page's edits come in on an atom input, `commands`: moving a keyframe (milestone 3; plan, 4.8).

### Reel LFO

| Control | Kind | Notes |
| --- | --- | --- |
| Rate | 0 to 1 | 0.0134 Hz to 110 Hz, an octave every 0.077 |
| Shape | 0 to 1 | Through the wavetables |
| Shape spread | 0 to 1, centre 0.5 | |
| Spread | 0 to 1, centre 0.5 | Above: phase; below: each next output faster |
| Coupling | 0 to 1, centre 0.5 | |
| BPM | 30 to 300, default 120 | Peepo sets it from the patch tempo (overlay `"tempo": "bpm"`), as for its other tempo modules |
| Tempo sync | on or off, default off | On: a cycle lasts Note at BPM; Rate is ignored, Rate CV still adds |
| Note | 8 bars, 4 bars, 2 bars, 1 bar, 1/2, 1/4, 1/8, 1/16; default 1 bar | Free-running at that rate: Peepo has no transport, so the phase is not aligned to the beat |

CV input: Rate CV (added to Rate; 1.0 is the whole range, as Frame CV; the sum is clamped to Rate's range). CV outputs: Out 1-4, the hardware's VCA gain at the LFO's DAC codes, 1/4096 to 1.

## Levels, floor and silence

Reel copies the response curve in floating point, so level 1 is unity at every response and the in-between curves match the hardware's (within 0.04 dB from level 1/16 up). At level 0 the hardware leaves -72 to -75 dB through. **Settled (milestone 1), as recommended:** level 0 is silence (the gain is forced to 0 there), because a crossfade on a pedal should end in silence; everything above 0 follows the hardware. Level 1/65535 is silence too, since that is where the easing curves' fades to 0 stop (Keyframes, above).

Peepo's CV convention is 1.0 = 5 V, and gates and triggers are high above 0.4. The Level out ports (0 to 1) match the hardware's 0 to 5 V normalled level. The trigger is 1.0 for **10 ms** (the hardware's 1 ms is shorter than a Peepo module reading CV once per period of 256 frames could see; **tune**).

## Editing in Reel

Every rule in "Editing" above is kept, with controls for knobs and buttons:

- A **change** of a Level control is a knob move (it must differ from the last value Reel saw by at least 1/1024); a Level control that doesn't change does nothing. The values a patch loads with are not edits. Ingen sets them one by one after it has restored the keyframes and started the block, so for a second after the keyframes are restored a Level change is taken as a loaded value (milestone 1; plan, 4.2).
- **Add**, **Delete**, **Clear all** act on their rising edge.
- **Moving a keyframe** (Reel's own; the hardware can't): the page sends "move keyframe *k* to *p*" on the `commands` input. The keyframe keeps its levels and id, and stays between its neighbours and at least 1/32 from them, as Add leaves keyframes, so their order never changes.
- The active keyframe follows the **Frame control** only, as the hardware's knob; Frame CV never selects one for editing.
- Because Level controls can't follow the active keyframe's values (plugins can't write their inputs), the page shows the keyframe's values from telemetry and writes the Level control when the user moves it.

## State

Reel saves, as LV2 state: the keyframe list (position, id and four levels each). Easing, response, mode and the Level controls are control ports, saved by the host. Keys under `https://github.com/ABustos2606/reel#`, permanent once a patch is saved. Flash saving and erasing are not needed: the patch saves.

On restore the keyframes come from state and the Level controls keep their saved values without editing a keyframe. With no keyframes, the levels follow the Level controls (`SyncWithPots`).

## Bypass

Ingen's own block bypass never calls `run()`; it copies audio input *n* to audio output *n*: In 1-4 to Out 1-4 at unity, the Mix silent, the CV outputs silent. A patch that uses Mix goes silent when bypassed. **Settled (milestone 0):** an `lv2:enabled` port with `"bypass": "port"` in Peepo's overlay; off glides every gain to unity over 10 ms (Out n = In n, Mix = the sum of the inputs times Output), holds the Level out ports and keeps the trigger low (built in milestone 1).

## The Peepo page

The generic bars can't show keyframes. The Reel page draws the Frame axis with its keyframes, the current position and the four levels, and lets the user add, delete and select keyframes by touch. Specified in [peepo-page.md](peepo-page.md); its layout was accepted on 20 October 2026, before Peepo's 21 October rule that every page is built like a module page, so it needs a fresh mockup and approval before it is built.

## Decisions

Settled with the user on 20 October 2026:

- **The Reel page layout** in [peepo-page.md](peepo-page.md) (to be re-approved; see there).
- **Reel LFO is a separate plugin** in the same bundle.
- **Easing and response are plain controls**, not the hardware's long-press modes.

Settled in this revision, from the sources (20 November 2026):

- **Audio VCAs**, with Level CV outputs (section "Audio or CV").
- **No hidden gestures**: Mode and the Reel LFO plugin replace them.

Settled in milestone 0 (21 November 2026):

- **Bypass port**, as recommended: `enabled` (index 34, `lv2:enabled`), so Peepo's overlay can say `"bypass": "port"`. Off glides every gain to unity (section "Bypass"); built in milestone 1, inert until then.

Settled in milestone 1 (21 November 2026), as recommended:

- **Silence at level 0:** the gain is 0 at level 0 (and 1/65535), not the hardware's -72 dB.
- **Level defaults:** 1, so a fresh Reel with no keyframes passes all four channels.
- **The nearest keyframe for the page:** `t_nearest` is the index of the keyframe nearest the effective Frame, counted from 0, not upstream's `nearest_keyframe()` (which the trigger still uses).
- **Bounce fixed:** upstream's wraps where its curve falls; Reel's doesn't (plan, section 7).

Settled in milestone 2 (21 November 2026):

- **Telemetry of the keyframes,** as recommended: one atom output, `t_keyframes` (index 40, appended), sent when the list changes (at most 25 times a second) and every 0.5 s. The list is one string, `1:` then 24 hex digits per keyframe, because Ingen hands a watched atom output's events to its clients as Turtle literals and Peepo reads literals, not RDF lists (plan, 4.7). Checked through WSL's Ingen.
- **Reel LFO's outputs** are the hardware's: the VCA's gain at `PolyLfo`'s DAC codes at linear response, 1/4096 to 1.
- **Spread below its centre** speeds each next LFO up (measured; the spec said it slowed them).
- **A BPM for Reel LFO** (asked; the user chose "like other modules do"): `bpm`, `sync` and `note` appended (indices 10-12); Peepo sets `bpm` from the patch tempo.
- **The page:** the mockup approved as drawn; encoders as on every module page (E1 picks a bar, E2 turns it; Frame is a bar).

Settled with the user in milestone 3 (22 November 2026):

- **Moving a keyframe:** yes, as recommended: a `commands` atom input (index 41, appended), "move keyframe *k* to *p*" as one integer; the page drags a keyframe by its tag. Ingen saves the last value set on the port and sets it again when a patch loads, so moves are ignored in the second after the keyframes are restored, and the page ends each drag with 0, which does nothing (plan, 4.8).
- **Touch editing:** dragging a lane up or down on the editing target's mark sets that channel's level, starting from the keyframe's own; sideways from there still scrubs Frame.
- **Next keyframe on a stomp:** a Peepo stomp action (Next keyframe, Previous keyframe) that sets Frame onto the keyframe after (or before) it, wrapping; no plugin change. Add and Delete on stomps are their momentary controls.
- **Knobs editing a keyframe:** the hardware's rule kept; the page outlines the editing target and names it.

## Open questions

None.

## Milestones

- [x] Milestone 0 (21 November 2026, [record](milestone-0-validation.md)): vendored code, Makefile, LV2 glue and both `.ttl` files with the full port lists, state stubs. Reel passes audio through four VCAs at the Level controls. Loads in jalv and in Ingen under WSL. Tag `v0.0.1`.
- [ ] Milestone 1: keyframes: Add, Delete, Clear all, editing, easing, response, Frame CV, the trigger, LV2 state saved and restored (jalv, Carla, Ingen). Pi xrun check. Tag `v0.1.0`. *Built 21 November 2026 ([record](milestone-1-validation.md)): offline tests on x86-64 and arm64, jalv and Ingen pass; Carla is to do; the Pi check was done with milestone 2's.*
- [x] Milestone 2: the sequencer, Reel LFO, telemetry; Peepo's Reel page, first version, from an approved mockup. Tag `v0.2.0`. *Done 21-22 November 2026 ([record](milestone-2-validation.md)): offline tests on x86-64 and arm64, jalv and Ingen pass; the page built from the approved mockup; on the Pi 0 xruns in 30 minutes and the keyframes back after a reboot.*
- [ ] Milestone 3: the page complete (touch editing, moving keyframes if wanted), stomp bindings, listening on the Pi. *Built 22 November 2026 ([record](milestone-3-validation.md)): `commands` appended for moving keyframes; offline tests on x86-64 and arm64, jalv and Ingen pass; Peepo's page and stomps pass its tests on Windows. Listening on the Pi is to do.*

# The Reel page in Peepo

**Written:** 20 October 2026. **Updated 20 November 2026:** the contract follows the plan's port list (indices, `t_active` and `t_nearest` told apart, `t_step`, the keyframe list as one atom output), and the layout is marked for re-approval. **Follows:** [design.md](design.md). **Status:** the layout below was **accepted by the user on 20 October 2026**. On 21 October Peepo's rule became that every page is built like a module page: actions as header pills (main one white and first), one scrolling list of 300 x 40 bars two per row, no bottom button rows, no new bars on the small screen. The drawing's bottom row of buttons does not follow that rule. **Before anything is built, a new mockup that keeps this layout's lanes and moves its buttons into the header goes to the user and is approved.** The look is Peepo's own (Peepo `docs/03-interface-and-controls.md`, "Visual style", and its black pill-header design).

The page is Peepo code (LVGL, `src/ui/`), not an LV2 UI: Ingen runs plugins without their interfaces, and Peepo already draws its own pages for the Tuner, the Euclidean sequencer and Loo-pi. This document is the contract between the two repositories: what the page shows, and which ports it reads and writes. It follows Loo-pi's `docs/peepo-page.md`.

## The page, as accepted on 20 October 2026

800 x 480, Peepo's frame: header with breadcrumb, title and pills; footer with Back, a message line, BPM and CPU.

```
+------------------------------------------------------------------------------+
| o PEEPO // MODULE                              [KEYFRAMES] [CURVES] [ON]     |
| REEL                                                                         |
+------------------------------------------------------------------------------+
|  1 |----o--------O-----------o-----o--------------------------o----|  ch 1  |
|  2 |                                                               |  ch 2  |
|  3 |       level lanes: each channel's level drawn along Frame     |  ch 3  |
|  4 |                                                               |  ch 4  |
|    0                        ^ Frame                                1        |
+------------------------------------------------------------------------------+
| [ ADD ]  [ DELETE ]   FRAME 0.42   nearest: 3 of 7      [ CLEAR ALL ]        |
+------------------------------------------------------------------------------+
```

- **Lanes.** The Frame axis left to right; one lane per channel showing its level between keyframes (the easing drawn, step at the midpoint). Keyframes are vertical marks across all lanes, coloured by their id (`palette_` in `keyframer.cc`, 8 colours); the active keyframe (the editing target) is highlighted; the current Frame is a cursor.
- **Touch.** Tap on the axis moves Frame there; drag the cursor scrubs. Tap a keyframe mark to jump to it; the four channel bars on the right then edit it.
- **Buttons.** Add, Delete, Clear all (with confirmation). To move into the header (above).
- **CURVES** section: easing and response per channel, as four rows of choices.
- **Encoders.** Encoder 1 moves Frame; encoder 2 edits the selected channel of the active keyframe (**tune** with the user).

## The contract

Port numbers are the plan's (section 3). Symbols are permanent once a patch is saved.

### What the page writes

| Action | How |
| --- | --- |
| Move Frame | `frame` (9). Tapping a keyframe writes its exact position, so it becomes active |
| Add, Delete, Clear all | Pulse `add` (15), `delete` (16), `clear_all` (17): 1, then 0 after at least one period |
| Edit a keyframe's level | Make it active (Frame within 1/32 of it), then set `ch_n` (11-14), as on the hardware. The bar starts from the keyframe's level read from telemetry, so the first touch doesn't jump |
| Easing, response, mode | `easing_n` (18-21), `response_n` (22-25), `mode` (26), so the patch saves them |
| Move a keyframe | **Open**: a `commands` atom input (as Loo-pi's) carrying "move keyframe i to position p"; milestone 3, if the user wants it |

Frame CV never selects the active keyframe: only `frame` does (`ui.cc`). The page shows the effective Frame (`t_frame`) as the cursor and the knob's own value where it differs.

### What the page reads

Watched only while the page is open, like the Tuner's `freq_out` and Loo-pi's loop ports:

| Port | Meaning |
| --- | --- |
| `t_frame` (35) | the effective Frame (control plus CV, or the sequencer's step), 0-1 |
| `t_count` (36) | number of keyframes, 0-64 |
| `t_active` (37) | the active keyframe, the editing target, or -1 |
| `t_nearest` (38) | upstream's `nearest_keyframe()` at the effective Frame, -1 to 64 (declared range); -1 with no keyframes. Upstream counts it from 1 when Frame is nearer the keyframe after it, so milestone 1 checks what the page should be told |
| `t_step` (39) | the sequencer's step |
| `level_1` ... `level_4` (29-32) | current gains, 0-1 |
| keyframe list | **Open, recommended:** one atom output, appended in milestone 2, carrying the whole list (position, id, four levels per keyframe) each time it changes and once when the page starts watching. 64 positions and 256 levels as control outputs would be 320 ports. Decide after checking what Peepo's Ingen adapter can watch |

With the list, the page draws the lanes itself: between two keyframes, each channel's easing from `easing_n`, as `Keyframer::Easing` does.

### Peepo-side work

1. A mockup of the page, approved by the user, before anything else.
2. A Reel page in `src/ui/`, chosen by the plugin URI, as the Euclidean and Loo-pi pages are.
3. A `watch_reel` / `reel_status` in the engine interface and the Ingen session, following `watch_euclidean`.
4. The overlay entry, hiding the generic page's controls that the Reel page replaces.
5. Stomp bindings: Add, Delete, next keyframe.
6. Tests: LVGL captures at 800 x 480 of empty, one keyframe, many keyframes, sequencer mode; adapter tests for the telemetry; a live WSL check with the real plugin.

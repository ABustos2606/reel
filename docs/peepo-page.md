# The Reel page in Peepo

**Written:** 20 October 2026. **Updated 21 November 2026 (milestone 2):** the keyframe list settled as `t_keyframes` (40), one string (below); a new mockup that follows Peepo's 21 October page rule, waiting for approval (next section). **Updated 21 November 2026 (milestone 1):** `t_nearest` counts from 0; Level writes in the second after a patch loads are not edits. **Updated 20 November 2026:** the contract follows the plan's port list (indices, `t_active` and `t_nearest` told apart, `t_step`, the keyframe list as one atom output), and the layout is marked for re-approval. **Follows:** [design.md](design.md). **Status:** the layout below was **accepted by the user on 20 October 2026**. On 21 October Peepo's rule became that every page is built like a module page: actions as header pills (main one white and first), one scrolling list of 300 x 40 bars two per row, no bottom button rows, no new bars on the small screen. The drawing's bottom row of buttons does not follow that rule. **Before anything is built, a new mockup that keeps this layout's lanes and moves its buttons into the header goes to the user and is approved.** The look is Peepo's own (Peepo `docs/03-interface-and-controls.md`, "Visual style", and its black pill-header design).

The page is Peepo code (LVGL, `src/ui/`), not an LV2 UI: Ingen runs plugins without their interfaces, and Peepo already draws its own pages for the Tuner, the Euclidean sequencer and Loo-pi. This document is the contract between the two repositories: what the page shows, and which ports it reads and writes. It follows Loo-pi's `docs/peepo-page.md`.

## The new mockup (milestone 2), waiting for approval

Drawn 21 November 2026 to Peepo's rule of 21 October (header pills, the main one white and first; one list of 300 x 40 bars two to a row with the rail on the left; no bottom button rows; confirmations in the header), keeping the accepted layout's lanes. Rendered from `tools/mockup/reel-page.html` by `tools/mockup/render.sh`; the colours are the CV + UTIL group's (Reel's catalogue category is still to check, plan section 9). **Nothing is built in Peepo until the user approves it.**

| Image | State |
| --- | --- |
| `images/mockup-reel-1-on-keyframe.png` | Frame on keyframe 3 of 5: it is the editing target, outlined in white with a white tag; ADD is dim (Add stores nothing there); the Level bars show that keyframe's levels |
| `images/mockup-reel-2-between.png` | Frame at 0.40, between keyframes 2 and 3: ADD is white (the main action), DELETE dim; the Level bars are the levels Add would store |
| `images/mockup-reel-3-curves.png` | The list scrolled to CURVES (rail: CRV): Easing 1-4 as named values, Response 1-4 |
| `images/mockup-reel-4-sequencer.png` | MODE (rail: MODE): Sequencer on; the white cursor is the step's keyframe (2 of 5), while keyframe 3 stays the editing target because Frame sits on it |
| `images/mockup-reel-5-clear.png` | CLEAR ALL asks first: its two choices in the header, the list dimmed |

- **Header pills:** ADD (white when Add would store a keyframe, dim when Frame sits on one), DELETE (dim with no editing target), CLEAR ALL, ON (bypass, the `enabled` port) and ACTIONS, as on every module page. Each button pulses its port (below).
- **Lanes,** under the header, always visible: the Frame axis left to right, one lane per channel with its level along Frame, drawn with each channel's easing; keyframes as vertical marks in their colour (`palette_` by id), with numbered tags; the editing target outlined in white; Frame as the green cursor with a handle, and under it the 1/32 zone in which a keyframe becomes the target. On the right, each channel's current level (`level_1`-`level_4`).
- **Touch on the lanes:** tap moves Frame there; a tap within 1/32 of a keyframe writes its exact position, so it becomes the target; dragging scrubs.
- **The list,** under the lanes: FRAME and FRAME CV AMT; the editing target's heading (KEYFRAME 3 OF 5, or NO KEYFRAME AT 0.40) and LEVEL 1-4; CURVES (EASING 1-4, RESPONSE 1-4); MODE (Keyframes or Sequencer, a choice bar) and OUTPUT. A Level bar starts from the keyframe's level read from `t_keyframes`, so the first touch doesn't jump.
- **Encoders,** as on every module page: E1 picks a bar, E2 turns it (the accepted layout's "E1 moves Frame" is replaced by the FRAME bar under E1 and E2; **to confirm**).
- **Footer:** Frame and the editing target, or the step in Sequencer mode.

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

For a second after a patch loads (after Reel takes restored keyframes), Level writes are taken as the patch's values and edit no keyframe: Ingen sets the saved port values after the block is running, and they must not edit (plan, 4.2). The page has nothing to do about it; a user can't open the page and move a level that fast.

Add, Delete and Clear all act on the rising edge, read at every `run()` and kept until the next 8-frame grid step, so a pulse of one period is always seen.

### What the page reads

Watched only while the page is open, like the Tuner's `freq_out` and Loo-pi's loop ports:

| Port | Meaning |
| --- | --- |
| `t_frame` (35) | the effective Frame (control plus CV, or the sequencer's step), 0-1 |
| `t_count` (36) | number of keyframes, 0-64 |
| `t_active` (37) | the active keyframe, the editing target, or -1 |
| `t_nearest` (38) | the index of the keyframe nearest the effective Frame, counted from 0 (ties to the left), -1 to 63; -1 with no keyframes. Settled in milestone 1: upstream's `nearest_keyframe()` counts from 1 when Frame is nearer the keyframe after it, so it is not passed through |
| `t_step` (39) | the sequencer's step |
| `level_1` ... `level_4` (29-32) | current gains, 0-1 |
| `t_keyframes` (40) | the keyframe list (settled in milestone 2), below |

With the list, the page draws the lanes itself: between two keyframes, each channel's easing from `easing_n`, as `Keyframer::Easing` does.

### The keyframe list, `t_keyframes`

An atom output (`atom:Sequence` of `atom:String`). Watch it as the Euclidean and Loo-pi outputs are watched, with `ingen:broadcast` true on `<block>/t_keyframes` (false when the page closes). Ingen then sends each list as a `patch:Set` with property **`ingen:activity`** (not `ingen:value`) and the text as a plain literal, for example (two keyframes, from WSL's Ingen):

```
[] a patch:Set ;
	patch:subject <ingen:/main/reel/t_keyframes> ;
	patch:property ingen:activity ;
	patch:value "1:33330000ffff800000004000cccc000100004000ffffbfff" .
```

- **Text:** `1:` (the format; anything else is not understood and is ignored), then 24 lowercase hex digits per keyframe, in position order: position, id, level 1, level 2, level 3, level 4, four digits each, 0 to 65535 (divide by 65535 for 0 to 1). `1:` alone means no keyframes. At most 64 keyframes, 1538 characters.
- **When:** within 40 ms of a change, at most 25 times a second, and every 0.5 s while nothing changes, so a page that starts watching has the list within half a second, and a lost message is replaced.
- **Colour:** keyframe `k`'s mark takes `palette_[id % 8]`. **Index:** `t_active`, `t_nearest` and `t_step` count keyframes from 0 in this list's order.
- These values are telemetry: never saved, never written back, and they must not mark the patch modified (as Loo-pi's `commands`).

### Peepo-side work

1. A mockup of the page, approved by the user, before anything else.
2. A Reel page in `src/ui/`, chosen by the plugin URI, as the Euclidean and Loo-pi pages are.
3. A `watch_reel` / `reel_status` in the engine interface and the Ingen session, following `watch_euclidean`: the control outputs `t_frame`, `t_count`, `t_active`, `t_nearest`, `t_step`, `level_1`-`level_4` as `ingen:value`, and `t_keyframes` as `ingen:activity` (above); `Session::merge` must take `ingen:activity` on `t_keyframes` without treating it as a change to the graph.
4. The overlay entry, hiding the generic page's controls that the Reel page replaces.
5. Stomp bindings: Add, Delete, next keyframe.
6. Tests: LVGL captures at 800 x 480 of empty, one keyframe, many keyframes, sequencer mode; adapter tests for the telemetry; a live WSL check with the real plugin.

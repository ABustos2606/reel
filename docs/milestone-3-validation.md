<!-- SPDX-License-Identifier: MIT -->
# Milestone 3 validation

**Built: offline, in jalv and Ingen, and Peepo's page and stomps in Peepo's tests; listening on the Pi is to do.** Recorded 22 November 2026. The page can now move a keyframe. As the user chose, Reel has a new atom input, port 41 `commands`, that takes "move keyframe *k* to *p*" as one integer. A moved keyframe keeps its levels and id and stays 1/32 from its neighbours. In Peepo, dragging a keyframe's tag on the Reel page moves it. Dragging a lane up or down on the editing target's mark sets that channel's level. Two new stomp actions, Next keyframe and Previous keyframe, set Frame onto the keyframe after or before it, wrapping at the ends. The checks cover the move rules, slicing as Ingen and jalv do it, and patch loading. They also cover threads (TSan), ASan and UBSan, allocation, x86-64 and arm64, a move sent through WSL's Ingen as Peepo sends it, and the page and stomps in Peepo's adapter and LVGL tests. Not covered: Carla (not installed), and the page and stomps on the Pi, by touch and by ear (below).

| Check | Result |
| --- | --- |
| x86-64 `make -j8 test`, Debian 13 under WSL | PASS, 29 checks (`ttl_check`, 10 `scaffold`, 12 `keyframes`, 6 `lfo`); GCC 14.2; no warnings from Reel's code |
| `make test SAN=1` | PASS, 27 checks (both allocation checks skipped under sanitizers) |
| `make test SAN=thread` (under `setarch -R`) | PASS, no reports |
| arm64 `make -j8 test BUILD_DIR=build/arm64 OPT="-O2 -mcpu=cortex-a76"` | PASS, 29 checks; Debian 12 root `/opt/peepo-arm64`, GCC 12.2, QEMU user emulation, from a temporary copy removed after the check |
| `make install DESTDIR=build/stage PREFIX=/opt/peepo` | PASS; `reel.so`, three Turtle files, `LICENSE`, `THIRD_PARTY_NOTICES` |
| `lv2info` on the staged bundle | Reel: 42 ports, port 41 `commands` an `atom:AtomPort` input; Reel LFO: 13 ports |
| `nm -D --defined-only reel.so`, x86-64 and arm64 | Only `lv2_descriptor` |
| `lv2lint`, both plugins | No errors; the author-email warning of milestone 0 |
| `make test-timing`, x86-64 under WSL, 48 kHz, 128 frames, 10 simulated minutes | Reel (moves now among the random edits, a save every 10 ms from another thread): mean 2.9 us per period (0.11% load), 99.9% 24 us, worst 0.67 ms. Reel LFO: mean 6.3 us, 99.9% 34 us, worst 1.1 ms. The worst cases are WSL scheduling |
| `make test-hosts`, jalv, 1024- and 128-frame periods | As in milestone 2 |
| `make test-hosts`, Peepo's Ingen, 1 and 4 threads, 1024- and 128-frame periods | Milestone 2's checks pass unchanged. **Moves:** `patch:Set` of `ingen:value` `"16882073"^^xsd:int` on `ingen:/main/reel/commands` moves keyframe 2 from 0.8 to 0.6 (the watching client's `t_keyframes` says so, levels and ids kept), a second move puts it back, then 0. The saved patch has `<reel/commands> ingen:value 0`; the reload's keyframes and levels are milestone 2's. 0 xruns logged by JACK |
| Carla | **Not done**: not installed in WSL, and Debian 13 has no `carla` package |
| Peepo, Windows (`cmake --build --preset windows`, `ctest --preset windows`) | PASS, 8 of 8. `ingen_adapter`: a stomp's Reel list watched with the page closed and taken as telemetry, not an edit; the page starting from it; dropped when no stomp needs it; a move sent as `xsd:int` on `commands`, then 0; nothing sent without the port, when disconnected or for a negative value; Peepo's `reel_step` and `reel_move_limit` against Reel's rules. `physical_controls`: Next and Previous keyframe assignments encoded and decoded. `ui_workflows`: dragging up on the target's lane sets Level 3 from the keyframe's 0.6 to 0.8, Frame unmoved; sideways scrubs Frame; a keyframe's tag sends the move, then 0, and the footer says where it went; it stops 1/32 before its neighbour; dragging the target's tag takes Frame with it; stomps step Frame to the next keyframe, wrap after the last, and step back. `ui_workflows` hung once of five runs after every check passed, in LVGL's Windows teardown, as Peepo's plan records (P14.3) |
| **Pi: the page and stomps by touch, and listening** | **To do** (below) |

## What the offline tests cover

New in this milestone, through the in-process host:

- `commands` (`tests/test_keyframes.cpp`), with the host preparing `commands` as Ingen does (one sequence for every slice of a period, times from its start) and as jalv does (fresh before every `run()`). After a move, keyframe 2 is at 0.6 with its id and levels, and `t_keyframes` says so within 40 ms. Moved toward 0.79 it stops 1/32 before the next keyframe; toward 0 it stops 1/32 after the previous one. The first and last keyframes go to 0 and 1. Malformed values, op 2, keyframe 64 and keyframes that don't exist move nothing. After the moves, the Level outs at 1000 positions equal those of a list added at the new positions. With Frame on keyframe 2, moving it away leaves no editing target, so a Level change edits nothing stored; Frame on it again makes it the target and a Level change edits it. A keyframe moved onto Frame becomes the target. In Sequencer mode the step stays and Frame is the step's new position. A move set within a second of a restore is ignored, and one after it moves. Without `urid:map` nothing is read.
- `slicing`: the performance now sends a move every fourth change; whole periods, random slices and 37-frame periods still give identical outputs and lists.
- `allocation` and `rt_timing` include moves.
- `ttl_check`: 42 ports; `commands` is an atom input, a sequence of integers, with its symbol frozen after `t_keyframes`.

## Found and fixed during the milestone

- **Ingen hands every slice of a period the same atom input.** `Buffer::port_data` ignores the slice's offset for atom ports, so each `run()` of a split period sees the same events, timed from the period's start. A move is absolute, so applying it twice changes nothing. Reel still skips a sequence identical to the one it read last, so a Delete in a later slice of the same period can't renumber the keyframes under a repeated move (plan, 4.8). Loo-pi's `commands` reads every slice's events as new (its `CommandEvents`), which could repeat a press when Ingen splits a period at a control change. That is worth a look in Loo-pi.
- **Ingen saves the last value set on an atom input and sets it again on load.** Loo-pi's saved patches have `<loopi/commands> ingen:value 778`. For Reel a replayed move could have moved whichever keyframe now has that number. So moves are ignored in the load second, as Level changes are, and the page ends every drag with 0. `make test-hosts` checks that the saved value is 0.
- **Peepo: the point where a finger lifts was lost.** The lanes reacted to pressing and moving only. When the last movement arrived with the release, as in Peepo's LVGL test, a Level drag stopped short (0.77 instead of 0.80). The release point now counts.

## Decisions taken in this milestone

Recorded in the spec (Decisions, 22 November 2026), chosen by the user: moving keyframes through a `commands` input, with the page dragging a keyframe by its tag; touch editing by dragging a lane up or down on the editing target's mark; Next and Previous keyframe as Peepo stomp actions, with no plugin change. Both plugins are `lv2:minorVersion` 3.

## On the Pi (to do)

Peepo's interface on the Pi predates the Reel page (milestone 2's record), so the next interface install brings the page, these gestures and the stomps. Then, with Reel `v0.3.0` pinned in Peepo:

1. On the page: drag a keyframe's tag, drag a lane on the editing target, scrub Frame; check that dragging a lane never moves the page.
2. Assign Next keyframe and Previous keyframe to two stomps; step through a crossfade between two effects with a guitar, and listen for clicks at each step (Frame glides over 5 ms).
3. Save, reboot, load: the moved keyframes come back, and nothing moves on load.

`tools/pi_check.py`'s xrun check needs no rerun for this milestone: `run()` changes only by reading the `commands` input, and `rt_timing` measures it.

## Not in this milestone

Carla. A new page mockup was not drawn: the gestures add nothing to the approved layout.

## Release

`v0.3.0` (Turtle `lv2:minorVersion 3`, `lv2:microVersion 0`, both plugins) is ready to tag once the Pi check passes. Peepo's pin (`tools/ingen-spike/14-more-plugins.sh`, `reference/manifest.json`) moves to it then; Peepo's page and stomps need Reel 0.3's `commands` port to move keyframes, and without it a drag moves nothing. Nothing is committed, tagged or pushed.

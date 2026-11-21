# Reel

An LV2 keyframe mixer and four-output LFO, written for Peepo and other LV2
hosts. MIT licensed, Copyright (c) 2026 Alejandro Bustos.

**Status, 20 November 2026: planned.** This repository holds the spec, the
implementation plan and the Peepo page contract only, all checked against the
upstream source. Nothing is built yet; milestone 0 starts once the spec's
bypass question is settled, and the Reel page needs a fresh mockup approved
before it is built.

Reel stores up to 64 snapshots of four channel levels along a Frame axis.
Moving Frame, by hand or by CV, morphs the levels from one snapshot to the
next, each channel with its own easing curve. The levels set four VCAs on
four audio inputs, a mix output, and four level CV outputs, so one control
can crossfade effects or move several modulation depths together. A sequencer
mode steps through the snapshots on each clock, and a trigger output fires
each time Frame passes a snapshot. Reel LFO, a second plugin in the same
bundle, gives four related LFO outputs with spread shapes, phases or rates.
The snapshots are saved with the patch.

| Plugin | URI |
| --- | --- |
| Reel | `https://github.com/ABustos2606/reel#reel` |
| Reel LFO | `https://github.com/ABustos2606/reel#lfo` |

Build on Linux with a C++17 compiler, GNU Make, pkg-config and LV2 headers
(`build-essential pkg-config lv2-dev` on Debian). The build needs no network:

```sh
make
make install PREFIX=/usr/local
make install LV2DIR=/opt/peepo/lib/lv2     # where Peepo finds plugins
make OPT="-O2 -mcpu=cortex-a76"            # native Pi build
make test
```

The behaviour specification is [docs/design.md](docs/design.md); the build
and milestone requirements are in
[docs/implementation-plan.md](docs/implementation-plan.md); Peepo's Reel page
is specified in [docs/peepo-page.md](docs/peepo-page.md). Do not rename plugin
URIs, port symbols or state keys after patches use them. Peepo's catalogue
overlay, the Reel page, pinned build integration and hardware evidence belong
in Peepo's repository.

## Credits

Reel's keyframer and LFO are the code of Frames, a eurorack module designed
by Emilie Gillet, published under the MIT license at
https://github.com/pichenettes/eurorack. The vendored files in
`third_party/eurorack/` keep their copyright notices. Reel is not affiliated
with or endorsed by Mutable Instruments, a registered trademark.

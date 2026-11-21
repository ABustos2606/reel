<!-- SPDX-License-Identifier: MIT -->
# Vendored upstream code

The keyframer and poly LFO of Frames, a eurorack module by Emilie Gillet, with their lookup tables, and the parts of her `stmlib` they need. MIT licensed: the notice is at the top of every frames file, and in `stmlib/LICENSE`. Copied by `tools/vendor.sh`; do not edit by hand.

| Source | Commit |
| --- | --- |
| https://github.com/pichenettes/eurorack | `f4cfa7697095a28d292bff758beb51deb49b5a86` (12 October 2026) |
| https://github.com/pichenettes/stmlib (eurorack's `stmlib` submodule at that commit) | `e3bd7c9cc00e4364166f9905c0509b6ffd0535ec` |

## Files

- `frames/keyframer.{h,cc}`: the keyframe list, easing and the VCA's response code.
- `frames/poly_lfo.{h,cc}`: the four-output LFO.
- `frames/resources.{h,cc}`: the lookup tables and LFO wavetables; `frames/resources/lookup_tables.py`, the script that made them, kept for reference (the build does not run it).
- `stmlib/`: `LICENSE`, `stmlib.h`, `utils/dsp.h`.

Reel compiles these with `-DTEST`, as upstream's own desktop builds do. That compiles out the keyframer's flash storage (`stmlib/system/storage.h` is not included, so it is not vendored): `Keyframer::Init`, `Save` and `Calibrate` touch no storage, and `Init` sets nothing, so Reel's glue sets the defaults the hardware's `Init` sets on a fresh unit.

## Local changes

None. If one is ever needed it goes in `tools/patches/`, applied by `tools/vendor.sh`, and is listed here.

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

Applied by `tools/vendor.sh` from `tools/patches/`; each changed line is marked `Reel:` in the file.

- `keyframer-easing-bounds.patch` (milestone 1, 21 November 2026), `frames/keyframer.cc`:
  - `Keyframer::Easing` multiplies the table step by the fraction `(scale << 10) & 0xffff`, an unsigned value. Where the table falls (only Bounce's does) the product turns unsigned and the shaped scale comes out 131072 too high; the next multiplication then overflows `int`, undefined behaviour. With wrapping arithmetic, as the hardware has, a Bounce fade between two levels jumps by up to half of full scale in its falling parts, unless the levels differ by about 1/2 or all of full scale. The fraction is now an `int32_t`.
  - `Easing` read one entry past its 1025-entry table at scale 65536 (Frame exactly on the right keyframe), where the entry's weight is 0. It reads the last entry instead; the result is unchanged.
  - `Evaluate` read `keyframes_[64]` past the 64th keyframe to find the nearest keyframe. It reads 0 there, as the cleared slots after the last keyframe hold.

Without the patch, `make test SAN=1` reports all three and the `easing` test fails on Bounce.

## Build flag

Not a change to the files. The vendored code is compiled with `-fwrapv` (milestone 2, 21 November 2026; `Makefile`, `VENDOR_FLAGS`). stmlib's `Interpolate824` and `Crossfade` for 8-bit tables (`stmlib/utils/dsp.h`, used by `PolyLfo::Render`) multiply a table step by a 24- or 16-bit fraction in `int`, which overflows: undefined behaviour, reported by `make test SAN=1`. Wrapped, as the hardware's arithmetic does, the product is off by a multiple of 2^32; after `>> 16` that is a multiple of 65536, which the functions' `int16_t` result drops, so the result is exact. `-fwrapv` makes the wrapping defined.

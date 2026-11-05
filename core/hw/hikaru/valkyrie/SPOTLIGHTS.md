# Experimental spotlight decoding

The optional Hikaru renderer uses guest command `451` for the spotlight cutoff,
penumbra scale and curve selector, and command `194` for uploaded curves. It keeps
positional Lambert lighting and distance attenuation separate from angular
falloff. The game’s enable bit controls whether angular falloff applies.

Air Trix's command builder establishes these payload fields:

- Bits 0–15 contain a cosine multiplied by 32768 and truncated. Observed cones
  use nonnegative cosines; unsigned Q1.15 preserves the exact 1 endpoint, `0x8000`.
- Bits 16–23 contain the scale code
  `(float_bits(1.0) - float_bits(cos(inner) - cos(outer))) >> 19`, masked to eight bits.
- The curve selector starts at bit 25. The current implementation selects four
  curves using bits 25–26 and preserves the complete raw payload.

The software curve uploader writes 16 samples per curve, pairing the earlier
sample in the high halfword. [Sega patent US6333742B1, Figure 19](https://patents.google.com/patent/US6333742B1/en)
describes the corresponding sequence: direction dot product, cutoff subtraction,
penumbra scaling, clamping and interpolated curve lookup. The patent does not
identify Hikaru opcodes or establish their bit formats.

The current rendering contract has explicit limits:

- Scale reconstruction as four exponent and four mantissa bits is inferred from
  the game's reciprocal encoding; hardware rounding has not been measured.
- The 16 Q10 curve samples are placed at 1/16 through 1, with an implicit zero
  endpoint. The captured linear curve supports this interpretation, but exact
  hardware interpolation rounding remains unverified.
- The `194` bank is provisionally the active light slot and the curve is selected
  by `451`. Captured bank/curve matrices are symmetric, including custom curve 3 in
  bank 3, so they cannot distinguish this assignment from its transpose.
- Negative-cosine cutoffs and payload bits beyond the known fields remain
  unverified. No broader cone domain is claimed.
- Curves are read when the queued frame is drawn. Mid-frame table replacement
  needs a separate capture before introducing per-mesh table snapshots.
- The old `451` RGB/specular alias is retained to isolate the angular change;
  actual specular semantics remain separate work.

The standalone `hikaru_valkyrie_spot_pixels` target reaches the real `451` handler,
production GLSL and uniform cache. It covers captured 14°/22.5° cones, cutoff and
lookup endpoints, compressed scale, nonlinear interpolation, payload storage,
disabled/absent `451` and point/directional/unlit controls. It checks the documented
experimental contract and does not serve as a hardware golden reference.

Spotlight parameter and curve uploads are cached per linked program and light.
Changed controls, changed table words and program replacement invalidate the
corresponding values. Pixel and upload-counter tests cover these transitions.

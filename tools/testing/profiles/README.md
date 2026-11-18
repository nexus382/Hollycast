<!-- Portions Copyright 2026 The Hollycast Authors -->
# Planet Harriers mappings

These optional mappings require a Hikaru-enabled branch and the exact tested
content and ROM identities. Current dev does not include Hikaru game support.

`planet-harriers-live.json` supplies the first gameplay profile. The game's
`System` table at `0x0C1D41D4` labels main/sub mode bytes at `0x0C27D108/109`.
The `Player %d Info` table at `0x0C1D5FC8` labels global XYZ position, energy,
and stage rate at the offsets in the profile. Its read-only type codes `0x18`
and `0x15` describe floats and signed 16-bit values respectively; adjacent
byte fields establish the width of the mode codes.

Live checks followed native START input into character selection (`main_mode=1`),
confirmation into `MD_GAME` (`main_mode=3`), the stage introduction, and stage
gameplay. The game itself supplied the `MD_GAME` name. Position and stage rate
changed during stage movement; the tested player started with energy 30.
These observations establish the sampled fields and those two mode labels.
They do not establish every player's layout, damage/continue behavior, energy
to HUD-heart conversion, or all mode/sub-mode names. In particular, `game`
also includes its introduction; it does not alone establish active combat.

Units retain the game's values. Stage rate is not advertised as a percentage,
and energy is not converted to hearts. The live profile is validated for the
tested first-player session. Score, remaining time, numeric stage identity,
other players, and unobserved mode meanings remain unmapped.

## Renderer diagnostics

`planet-harriers-diagnostics.json` maps the game's own renderer diagnostic
variables, bound to the tested archive SHA-256 and assembled BIOS/program/data
CRC32 identities. It contains addresses and metadata; it contains no game data.

The game's diagnostic table at master RAM physical address `0x0C1D4E38`
contains 16-byte records with a format-string pointer and a variable pointer.
Entries at `0x0C1D4E48` through `0x0C1D4F78` associate the mapped variables
with `TexUpdateEnd`, `CmdsAnalyzeEnd`, `PolygonOutEnd`, `PlotEnd`, and the
primitive/material/light counts. Their `%ld` formats identify 32-bit signed
values. The upstream observation in
`core/hw/hikaru/valkyrie/src/mach/hikaru/hikaru-gpu-private.h` also identifies
this table and the IRQ-updated variables beginning at `0x0C27D0BC`.

Offsets are relative to master RAM base `0x0C000000`. End markers retain raw
guest values because their conversion to elapsed time has not been established.
These are game renderer diagnostics, not host GPU timings or gameplay progress.
The debug `START_STAGE` setting is deliberately excluded: a requested debug
start stage does not establish the current stage.

Add further fields only after verifying their storage, width, lifetime, and
meaning against reproducible game transitions. Repacked archives require a
separately verified content identity; do not disable the identity checks.

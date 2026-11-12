<!-- Portions Copyright 2026 The Hollycast Authors -->
# Experimental Sega Hikaru support

The optional Hikaru backend recognizes **Air Trix (`airtrix`)** and
**Brave Firefighters (`braveff`)**, **Planet Harriers (`pharrier`)**, and
**Virtual-On Force (`von4`)**, **NASCAR Arcade Rev A (`sgnascar`)**, and
**Star Wars Racer Arcade (`swracer`)** in the
normal game library and loader. It uses
a separate Hikaru CPU/GPU backend. Air Trix testing has reached attract mode, player/stage selection,
and an active skating stage with calibrated steering and awarded trick coins.
Compatibility is experimental: rendering and
timing are still under investigation. The desktop frontend runs both AICA/ARM7
sound boards and produces music and effects, but playback currently breaks up
when emulation cannot supply samples in real time. Other
Hikaru games and the `airtrixo` revision remain unsupported. Planet Harriers
uses its own 315-5881 key, four sparse program-ROM pairs, eight data-ROM pairs,
and separate nonvolatile storage. It is experimental; cabinet networking is
not emulated. Brave Firefighters
has a separately validated ROM manifest, ROM-board layout and unpopulated
protection-device mapping. Native testing now reaches its first firefighting
stage with coherent HUD and room textures, registered sound banks, and music
and effects confirmed audible on macOS. Full cabinet calibration remains
unverified. Each title keeps its own battery-backed RAM and MIE EEPROM files.

Tested Apple Silicon scenes can reach real time at the native 200 MHz setting;
heavier rendering and competing host workloads can still reduce speed. Startup
includes the game's warning screen and a blank loading period; these take
longer at reduced emulation speed. The image keeps a 4:3 aspect
ratio. **Internal Resolution** in Hollycast's video settings now controls the
3D render targets: Native is 640×480 and x2 is 1280×960. Higher resolutions
increase GPU and memory use; original textures and CPU-drawn 2D layers keep
their native pixel data. Direct 16-bit overlays now respect coverage alpha,
and framebuffer copies preserve pixel order at odd coordinates and widths.
These correct washed-out selection panels and striped results lettering.
Untextured, unlit geometry now uses its primary material colour unless the
alternate-colour flag is set. This restores separate white letters and blue
backing for messages such as `FAILURE`, grey timer geometry and coin outlines,
and black geometry previously coloured pink. The textured HUD retains its
existing colour path, including green counters and blue/orange trick labels.
The change was checked with shader pixel tests and a scripted game replay.
Opaque geometry now receives its guest-supplied depth bias too, restoring
projected shadow coverage and the timer's front faces. Offset state changes
only when the bias changes. Fully transparent texture fragments are discarded
so their RGB and depth cannot hide geometry behind a texture cutout. These
paths have shader/depth regressions and saved-frame rendering checks.
Brave Firefighters exposed an inherited global UV translation derived from
an unresolved `0D1` command. Raw texture coordinates already select the correct
atlas regions; removing that speculative translation restores its HUD, room
surfaces and fire sprites. A real command-packet regression covers 192 cases,
including all recorded modes and doubled-size 4-bit textures. The command
state is retained for research; its hardware meaning remains unresolved.
The title screen and the return from Game Over use a combined bitmap mode.
Layer enable is decoded separately from the direct/combined mode, so those
bitmaps remain visible when direct overlays are switched off. The combined
path places the title background and prompt/credits beneath the scene while
preserving the existing gameplay overlay order. This Air Trix composition
model is inferred from captured frames and guest mode changes; register `1BC`
and broader hardware priority remain unverified. Synthetic tests cover raw
register writes, mode transitions, individual enables, and scene transparency.
Spotlights now use the game's encoded cutoff, penumbra scale and uploaded
falloff curves, replacing a fixed hard edge with the authored spread and fade.
The encoding was traced through Air Trix's command generator and checked with
synthetic shader cases and saved selection/gameplay frames. The OpenGL material path decodes texture/color selection, color modulation,
emission and blend modes with scene coverage preserved through bitmap
composition. Guest fog controls and screen-door patterns are also applied.
These rules address the black faces, triangular smoke and missing atmosphere
seen in Brave Firefighters; visual comparison is still required for the latest
fog changes. Native Vulkan and Metal shaders still need the corresponding
material and pattern equations. Exact spotlight rounding, mixed
bank/curve selection and other interpretation limits are described in
[SPOTLIGHTS.md](../core/hw/hikaru/valkyrie/SPOTLIGHTS.md); these changes do not
establish hardware-accurate rendering.

**Match audio to game speed** in Audio settings is enabled by default for
Hikaru. It stretches the stereo mix without lowering pitch when emulation runs
slowly. This adds buffering and slows the music with the game; it does not
restore full-speed gameplay. It can be disabled to hear the original sample
timing, which produces gaps when emulation is too slow. Pacing keeps a continuous
SoundTouch processing history even at unity tempo, adding its processing
latency. The bundled library explicitly disables its former unity bypass
because switching across unity can click; the host does not re-enable it.
The SDL backend consumes available Hikaru samples during a partial underrun
and silences only the missing tail. Converted samples that exceed one callback
are carried into the next callback. This reduces avoidable gaps but cannot
replace samples that emulation has not produced.

The sound-board aperture now models an estimated **320 ns CPU-read service
time**. Without external-bus waiting, Brave Firefighters abandons its first
sound-bank handshake before the ARM firmware responds. The estimate applies
to resolved sound RAM on either board, independently of the ROM set, and is
converted using the selected SH-4 clock. Fractional cycles and outstanding
waits carry across scheduler slices; SH-4 and memory-controller DMA are
excluded. This is a compatibility timing approximation, not a measured bridge
latency. Tests cover 100–300 MHz, fractional rates, short slices, delay slots,
remapping, DMA and state round-trips. Changing the clock during an outstanding
submicrosecond wait does not rescale debt already incurred. The private vendor
state format is version 3; frontend snapshots additionally include both sound
boards, I/O, protection and card-reader state.

Both SH-4 CPUs have independent registers and local RAM. The playback engine
runs the master and slave in alternating slices on one host thread, then
advances their shared devices. This approximates the original board's concurrent
processors; it is not a cycle-accurate timing model. Shared command memory,
cross-CPU memory access, and interrupt ordering currently prevent assigning the
CPUs to independent host threads safely.
Hikaru uses its interpreter regardless of the Dreamcast **CPU Mode** setting.
The shared **SH4 Clock** setting adjusts both Hikaru CPUs, with a default of
200 MHz each; video and audio clocks remain unchanged. Higher settings require
more host CPU work and can change game timing.
Instruction timing is simplified too: the interpreter generally charges one
unit per instruction. Real SH-4s can issue compatible instruction pairs and
stall for dependencies, caches, and bus access. Matching the board's nominal
CPU clock therefore does not establish accurate instruction timing.

The desktop SDL frontend uses adaptive VSync for Hikaru when supported and a
working audio queue is pacing the game. This avoids an extra display wait after
a slow frame. Unsupported drivers fall back to ordinary VSync. Every guest
frame is rendered; automatic frame skipping is not enabled.

## Shared performance paths and diagnostics

With multi-threading enabled, one worker advances both SH-4 CPUs and their
shared devices while the graphics thread renders the preceding frame. Two
reusable packets retain each frame's geometry, textures, bitmap layers and
lighting state; the renderer never reads memory being changed by the worker.
The graphics thread owns all graphics resources and keeps the selected
transparency ordering. Queue backpressure limits producer lead to one frame;
normal rendering does not overwrite or drop completed packets. A one-time
texture-cache refresh preserves updates when queued work is discarded.

Frame audio is collected with emulation and submitted by the consumer. Pause,
reset, loading, graphics changes and shutdown quiesce the producer before
changing machine or graphics ownership. Saving uses a completed machine
boundary, which may be one frame ahead of the displayed image. Loading discards
queued work from the prior timeline before presenting restored frames.

Bitmap-layer textures and their 16-bit conversion staging memory are retained
until the renderer is destroyed. Every active layer still receives a full-image
update each frame; its alpha, pixel layout and compositing order are unchanged.
Native texture updates are ordered after prior draws and before later draws.

MEMCTL DMA can bulk-copy disjoint, contiguous plain little-endian RAM ranges.
Overlaps, custom buffer accessors, device windows and other bus side effects
retain the per-word path. Both paths consume staging data immediately at the
start command, preserving VON4's model-loader requirements.

Set the process environment variable `HIKARU_TRACE_TIMING=1` before launch to
report average host wall time every 60 completed frames. The phase line separates
master/slave SH-4 execution, MEMCTL DMA, MIE, AICA advance, GPU command processing,
render submission, host graphics-state preservation and remaining frame work.
Nested DMA is charged to MEMCTL rather than counted again in its CPU phase.
Other synchronous device I/O remains in the phase that invoked it. In the
threaded path, the engine phase line measures producer work, including packet
capture; drawing is measured separately by the frontend. Rendering measurements
include host submission and any driver waits, not asynchronous GPU execution
time. The frontend reports worker wait time, measured build/draw overlap and native
presentation time; OpenGL's swap remains
in the outer host timing line. Use that outer total when comparing frame time
so worker waits and presentation are included. Profiling is off by default and
does not change guest clocks, scanline boundaries, sorting or audio pacing.

## Build and run

Hikaru is enabled by default in standalone Windows, Android, Linux and macOS
builds. Other desktop hosts can opt in if their graphics requirements are met;
those configurations are unverified. It can be forced on or off at configure time:

```sh
cmake --preset <PRESET> -DUSE_HIKARU=ON
cmake --build --preset <PRESET>
```

Optimized GNU/Clang builds use `-O3` for the optional playback engine without
enabling fast-math. `-DHIKARU_VENDOR_OPTIMIZE=OFF` keeps the build configuration's
original optimization flags. Debug and sanitizer builds retain their requested
flags.

Hikaru supports desktop OpenGL 3.2, OpenGL ES 3.0 on Android, and native
Vulkan, Metal or Direct3D 11 where those backends are available. On Windows,
choose DirectX 11 for the native Direct3D path. DirectX 9 and OpenGL ES 2
cannot run Hikaru. The Advanced **Multi-threading** setting enables Hikaru's
bounded emulation/render pipeline. Disabling it uses the direct frame path.

**Transparent Sorting** in Video settings selects per-strip, per-triangle or
per-pixel sorting for translucent geometry. Per-strip sorts retained strips
by transformed depth centroid; per-triangle sorts individual triangles by
their transformed depth centroids. Per-pixel sorting uses depth peeling and
retains the configured **Maximum Layers** count, from 1 to 128 (default 32).
If another layer is present beyond that bound, deeper fragments are discarded
and a warning is reported. Hikaru keeps its sorting and layer settings separate
from Dreamcast/Naomi. Returning to those systems recreates their requested
host renderer. Hikaru retains its per-pixel layer setting when changing the
graphics API. The pixel-buffer memory budget applies to its depth-peeling
storage.

Depth peeling submits visibility queries in batches. Each viewport uses its
previous depth as an initial estimate and reads the current frame's results
before extending the batch, up to the configured limit. Vulkan batches clears and texture uploads with draws, retains
transient resources until their submission fences complete, and reuses
compatible texture allocations. CPU readbacks wait for a submission fence;
renderer destruction still drains the device to release presentation views.
These changes remove synchronization costs; they do not establish a measured
frame-rate improvement.

Malformed or unreadable nonvolatile files now use in-memory defaults for the
session. They are preserved and will not be overwritten on exit. The frontend
retains cartridge ROM storage for the engine instead of copying it again;
the BIOS is still copied privately for the optional compatibility patch.

Place a supported game ZIP (`airtrix`, `braveff`, `pharrier`, `von4`,
`sgnascar`, or `swracer`) beside `hikaru.zip` and
`mie.zip`, then add their folder to the game library or open the game with
**File > Load ROM**. `.7z` archives are also accepted. The loader also searches
the configured Naomi BIOS folders for the Hikaru and MIE archives. Every
required chip is checked for size and CRC before execution; ROMs may be found
by their original name or CRC.

The tested BIOS is **0.92, `epr-23400.ic94`**. The loader prefers this revision
when several are present. Revisions 0.84 and 0.96 have manifests but have not
received equivalent gameplay testing. MIE requires the original 2 KiB
`315-6146.bin` firmware. The Air Trix manifest contains two 4 MiB program chips
and eight 16 MiB data chips. Brave Firefighters has six 2 MiB program chips and
24 8 MiB data chips, with sparse 16 MiB program slots; see `core/hw/hikaru/roms.cpp` for exact names and
checksums. No BIOS, firmware or game ROM images, decrypted program images, or factory data
are distributed with the source or used by the automated synthetic tests.

An inherited BIOS EEPROM-check bypass is enabled for the experimental player.
It changes a private in-memory BIOS copy; original files are not modified.
Game instructions and animation data are not patched.

## Controls

Hollycast uses its existing input mapping. These are the default keyboard
bindings; custom mappings may differ:

The Controls tab shows **Game compatibility controls** only while a Hikaru
title is loaded. Each cabinet has its own actions in Controller Mapping:
Air Trix board/kick controls, Brave Firefighters hose controls, and Planet
Harriers flight/weapon controls. Brave's mouse-aim option appears only with
Brave loaded. The options retain their values when switching titles, but only
the matching cabinet uses them. Existing controller mapping files are retained.

| Air Trix control | Default keyboard | Default SDL arcade gamepad |
| --- | --- | --- |
| Coin | A (US keyboard layout), or D | Right shoulder |
| Start / confirm | Return | Start |
| Tail kick (DOWN switch) | X | A |
| Nose kick (UP switch) | C | B |
| Cabinet left / right buttons | F / V (also S for right) | X / Y (or triggers) |
| Board yaw (swing) | J / L | Left-stick horizontal |
| Board roll (tilt) | I / K | Left-stick vertical |
| Operator test / service | F2 / F3 | Remap Test / Service as needed |

Enable **D-pad board steering** for Air Trix to use left/right for yaw and
up/down for roll, leaving the action buttons free for separate kicks.
Left/right still operate the cabinet buttons for menu navigation. The option
defaults off to preserve the original D-pad up/down kick switches and analog
board motion. Opposing D-pad directions leave the corresponding analog axis intact.
The former C/X credit aliases are now cabinet left/right actions; Coin and
button 5 provide credits instead. Custom device mappings can differ from the
defaults above; use the named actions in Controller Mapping.

Air Trix tricks combine board motion and nose/tail kick timing. Steering alone
is not a trick command. Sega's service manual describes yaw, roll and the
independent kick switches in its [Input Test section](https://usermanual.wiki/Arcade/AirTrixManual.1962453288.pdf).
The new compatibility mappings are covered by host regressions; gameplay and
individual trick combinations still require a controller test. The manual also
documents **Game Test Mode > Game Assignments > Language > English**; check
whether your regional ROM provides that option to make the tutorial readable.

On macOS, **System > Insert Credit** inserts a credit for the loaded Hikaru
game through the same cabinet input path as a physical coin button.
Brave retains its unused C/X credit aliases. Planet reserves those buttons
for weapons/views and uses the dedicated Coin binding.

The game requires foot-controller calibration on a fresh machine. Its axis
endpoints are stored in the MIE EEPROM; blank endpoints cause the game to ignore
steering. The frontend preserves the original 128-byte EEPROM in
`airtrix.mie.eeprom` and the separate 64 KiB backup RAM in
`airtrix.hikaru.nvmem`, both in Hollycast's writable data directory. Files are
saved on graphics teardown/game close and restored after engine creation.
Calibration data is created by the game, never embedded in the source.

For a new calibration, press F2 to enter the operator menu. Use F3 to move
the selection and F2 to choose
**Game Test Mode > Calibration > Volume Calibration**.
Start calibration, move both board axes through their full ranges, and release
them to the center. Press F3 to finish, then select Exit and confirm with F2
to save. With the default keyboard, J/L move yaw and I/K move roll. Recalibrate
after changing the controller's usable range.

Planet Harriers uses one analog stick per cabinet. The default keyboard
provides arrows or I/J/K/L for movement, X for bullets, C for missiles, F for
bombs, S/D for the left/right view buttons, A for coins, and Return for Start.
The controller mapping page exposes all five cabinet buttons. Its title-only
**D-pad flight controls** option defaults on, preserving arrow/D-pad steering;
disable it to use only the mapped analog stick. Configure and
calibrate the stick in **Game Test Mode > Joystick Calibration** if needed;
F2 is Test and F3 is Service. Cabinet link play is not available. Its saves
use `pharrier.hikaru.nvmem` and `pharrier.mie.eeprom`.

Save slots, quicksave/quickload, automatic states and library resume are
available after the first frame. Loading restores the CPUs, mutable graphics
memory, sound boards, cabinet I/O, EEPROM and card reader together at the next
frame boundary. States require the same game and BIOS images and a compatible
native engine layout; incompatible or corrupt states are rejected. State
previews are not currently captured for Hikaru. Restoring a state also restores
its cabinet calibration and battery-backed records, which are persisted normally
on exit. Rollback/netplay and RetroAchievements remain disabled. The backend
currently keeps one emulated machine active.

### NASCAR Arcade controls and bring-up

NASCAR uses the Standard cabinet layout and its own 837-13844 JVS board profile.
The loader validates the Rev A program pair and all sixteen data chips against
MAME's manifest; the older `sgnascaro` revision is not enabled. Fresh cabinets
adopt the BIOS's validated records with the required one-player setting.
Existing EEPROM, calibration and records are never replaced by these defaults.
Saves use `sgnascar.hikaru.nvmem` and `sgnascar.mie.eeprom`; cabinet region is
independent of the other games.

Its **Gamepad driving controls** option appears only while NASCAR is loaded.
It defaults on and selects a built-in gamepad preset with D-pad steering and
a latched shifter. The controller API supplies the standard button and axis
assignments. Disabling the temporary preset restores the saved mapping.
Physical wheels retain their mapped axes.
With the preset enabled:

- Left stick steers; RT accelerates and LT brakes.
- A/B/X/Y select gears 1/2/3/4. Right shoulder changes view; left shoulder selects neutral.
- Start begins a race. L3 inserts a coin, R3 opens cabinet test, and Back opens the menu.
- Turning the option off removes the D-pad alias and gear latch for a physical
  shifter whose selected gear button remains held. Analog steering and pedals
  retain their cabinet assignments.

NASCAR's original 838-13366 drive-board commands feed the same native constant
torque effect used by Naomi. Force-feedback devices receive signed steering
force; ordinary vibration-only controllers receive short rumble pulses as a
fallback. The controller's existing vibration-strength setting scales the native
effect. Cabinet right/left commands are converted to the host effect's steering
convention. A steady force remains running until its level changes or it stops.
Pause, cabinet reset, graphics teardown, controller reassignment and game unload
release the force. Output is derived from the serialized JVS state, so loading a
state restores the game's command without serializing a host device effect.
The command ranges follow the documented
[EPR-20512 drive-board protocol](https://github.com/njz3/vJoyIOFeederWithFFB/blob/master/DRIVEBOARD.md).

Controller Mapping exposes each gear, neutral, view, wheel and pedal separately.
Standard cabinet wheel/gas/brake use JVS analog 0/1/2; low is left/released.
View is P1 Down. The shifter uses P2 Left/Up/Down, with switch words
`0x2800`, `0x1800`, `0x2000`, `0x1000` for gears 1 through 4.
The latch uses the existing serialized MIE input snapshot, so a restored state
restores its gear without changing the save format. Conflicting gear buttons
retain the prior selection; Neutral explicitly disengages it.

Use Game Test > Input Assignments for wheel and pedal calibration. The game
also offers automatic transmission. Attract mode has booted with the track
and cars rendered; race performance and the completed input integration still
require runtime verification. Physical force direction and strength still need
confirmation on a supported wheel. Linked play and Deluxe cabinet wiring are
not implemented here. The
[Sega Standard cabinet manual](https://backup.segakore.fr/sauservice/Manuals/Nascar/NascarSTD/NascarSTD.pdf)
documents the cabinet settings and calibration procedure.

### Star Wars Racer Arcade controls and bring-up

The loader recognizes `swracer`, validates four program and thirty-two data
chips, and uses this title's 315-5881 security key. Its SOP44 data board uses
8 MiB chips; program pairs occupy separate 16 MiB source slots. The cabinet
uses the existing Sega 837-13551 JVS profile. Saves and cabinet region are
separate from the other games (`swracer.hikaru.nvmem` / `swracer.mie.eeprom`).

Star Wars startup exercises a static-vertex scale of 2.0 (command 008E0901).
The inherited integer-shift decoder made that scale negative. Precision now
uses floating exponent arithmetic, with regression coverage for all 256
encodings and the static vertex handlers; the assertion remains in place.

The left/right stick Y axes operate the left/right pod levers independently.
Push both forward to accelerate, pull both back to decelerate, and move them
in opposite directions to turn. Left lever forward/right lever back turns
right; the reverse turns left. Start confirms menu choices and changes view
during a race. A is Boost, B is Brake, and X/Y are the left/right buttons.

The title-only **Gamepad pod controls** option defaults on. It adds LT/RT
as Boost/Brake and left/right shoulders or D-pad as the left/right buttons,
so both thumbs can remain on the levers. Disabling it removes those aliases;
the separately remappable levers and cabinet buttons keep their assignments.
JVS analog 0/1 carry the left/right levers, with low values forward. Boost and
Brake use P1 Push1/Push2; the left/right buttons use P1 Right/Left respectively.
These bit/channel assignments are hardware facts from the Odin reference
manifest; the implementation uses Hollycast's own input paths.

For one machine, select **NO LINK** in Game Assignments. Game Test's Input
Assignments calibrate each lever's forward/back endpoints independently.
The [Sega operator manual](https://primetimeamusements.com/wp-content/uploads/2015/05/StarWarsRacerArcade_Hikaru_US_Manual_Deluxe.pdf)
describes controls, Input Test and calibration on printed pages 31-32, 55,
and 60-61. Linked cabinets and the Deluxe boost-lock actuator are not
emulated. Boot, gameplay and physical input behavior require runtime
validation; adding the manifest alone does not establish compatibility.

### Virtual-On Force controls and bring-up

VON4 uses two digital levers for one player: JVS player slot 1 is the left
lever and slot 2 is the right lever. Its title-only **Single-controller bindings** option defaults on: controller A's left/right sticks provide the
two levers, LT/RT fire, arcade buttons 3/4 operate left/right turbo, button 5
is Personal Information, and Coin/Start remain separate. A/B also provide
left/right fire; button 6 is an alternate left turbo. Axis-to-digital conversion
uses half-range thresholds after the normal controller dead zone. Disabling
the option assigns one controller to each lever of the same cabinet.

The loader validates the four Rev E/A program chips and sixteen data chips
against MAME's manifest. It retains the 16 MiB program socket spacing and
zero-filled unpopulated slots. No 315-5881 chip is fitted. The normal MIE,
sound and renderer paths are reused. The card reader uses Hollycast's existing Sanwa protocol implementation through
the MIE serial adapter; its completion latency is modeled at 10 us rather than
measured electrical timing. Windows manual testing has reached playable combat;
performance remains under investigation. Linked cabinets and the full inserted-card
workflow remain unverified. Saves use `von4.mie.eeprom` and
`von4.hikaru.nvmem`, with an independent cabinet-region selection.

The command-RAM apertures mirror the same 4 MiB backing for both CPUs. MEMCTL
DMA consumes its source when the start command is written: VON4 immediately
refills the staging buffer for the next model, so copying it at a later device
slice would mix geometry blocks and stop the GPU command stream.

## Cabinet region and language

While a title is loaded, open **Settings > Controls > Game compatibility
controls > Cabinet region**. Each game remembers its own selection:

- **Keep saved region** (default): leave the cabinet country unchanged.
- **Japan**: Japanese cabinet configuration.
- **USA (English)**: English cabinet configuration.
- **Export**: export configuration and the game's available language options.

Changing country restarts the emulated cabinet after EEPROM writes finish and
both records are valid. For an entirely erased chip, a fresh cabinet adopts
only the BIOS's own matching working records after their game ID and system
header validate, then restarts with the selected country. Existing or damaged
nonblank data is never replaced with defaults. No factory EEPROM image is
embedded or substituted. Existing calibration and other EEPROM fields are
retained: only country byte 5 and the checksum of each 64-byte record change.
Battery-backed RAM is captured and restored through the normal reset path.

Before the first change, the frontend keeps `GAME.mie.eeprom.before-region`
and, when present, `GAME.hikaru.nvmem.before-region` beside the writable saves.
The BRAM backup captures live records if a game is running. Existing backups
are never overwritten. A failed backup blocks the region change. Do not delete
calibration or score files to switch language. Game-language options can still
depend on the selected country and title; the English screens require manual
verification in each game.

## Standalone development tools

The standalone player exercises the same embedded CPU/GPU engine with SDL2:

```sh
cmake -S tools/hikaru -B /tmp/hollycast-hikaru-player -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/hollycast-hikaru-player
/tmp/hollycast-hikaru-player/hollycast_hikaru \
  --bios /path/to/epr-23400.ic94 \
  --rom-dir /path/to/extracted-airtrix-chips \
  --mie /path/to/315-6146.bin
```

Its controls are F7 coin, F8 start, arrow keys cabinet buttons, A/D yaw, W/S
roll, F5 test, F6 service, and Escape quit. `--frames N` limits the run;
`--strict-bios` disables the EEPROM-check bypass.
`--eeprom /path/to/settings.eeprom` loads and saves the original 128-byte MIE
EEPROM so operator calibration persists between standalone runs. Keyboard axes
cover the full 16-bit range; calibrate with that same range.
This development player does not attach the desktop frontend's sound adapter
and remains silent.

`hikaru_boot` is a separate native hardware bring-up harness. It uses the new
C++ board, dual SH4 contexts, bridge, peripheral, DMA, and EEPROM modules rather
than the embedded playback engine. It can validate a local ROM set and produce
bounded BIOS traces:

```sh
/tmp/hollycast-hikaru-player/hikaru_boot \
  --bios /path/to/epr-23400.ic94 \
  --rom-dir /path/to/extracted-airtrix-chips --verify-only
```

That native machine is incomplete. Its current BIOS trace stops at an
unconnected MIE bus-request register. It is not the game playback path, and
passing its tests does not establish full BIOS or game compatibility.

## Validation

Run the hardware tests without any commercial images:

```sh
cmake -S tests/hikaru -B /tmp/hollycast-hikaru-tests -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/hollycast-hikaru-tests
ctest --test-dir /tmp/hollycast-hikaru-tests --output-on-failure
```

The eleven suites cover the bridge, board memory, native SH4 execution,
peripherals, DMA, ROM loading, JVS, MIE, native machine, EEPROM, and 315-5881
protection. They are also included by `ENABLE_CTEST=ON`. Optional embedded
renderer tests (`HIKARU_VENDOR_BUILD_SMOKE=ON` on Windows, Linux or macOS) use synthetic data and
a hidden GL window to exercise texture bounds, normal transforms, framebuffer
output, host GL state restoration, and recoverable engine failures. Synthetic
instruction tests also compare guarded RAM access and polling-loop shortcuts
with the original interpreter, including device overlays and interrupts.
The instruction-dispatch differential test covers all opcode mappings and
generated programs with delay slots, patches, interrupts and floating-point
edge cases. After editing instruction descriptors, regenerate the checked-in
handler-name list with
`python3 core/hw/hikaru/valkyrie/tools/generate-sh4-dispatch.py` (or use `--check`
to verify it). Ordinary builds do not run the generator.

The main emulator test executable includes AICA/ARM state-isolation and sound
frame-scope checks. With Hikaru enabled, audio-pacing tests verify pitch,
stereo, speed estimation, queue recovery, and clearing after pauses or stalls.
SDL callback tests cover partial underruns, bounded conversion, and converted
sample carry across callbacks.
Optional firmware tests require a user-supplied local image and are skipped
when it is absent.

These checks cover specific software contracts. Runtime evidence must name
the game/BIOS revision and distinguish boot, attract mode, controls, gameplay,
rendering, and sound. Audio underruns and rendering defects mean Air Trix
must not yet be described as fully emulated or graphically accurate.

## Sources and licensing

The playback backend embeds
[Valkyrie revision fa52073](https://github.com/stefanoteso/valkyrie/tree/fa52073f2ac6264d2f77242b3e3cca2935c31392)
under **GPL-3.0-or-later**, preserving the original notices and license in
`core/hw/hikaru/valkyrie`. Its adapter uses Hollycast-owned ROM buffers, GL
context, inputs, the original MIE firmware, and an isolated 315-5881 protection
device. It includes local rendering, bounds, lifecycle, and portability fixes.
See that directory's `PROVENANCE.md` for the import boundary. Builds including
this backend must respect its GPLv3-or-later terms.

Hardware references are the
[MAME Hikaru driver](https://github.com/mamedev/mame/blob/43b995da4a2af46c9f48c6a5536953439adfb1ab/src/mame/sega/hikaru.cpp),
[315-6154 bridge](https://github.com/mamedev/mame/blob/43b995da4a2af46c9f48c6a5536953439adfb1ab/src/mame/sega/315-6154.cpp),
MIE/315-5881 implementations at the same pinned revision, and the
[Renesas SH7750 hardware manual](https://www.renesas.com/en/document/mah/sh7750-sh7750s-sh7750r-group-users-manual-hardware)
for SH-4 pipelining and instruction timing. Retained BSD
notices identify reused implementations. The MIE Z80 core comes from
[floooh/chips revision 9e88298](https://github.com/floooh/chips/tree/9e88298ce56319953ac7a43213a1120359f7a3a6)
under its retained zlib license in `core/deps/chips`.

Pitch-preserving audio pacing uses
[SoundTouch 2.4.1](https://codeberg.org/soundtouch/soundtouch/src/tag/2.4.1)
under LGPL-2.1-or-later. The unchanged selected library sources, license, and
provenance are retained in `core/deps/soundtouch`.

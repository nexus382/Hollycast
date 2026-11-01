# Experimental Sega Hikaru support

The optional Hikaru backend recognizes **Air Trix (`airtrix`)** and
**Brave Firefighters (`braveff`)** in the normal game library and loader. It uses
a separate Hikaru CPU/GPU backend. Air Trix testing has reached attract mode, player/stage selection,
and an active skating stage with calibrated steering and awarded trick coins.
Compatibility is experimental: rendering and
timing are still under investigation. The desktop frontend runs both AICA/ARM7
sound boards and produces music and effects, but playback currently breaks up
when emulation cannot supply samples in real time. Other
Hikaru games and the `airtrixo` revision are not supported. Brave Firefighters
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
surfaces and fire sprites. A real command-packet regression covers 96 cases,
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
synthetic shader cases and saved selection/gameplay frames. Texture environment
modes and specular lighting remain incomplete. Exact spotlight rounding, mixed
bank/curve selection and other interpretation limits are described in
[SPOTLIGHTS.md](../core/hw/hikaru/valkyrie/SPOTLIGHTS.md); these changes do not
establish hardware-accurate rendering.

**Match audio to game speed** in Audio settings is enabled by default for
Hikaru. It stretches the stereo mix without lowering pitch when emulation runs
slowly. This adds buffering and slows the music with the game; it does not
restore full-speed gameplay. It can be disabled to hear the original sample
timing, which produces gaps when emulation is too slow.
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
state format is version 2; the desktop frontend still disables Hikaru states.

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

## Build and run

Hikaru is disabled in standard builds. Enable it in a desktop macOS or Linux
build with desktop OpenGL support:

```sh
cmake --preset <PRESET> -DUSE_HIKARU=ON
cmake --build --preset <PRESET>
```

Optimized GNU/Clang builds use `-O3` for the optional playback engine without
enabling fast-math. `-DHIKARU_VENDOR_OPTIMIZE=OFF` keeps the build configuration's
original optimization flags. Debug and sanitizer builds retain their requested
flags.

The backend requires OpenGL 3.2. Loading a supported Hikaru game selects OpenGL and disables
threaded rendering for that session. Vulkan, Metal, OpenGL ES, Windows, mobile,
and libretro Hikaru builds are not supported by this experimental integration.
macOS Apple Silicon has received runtime testing; Linux has not yet received
equivalent runtime validation.

Place `airtrix.zip` or `braveff.zip` beside `hikaru.zip` and `mie.zip`, then add
their folder to the game library or open the game with **File > Load ROM**. `.7z`
archives are also accepted. The loader also searches the configured Naomi BIOS
folders for the Hikaru and MIE archives. Every required chip is checked for
size and CRC before execution; ROMs may be found by their original name or CRC.

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

| Air Trix control | Default keyboard |
| --- | --- |
| Coin | A (US keyboard layout) |
| Start / confirm | Return |
| Cabinet left / right buttons | Left / Right arrows |
| Front / back kick | Up / Down arrows |
| Board yaw / roll | I / J / K / L analog bindings |
| Operator test / service | F2 / F3 |

On macOS, **System > Insert Credit** also inserts one credit for the loaded
Hikaru game. The action sends a press and release through the same cabinet
input path as a physical coin button.

Unused gamepad face buttons mapped to C or X also insert credits. The controller
mapping page labels the Hikaru controls. Board motion uses two analog channels;
the four cabinet buttons are distinct from the analog board axes. Runtime
testing has verified both calibrated axes, front/back switch mapping, and a
landed spin trick awarded a B grade and nine coins. Every trick and control
combination has not been validated.

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

Save states, automatic states, rollback/netplay, and RetroAchievements are
disabled for Hikaru. The backend currently keeps one emulated machine active.

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
renderer tests (`HIKARU_VENDOR_BUILD_SMOKE=ON` on macOS) use synthetic data and
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

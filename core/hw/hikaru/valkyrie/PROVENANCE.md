<!-- Portions Copyright 2026 The Hollycast Authors -->
<!-- Portions Copyright 2026 The Hollycast Authors -->
Valkyrie source, revision fa52073f2ac6264d2f77242b3e3cca2935c31392,
https://github.com/stefanoteso/valkyrie. The original per-file notices and
GPL-3.0-or-later LICENSE are retained. The embedded adapter removes the SDL
window/event loop and JSON game loader; uses the host's GL context, ROM data,
and external MIE device; and contains documented rendering/portability fixes.
The standalone Windows, Android, Linux and macOS port keeps the desktop
OpenGL 3.2 path and adds an OpenGL ES 3.0 path with shared shader bodies,
ES texture-upload conversion, and graphics release/restore across host
context recreation. Hikaru remains unsupported in libretro, Xbox/UWP,
Switch and iOS builds. BSD is opt-in and unverified. No platform runtime claim is made for a
backend that has not been built and run on that platform.

The host graphics dispatch now preserves the vendor scene construction while
supporting native Metal and Vulkan draw backends alongside OpenGL. The host's
API and transparency settings select strip sorting, triangle sorting, or bounded
depth peeling. CPU-retained vertices supply transformed sorting keys; sampleable
depth attachments and the existing bitmap compositor resolve peeled layers.
Synthetic GPU fixtures cover ordering, layer-capacity overflow, and compositing;
these are compatibility implementations rather than measured Hikaru hardware
transparency semantics.

The optional BIOS EEPROM-check bypass is inherited from upstream and applied
only when explicitly requested by the host to a private in-memory BIOS copy.
No BIOS, game ROM or MIE firmware is distributed with this source.

Adapted upstream files carry dated Hollycast modification notices. Local changes
include host integration, isolated MIE/protection callbacks, dual-matrix skinning,
viewport and packed-texture decoding, GL state isolation, configurable internal
resolution, alpha-keyed ABGR1555/A2BGR10 bitmap coverage and layer priority, separate bitmap
layer enables and Air Trix combined/direct composition, logical-pixel framebuffer
DMA addressing, primary/alternate material colour selection for
untextured unlit geometry (with textured colour handling retained), zero-alpha
texture cutouts, per-mesh depth bias for opaque and translucent passes with
loop-local offset-state caching, bounded texture
invalidation, instruction/data read-span and RAM
write-span caching, linked-program uniform-location and spotlight-value caching,
bounded per-frame vertex chunks with per-mesh upload fallback and loop-local VAO binding,
configurable dual-SH-4 cycle budgets with fixed-rate board devices,
same-body switch instruction dispatch, inline validated cached read/write hits with
original special-space/endian fallbacks, defined unsigned rotate shifts,
safe same-slice polling-loop acceleration, and external
dual-board sound callbacks with MIE-masked interrupts.

The host engine accepts separate Air Trix, Brave Firefighters and Planet
Harriers definitions. Brave Firefighters and Planet Harriers use sparse EPROM
source slots with their respective within-bank mirroring. Synthetic tests
exercise all three selected board layouts and
the legacy Air Trix entry point without game data or executed game patches.

Spotlight decoding uses guest-authored cosine cutoffs and uploaded angular
curves, with command451 payload storage preserving the existing light ABI.
The implementation and remaining hardware interpretation limits are documented
in SPOTLIGHTS.md. No game program, firmware, or proprietary renderer source is
distributed as part of that change.

GPU IDMA completion accounting now retains the number of consumed descriptors
until the guest acknowledges the count. Air Trix and Brave Firefighters use
the same read/clear/add sequence; a synthetic MMIO regression checks the count,
interrupts and copied pixels. Busy-time count semantics retain the inherited
behavior. Per-frame renderer snapshots grow within the existing mesh-slot
bound instead of stopping at 4096 entries; regression coverage checks growth,
index stability and frame reset.

Texture coordinates now retain raw packet UVs without the inherited speculative
0D1 global translation. Recorded command state and layout remain intact; the
command semantics are unresolved. Tests exercise the actual packet decoder
and vertex baker with nonzero offsets and all supported texture dimensions.

CPU reads through a resolved sound-board RAM aperture use an estimated 320 ns
compatibility latency; this is not a measured hardware value. CPU cycle debt
and fractional conversion persist across slices and private vendor save states;
DMA reads are excluded. Vendor state version 2 rejects earlier state files
because the SH-4 serialized layout includes the new timing fields.

Captured Brave Firefighters textured heads with zero diffuse/ambient coefficients
use the retained 0x691 RGB channel for prelit modulation. This is a narrow
compatibility interpretation, with shader regressions and saved-frame replay;
other 0x691 uses and hardware-accurate material lighting remain unresolved.

SH-4 double arithmetic assembles FRn as the high word and FRn+1 as the low
word (high word in FRn, low word in FRn+1). FMOV width depends only on FPSCR.SZ,
independently of FPSCR.PR. Synthetic instruction regressions cover conversion,
arithmetic, comparison, sign, paired moves, and unsigned decimal division;
no captured game instructions are included in these tests.

OpenGL material combination and scene coverage (2026-11-03): material equations
follow Odin snapshot aa89944add2787233d8161cca380255193b92f15 (GPL-3.0-or-later,
Copyright 2026 Reda Cherif-Touil), specifically hikaru_draw.hpp, mesh.frag and
renderer_scene.cpp. 2C1 texture selection, 881 alpha/color selection and 691
ambient/emission are independent of A81 blending. Hollycast retains its coverage
alpha convention for bitmap composition; Odin's inverse transmittance factors
cannot be copied to this framebuffer. Hidden-window production-shader probes
cover texture/color selection, emission and coverage for all four blend modes.
Scene clear colors are premultiplied with their coverage. Depth-writing
surfaces force full coverage after alpha cutout rejection; blended surfaces
retain fractional coverage. Bitmap keys remain binary, but the 3D scene is
composed using its actual coverage rather than a nonzero-alpha threshold.

Double-sided triangles apply the closing vertex's winding bit before emitting
front and reversed-normal back faces, as in Odin's antarctic_mesh.cpp at the
same snapshot. Ignoring that bit assigned back-face lighting to alternating
strip triangles. Captured Brave Firefighters smoke reproduces the resulting
hard dark triangles; the production baker regression covers both windings,
with and without back-face copies. This change corrects vertex data shared by
all host backends rather than forcing two-sided lighting in their shaders.

OpenGL fog now defaults on and follows the guest's material enable (881 bit 18)
and viewport colour-disable control (621 bit 17). Its linear start/range is
applied to eye-space depth, following Odin's mesh.frag and renderer_scene.cpp
at the same snapshot. Fog controls, cutouts and coverage have production-shader
pixel coverage; the captured hallway's fog appearance still needs a live
visual check because its disabled uniforms were absent from the old capture.

The 621 fog conversion now follows Odin's view_fog (hikaru_draw.hpp): P is the
inverse range and Q is near/end, so start = near/Q - 1/P. Regression coverage
uses upload_viewport itself rather than injecting decoded uniforms, including
negative starts and a disabled inverse range.

OpenGL applies the 32 screen-door masks selected by 501, following Odin's
pattern_bits and mesh.frag at the same GPL-3.0-or-later snapshot. Patterns are
snapshotted with each mesh and restored across calls using Odin's six-level
scope heuristic, preventing a smoke model's mask from affecting furniture.
The state uses previously reserved flag bits without changing serialized CP
or drawing-block layout. All masks and nested scope have focused regressions.
Native Vulkan/Metal shaders still require their separate pattern implementation.

The November audit separates the host's Hikaru sorting preference from shared
Dreamcast/Naomi renderer selection. GPU visibility queries replace peel-target
color scans; Vulkan transfers and clears use fence-owned batches, and Metal
internal viewport/scissor/readback coordinates follow the shader's Y mapping.
ES3 depth24 allocations use unsigned-int data types and texture bias is a
shader argument instead of the unsupported ES texture parameter.
Invalid texture uploads restore the untextured shader path. Empty command
meshes no longer disable sorting. SH-4 FCNVSD/FCNVDS require PR=1, matching the
shared SH-4 interpreter. Cartridge views explicitly borrow immutable host
storage while BIOS data remains private and patchable. Original copying
semantics remain the engine API default.

The follow-up audit registers synthetic tests even in a playable build with
BUILD_TESTING disabled. The native CPU/bus bring-up model is now an excluded
library required by its tests; the player links only its shared board I/O.
Protection aperture holes and unused registers follow MAME revision
43b995da4a2af46c9f48c6a5536953439adfb1ab's localbus_map and iomap_le:
unmapped reads return zero, unused writes are ignored, high-address writes
force zero, and wide reads visit each 16-bit data port once. Width, alignment
and aperture validation remain explicit host API constraints. The cipher's
block-header reset already restores its read cursor; the alleged cursor
heap over-read was not established by this review.

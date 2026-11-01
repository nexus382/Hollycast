Valkyrie source, revision fa52073f2ac6264d2f77242b3e3cca2935c31392,
https://github.com/stefanoteso/valkyrie. The original per-file notices and
GPL-3.0-or-later LICENSE are retained. The embedded adapter removes the SDL
window/event loop and JSON game loader; uses the host's GL context, ROM data,
and external MIE device; and contains documented rendering/portability fixes.
The optional BIOS EEPROM-check bypass is inherited from upstream and applied
only when explicitly requested by the host to a private in-memory BIOS copy.
No BIOS, game ROM or MIE firmware is distributed with this source.

Adapted upstream files carry dated Hollycast modification notices. Local changes
include host integration, isolated MIE/protection callbacks, dual-matrix skinning,
viewport and packed-texture decoding, GL state isolation, configurable internal
resolution, coverage compositing for direct ABGR1555 layers, separate bitmap
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

The host engine accepts separate Air Trix and Brave Firefighters definitions.
Brave Firefighters uses sparse EPROM source slots with the existing small-chip
within-bank mirroring. Synthetic tests exercise both selected board layouts and
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

# Native Metal rendering investigation

This document records the original investigation. See [Native Metal renderer](Native%20Metal%20renderer.md) for the implementation and current limitations.

Date: 31 October 2026. Hollycast revision: `38d92cd8740170bae9f30a3dd35585a0efdeb396`.

## Recommendation

An independent, Hollycast-owned Metal renderer is feasible. Build it as an optional backend alongside OpenGL and Vulkan, using Hollycast's existing emulation, texture and UI interfaces. Implement the device code and Metal Shading Language (MSL) shaders within this project; do not import the external Flycast Metal implementations.

Start with standalone macOS and the regular sorting modes. Add per-pixel sorting only after its separate algorithm and GPU synchronization are validated. Keep the current default and existing backends available during development. iOS/tvOS and libretro require separate frontend work.

Use conventional Metal command queues and command buffers with runtime capability checks. Native Metal does not require adopting the Metal 4 API. Preserve the application's macOS 10.15 deployment target and universal Intel/Apple Silicon packaging; determine the new backend's actual supported OS/GPU matrix from the APIs used and hardware validation. Another implementation's macOS 12 minimum is not an inherent requirement for ours.

This is a substantial renderer project, not a settings toggle. Removing Vulkan-to-Metal translation is a potential benefit; no speedup has been measured, and CPU-bound emulation may see little improvement.

## Current integration points

The macOS settings entry currently called **Vulkan (Metal)** is the Vulkan renderer running through MoltenVK. The new entry should be **Metal (Experimental)** during development, distinct from that existing option. See `core/ui/settings_new.cpp:4480` and MoltenVK loading in `core/sdl/sdl.cpp:1234`.

Three existing interfaces provide useful boundaries:

| Interface | Existing source | Metal responsibility |
| --- | --- | --- |
| `Renderer` | `core/hw/pvr/Renderer_if.h:61` | PVR scene processing, draws, framebuffer rendering, screenshots and custom-texture hooks |
| `GraphicsContext` | `core/wsi/context.h:30` | Device/window lifecycle, capabilities, resizing and presentation interval |
| `ImGuiDriver` | `core/ui/imgui_driver.h:25` | UI frames, presentation, named texture lifetime and per-texture sampling |

Reuse the TA parser and CPU sorting, Dreamcast texture decoding and VRAM invalidation, framebuffer pack/unpack helpers, transform calculations, and palette/fog/Naomi 2 input data. Existing Vulkan `Process` calls `ta_parse(ctx, true)` (`core/rend/vulkan/vulkan_renderer.cpp:125`). Its API-specific pipelines, command submission, GPU allocation and shaders remain outside the new backend.

## Proposed backend

Keep the new implementation under `core/rend/metal/`, with ordinary C++ declarations at shared boundaries and Apple objects inside Objective-C++ implementation files. Use per-source ARC settings rather than changing the whole application.

| Module | Responsibility |
| --- | --- |
| `metal_context` | Device, queue, SDL Metal view/layer, drawables, frame slots, resizing, presentation and teardown |
| `metal_renderer` | `Renderer` implementation, shared TA parsing, palette/fog updates, game lifecycle, screen/RTT/framebuffer orchestration |
| `metal_draw` | Opaque, punch-through and translucent lists, depth/stencil/blending, modifier volumes and Naomi 2 data |
| `metal_texture` | `BaseTextureCacheData` implementation, samplers, uploads, compressed replacements, preloads and deferred GPU resource release |
| `metal_pipeline` and `shaders/*.metal` | Hollycast-authored MSL, shader specialization, pipeline/depth-state caches and diagnostics |
| `metal_imgui` and `metal_overlay` | UI/artwork/fonts, VMU screens, crosshairs and OSD composition |
| `oit/` | Per-pixel fragment collection, ordering, modifier-volume processing and resolve |

Start with one ordered command queue and a bounded pool of reusable frame slots. Retain upload buffers, textures and render targets until GPU completion. Acquire drawables late; handle minimized windows or unavailable drawables without blocking indefinitely. On resize, retire old size-dependent resources after their last use. On backend switching, stop submissions and drain work before destroying the UI, Metal view and SDL window.

SDL2 provides [`SDL_Metal_CreateView`](https://wiki.libsdl.org/SDL2/SDL_Metal_CreateView) and layer access; Hollycast must associate the device and own the lifecycle. Use [command-buffer completion handlers](https://developer.apple.com/documentation/metal/mtlcommandbuffer/addcompletedhandler(_:)) for frame-slot/resource retirement rather than choosing a newer OS minimum merely for a particular waiting API.

Compile authored MSL into a bundled shader library where practical, then specialize shader functions and cache pipeline states. Keys must cover the relevant PVR state, target formats, sorting mode, Naomi 2 path and Hollycast texture filters. Avoid shader or pipeline compilation in steady-state rendering; measure first-use stalls before adding persistent caches or asynchronous compilation.

## Required Hollycast integration

- Append explicit renderer IDs to `core/types.h:98`; preserve stored IDs 0–6. Add `isMetal`, factory cases in `core/hw/pvr/Renderer_if.cpp:468`, context creation/fallback in `core/wsi/switcher.cpp:32`, and a distinct API group in `core/ui/mainui.cpp:132`.
- Extend Hollycast's actual settings in `core/ui/settings_new.cpp:4418` and `:5692`, including API count, selection mapping, descriptions and sorting capability handling. The current option arrays have capacity four; revisit their sizing. Keep unsupported choices unavailable and explain capability failures.
- Add an Apple-only `USE_METAL` build option, initially opt-in, backend sources, Metal/QuartzCore linkage and shader resources. Retain MoltenVK packaging while Vulkan is included. Audit renderer defaults and fallback for each supported build combination.
- Update SDL drawable pixel sizing in `core/sdl/sdl.cpp:875` for Metal, including Retina scale, fullscreen and display changes.
- Implement the full `ImGuiDriver` contract, including `nearestSampling`. Existing Vulkan uses distinct nearest/linear samplers (`core/rend/vulkan/vulkan_driver.h:101`); VMU and pixel-art assets must retain their sampling. Defer destruction of replaced UI textures until referencing draws finish.
- Integrate `CheckCustomTexture()` when asynchronous replacements become available, Metal format capabilities, prepared mip uploads and ownership of GPU-preloaded replacements. See `core/rend/TexCache.h:156`, `core/rend/TexCache.cpp:789`, `core/rend/custom_texture/CustomTextureTypes.h:78` and `core/hw/pvr/Renderer_if.h:86`. Include an RGBA fallback; query BC/ETC/ASTC support per device rather than assuming the same formats on Intel and Apple GPUs.
- Implement Hollycast's sample reconstruction filters and size limits, using current Vulkan behavior as a specification (`core/rend/vulkan/pipeline.cpp:665`). Shared CPU texture upscaling alone does not implement these shader filters.
- Preserve `GetLastFrame` orientation, RGB format and size contract for screenshots and save-state thumbnails (`core/hw/pvr/Renderer_if.h:79`, `core/ui/gui.cpp:771`). Capture from an owned game image rather than a transient drawable.
- Add native Syphon publication of the composed GPU texture, with correct orientation/scaling and resource lifetime. Current macOS routing unwraps Vulkan/MoltenVK objects (`shell/apple/emulator-osx/emulator-osx/osx-main.mm:324`); native Metal needs its own entry point.
- Report Metal in the performance overlay and implement requested GPU timing, pass/upload counts and resolution metrics through `core/ui/performance_overlay.h`. Preserve the existing GPU timing scope: PVR screen rendering and direct framebuffer uploads, excluding unrelated UI and RTT work. [Metal command-buffer timestamps](https://developer.apple.com/documentation/metal/mtlcommandbuffer/gpustarttime) can measure completed buffers; use finer counters or separately scoped submissions if whole-buffer timing would include unrelated work. Report unavailable when equivalent measurement cannot be provided.

## Correctness before performance

The regular renderer must cover texture formats/palettes/mips, fog, clipping, depth and blend rules, shadows/modifier volumes, render-to-texture and VRAM writeback, direct framebuffer display and full framebuffer emulation. Naomi 2 lighting/transforms also need native shader and uniform implementations; shared TA parsing does not supply those GPU stages.

Per-pixel sorting is a separate compatibility feature. `core/hw/pvr/ta_vtx.cpp:1194` changes sorting and index preparation for per-pixel backends. Do not select a new OIT enum before its draw algorithm exists. Design bounded fragment storage, depth-before-collection behavior, deterministic ordering, modifier-volume interactions and an explicit overflow policy. Capability-gate the implementation; do not substitute approximate weighted transparency for the emulator's per-pixel behavior.

## Delivery and verification

1. **Platform foundation:** optional selection, Metal context, UI-only and paused redraws, correct sampling, drawable sizing, API switching, fallback and teardown. This is a development milestone, not a playable renderer.
2. **Playable regular renderer:** complete Dreamcast draws, textures and asynchronous custom replacements, RTT/framebuffer/readback, screenshots and overlays. Validate Naomi 2 before describing the backend as covering Hollycast's supported systems. Expose only implemented sorting/features.
3. **Hollycast feature completion:** compressed replacement formats, preload behavior, shader filters, Syphon and comparable overlay timing. Any temporarily unavailable feature needs explicit UI handling.
4. **Per-pixel renderer:** implement and validate collection/resolve and modifier volumes across supported GPU families. Keep it unavailable until then.
5. **Performance and release coverage:** compare representative deterministic scenes with Vulkan using the same resolution, sorting, VSync and texture settings. Measure CPU rendering cost, GPU work, frame-time spikes, compilation stalls and memory. Exercise threaded/unthreaded rendering, save/load, game changes, pending uploads, repeated resize/switching and long runs under Metal validation.

Start hardware bring-up on Apple Silicon, then validate Intel/AMD Macs and older supported macOS configurations before claiming their native-renderer support. Also test clean fallback on unsupported configurations, Metal-enabled/disabled builds, and the existing Vulkan/OpenGL builds. Standalone iOS currently uses `GLKView`/`EAGLContext`; a mobile Metal view and lifecycle integration is a separate milestone, not something gained by enabling the macOS build flag.

## External research and limits

As of this investigation, Flycast `dev` at `2c83abcce4b80d51730e4b38307438a4bed5a108` contains no Metal renderer directory. [PR #2563](https://github.com/flyinghead/flycast/pull/2563), inspected at `667999a892d672dc4b900d290aede363be581489`, is open and unmerged. Its known gaps include a per-pixel visual defect and untested Intel/older-macOS runtime paths. Its current build disables iOS/libretro despite the broader title. [Metal 4 PR #2559](https://github.com/flyinghead/flycast/pull/2559) was closed without merging; its compatibility and maintenance discussion reinforces keeping older platforms available. These are research observations, not implementation dependencies.

Local interfaces and selected external source files were inspected. No renderer implementation was imported or written, and no new Metal build, gameplay validation or performance benchmark was run. Compatibility and performance remain to be established by the implementation milestones above.

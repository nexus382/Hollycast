# Native Metal renderer

Hollycast's native Metal backend is an independently implemented, experimental option for standalone macOS. It uses Metal directly and does not route drawing through Vulkan or MoltenVK. Standalone macOS builds include it by default, and the runtime option appears automatically when usable Metal support is detected. Existing renderer preferences are preserved.

## Build and select

Use the usual macOS CMake configuration; `USE_METAL` defaults to `ON` for standalone macOS and `OFF` elsewhere. Both Xcode and Ninja builds are supported. Keep the existing MoltenVK path configured if Vulkan is also enabled; a build with `USE_VULKAN=OFF` does not require MoltenVK. An explicit `-DUSE_METAL=OFF` excludes the backend.

```sh
cmake --preset darwin -DMOLTENVK_DYLIB=/path/to/libMoltenVK.dylib
cmake --build --preset darwin
```

Choose **Settings > Video > Graphics API > Metal (Experimental)**, then per-triangle, per-strip or per-pixel sorting. Renderer IDs 0–6 retain their existing meanings; regular native Metal uses ID 7 and per-pixel Metal uses ID 8. The API entry appears only if a runtime device, queue, presentation pipeline and sampler probe succeeds. Per-pixel support additionally probes fragment atomics, primitive IDs and multiple color attachments. Saved selections fall back to Vulkan/OpenGL when Metal is unavailable, or to regular Metal when only per-pixel support is unavailable. macOS CI packages include the backend automatically.

The build option is restricted to standalone macOS. iOS, tvOS and libretro integration are outside this first version. The existing macOS deployment target is preserved, but runtime coverage on older macOS and Intel/AMD GPUs is still needed. Metal 4 is not required.

## Included

- Native opaque, punch-through and translucent geometry, regular sorting, depth/stencil/blend state, clipping, flat/Gouraud colors, fog, bump mapping, palettes and modifier-volume shadows.
- Native per-pixel transparency: manual opaque-depth rejection before fragment allocation, bounded atomic fragment lists, deterministic depth/submission/primitive ordering, two-volume and secondary-accumulator blending, and ordered translucent modifier volumes. Opaque color replay retains its own depth history. Native depth interpolation works in both modes.
- Screen rendering, direct framebuffer display, full framebuffer emulation, render-to-texture caching and VRAM writeback.
- Dreamcast texture decoding through the shared cache, mipmaps, anisotropy, asynchronous custom-texture replacement and GPU preloading. Compressed replacement packs use the existing transcoder's RGBA fallback; native compressed GPU uploads are not advertised.
- Standard sampling and every active reconstruction mode (IDs 1–13 and 15–19); retired ID 14 is normalized to disabled, as in the other backends.
- ImGui UI, dynamic fonts, artwork, VMU and crosshair overlays, screenshots/save-state thumbnail readback, resize/Retina sizing, rotation, interpolation, integer scaling and presentation intervals.
- Native Syphon publication, including its output scaling option. GPU resources stay alive through command-buffer completion.
- Renderer/resolution metrics and completed-command-buffer GPU timing for screen render work, excluding general UI and RTT rendering. Per-pixel timing sums its PVR command buffers. Full-framebuffer presentation retains evidence of that PVR work; CPU-only framebuffer video reports GPU timing unavailable.

## Current limitations

This is experimental and does not establish whole-game compatibility parity. Naomi 2 geometry is not implemented. The regular renderer rejects secondary-accumulator drawing; that path is implemented by the per-pixel renderer.

Per-pixel rendering prioritizes correctness over throughput: collection, dependent modifier steps and resolve use explicit GPU ordering/waits, and resolve sorting is quadratic in fragment count. Pixel Buffer Size is a hard budget bounded by the device allocation limit; allocation grows lazily within it and exhaustion reports an error. Maximum Layers keeps the nearest configured number of fragments deterministically and reports truncation. Full-framebuffer dithering runs at the existing CPU readback boundary on RGBA8 pixels; fractional pre-quantization parity with other backends is not guaranteed.

Broader game and composed-display comparisons, Intel/AMD and older-macOS runtime coverage, long-running resource stress and OBS/third-party Syphon receiver coverage remain necessary. No performance advantage over Vulkan is claimed.

## GPU verification

With Metal enabled, this explicit build target compiles and runs 19 offscreen checks, including 16 GPU tests, renderer selection, nil-device availability and framebuffer dithering/timing evidence:

```sh
cmake --build build-metal --target metal-smoke
```

The checks exercise actual Metal resources and the renderer's authored shader source: texture mip contents and replacement lifetime, depth rejection, punch-through, fog, blending, alpha-aware reconstruction, modifier-volume depth/stencil/shadow behavior, polygon and modifier-volume culling against OpenGL, flat-shaded strips/sorted triangles, bump/punch-through alpha semantics, and screen-target preservation across RTT/framebuffer uploads and grow/shrink transitions. They are excluded from ordinary release builds. A machine without an accessible Metal device cannot run them.

The separate `metal-context-smoke` target creates a real SDL window and exercises the production Metal context: RGB screenshot readback, rotation and size bounds, drawable resizing, VSync/fast-forward transitions, and context destruction/recreation. App-level UI, configuration and overlays are stubbed for that test; it does not replace game or UI validation.

```sh
cmake --build build-metal --target metal-context-smoke
```

The updated suite includes a 20,480-channel blend matrix, 216 OpenGL/Metal reconstruction comparisons, hidden-fragment rejection before allocation, buffer growth/retry, layer limits, secondary persistence, two-volume shading, native depth, overlapping modifier truth tables, and separate opaque depth/stencil replay. Two additional real-window targets exercise production presentation transforms and queued ImGui/font-atlas/texture lifetime:

```sh
cmake --build build-metal --target metal-context-presentation-smoke metal-imgui-smoke
```

The implementation has been built on macOS arm64 with Metal enabled and disabled. The current host is an Apple M5. An x86_64 macOS 10.15 availability check passed for the context; the timed-present fallback for macOS 10.15.0–10.15.3 has not been run on that OS. The tests are native-GPU evidence on this host, not universal hardware certification.

### Crazy Taxi attract-demo regression

Crazy Taxi (USA) initially rendered large black gaps in the attract demo: the road, sky and building faces were missing. The regular geometry cull mapping selected the opposite faces. Removing that inversion restored the surfaces. The renderer also now converts flat shading to Metal's first provoking vertex before uploading geometry; this preserves polygon colors and punch-through alpha for strips and sorted triangles. A separate shader correction preserves calculated bump-map alpha when texture alpha is ignored.

Using the same input replay, 640×480 resolution, per-triangle sorting and single-threaded rendering, the original Metal capture had visible OpenGL pixels replaced by near-black across 73.793% of the image. After the fixes, that measure was 0% at both the 50-second and 70-second attract-demo checkpoints. RGB mean absolute differences against OpenGL were 0.154/255 and 0.146/255 respectively. These checks cover those scenes; they do not establish whole-game compatibility or exact pixel parity.

To repeat the comparison, build with `-DTEST_AUTOMATION=ON` and run `tests/metal/capture_game.py` twice with renderer IDs 0 (OpenGL) and 7 (Metal), the same local game/BIOS/VMU inputs, and `--input tests/metal/crazy_taxi_attract.input`. Each run requires a fresh `--output` directory. Compare the resulting screenshots with `tests/metal/compare_capture.py` (Pillow required). The supplied replay ends at 50 seconds; extending its final timestamp from 10000000000 to 14000000000 selects the later checkpoint. The comparator detects missing black surfaces and reports color differences; it is not a general visual-parity assertion. Game, BIOS, saves and captured images remain local and are not included in the tests. Disable `TEST_AUTOMATION` again for normal interactive builds.

### Flycast City Escape report

The known gap in [Flycast PR #2563](https://github.com/flyinghead/flycast/pull/2563), inspected at head `667999a892d672dc4b900d290aede363be581489`, is intermittent solid-black areas beside the road and in front of cars in Sonic Adventure 2's City Escape. The report identifies its per-pixel renderer (`rend=8`) as affected and says regular rendering (`rend=7`) is fine. Hollycast's independent per-pixel path explicitly rejects hidden fragments before appending and has GPU regressions for that behavior, ordered modifiers and opaque replay. Sonic Adventure 2 media is unavailable locally; City Escape itself has not been tested, and this is not a claim that the scene is artifact-free.

Follow-up tests did expose two independent regular-renderer defects:

- Modifier-volume faces were culled opposite to OpenGL under the backend's Y transform. A real GL/Metal comparison of both triangle windings and ISP modes 2/3 failed in all four cases before the mapping correction and matched afterward.
- An RTT pass or CPU framebuffer upload discarded the retained screen target. A subsequent screen scene requesting color preservation therefore cleared untouched pixels to black. The production target owner now retains screen attachments independently. An actual GPU sequence paints a red screen, performs an unrelated RTT/upload, then updates one pixel blue; it verifies that the remaining pixels stay red. Both transitions failed before the fix and passed afterward.

Neither defect is established as the cause of Flycast's per-pixel City Escape issue. The capture harness also supports `--resolution`, `--emulate-framebuffer`, `--threaded`, `--per-strip` and `--no-modifier-volumes` for controlled comparisons, and records the settings plus executable/replay hashes in each local output directory.

### Additional game comparisons

The following local comparisons used the same 50-second SH4-cycle replay, 640×480 window, local game/BIOS/save inputs, no custom textures, and Metal API validation. Each OpenGL/Metal pair used the same executable and replay hashes. Captures were inspected visually and checked by `compare_capture.py`; RGB mean absolute error is reported, not treated as proof of exact parity.

| Game and scene | Rendering settings | Raw capture size | Reference-visible pixels made near-black | RGB MAE / 255 |
| --- | --- | --- | --- | --- |
| Crazy Taxi (USA), attract demo | 960-line internal resolution, per-triangle, single thread | 1280×960 | 0% | 0.154 |
| Crazy Taxi (USA), attract demo | 480-line, per-strip, single thread | 640×480 | 0% | 0.154 |
| Crazy Taxi (USA), attract demo | Full framebuffer emulation, per-triangle, single thread | 640×478 | 0% | 2.600 |
| Jet Grind Radio (USA), title over city scene | 480-line, per-triangle, single thread | 640×480 | 0% | 0.129 |

The full-framebuffer comparison initially failed on image dimensions because OpenGL automation writes raw framebuffer pixels while Metal automation used aspect-corrected screenshots. Metal automation now writes the raw owned game image too; normal screenshots retain their aspect/rotation behavior. This is a test capture correction, not a change to framebuffer rendering.

The comparator gates missing pixels as a fraction of visible reference pixels, so losing a small illuminated object in a mostly dark scene cannot pass merely because the rest of the image is black. It rejects an all-black reference as inconclusive. Its three CLI regression cases can be run with `python3 tests/metal/test_compare_capture.py`; both false-pass cases failed before the comparator correction and pass afterward. Raw scene comparisons do not test the composed display's overlays, aspect fit or video shifts.

Threaded per-strip Crazy Taxi also ran to the checkpoint without Metal validation errors, but the two captures landed on different animation frames (RGB MAE 19.155/255). That run establishes a runtime smoke result only; it does not establish threaded image parity. The current replay-end screenshot trigger is unsuitable for exact comparisons across independently scheduled rendering threads.

### Per-pixel and renderer-option verification

The final code was integrated with `dev` at `130414d87`, including HarfBuzz text shaping, before verification. Metal-enabled and disabled arm64 builds passed. All nine production Objective-C++ Metal sources also passed an x86_64 macOS 10.15 syntax/availability check. The 19 offscreen checks, three context/presentation/ImGui checks and three comparator regressions passed.

`renderer_entry_regression.py` links an isolated fixture to the built application's real renderer and dependencies. It replaces only TA parsing with pre-parsed scenes and ImGui presentation, and calls the actual `Process`/`Render` entry points. Six GPU pixel assertions cover blended opaque shadows, unsorted translucent shadows/two-volume color, disabled depth writes and multi-pass translucent depth masks. Additional assertions verify RTT storage followed by screen sampling, actual full-framebuffer rendering, nonzero per-pixel GPU timing with the expected epoch, and preservation/clearing of that evidence across framebuffer uploads. Temporary objects, dependency files and execution data stay outside the shared build and user data.

```sh
# Requires an already-built TEST_AUTOMATION Ninja app and compile_commands.json.
python3 tests/metal/renderer_entry_regression.py build-metal
```

The options runner passed **155/155** hidden-window Crazy Taxi saved-scene configurations. It covers triangle/strip/pixel selection, every UI resolution from 240 through 4320 lines, all reconstruction modes, sampler modes, anisotropy, filter-size limits, layer and buffer budgets, frame skipping, texture upscaling, native depth, fog/shadows, framebuffer/RTT controls and display/routing toggles. Continuous controls use representative values and boundaries; this is not an exhaustive cross-product. These runs assert backend identity, state load, nonblack output, expected resolution where applicable and absence of Metal validation failures. Shader/presentation fixtures provide separate semantic checks.

```sh
python3 tests/metal/options_capture_matrix.py \
  --app /path/to/TEST_AUTOMATION/Hollycast \
  --game /path/to/game.cue --bios-dir /path/to/local/bios \
  --state /path/to/isolated.state --output /path/to/new/results
```

The capture harness defaults to hidden background windows and rejects binaries without that support before launch. macOS monitoring confirmed no visible Hollycast windows and no foreground-app change during Metal, OpenGL and Vulkan captures. A fixed 500 ms emulated delay after replay completion lets a restored GPU-rendered state rebuild its initially unwritten VRAM front buffer; it is shared across all backends and does not skip frames based on pixel color. Frame-skip timeout handling starts at replay completion, avoiding immediate expiration against an older replay timestamp after state restore.

Final per-pixel comparisons used the same executable, game state/replay and settings within each pair:

| Scene | Reference | Native Metal settings | Missing visible surfaces | RGB MAE / 255 |
| --- | --- | --- | --- | --- |
| Crazy Taxi saved attract scene | Vulkan per-pixel | 960 lines, native depth, reconstruction 12, anisotropy 16, 128 layers | 0% | 0.129 |
| Jet Grind Radio title/city scene | Vulkan per-pixel | 480 lines, default per-pixel settings | 0.001% | 0.377 |
| Crazy Taxi saved attract scene | OpenGL full framebuffer | Full framebuffer, 640×478 raw output | 0% | 0.296 |

A local generated pack replacing 157 dumped textures visibly changed the game. Asynchronous replacement, CPU preloading and GPU preloading produced pixel-identical captures. The test media, BIOS, saves, replacement pack and images remain local and are not committed.

Actual UI checks exercised regular API switching, selecting Metal per-pixel and hiding Metal under a nil-device interposer. Saved IDs 7 and 8 both fell back to Vulkan when the Metal probe failed. A real Syphon Metal client received unscaled and scaled/rotated output with orientation matching the existing OpenGL publisher. The live regular renderer statistics overlay showed GPU work; the full renderer fixture verified the per-pixel producer and framebuffer timing behavior without foreground windows.

### Independent review

**Standards:** no hard violations were found. The optional duplicated-filter-selection cleanup was applied.

**Spec/correctness:** independent review found and fixed lost secondary state, incorrect modifier exclusion, opaque depth replay, and unsorted/non-shadow polygon routing. GPU and complete-renderer regressions cover those triggers. Exact City Escape, Intel/AMD runtime behavior and whole-game compatibility remain unverified.

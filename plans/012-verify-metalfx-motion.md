# 012. Test MetalFX with known motion across frames

| Audit finding | Priority | Effort | Risk of change | Area | Planned at |
| --- | --- | --- | --- | --- | --- |
| 12 | P2 | M | Low | Renderer tests | `ebf8fbb79` · 2026-11-26 |

The presentation smoke exercises scaler setup, a color change and subpixel jitter. A uniformly cleared image offers no spatial feature to track. A deterministic moving fixture can detect a reversed or incorrectly scaled motion vector before it reaches players.

## Before starting

Work from the Hollycast repository root. Branch from `dev` as `test/metalfx-known-motion`. Keep the change in one logical commit or a short series with imperative subjects. The target branch for an authorized pull request is `dev`.

This plan can start independently. Compare the scoped history against the baseline. If a hunk has moved, re-find its symbol and check that the behavior below still applies.

```sh
git diff --stat ebf8fbb79..HEAD -- \
  'tests/metal/context_presentation_smoke.mm' \
  'core/rend/metal/CMakeLists.txt' \
  'core/rend/metal/metal_fx.h' \
  'core/rend/metal/metal_fx.mm'
```

## Current behavior and local conventions

The existing temporal sequence fills the entire input with one color per frame. [tests/metal/context_presentation_smoke.mm:97](https://github.com/nexus382/Hollycast/blob/ebf8fbb79022b7ad85509759a5ff5ab074d9dbc7/tests/metal/context_presentation_smoke.mm#L97)

```objective-c++
	for (int frame = 0; frame < 3; ++frame) {
		auto cmd = [queue commandBuffer];
		pass.colorAttachments[0].clearColor = MTLClearColorMake(1, frame * .25, 0, 1);
		[[cmd renderCommandEncoderWithDescriptor:pass] endEncoding];
		require(spatial.encode(cmd, color, w, h) == color, "native-size frame bypasses spatial scaling");
		auto output = spatial.encode(cmd, color, w * 2, h * 2);
		require(output != color && output.width == w * 2 && output.height == h * 2, "spatial upscale");
```

The implementation estimates current-to-previous displacement for each 4×4 block. [core/rend/metal/metal_fx.mm:120](https://github.com/nexus382/Hollycast/blob/ebf8fbb79022b7ad85509759a5ff5ab074d9dbc7/core/rend/metal/metal_fx.mm#L120)

```objective-c++
	// current-to-previous motion per 4x4 block, comparing at unjittered positions.
	NSString *source = @R"(
#include <metal_stdlib>
using namespace metal;
```

The scaler consumes jitter, depth and motion in the same presentation path. [core/rend/metal/metal_fx.mm:275](https://github.com/nexus382/Hollycast/blob/ebf8fbb79022b7ad85509759a5ff5ab074d9dbc7/core/rend/metal/metal_fx.mm#L275)

```objective-c++
		scaler.reset = !hasHistory;
		scaler.depthReversed = YES;
		scaler.motionVectorScaleX = scaler.motionVectorScaleY = 1.f;
		scaler.jitterOffsetX = offset.x;
		scaler.jitterOffsetY = offset.y;
		scaler.colorTexture = color;
		scaler.depthTexture = sceneDepth;
```

Run the existing presentation target with the Metal debug layer enabled. [core/rend/metal/CMakeLists.txt:249](https://github.com/nexus382/Hollycast/blob/ebf8fbb79022b7ad85509759a5ff5ab074d9dbc7/core/rend/metal/CMakeLists.txt#L249)

```cmake
add_custom_target(metal-context-presentation-smoke
    COMMAND ${CMAKE_COMMAND} -E env MTL_DEBUG_LAYER=1 $<TARGET_FILE:metal_context_presentation_smoke>
    DEPENDS metal_context_presentation_smoke USES_TERMINAL)
```

## Scope

- `tests/metal/context_presentation_smoke.mm`
- `core/rend/metal/CMakeLists.txt`
- `core/rend/metal/metal_fx.h`
- `core/rend/metal/metal_fx.mm`

Keep shipping output and settings behavior intact. Prefer the existing composed-image readback. Where hardware output alone cannot distinguish sign or scale, expose the real generated motion texture only to the presentation smoke build through a target-local compile definition.

## Implementation and verification

### 1. Run the current presentation test on hardware

Use a Mac with a Metal device, MetalFX temporal support and an SDL video session. Build the actual smoke target. Record GPU, macOS and whether the existing success line says spatial and temporal. A spatial-only device still provides fallback coverage.

Run the **GPU baseline** command below. The existing context smoke runs. On a supported machine the output includes “MetalFX spatial and temporal GPU presentation passed” and the Metal validation layer reports no error.

### 2. Submit a known translated scene

Extend `checkMetalFX` with a high-contrast, asymmetrically textured patch on a stable background. Use two-axis motion inside the existing search range, with a static reference frame, positive and negative displacement, consistent depth, actual jitter and completed command buffers. Select blocks well inside the patch. A patch moving right by four input pixels should point about four pixels left into the previous frame. Keep captured presentation pixels and motion-vector units distinct.

Run the **Compile fixture** command below. The smoke executable links, including shader/test readback code.

### 3. Assert direction and scale with a negative control

Check nonzero direction and approximate displacement for moving blocks, near-zero background motion, and correct output placement within a documented tolerance. Account for the real jitter delta. For a disposable negative-control run, reverse the production-generated vector or double its effective scale in the test build while keeping the expected displacement fixed. The normal assertion must fail. Remove the mutation and rerun. Emit the new evidence line only after the supported temporal branch executes and passes. Exercise reset/size change, held frame and spatial fallback.

Run the **Known motion** command below. Both evidence lines are present from the same successful GPU run. Inspect the log for validation errors.

## Test plan

- Use a pattern with unique local features and a displacement recoverable by the current coarse-to-fine search. Check both axes, static background and a fresh first frame.
- Use interior blocks to avoid occlusion edges. If reading an RG16Float motion buffer, decode half floats and respect row stride. Use the existing blit/readback conventions after GPU completion.
- Require a sign or scale error to fail the controlled temporal assertion on supported hardware. Keep the existing retained-frame, mismatched viewport and framebuffer spatial fallback assertions.

## Verification commands

**GPU baseline**

```sh
cmake -S . -B build-metal-tests -G Ninja -DENABLE_CTEST=ON -DUSE_METAL=ON -DUSE_VULKAN=OFF && cmake --build build-metal-tests --target metal-context-presentation-smoke
```

The existing context smoke runs. On a supported machine the output includes “MetalFX spatial and temporal GPU presentation passed” and the Metal validation layer reports no error.

**Compile fixture**

```sh
cmake --build build-metal-tests --target metal_context_presentation_smoke
```

The smoke executable links, including shader/test readback code.

**Known motion**

```sh
cmake --build build-metal-tests --target metal-context-presentation-smoke > build-metal-tests/known-motion.log 2>&1 && rg -F 'MetalFX known-motion checks passed' build-metal-tests/known-motion.log && rg -F 'MetalFX spatial and temporal GPU presentation passed' build-metal-tests/known-motion.log
```

Both evidence lines are present from the same successful GPU run. Inspect the log for validation errors.

**Scope**

```sh
git diff --check && git diff --cached --check && git status --short
```

Both whitespace checks exit zero. Compare the change list with the scope below.

## Done criteria

- [x] A real supported GPU emits the known-motion marker, the negative control fails for the expected assertion, and the restored run passes with the debug layer. Record OS, device, displacement and measured tolerance.
- [x] The step gates and applicable platform checks pass, with the host and observed results recorded.
- [x] The **Scope** gate matches the listed files. Record completion in the [plan index](README.md), or send the evidence to the reviewer maintaining it.

## Independent review

Reviewed `226e00e4`. On Apple M5 with macOS 27.2.1 and Metal API Validation enabled, both normal runs measured a mean motion vector of (−4.00, +3.00) within 0.75 input pixels. The composed marker moved (+14.2, −11.5) against an expected (+16, −12), within four output pixels. The reversed-vector run failed the intended direction assertion at both sampled blocks.

## Stop conditions

If only spatial MetalFX is available, record that result and leave the temporal acceptance check pending. If output pixels are insensitive to a reversed vector, use the test-only vector seam instead of widening image tolerances. If upstream has already fixed the behavior, demonstrate it with the regression before retiring the plan.

## Maintenance notes

Keep tolerances in input-pixel units for motion and output-pixel units for composition. Use device-independent regions and signaling so a skipped temporal branch cannot count as a pass.

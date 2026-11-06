<!-- Portions Copyright 2026 The Hollycast Authors -->
# Native Metal display shaders

Standalone macOS builds include the shader executor whenever `USE_METAL=ON`.
It runs directly on Metal for both regular and per-pixel rendering. Shader
selections remain off for new configurations; existing selections are preserved.

The executor uses the shared GLSL, GLSL preset, Slang preset and supported FX
loaders. Their typed stages are translated to Metal Shading Language using the
same pinned SPIRV-Cross revision as the other shader translators. Compilation
and lookup uploads finish before replacing the active chain. A rejected selection
keeps the previous chain and reports its diagnostic on the Shader page.

Implemented paths include ordered layers, multiple passes, live parameters,
compute dispatches, named/persistent textures, history and feedback, lookup
textures, mipmaps, floating-point and sRGB targets, multiple color outputs,
blending, stencil and scene depth. Display effects process the game pane's
borders; native presets retain their own source/viewport scaling. The menu and
live-preview controls stay outside the filtered image. Screenshots use the
processed image, while raw automation captures retain their existing contract.

The per-pass binding limits are 16 sampled textures, eight color attachments
and eight writable textures. Sampled-resource arrays and filtered integer
mipmap generation are rejected explicitly. Texture formats, render pipelines
and compute workgroups must also satisfy the selected device's capabilities.
Framebuffer-only video has no scene depth; a depth-dependent effect reports a
recoverable frame error until a scene with depth is available.

## Automated verification

The portable test build translates all bundled shaders to MSL. The optional
external inventory test also translates its full set of files and techniques.
Translation alone does not prove that Apple's compiler or GPU accepts a shader.

On a Mac, build the test executable and opt into the actual GPU checks:

```sh
cmake -S . -B build-metal-tests -G Ninja -DENABLE_CTEST=ON -DUSE_METAL=ON -DUSE_VULKAN=OFF
cmake --build build-metal-tests --target Hollycast --parallel
MTL_DEBUG_LAYER=1 HOLLYCAST_SHADER_METAL_TESTS=1 ./build-metal-tests/Hollycast --gtest_filter='MetalShader*'
```

The pixel checks exercise the production executor and fail if no Metal device
is accessible after opting in. They cover pass orientation, parameter edits,
candidate rejection, borders/rotation, sRGB/mipmaps, compute and depth recovery.
The existing `metal-smoke` and `metal-context-presentation-smoke` targets cover
the base renderer and presentation independently.

## Release-build checks

1. Select Metal in Video settings and load a title. Confirm shaders are off in
   a fresh configuration. Enable one effect, then a multi-pass preset and a
   stack of several different effects.
2. Open Shader Live Preview. Change a parameter while playing, then pause and
   edit it again. Verify the image responds, the controls do not obscure the
   game pane, and the compiling notice appears before a shader change stalls.
3. Check normal and per-pixel Metal modes, resize the window, rotate the image,
   and check a title with borders. Verify screenshots match the visible effect.
4. Try a known-invalid shader and confirm the previous effect remains active
   with a useful error. Disable all effects and confirm normal presentation.
5. Compare the same scene/settings with another renderer, including a shader
   that uses feedback, a compute pass, and a depth-dependent effect.

Record macOS version, GPU, build commit, shader/preset names and any compiler
diagnostic. Apple Silicon and Intel/AMD GPU coverage are separate results;
a successful build or one machine's tests do not establish universal parity.

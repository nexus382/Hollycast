<!-- Portions Copyright 2026 The Hollycast Authors -->

# Shader system

Display effects run as ordered layers. Each layer can be a GLSL shader, a GLSL preset (`.glslp`), a Slang stage, a Slang preset (`.slangp`), or a ReShade FX effect. Preset passes execute in their declared order before the next layer receives the result.

## Compilation and resources

ReShade FX uses the upstream standalone effect compiler in `core/deps/reshadefx`, with its license and pinned revision kept alongside the sources. GLSL-to-HLSL compilation uses glslang, SPIR-V, and SPIRV-Cross instead of expression substitutions. The same expanded shader sources and reflected parameters feed the graphics backends. Uniform arrays retain each element, and numeric vectors and square or rectangular matrices use the shared parameter layout. Runtime timer, frame-time, date, keyboard, and mouse sources are sampled by the host and excluded from saved controls.

The graph tracks pass outputs, aliases, previous-frame inputs, feedback, lookup textures, filtering, wrap modes, mipmaps, and per-axis scaling. Preset references retain the directory of inherited resources. Resources from unrelated layers remain separate. Parameters and technique selections follow their layer when it is moved or removed.

A shader may appear in multiple layers. Use **Duplicate** in Active Shader Layers and choose the technique for each copy. Effects that save and restore an image around other effects need their producer technique first and their consumer technique later. For example, a UI mask uses a top layer, the effects to mask, and a bottom layer from the same FX file. Its mask image must be supplied by the shader package; a missing image is an error.

Graphics passes retain independent color and alpha blending, per-target write masks, clear state, stencil state, primitive topology, and vertex count. Named effect textures inherit their preceding write across passes and frames. A pass writing an auxiliary texture leaves the current backbuffer available to subsequent passes.

Compute passes retain their local workgroup sizes and explicit dispatch group counts. Writable images are synchronized before subsequent sampling. Floating-point and integer target formats retain their texel type. Lookup assets support 1D, 2D, array, cube-face, and 3D image layouts, including normalized-domain `.cube` LUTs. Selection preflight reports missing or undecodable assets before activation.

Depth effects receive the renderer's scene depth where that renderer exposes a sampleable depth surface. Depth effects require a rendered scene with sampleable depth. If a frame has none, the backend reports that limitation and presents the original image. Depth reconstruction settings remain specific to the effect and rendering API.

## Boundaries

Compatibility is determined by the shader's required resources, render state, and graphics API capabilities. This is not a promise that every third-party shader package works. Compute needs DirectX feature level 11, OpenGL 4.3 / GLES 3.1, or a Vulkan graphics queue with compute support. GLES cannot provide native 1D images. Resource binding arrays and writable FX views above mip level zero are currently rejected. Per-binding sampler states outside the supported filter/wrap profile are also rejected. The UI and backend report unsupported requirements rather than silently changing the effect. External textures and includes must be present within the shader library's allowed package paths.

## Regression coverage

The shader tests cover parsing, graph dependencies, parameter packing, preset references, resource isolation, and generated graphics and compute stages, resource preflight, host input sources, and native pass states. On Windows, generated HLSL is checked with Microsoft's D3D compiler and the parameter buffer layout is inspected through reflection. A Windows software-device regression also executes a translated compute stage and checks its pixels through readback.

Set `HOLLYCAST_SHADER_TEST_ROOTS` to semicolon-separated shader directories or preset files to compile an external inventory in the test executable. This also checks Vulkan stages when Vulkan is enabled. Compiler checks do not replace live rendering checks for image orientation, depth, feedback, blending, and presentation.

# Planet Harriers rendering review — 2026-11-02

Base: `0964a6750` (`hikaru-arcade-support`), fetched and fast-forwarded from `198be7ef6`. Prepared on local branch `hikaru-renderer-settings` for [PR #46](https://github.com/nexus382/Hollycast/pull/46), whose head is `hikaru-arcade-support`.

## Scope and environment

Manual checks used the full Hollycast SDL application on Apple Silicon/macOS, compiled with OpenGL, native Metal, and Vulkan through MoltenVK. The user’s local `pharrier` chip folder was packaged locally for the normal ROM loader. No ROM data is included in this change. Intro, character selection, the opening Windel corridor/outdoor section, enemies, projectiles/explosions, HUD, and changing gold digits were sampled. This is not a full-game texture audit or a hardware-accuracy certification.

The matrix was selected through transient Hollycast configuration values, using the same RendererType/PerStripSorting settings consumed by the UI. Saved API, sorting, and audio preferences were not overwritten. Most later runs were muted after the user reported screeching.

## Full-application matrix

| Graphics API | Per strip | Per triangle | Per pixel |
| --- | --- | --- | --- |
| OpenGL | Gameplay/HUD/effects; gold 80 readable | Gameplay/HUD/effects; gold 40 readable | Gameplay/HUD/effects; gold 640 readable; additional default-32-layer run |
| Metal | Gameplay/HUD/effects; gold 640 readable | Gameplay/HUD/effects; gold 40 readable; final batched build 140 → 380 readable | Gameplay/HUD/effects; gold 100 and 600 readable |
| Vulkan / MoltenVK | Gameplay/HUD/effects; gold 40 readable; shop text also inspected | Gameplay/HUD/effects; gold 300 readable | Gameplay/HUD/effects; gold 400 readable |

Per-pixel sampling initially used an eight-layer limit; the later OpenGL audio reproduction used the default 32. Synthetic regression cases additionally cover the configured capacity and overflow behavior. Scene complexity affects speed: Vulkan strip reached approximately 16–17 ms/frame in sampled gameplay, while heavier per-pixel scenes exceeded the real-time budget. These timings are observations, not a broad performance guarantee.

The reported corrupted nonzero gold counter did not reproduce in these samples. That does not establish that every animated digit or game stage is correct.

The corridor floor has alpha-zero regions whose pattern matches the visible cutouts. These were observed in Metal, Vulkan, OpenGL per-pixel and the final OpenGL triangle run. A temporary diagnostic forcing sampled alpha to one filled the gaps but broke intended sprite transparency; it was removed. Texture uploads contain the matching alpha pattern. Whether the original material/alpha interpretation matches hardware remains unverified; no blanket opacity workaround was retained.

## Changes and review

Hikaru now keeps Hollycast’s selected API and sorting mode. The vendor scene path dispatches graphics operations to actual Metal/Vulkan resources, shaders and draws, with the normal host context supplying UI and presentation. It does not present an OpenGL-rendered frame as a substitute for native rendering. Translucent sorting supports transformed mesh/instance ordering, triangle ordering, and bounded depth peeling. Unsupported DirectX choices are hidden for Hikaru.

Six GPT-6 Luna subtasks contributed implementations and reviews. Parent review caught and corrected full-app ARC flags, native OSD double submission, sampler-state remapping, fractional LOD bias, Metal color-mask mapping and encoder state, Vulkan winding/lifetimes/barriers, cached GL program rebinding, and repeated uniform conversion overhead. Tests use synthetic data, including Metal GPU pixel checks for depth peeling, composition, masks, mip selection, culling and depth-disabled drawing. Native Metal requires ARC at compile time. The final triangle path batches up to 256 adjacent triangles sharing a mesh and instance, preserving global sorted order. Its pixel regression contains 257 triangles and requires the visible triangle in the second batch to render. Native device batching also snapshots vertices before the vendor scratch buffer is reused or released.

## Audio investigation

The user described constant screeching during music in the earlier live OpenGL per-pixel run. Raw AICA output and post-pacer PCM were captured locally before host volume scaling. The first strip-mode clips had nearly identical levels/spectra and no clipping; the user confirmed neither contained the reported screech. An intro-heavy OpenGL per-pixel capture likewise showed no clipping or high-frequency amplification. A subsequent quiet default-32-layer live run retained gameplay audio. The user reported that it played too slowly to judge correctness. Its tempo varied around 0.6–0.8 in heavy scenes; the raw and paced latest-20-second windows therefore cover different guest-time intervals and cannot support a matched distortion comparison. Power above 8 kHz remained below 0.1% in both, which does not establish an audible fix. The final clean app was then checked with OpenGL triangle sorting and Metal triangle sorting at quiet volume. Batching reduced draw submission overhead, but these were not matched-frame benchmarks. The final Metal gameplay sample still took approximately 24–29 ms per frame, with audio tempo around 0.63–0.71; it therefore does not establish normal-speed audio correctness. A final Vulkan/per-strip comparison also took approximately 25–28 ms per frame in the corridor, with tempo around 0.61–0.72. Earlier near-real-time samples are scene-specific. A three-second profile (2,314 main-thread samples) divided the engine work roughly between renderer end-frame submission and board emulation; this is not solely a presentation/vsync delay. No full-speed live audio confirmation was obtained.

No sound-decoder or time-stretch algorithm change is justified by this evidence. Source review found no verified ADPCM stream, board context, channel-order or SDL sample-format defect. Heavy scenes can still slow emulation and produce underruns/time stretching. The original screech is not yet established as fixed. Temporary capture/opacity/texture-dump instrumentation is removed from the source.

## Validation

After final source cleanup, the full Hollycast app build succeeded. Pre-commit verification: vendor CTest 19/19 passed (16.59 s); integrated Hikaru CTest 14/14 passed (0.59 s), including graphics dispatch, native shader compilation and actual Metal GPU pixel tests. Host GPU access was used. `git diff --check` passed. Ninja compile commands confirm `-fobjc-arc` for Metal and `SOUNDTOUCH_FLOAT_SAMPLES=1` for the audio pacer. The latter CMake property is now explicitly applied in the executable target directory; it does not establish that the reported audio issue is fixed. Manual platform coverage is macOS only; other platform/API combinations remain unverified.

Local logs, recordings and diagnostic images are under `/private/tmp/planet-harriers-texture-review/`. They are not repository fixtures and contain local game output. Native screenshot files were also produced; their capture time can differ from the preceding UI observation, so they are not treated as exact-frame gold-counter evidence.

## Final local state

The final full Hollycast Vulkan/per-strip comparison is paused in its game menu, with a transient app-volume override of 20%. The preceding final Metal/triangle run reached gold 380 with readable digits. Saved preferences remain OpenGL/triangle/volume 100. The source changes and this report are prepared for PR #46. Rendering samples and regression checks pass within the scope above; slow heavy scenes, the original live screech and hardware parity of the common floor-alpha interpretation remain open.

# Shared emulator rules

These decisions record which parts of Hollycast share a rule and where to change it.

## Rendering

- Define frame history in `resolveShaderHistory`. Feedback needs the preceding frame. A history input needs enough frames to cover its offset. Loading a previous attachment also retains its producer. OpenGL, DirectX 11, Vulkan and Metal use this plan when allocating their own textures.
- The Hikaru device owns renderbuffer IDs. Its backend interface carries the resource type and depth format explicitly. Each backend selects a supported native format. On Metal, a mesh pipeline selects a fragment output compatible with the attachments bound for that draw.
- Vulkan fragment-list tests use the same hardware requirements as the renderer. Devices without geometry shaders still exercise ordered drawing and fallback. Metal shader tests supply scene depth for bundled effects that sample it.

## Audio and media

- Use `audio::OutputSampleRate` for host output, buffer timing and playback speed. Android passes it to AudioTrack through JNI. SDL negotiates its fallback device rate separately. The guest sound board owns its hardware clock.
- The ROM catalog classifies arcade hardware for both loading and browsing. The disc-image extension list lives in `core/imgread/disc_image.h`. A library view applies the same eligibility rules to selection, list and grid.
- Artwork providers share the check for a missing cached image. A completed search can legitimately return no image. A downloaded image that disappears is eligible for another fetch. The hover card and details page also share title and image fallbacks.
- Each theme directory owns its migration state. Font handles belong to the current ImGui atlas and expire when it is replaced or destroyed.

## Saves

- `core/oslib/vmu_path.cpp` owns standalone VMU filenames, slot overrides and search order. Existing per-game and legacy cards retain their lookup paths.
- The phone VMU writer and restore menu use one snapshot format. A timestamp and optional six-digit sequence order successive backups within a second. Phone checkpoints retain five snapshots. Restoration verifies the selected image and backs up the current card before replacement.

## Networking and verification

- Each DCNet transport chooses its port where it creates the socket. Each service entry pairs its catalog ID with its availability probe and tooltip.
- The testing tools share atomic JSON publication, observation redaction and telemetry freshness. Request and evidence files use exclusive creation. Live status updates use replacement.
- HTTP client tests inspect requests on loopback, including multipart filenames and bytes. File uploads send the basename and read the contents from the full local path. SDL mapping tests pass text into the production parser. The Metal test source applies ARC in the CMake directory that owns the executable.

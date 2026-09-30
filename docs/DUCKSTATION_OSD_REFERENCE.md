<!-- Portions Copyright 2026 The Hollycast Authors -->

# DuckStation On-Screen Display Reference

This is a source-backed reference for DuckStation's performance OSD and the
closest Hollycast equivalents. This references
`https://github.com/stenzek/duckstation` at commit `18147a1` (2026-08-04) so
these locations remain easy to re-check. The screenshot supplied for this task
is the compact DuckStation performance overlay.

## Source locations

| Purpose | DuckStation location |
| --- | --- |
| OSD settings page and labels | `src/core/fullscreenui_settings.cpp:2728` (`MenuHeading("On-Screen Display")`) |
| Runtime OSD assembly and drawing | `src/core/imgui_overlays.cpp:385` (`DrawPerformanceOverlay`) |
| FPS, VPS, speed, frame-time, CPU-thread, render-thread, and GPU-time sampling | `src/core/performance_counters.cpp:168` (`PerformanceCounters::Update`) |
| FPS/VPS and OSD setting toggles | `src/core/hotkeys.cpp:74` |
| GPU/emulated-renderer counters | `src/core/gpu_backend.cpp:616` (`GetStatsString`) |
| GPU memory/copy counters | `src/core/gpu_backend.cpp:646` (`GetMemoryStatsString`) |
| Emulated PS1 GPU busy percentage (`U`) | `src/core/gpu.cpp:1766` (`GPU::UpdateOrGetGPUBusyPct`) |
| End-to-end latency line (`AL`, `AF`, `PF`, `IL`, `QF`) | `src/core/system.cpp:3526` (`System::FormatLatencyStats`) |
| GPU timestamp accumulation at present | `src/core/video_presenter.cpp:1573` |
| GPU timestamp backends | `src/util/d3d11_device.cpp:767`, `src/util/d3d12_device.cpp:478`, `src/util/opengl_device.cpp:914`, `src/util/vulkan_device.cpp:1082`, and Metal's matching implementation |
| DuckStation frame-time history size | `src/core/performance_counters.h:19` (`NUM_FRAME_TIME_SAMPLES = 152`) |

## What each screenshot line means

| Screenshot text | Meaning | Measurement class |
| --- | --- | --- |
| `2.98 FPS` | Internal game-frame rate over roughly a one-second sample. It is based on DuckStation's internal frame counter, so it can differ from video rate when the game submits fewer unique frames. | Emulated timing |
| `59.67 VPS` | Video/vblank rate, based on DuckStation's emulated video-frame counter. This is the rate the emulated display is producing, not necessarily the number of unique game frames or host presents. | Emulated timing |
| `100% (100%)` | Current emulation speed followed by the configured target speed. `100%` means the emulated video rate is at the target rate; `(Max)` or another value indicates the limiter target. | Emulation pacing |
| `OpenGL-MT HW` | OpenGL renderer, multi-threaded GPU backend, hardware renderer. | Configuration/state |
| `1 P` | Average emulated GPU primitive count per sampled frame. | Emulated GPU work |
| `15 DC` | Average host draw-call count recorded by the GPU backend. | Host renderer work |
| `0 RB` | Average host GPU readbacks/downloads. These can stall the renderer. | Host renderer work |
| `0 C` | Average emulated GPU copy operations. | Emulated GPU work |
| `1 W` | Average emulated GPU register/memory writes counted by the backend. | Emulated GPU work |
| `3% U` | Emulated PS1 GPU busy percentage. It is calculated from emulated GPU active ticks versus the PAL/NTSC frame budget, not host GPU utilization. It is refreshed about once per 50/60 frames. | Emulated timing pressure |
| `747MB VRAM` | Bytes currently tracked by DuckStation's GPU texture objects, rounded to MiB. This is an emulator allocation total, not a complete operating-system GPU-memory reading. | Host renderer allocation |
| `633KB STR` | Host buffer-streaming bytes accumulated by the GPU backend, rounded to KiB. | Host renderer transfer |
| `0 B` | Host GPU barriers. | Host renderer counter |
| `8 RP` | Host render passes. | Host renderer counter |
| `0 C` | Host GPU copies. | Host renderer counter |
| `1 TU` | Host texture uploads. | Host renderer counter |
| `3200x2399 NTSC Forced-Progressive` | Current emulated display dimensions, region/timing standard, and scan mode. | Emulated display state |
| `5x IR` | Internal rendering scale. | Render configuration |
| `PGXP` | Precision Geometry Transformation Pipeline is enabled. | Render configuration |
| `52ms AL` | Audio latency derived from the audio output buffer's current queued samples. | Host output latency |
| `17ms AF` | Active frame time: time spent actively processing the frame. | Frame pacing |
| `0ms PF` | Pre-frame sleep: time intentionally slept before the frame deadline. | Frame pacing |
| `17ms IL` | Estimated input latency from frame period, queued video frames, and run-ahead subtraction. It is a model, not a measured button-to-photon probe. | Latency estimate |
| `1 QF` | Number of frames queued in the GPU backend. | Present queue |
| Graph with `331ms` and `11ms` | A rolling graph of 152 frame-time samples. The labels are the current visible maximum and minimum; a spike indicates a long frame/present interval. | Frame pacing history |
| `CPU[182.7/IC]: 7.9% (1.32ms)` | The `182.7/IC` prefix describes CPU overclock and recompiler instruction-cache mode. `7.9%` is the core/emulation thread's CPU time divided by the wall-clock sample interval; `1.32ms` is average core-thread CPU time per frame. | Host thread CPU time |
| `RNDR: 4.1% (0.68ms)` | Video/render thread CPU time divided by the wall-clock sample interval, with average render-thread time per frame. | Host thread CPU time |
| `GPU: 56.3% (9.44ms)` | GPU timestamp time accumulated around DuckStation's backend present/render region, divided by wall-clock sample time; `9.44ms` is average measured GPU execution time per present. This is backend workload time, not total device utilization. | Host GPU execution time |

The labels `P`, `DC`, `RB`, `C`, `W`, `B`, `RP`, `TC`, and `TU` are compact
counters. They are not percentages and should not be read as “how much of the
console is used.” The `U` percentage is the exception: it is DuckStation's
emulated GPU timing percentage.

## Closest Hollycast equivalents

The current Hollycast implementation is in
`core/ui/performance_overlay.cpp` (`updateMetrics` and `draw`). The settings
page is `core/ui/settings_new.cpp` (`renderOnScreenDisplayTab`).

| DuckStation value | Hollycast value today | Important difference |
| --- | --- | --- |
| FPS | `metrics.gameFps`, from completed non-RTT PVR scenes | Counts distinct screen PVR scenes, matching DuckStation's internal game-frame meaning. |
| VPS | `metrics.vps`, from `vblankCount` and `recordSh4Vblank()` | Counts Dreamcast vblank samples. This is an emulated timing signal. |
| Output FPS | `metrics.outputFps`, from `recordPresentedFrame()` | Counts successful host presentation calls, including Vulkan frame duplicates. SDL2 counts completed swap calls because it exposes no result. Native iOS reports unavailable because GLKView owns presentation. This is an optional display-cadence diagnostic, not DuckStation's FPS counter. |
| Speed | `formatSpeed()` | Uses VPS against PAL/NTSC target and fast-forward settings. |
| CPU | `Host CPU` and `Host CPU (SH4 Thread)` | `Host CPU` separates the Hollycast process share of all logical cores from total device use. The SH4-thread line samples the registered execution thread independently of vblank, so a stalled SH4 loop remains visible; it is still host single-core usage, not Dreamcast SH4 saturation. |
| RNDR | `Host CPU (PVR Renderer)` | PVR renderer host-thread CPU work. It exposes emulator and driver-preparation cost, not hardware GPU utilization. |
| Emulated CPU | `Dreamcast SH4: target execution rate` | Emulated SH4 cycles advanced relative to the real-time target. Below 100% means Hollycast is not advancing SH4 time at the expected rate. |
| Emulated GPU `U` | `Dreamcast PVR: % of frame budget` | All renderer-complete delays scheduled during the sample divided by one active Dreamcast video-frame budget per vblank. Multiple renders accumulate, and values can exceed 100%. It is scheduler pressure, not a complete PVR busy-cycle percentage. |
| GPU | `Host GPU: percent (ms/render)` | Uses non-blocking timestamp queries on Vulkan, OpenGL/GLES, DirectX 11, and DirectX 9. A sample measures a PVR screen render or a direct framebuffer upload/render; RTT is excluded. Full framebuffer emulation can perform both operations per video frame, so the average is per timed render, not per presentation. The percentage sums their measured time over the sample interval, not total device utilization. Standalone iOS OpenGLES reports unavailable because it does not use the GLAD timer-query implementation. |
| GPU statistics | `formatGpuStatistics()` | Reports API, PVR primitive batches, PVR vertices, PVR render passes, and texture uploads per second. These are not host API draw-call or copy counters. |
| Resolution | `formatResolution()` | Reports raw, rendered, viewport, and output dimensions. Direct framebuffer frames report the backend's decoded/uploaded image size for raw and rendered; this includes 240p/interlace handling and a 1x1 border image on backends that use it for blank video. |
| Frame times | Present-interval average/min/max and 152-sample graph | Uses the same host presentation calls as Output FPS. These are API-level intervals, not physical scanout or full emulation frame-start intervals. Native iOS reports unavailable. |
| Latency | No row is shown until a probe exists | No audio-buffer, active-frame, pre-frame, queue, or input-to-photon instrumentation is currently exposed. |
| VRAM | DXGI per-process adapter memory | Shows Hollycast memory used/budget when Windows provides it. Fixed Dreamcast VRAM was removed because it never changes. |
| RAM | Hollycast process resident memory | Changes with content, caches, and allocations. Fixed Dreamcast RAM was removed because it never changes. |

## Four bottleneck domains

The Full OSD starts with four intentionally separate readings:

1. **Host CPU**: Hollycast's share of all logical CPU capacity and total device
   CPU use. Use the supporting SH4/PVR host-thread rows to find one saturated
   emulation thread on a multicore device.
2. **Host GPU**: timestamped backend execution time. `ms/render` averages completed
   timestamp intervals. Compare with the 16.67 ms budget at 60 FPS only when there
   is one timed render per video frame.
3. **Dreamcast SH4**: emulated SH4 execution rate versus its real-time target.
   It indicates whether SH4 time is advancing fast enough, not host CPU use.
4. **Dreamcast PVR**: the emulated render-complete delay as a share of the
   Dreamcast video-frame budget. It is explicitly a scheduler metric, pending
   a future source-owned PVR busy-cycle meter.

The optional frame-time graph uses the last 152 successful presents. Host CPU
and Host GPU graph options use 152 one-second samples. The CPU graph has a
Hollycast-process line and a total-device line; the GPU graph only appears
when the backend is returning real timestamp samples.

Open the page directly through Settings > On Screen Display in the in-app
dropdown or the macOS native menu. Both entries select the same settings tab;
the native menu dispatches the action onto the existing UI thread queue.

Position `0,0` defaults to the top right, 15 pixels from the right edge and
15 pixels below the visible menu bar. When the bar is hidden, the top inset
is 15 pixels. The overlay uses the bar's effective height for the current
frame, including fullscreen auto-hide. All selected rows and graphs shrink
together when needed to fit the available area; the configured text size is
the maximum. Position offsets use screen pixels and keep the entire block
below the menu bar and inside the screen.

The renderer-statistics row reports texture uploads per second. It deliberately
does not estimate live texture count or VRAM residency; those require
authoritative backend or operating-system data.

## Collection and query lifetime

Visible text and its measured widths are cached between samples. Changes to
metric selection, language, fonts, rendering dimensions, and live speed or
renderer settings invalidate the relevant cache. Menu height, window bounds,
and position offsets are applied each frame without rebuilding unchanged text.

CPU, RAM, VRAM, and SH4 thread sampling only run when their readings are
selected; CPU and GPU graphs also request their corresponding collectors.
PVR CPU clock reads require the PVR work row, and presentation history requires
Output FPS, frame times, or the frame-time graph. A Full overlay with no
selected rows or graphs stops collection. Changing selections starts fresh
baselines and histories, so reenabled metrics exclude time spent hidden.
CPU and GPU graphs can also
be displayed without a text row.

Windows discovers VRAM adapters in one background task on first use or when
the adapter list changes. The drawing thread polls for a completed snapshot
without waiting; pending or failed discovery leaves the reading unavailable.
Completed adapter snapshots are reused for the regular memory queries. CPU,
RAM, and VRAM values retain their one second sampling interval.

Pause, resume, game start, and termination stop producers immediately and
request a reset on the next overlay draw. The drawing thread clears counters,
histories, and CPU baselines before starting a fresh interval. Paused time is
excluded, and GPU query generations reject results from the previous interval.
The SH4 sampler registers on the thread executing `Emulator::runInternal`,
including shared-thread mode; it reacquires its native thread reference only
after a stop or when a different thread begins execution. Apple process CPU
time uses `getrusage` so completed worker threads remain in the process total.

Vulkan timestamps start and end in the screen renderer's command buffer,
before unrelated render-to-texture or presentation work. A later submission
fence confirms completion without adding a GPU wait. Timestamp subtraction
uses the queue's valid bit width so hardware counter wrap remains valid.
Discarding renderer command buffers requires an idle queue and
`VulkanContext::ResetGpuTiming` to reset their query ownership.

GLES reads the disjoint flag once after collecting ready results. A disjoint
event discards all outstanding timings, including queries that finish later;
their slots remain reserved until completion. Both timer rings use bounded,
nonblocking polling and perform no query polling when empty.

## Implementation plan

### Phase 1: truthful values already sampled (implemented; needs gameplay visual test)

1. Format FPS, VPS, and emulation speed as one DuckStation-style rate line.
2. Show SH4 and PVR renderer CPU work as percentage plus average milliseconds
   per emulated video frame.
3. Retain a 152-sample successful-present interval history and render a compact
   min/average/max line with a graph.
4. Keep renderer API, texture-upload rate, resolution, host RAM, and host
   GPU-memory values, but make their labels precise.

### Phase 2: renderer-wide counters (implemented; needs device visual test)

1. Record PVR primitive batches, vertices, and render passes at the common PVR
   renderer interface. The line stays explicitly PVR-side so it cannot be read
   as a host graphics-driver counter.
2. Add four-slot, non-blocking GPU timestamp rings for Vulkan, OpenGL/GLES,
   DirectX 11, and DirectX 9. Each sample is collected only after its completion
   fence or query result is ready. Standalone iOS OpenGLES remains explicitly
   unavailable until it has a native timer-query implementation.
3. Accumulate every Dreamcast PVR renderer-complete delay scheduled by `spg.cpp`
   and divide by one active Dreamcast video-frame budget per vblank.

### Later renderer-counter work

1. Add complete, backend-consistent host draw-call, copy, readback, barrier,
   and texture-allocation accounting before showing DuckStation's `DC`, `RB`,
   `C`, `B`, or VRAM-style renderer counters.
2. Replace the scheduler-delay proxy with a full PVR active-tick busy metric
   only when a source-owned PVR timing model is available.

### Phase 3: latency instrumentation (separate implementation)

1. Measure audio-buffer latency, active frame time, pre-frame sleep, and the
   renderer queue depth.
2. Label queue-based input latency as an estimate. Do not call it measured
   button-to-photon latency without a real end-to-end probe.

## What must be true to match DuckStation cleanly

1. Keep four separate headline domains in the UI: host CPU, host GPU,
   Dreamcast SH4 execution, and Dreamcast PVR scheduling. Supporting host
   thread rows must never be presented as Dreamcast-side pressure.
2. Show a value's denominator in its label or help text. A per-thread
   percentage over a one-second wall interval is not “percent of the whole
   phone,” and a GPU timestamp percentage is not device-wide GPU utilization.
3. Add a real Dreamcast PVR busy metric only from Dreamcast/PVR timing data;
   do not infer it from host GPU usage, VRAM allocation, resolution, or PVR
   thread CPU time.
4. Only claim a `GPU: percent (ms)` line when the active backend reports real
   timestamp queries. Keep unavailable backends explicit.
5. Add a rolling frame-time history and distinguish emulation frame intervals
   from successful host-present intervals.
6. Add latency fields only when their source is explicit. A queue-based
   estimate can be useful, but it must not be presented as a measured
   button-to-photon result.

This explains the apparent contradiction seen during high-resolution tests:
Hollycast's SH4/PVR percentages can remain near 50% while the host GPU or
present path is saturated, because those values measure only timed CPU work on
specific emulator threads. The scheduler-pressure value can rise toward the
video-frame budget while host CPU and host GPU values remain low. Both are
different bottlenecks and need separate counters.

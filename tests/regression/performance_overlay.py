# Portions Copyright 2026 The Hollycast Authors
# Exercise unmodified sampling functions and GUI callback with a deterministic
# clock and counted host-stat adapters. This does not simulate a renderer/GPU.
from pathlib import Path
import argparse

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('output_directory', type=Path)
checks = parser.parse_args().output_directory.resolve()
checks.mkdir(parents=True, exist_ok=True)
overlay = (root / 'core/ui/performance_overlay.cpp').read_text()
gui = (root / 'core/ui/gui.cpp').read_text()
renderer = (root / 'core/hw/pvr/Renderer_if.cpp').read_text()

def function(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

shim = r'''
// Portions Copyright 2026 The Hollycast Authors
#include <cassert>
#include <iostream>
#include <map>
#include <string>
#ifndef _WIN32
#include <time.h>
#endif
#include "oslib/cpu_usage.h"
#include "oslib/process_memory.h"
#include "oslib/gpu_memory.h"
using u64 = std::uint64_t;
static u64 clockMs = 1000;
static int cpuQueries = 0, memoryQueries = 0, vramQueries = 0;
static std::uint32_t auditItems = 0;
static int threadClockReads = 0, failThreadClockRead = 0;
static u64 threadClockMicroseconds = 1000, threadClockStep = 500;
int testClockGettime(clockid_t clock, timespec* value) {
    assert(clock == CLOCK_THREAD_CPUTIME_ID);
    if (++threadClockReads == failThreadClockRead) return -1;
    value->tv_sec = threadClockMicroseconds / 1000000;
    value->tv_nsec = (threadClockMicroseconds % 1000000) * 1000;
    threadClockMicroseconds += threadClockStep;
    return 0;
}
u64 getTimeMs() { return clockMs; }
u64 getTextureUploadCount() { return 0; }
float spg_getSh4ExecutionRate() { return 100; }
namespace cpu_usage { Stats queryCurrentProcessAndSystem() { ++cpuQueries; return {12, 25, 8, true, true}; } }
namespace process_memory { Stats queryCurrentProcess() { ++memoryQueries; return {1024, true}; } }
namespace gpu_memory { Stats queryCurrentProcess() { ++vramQueries; return {}; } }
namespace i18n { const char* T(const char* p) { return p; } }
namespace config { bool IntegerScale = false; bool ThreadedRendering = true; }
namespace config {
static std::map<std::string, bool> boolOverrides;
bool loadBool(const char*, const char* key, bool defaultValue) {
    auto it = boolOverrides.find(key);
    return it != boolOverrides.end() ? it->second : defaultValue;
}
}
struct Settings {
    struct { int width = 1920, height = 1080; } display;
    struct { int system = 0; } platform;
    struct { std::string gameId, path; } content;
} settings;
constexpr int DC_PLATFORM_DREAMCAST = 0;
enum class Event { Resume, Start, Terminate, Pause, VBlank };
bool game_started = false;
struct Boxart { void resumePlaytime() {} void startPlaytime(const std::string&, const std::string&) {} void checkpointPlaytime(bool) {} } boxart;
struct GamepadDevice { static void load_system_mappings() {} };
namespace vgamepad { void startGame() {} }
void markLibraryGameBooted(const std::string&, const std::string&) {}
'''
stdincludes = overlay[overlay.index('#include <algorithm>'):overlay.index('namespace performance_overlay')]
declarations = overlay[overlay.index('namespace performance_overlay'):overlay.index('std::uint32_t visibleItems')]
pieces = [shim, stdincludes, declarations,
          function(overlay, 'void updateMetrics('),
          function(overlay, 'std::string formatResolution('),
          function(overlay, 'std::string formatThreadWork('),
          function(overlay, 'std::string formatPvrWork('),
          '} // anonymous namespace',
          'int mode() { return 2; }',
          'std::uint32_t visibleItems(int) { return auditItems; }',
          function(overlay, 'std::uint32_t visibleItems(').replace('visibleItems', 'realVisibleItems', 1),
          function(overlay, 'bool currentThreadCpuTimeMicroseconds(').replace('clock_gettime(', 'testClockGettime(')]
for signature in ['bool isCollectionEnabled(', 'bool isPvrWorkRequested(',
                  'bool isRendererStatsRequested(', 'bool isResolutionRequested(',
                  'bool isGpuTimingRequested(', 'std::uint32_t getGpuTimingEpoch(',
                  'void recordGpuTimingSample(', 'void recordPresentedFrame(',
                  'void recordRenderedResolution(', 'void recordOutputViewport(', 'void reset(',
                  'void recordSh4Vblank(', 'void recordPvrWork(', 'void recordPvrScene(',
                  'void recordRendererPass(', 'void recordPvrRenderSchedule(',
                  'void recordPvrFrameBudget(']:
    # Keep presentation intervals deterministic without sleeping between frames.
    pieces.append(function(overlay, signature).replace(
        'std::chrono::steady_clock::now()',
        'std::chrono::steady_clock::time_point(std::chrono::milliseconds(clockMs))'))
draw = overlay[overlay.index('void draw()'):overlay.index('\n\t// Most readings change once a second.')]
pieces += [draw.replace('void draw()', 'void drawSample()', 1) + '\n}',
           '} // performance_overlay', function(gui, 'static void emuEventCallback(')]
pieces += [r'''
// Exercise the actual dispatch/timing boundary with injected OS-clock failures.
struct TestRenderQueue {
    enum Type { Render, RenderFramebuffer, Present, Stop, NoMessage };
    struct Message { Type type; int config = 0; };
    int dispatched = 0;
    void render() { ++dispatched; }
    void renderFramebuffer(int) { ++dispatched; }
    void present() { ++dispatched; }
''', function(renderer, 'bool execute(Message msg)'), '};']
pieces.append(r'''
int main() {
    using namespace performance_overlay;
    auditItems = Fps | Speed | HostCpu | Ram | Vram | PvrWork | FrameTimes | RendererStats;
    emuEventCallback(Event::Resume, nullptr);
    drawSample();
    for (int frame = 0; frame < 180; ++frame) {
        clockMs += 17;
        recordSh4Vblank();
        emuEventCallback(Event::VBlank, nullptr);
        recordPvrScene(true, 12, 36);
        recordPvrWork(2000);
        drawSample();
    }
    assert(cpuQueries == 4 && memoryQueries == 4 && vramQueries == 4);
    assert(metrics.gameFps > 58 && metrics.gameFps < 60 && metrics.hostProcessCpuHistory.count == 3);
    std::cout << "PASS VBlank lifecycle: " << cpuQueries << " host queries per collector, FPS=" << metrics.gameFps << ", CPU graph samples=" << metrics.hostProcessCpuHistory.count << '\n';
    for (auto event : {Event::Pause, Event::Resume, Event::Terminate, Event::Start}) {
        emuEventCallback(event, nullptr);
        assert(!isCollectionEnabled());
        drawSample();
        assert(metrics.gameFps == 0 && metrics.hostProcessCpuHistory.count == 0);
    }
    std::cout << "PASS pause/resume/start/terminate reset baselines and history\n";
    auditItems = Ram;
    drawSample();
    recordPvrScene(true, 12, 36);
    recordRendererPass(2);
    recordPvrRenderSchedule(1000);
    recordPvrFrameBudget(2000);
    recordSh4Vblank(); recordOutputViewport(1440, 1080); recordRenderedResolution(640, 480, 640, 480);
    recordPvrWork(1000); recordPresentedFrame();
    assert(pvrPrimitiveCount == 0 && rendererPassCount == 0 && pvrScheduledRenderCycles == 0);
    assert(vblankCount == 0 && rawRenderWidth == 0 && outputViewportWidth == 0 && gameFrameCount == 0);
    assert(pvrWorkMicroseconds == 0 && presentSamples.presentCount == 0);
    std::cout << "PASS RAM-only skips unrelated producers\n";
    auditItems = Fps | PvrPressure | RendererStats | Resolution;
    drawSample();
    recordSh4Vblank(); recordPvrScene(true, 12, 36); recordPvrScene(false, 4, 12);
    recordRendererPass(2); recordPvrRenderSchedule(1000); recordPvrFrameBudget(2000);
    recordOutputViewport(1440, 1080); recordRenderedResolution(640, 480, 640, 480);
    assert(vblankCount == 1 && gameFrameCount == 1 && rendererSceneCount == 2);
    assert(pvrPrimitiveCount == 16 && pvrVertexCount == 48 && rendererPassCount == 2);
    assert(pvrScheduledRenderCycles == 1000 && pvrVideoFrameCycles == 2000);
    assert(rawRenderWidth == 640 && outputViewportWidth == 1440);
    std::cout << "PASS requested producers and RTT-excluded game FPS\n";
    auditItems = PvrWork;
    drawSample();
    recordPvrWork(5000);
    clockMs += 1000;
    drawSample();
    assert(metrics.vps == 0 && metrics.pvrTimePerVblank == 0 && !metrics.pvrTimePerVblankValid);
    assert(formatPvrWork().find("no video frame") != std::string::npos);
    std::cout << "PASS no fabricated per-VBlank time: " << formatPvrWork() << '\n';
    renderedWidth = 640; renderedHeight = 480; rawRenderWidth = 640; rawRenderHeight = 480;
    outputViewportWidth = 1440; outputViewportHeight = 1080;
    auto text = formatResolution();
    assert(text.find("Viewport") != std::string::npos);
    std::cout << "PASS actual viewport without integer scaling: " << text << '\n';
    auditItems = HostGpu;
    drawSample();
    auto epoch = getGpuTimingEpoch();
    recordGpuTimingSample(1000000, epoch);
    assert(gpuTimingSampleCount == 1);
    auditItems = 0;
    drawSample();
    auto queries = cpuQueries + memoryQueries + vramQueries;
    for (int i = 0; i < 300; ++i) { ++clockMs; recordSh4Vblank(); recordPvrScene(true, 8, 8); recordGpuTimingSample(100, epoch); drawSample(); }
    assert(queries == cpuQueries + memoryQueries + vramQueries && gpuTimingSampleCount == 0);
    auditItems = HostGpu;
    drawSample();
    recordGpuTimingSample(1000000, epoch);
    assert(gpuTimingSampleCount == 0 && getGpuTimingEpoch() != epoch);
    recordGpuTimingSample(1000000, getGpuTimingEpoch());
    assert(gpuTimingSampleCount == 1);
    std::cout << "PASS Off skips sampling and old GPU epochs are rejected after re-enable\n";
    config::boolOverrides["OverlayShowFrameTimes"] = false;
    config::boolOverrides["OverlayShowFrameTimeGraph"] = true;
    assert((realVisibleItems(2) & FrameTimeGraph) != 0);
    auditItems = FrameTimeGraph;
    drawSample();
    {
        for (int frame = 0; frame < 3; ++frame) {
            clockMs += 17;
            recordPresentedFrame();
        }
        assert(presentSamples.presentCount == 3 && presentSamples.intervalHistoryCount == 2);
        clockMs += 1000;
        drawSample();
        assert(metrics.frameTimeHistoryCount == 2 && metrics.frameTimeAverage == 17.f);
    }
    std::cout << "PASS frame-time graph collects without the frame times row\n";
    config::boolOverrides.clear();
    config::boolOverrides["OverlayShowLatencyStatistics"] = true;
    // Bit 13 is the retired Latency row: a stale saved setting must not resurrect it.
    assert((realVisibleItems(2) & (1u << 13)) == 0);
    std::cout << "PASS retired latency key sets no row\n";
    auditItems = PvrWork;
    drawSample();
    TestRenderQueue queue;
    for (int failure : {1, 2}) {
        threadClockReads = 0; failThreadClockRead = failure;
        assert(queue.execute({TestRenderQueue::Render}));
        assert(threadClockReads == failure && !pvrWorkSampled && pvrWorkMicroseconds == 0);
        recordSh4Vblank(); clockMs += 1000; drawSample();
        assert(!metrics.pvrThreadCpuValid && !metrics.pvrTimePerVblankValid);
        assert(formatPvrWork().find("sampling thread usage") != std::string::npos);
    }
    assert(queue.dispatched == 2);
    std::cout << "PASS failed start or end CPU-clock reads never display fabricated zero work\n";
    failThreadClockRead = 0; threadClockReads = 0; threadClockStep = 0;
    for (auto type : {TestRenderQueue::Render, TestRenderQueue::RenderFramebuffer, TestRenderQueue::Present})
        assert(queue.execute({type}));
    assert(!queue.execute({TestRenderQueue::Stop}) && !queue.execute({TestRenderQueue::NoMessage}));
    assert(threadClockReads == 6 && queue.dispatched == 5 && pvrWorkSampled && pvrWorkMicroseconds == 0);
    recordSh4Vblank(); clockMs += 1000; drawSample();
    assert(metrics.pvrThreadCpuValid && metrics.pvrTimePerVblankValid && metrics.pvrUsage == 0.f);
    assert(formatPvrWork().find("0.0%") != std::string::npos);
    threadClockStep = 500;
    assert(queue.execute({TestRenderQueue::Present}));
    recordSh4Vblank(); clockMs += 1000; drawSample();
    assert(metrics.pvrThreadCpuValid && metrics.pvrTimePerVblank == 0.5f);
    // A later interval with failed reads must not reuse the previous validity.
    threadClockReads = 0; failThreadClockRead = 1;
    assert(queue.execute({TestRenderQueue::Present}));
    clockMs += 1000; drawSample();
    assert(!metrics.pvrThreadCpuValid && formatPvrWork().find("sampling") != std::string::npos);
    auditItems = Ram; drawSample();
    threadClockReads = 0;
    assert(queue.execute({TestRenderQueue::Render}) && threadClockReads == 0);
    std::cout << "PASS successful zero CPU work stays valid, recovery works, and hidden timing skips clock reads\n";
}
''')
output = checks / 'performance_overlay.cpp'
output.write_text('\n'.join(pieces))
print('Generated:', output)

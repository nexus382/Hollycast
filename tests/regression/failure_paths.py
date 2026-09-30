# Portions Copyright 2026 The Hollycast Authors
# Actual GPU query functions and GL render scope with deterministic adapters.
from pathlib import Path
import argparse

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('output_directory', type=Path)
checks = parser.parse_args().output_directory.resolve()
checks.mkdir(parents=True, exist_ok=True)

def function(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

dx = (root / 'core/rend/dx11/dx11context.cpp').read_text()
vulkan = (root / 'core/rend/vulkan/vulkan_context.cpp').read_text()
gl = (root / 'core/rend/gles/gles.cpp').read_text()
gl4 = (root / 'core/rend/gl4/gles.cpp').read_text()
gl_header = (root / 'core/rend/gles/gles.h').read_text()
emulator = (root / 'core/emulator.cpp').read_text()
vulkan_pool = function(vulkan, 'if (gpuTimingAvailable)')
shim = r'''
// Portions Copyright 2026 The Hollycast Authors
#include <vulkan/vulkan.hpp>
#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <utility>
using u32 = uint32_t;
using u64 = uint64_t;
constexpr std::size_t GpuTimingQueryCount = 4;
#define WARN_LOG(category, ...) ((void)0)
#define ERROR_LOG(category, ...) ((void)0)
#define verify(condition) assert(condition)
#define FAILED(result) ((result) < 0)
static int liveQueries = 0;
struct Query { Query() { ++liveQueries; } ~Query() { --liveQueries; } };
struct QueryRef {
    Query* ptr = nullptr;
    QueryRef() = default;
    QueryRef(const QueryRef&) = delete;
    QueryRef& operator=(QueryRef&& other) { delete ptr; ptr = std::exchange(other.ptr, nullptr); return *this; }
    ~QueryRef() { delete ptr; }
    Query*& get() { return ptr; }
};
struct D3D11_QUERY_DESC { int Query = 0; };
constexpr int D3D11_QUERY_TIMESTAMP = 1, D3D11_QUERY_TIMESTAMP_DISJOINT = 2;
struct DxDevice {
    int calls = 0, failOn = 0;
    int CreateQuery(D3D11_QUERY_DESC*, Query** output) {
        if (++calls == failOn) return -1;
        *output = new Query(); return 0;
    }
};
struct DX11Context {
    struct GpuTimingQuery { QueryRef disjoint, start, end; };
    std::array<GpuTimingQuery,4> gpuTimingQueries;
    unsigned gpuTimingReadIndex = 0, gpuTimingWriteIndex = 0, gpuTimingWaitingCount = 0;
    bool gpuTimingActive = false, gpuTimingAvailable = false;
    DxDevice* pDevice = nullptr;
    bool initGpuTiming();
};
namespace performance_overlay {
static bool sh4Registered = false;
static int sh4Releases = 0;
void clearSh4ThreadSample() { sh4Registered = false; ++sh4Releases; }
static int timingSamples = 0;
static std::uint64_t lastNanoseconds = 0;
static std::uint32_t lastEpoch = 0;
void recordGpuTimingSample(std::uint64_t nanoseconds, std::uint32_t epoch) {
    ++timingSamples; lastNanoseconds = nanoseconds; lastEpoch = epoch;
}
void setGpuTimingSupported(bool) {}
bool isGpuTimingRequested() { return true; }
std::uint32_t getGpuTimingEpoch() { return 7; }
}
// Minimal adapters matching only the members pollGpuTiming() touches, so the
// real function body is exercised with a deterministic fence/query device.
struct FakeFence {};
struct FakePool {};
struct CommandCounts { int resets = 0, writes = 0; };
struct FakeCommandBuffer {
    CommandCounts* counts = nullptr;
    explicit operator bool() const { return counts != nullptr; }
    bool operator!=(const FakeCommandBuffer& other) const { return counts != other.counts; }
    FakeCommandBuffer& operator=(std::nullptr_t) { counts = nullptr; return *this; }
    void resetQueryPool(FakePool&, std::uint32_t, std::uint32_t) { ++counts->resets; }
    void writeTimestamp(vk::PipelineStageFlagBits, FakePool&, std::uint32_t) { ++counts->writes; }
};
struct FakeDevice {
    vk::Result fenceStatus = vk::Result::eSuccess;
    vk::Result queryResult = vk::Result::eSuccess;
    std::uint64_t readyStamps[2] = {100, 200};
    vk::Result getFenceStatus(FakeFence&) { return fenceStatus; }
    vk::Result getQueryPoolResults(FakePool&, std::uint32_t, std::uint32_t, std::size_t,
            std::uint64_t* data, std::size_t, vk::QueryResultFlags) {
        if (queryResult == vk::Result::eSuccess) { data[0] = readyStamps[0]; data[1] = readyStamps[1]; }
        return queryResult;
    }
};
struct VulkanContext {
    FakeDevice* device = nullptr;
    bool gpuTimingAvailable = false;
    FakeCommandBuffer gpuTimingCommandBuffer;
    FakePool* gpuTimingQueryPool = nullptr;
    std::size_t gpuTimingReadIndex = 0;
    std::size_t gpuTimingWriteIndex = 0;
    std::size_t gpuTimingWaitingCount = 0;
    std::array<int, 4> gpuTimingFenceIndices { -1, -1, -1, -1 };
    std::array<std::uint32_t, 4> gpuTimingEpochs {};
    std::array<FakeFence*, 4> drawFences {};
    u64 gpuTimestampMask = ~u64(0);
    double gpuTimestampPeriod = 1.0;
    void pollGpuTiming();
    void BeginGpuTiming(FakeCommandBuffer);
    void EndGpuTiming(FakeCommandBuffer);
    void ResetGpuTiming();
};
struct Pool { bool allocated = false; void reset() { allocated = false; } };
struct TimestampDevice {
    vk::Result failure = vk::Result::eSuccess;
    Pool createQueryPoolUnique(const vk::QueryPoolCreateInfo&) {
        if (failure != vk::Result::eSuccess) throw vk::SystemError(vk::make_error_code(failure), "test allocation");
        return {true};
    }
};
namespace config { bool EmulateFramebuffer = false; }
struct RenderContext { bool isRTT = false; int framebufferWidth = 640, framebufferHeight = 480; } renderContext;
struct GlState { RenderContext* rendContext = &renderContext; } gl;
// Only the work inside Render() is mocked. The real render function and scope
// must close timing at all three throwing call sites and before FBO restoration.
struct OpenGLRenderer {
    friend class OpenGLGpuTimingScope;
    bool frameRendered = false, clearLastFrame = true, timingActive = false;
    int begins = 0, ends = 0, restored = 0, failAt = 0;
    bool Render();
    void saveCurrentFramebuffer() {}
    void restoreCurrentFramebuffer() { assert(!timingActive); ++restored; }
    void renderFrame(int, int) { if (failAt == 1) throw std::runtime_error("render"); }
    void drawOSD() { if (failAt == 2) throw std::runtime_error("OSD"); }
    void renderVideoRouting() { if (failAt == 3) throw std::runtime_error("routing"); }
protected:
    void beginGpuTiming() { assert(!timingActive); timingActive = true; ++begins; }
    void endGpuTiming() { assert(timingActive); timingActive = false; ++ends; }
};
struct OpenGL4Renderer : OpenGLRenderer { bool Render(); };
static bool failNextFrame = false;
namespace ggpo {
bool active() { return true; }
bool nextFrame() {
    if (failNextFrame) throw std::runtime_error("nextFrame");
    return true;
}
}
int sh4_sched_now64() { return 0; }
void setNetworkState(bool) {}
enum class Event { Pause };
struct EventManager {
    static void event(Event) { assert(!performance_overlay::sh4Registered); }
};
struct Emulator {
    enum { Running, Error } state = Running;
    int startTime = 0, stepRangeTo = 0;
    bool singleStep = false, failRun = false;
    struct Executor { void Start() {} void Stop() {} } executor;
    Executor* getSh4Executor() { return &executor; }
    void runInternal() {
        performance_overlay::sh4Registered = true;
        if (failRun) throw std::runtime_error("runInternal");
    }
    void run();
};
template<typename Renderer>
void testRenderScope() {
    config::EmulateFramebuffer = false;
    for (int failure = 0; failure <= 3; ++failure) {
        Renderer renderer; renderer.failAt = failure;
        bool threw = false;
        try { assert(renderer.Render()); }
        catch (const std::runtime_error&) { threw = true; }
        assert(threw == (failure != 0) && !renderer.timingActive);
        assert(renderer.begins == 1 && renderer.ends == 1 && renderer.restored == (failure == 0));
        renderer.failAt = 0;
        assert(renderer.Render() && renderer.begins == 2 && renderer.ends == 2);
    }
    Renderer renderer;
    renderContext.isRTT = true;
    assert(!renderer.Render() && renderer.begins == 0 && renderer.ends == 0 && renderer.restored == 1);
    renderContext.isRTT = false;
    config::EmulateFramebuffer = true;
    renderer.failAt = 2;
    assert(renderer.Render() && renderer.begins == 1 && renderer.ends == 1 && renderer.restored == 2);
}
'''
tests = r'''
int main() {
    {
        DX11Context context; DxDevice device; context.pDevice = &device;
        assert(context.initGpuTiming() && liveQueries == 12);
        for (int fail = 1; fail <= 12; ++fail) {
            device.calls = 0; device.failOn = fail;
            assert(!context.initGpuTiming() && !context.gpuTimingAvailable && liveQueries == 0);
        }
        device.calls = 0; device.failOn = 0;
        assert(context.initGpuTiming() && liveQueries == 12);
        context.pDevice = nullptr; assert(!context.initGpuTiming() && liveQueries == 0);
    }
    std::cout << "PASS DX11 releases partial rings at all 12 allocation failure points\n";
    for (auto result : {vk::Result::eSuccess, vk::Result::eErrorOutOfHostMemory, vk::Result::eErrorOutOfDeviceMemory, vk::Result::eErrorDeviceLost}) {
        TimestampDevice dev; dev.failure = result; auto device = &dev;
        bool gpuTimingAvailable = true; Pool gpuTimingQueryPool;
        constexpr size_t GpuTimingQueryCount = 4;
        bool threw = false;
        try { VULKAN_POOL }
        catch (const vk::SystemError&) { threw = true; }
        if (result == vk::Result::eSuccess) assert(!threw && gpuTimingAvailable && gpuTimingQueryPool.allocated);
        else if (result == vk::Result::eErrorDeviceLost) assert(threw);
        else assert(!threw && !gpuTimingAvailable && !gpuTimingQueryPool.allocated);
    }
    std::cout << "PASS Vulkan optional allocation errors disable timing; device loss propagates\n";
    {
        FakeDevice dev; FakePool pool; FakeFence fence;
        VulkanContext context;
        context.device = &dev;
        context.gpuTimingAvailable = true;
        context.gpuTimingQueryPool = &pool;
        context.drawFences.fill(&fence);
        context.gpuTimingWaitingCount = 1;
        context.gpuTimingReadIndex = 0;
        context.gpuTimingFenceIndices[0] = 0;
        context.gpuTimingEpochs[0] = 7;
        dev.fenceStatus = vk::Result::eNotReady;
        context.pollGpuTiming();
        assert(context.gpuTimingWaitingCount == 1 && context.gpuTimingReadIndex == 0);
        assert(context.gpuTimingFenceIndices[0] == 0 && performance_overlay::timingSamples == 0);
        dev.fenceStatus = vk::Result::eSuccess;
        dev.queryResult = vk::Result::eNotReady;
        context.pollGpuTiming();
        assert(context.gpuTimingWaitingCount == 1 && context.gpuTimingReadIndex == 0);
        assert(context.gpuTimingFenceIndices[0] == 0 && performance_overlay::timingSamples == 0);
        dev.queryResult = vk::Result::eSuccess;
        context.pollGpuTiming();
        assert(context.gpuTimingWaitingCount == 0 && context.gpuTimingReadIndex == 1);
        assert(context.gpuTimingFenceIndices[0] == -1 && performance_overlay::timingSamples == 1);
        assert(performance_overlay::lastNanoseconds == 100 && performance_overlay::lastEpoch == 7);
    }
    std::cout << "PASS Vulkan keeps not-ready slots queued and retires completed samples\n";
    {
        FakePool pool; CommandCounts counts; FakeCommandBuffer commands {&counts};
        VulkanContext context;
        context.gpuTimingAvailable = true; context.gpuTimingQueryPool = &pool;
        context.BeginGpuTiming(commands);
        assert(context.gpuTimingCommandBuffer && counts.resets == 1 && counts.writes == 1);
        context.BeginGpuTiming(commands);
        assert(context.gpuTimingCommandBuffer && counts.resets == 1 && counts.writes == 1);
        // A framebuffer upload must not close a screen pass's active query.
        CommandCounts uploadCounts; FakeCommandBuffer upload {&uploadCounts};
        context.BeginGpuTiming(upload);
        context.EndGpuTiming(upload);
        assert(context.gpuTimingCommandBuffer && context.gpuTimingWaitingCount == 0);
        assert(uploadCounts.resets == 0 && uploadCounts.writes == 0 && counts.writes == 1);
        // Teardown calls this after WaitIdle and discards the abandoned buffers.
        context.ResetGpuTiming();
        assert(!context.gpuTimingCommandBuffer && context.gpuTimingWaitingCount == 0);
        context.BeginGpuTiming(commands);
        context.EndGpuTiming(commands);
        assert(!context.gpuTimingCommandBuffer && counts.resets == 2 && counts.writes == 3);
        assert(context.gpuTimingWaitingCount == 1 && context.gpuTimingWriteIndex == 1);
        assert(context.gpuTimingEpochs[0] == 7 && context.gpuTimingFenceIndices[0] == -1);
        context.ResetGpuTiming();
        assert(context.gpuTimingWaitingCount == 0 && context.gpuTimingWriteIndex == 0);
        assert(context.gpuTimingReadIndex == 0 && context.gpuTimingEpochs[0] == 0);
    }
    std::cout << "PASS Vulkan retains active-pair ownership until teardown reset and then resumes sampling\n";
    testRenderScope<OpenGLRenderer>();
    std::cout << "PASS GL closes queries on all render failures, preserves timing boundaries, and excludes RTT\n";
    testRenderScope<OpenGL4Renderer>();
    std::cout << "PASS GL4 closes queries on all render failures, preserves timing boundaries, and excludes RTT\n";
    {
        Emulator emulator;
        emulator.run(); emulator.run();
        assert(performance_overlay::sh4Registered && performance_overlay::sh4Releases == 0);
        for (int failure = 0; failure < 2; ++failure) {
            emulator.state = Emulator::Running;
            emulator.failRun = failure == 0;
            failNextFrame = failure == 1;
            bool threw = false;
            try { emulator.run(); }
            catch (const std::runtime_error&) { threw = true; }
            assert(threw && emulator.state == Emulator::Error && !performance_overlay::sh4Registered);
            assert(performance_overlay::sh4Releases == failure + 1);
        }
    }
    std::cout << "PASS non-threaded errors release SH4 sampling before pause; normal frames retain registration\n";
}
'''
gpu_functions = '\n'.join(function(vulkan, signature).replace('vk::CommandBuffer', 'FakeCommandBuffer')
                          for signature in ['void VulkanContext::pollGpuTiming()',
                                            'void VulkanContext::BeginGpuTiming(',
                                            'void VulkanContext::EndGpuTiming(',
                                            'void VulkanContext::ResetGpuTiming()'])
output = checks / 'failure_paths.cpp'
output.write_text(shim + '\n' + function(dx, 'bool DX11Context::initGpuTiming()') + '\n' + gpu_functions + '\n'
                  + function(gl_header, 'class OpenGLGpuTimingScope\n') + ';\n'
                  + function(gl, 'bool OpenGLRenderer::Render()') + '\n'
                  + function(gl4, 'bool OpenGL4Renderer::Render()') + '\n'
                  + function(emulator, 'void Emulator::run()') + '\n'
                  + tests.replace('VULKAN_POOL', vulkan_pool))
print('Generated:', output)

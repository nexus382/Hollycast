# Portions Copyright 2026 The Hollycast Authors
# Actual GPU query allocation functions with deterministic device adapters.
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
vulkan_pool = function(vulkan, 'if (gpuTimingAvailable)')
shim = r'''
// Portions Copyright 2026 The Hollycast Authors
#include <vulkan/vulkan.hpp>
#include <array>
#include <cassert>
#include <iostream>
#include <utility>
using u32 = uint32_t;
#define WARN_LOG(category, ...) ((void)0)
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
struct Pool { bool allocated = false; void reset() { allocated = false; } };
struct TimestampDevice {
    vk::Result failure = vk::Result::eSuccess;
    Pool createQueryPoolUnique(const vk::QueryPoolCreateInfo&) {
        if (failure != vk::Result::eSuccess) throw vk::SystemError(vk::make_error_code(failure), "test allocation");
        return {true};
    }
};
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
}
'''
output = checks / 'failure_paths.cpp'
output.write_text(shim + '\n' + function(dx, 'bool DX11Context::initGpuTiming()') + '\n'
                  + tests.replace('VULKAN_POOL', vulkan_pool))
print('Generated:', output)

# Portions Copyright 2026 The Hollycast Authors
"""Exercise actual direct-framebuffer entry points with deterministic GPU adapters."""
from pathlib import Path
import argparse

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('output_directory', type=Path)
output_directory = parser.parse_args().output_directory.resolve()
output_directory.mkdir(parents=True, exist_ok=True)

def function(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

gl = (root / 'core/rend/gles/gldraw.cpp').read_text()
gl_header = (root / 'core/rend/gles/gles.h').read_text()
vulkan = (root / 'core/rend/vulkan/vulkan_renderer.cpp').read_text()

shim = r'''
// Portions Copyright 2026 The Hollycast Authors
#include <vulkan/vulkan.hpp>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>
using u8 = std::uint8_t;
using u32 = std::uint32_t;
#define ERROR_LOG(category, ...) ((void)0)
static bool glActive = false, vkActive = false;
static int failAt = 0, dimensionReports = 0;
static int rawWidth = 0, rawHeight = 0, renderedWidth = 0, renderedHeight = 0;
namespace performance_overlay {
void recordRenderedResolution(int rw, int rh, int w, int h) {
    ++dimensionReports; rawWidth = rw; rawHeight = rh; renderedWidth = w; renderedHeight = h;
}
}
struct FramebufferInfo {
    struct { int fb_enable = 1; } fb_r_ctrl;
    struct { int blank_video = 0; } vo_control;
    struct {
        u8 _red = 1, _green = 2, _blue = 3;
        float red() const { return 0; }
        float green() const { return 0; }
        float blue() const { return 0; }
    } vo_border_col;
    int decodedWidth = 640, decodedHeight = 240;
};
template<typename T> struct PixelBuffer { T pixel {}; T* data() { return &pixel; } };
void ReadFramebuffer(const FramebufferInfo& info, PixelBuffer<u32>&, int& w, int& h) {
    // Only decoding is mocked. It must precede GL query submission.
    assert(!glActive);
    if (failAt == 5) throw std::runtime_error("decode");
    w = info.decodedWidth; h = info.decodedHeight;
}
struct GlFramebuffer {
    int width, height;
    GlFramebuffer(int w, int h, bool, bool) : width(w), height(h) {}
    int getWidth() const { return width; }
    int getHeight() const { return height; }
    void bind() {}
};
struct Quad { void draw(int, bool, bool) { assert(glActive); } } quad;
struct {
    void* rendContext = nullptr;
    PixelBuffer<u32> framebufferBuffer;
    struct { int width = 0, height = 0, tex = 1; } dcfb;
    struct { int shiftX = 0, shiftY = 0; float aspectRatio = 0; } ofbo;
    struct { std::unique_ptr<GlFramebuffer> framebuffer; bool ready = false; } ofbo2;
    Quad* quadDrawer = &quad;
} gl;
struct {
    void Disable(int) {}
    void ClearColor(float, float, float, float) {}
} glcache;
constexpr int GL_SCISSOR_TEST = 1, GL_COLOR_BUFFER_BIT = 2, GL_BLEND = 3;
void glUploadFramebuffer() {
    assert(glActive);
    if (failAt == 1) throw std::runtime_error("upload");
}
void initVideoRoutingFrameBuffer() {}
void getVideoShift(int&, int&) {}
void glCheck() {}
void glViewport(int, int, int w, int h) { assert(w == gl.dcfb.width && h == gl.dcfb.height); }
void glClear(int) { assert(glActive); }
float getDCFramebufferAspectRatio() { return 4.f / 3.f; }
struct OpenGLRenderer {
    friend class OpenGLGpuTimingScope;
    int width = 0, height = 0, begins = 0, ends = 0;
    bool frameRendered = false, clearLastFrame = true;
    void RenderFramebuffer(const FramebufferInfo&);
    void saveCurrentFramebuffer() {}
    void restoreCurrentFramebuffer() {}
    void renderLastFrame() { if (failAt == 2) throw std::runtime_error("blit"); }
    void drawOSD() { if (failAt == 3) throw std::runtime_error("OSD"); }
    void renderVideoRouting() { if (failAt == 4) throw std::runtime_error("routing"); }
protected:
    void beginGpuTiming() { assert(!glActive); glActive = true; ++begins; }
    void endGpuTiming() { assert(glActive); glActive = false; ++ends; }
};
// Vulkan's real query ownership/ring functions are covered by failure_paths.py.
// Here adapters verify the actual upload/submit path's timing and error order.
static bool commandEnded = false;
struct FakeCommandBuffer {
    void begin(const vk::CommandBufferBeginInfo&) { commandEnded = false; }
    void end() { assert(!vkActive); commandEnded = true; }
};
struct CommandBufferDebugScope { CommandBufferDebugScope(FakeCommandBuffer, const char*, const float*) {} };
struct RendererException : std::runtime_error { using std::runtime_error::runtime_error; };
enum class TextureType { _8888 };
struct Texture {
    TextureType tex_type;
    vk::Extent2D size;
    void SetCommandBuffer(FakeCommandBuffer) {}
    void SetCommandBuffer(std::nullptr_t) {}
    void UploadToGPU(int w, int h, const u8*, bool) {
        assert(vkActive);
        if (failAt == 1)
            throw vk::SystemError(vk::make_error_code(vk::Result::eErrorDeviceLost), "upload");
        if (failAt == 2)
            throw vk::OutOfDeviceMemoryError("upload");
        size = vk::Extent2D(w, h);
    }
    vk::Extent2D getSize() const { return size; }
};
struct VulkanContext {
    int begins = 0, ends = 0;
    unsigned GetSwapChainSize() { return 2; }
    void BeginGpuTiming(FakeCommandBuffer) { assert(!vkActive); vkActive = true; ++begins; }
    void EndGpuTiming(FakeCommandBuffer) { assert(vkActive); vkActive = false; ++ends; }
};
struct CommandPool {
    bool submitted = false;
    void BeginFrame() { submitted = false; }
    FakeCommandBuffer Allocate() { return {}; }
    void EndFrame() {
        assert(commandEnded && !vkActive);
        if (failAt == 3)
            throw vk::SystemError(vk::make_error_code(vk::Result::eErrorDeviceLost), "submit");
        submitted = true;
    }
};
struct BaseVulkanRenderer {
    void* rendContext = nullptr;
    int framebufferTexIndex = 0;
    bool framebufferRendered = false, clearLastFrame = true;
    std::vector<std::unique_ptr<Texture>> framebufferTextures;
    CommandPool fbCommandPool;
    VulkanContext context;
    VulkanContext* GetContext() { return &context; }
    void RenderFramebuffer(const FramebufferInfo&);
};
'''

tests = r'''
int main() {
    FramebufferInfo info;
    OpenGLRenderer renderer;
    for (int height : {240, 480}) {
        info.decodedHeight = height;
        renderer.RenderFramebuffer(info);
        assert(!glActive && renderer.frameRendered && !renderer.clearLastFrame);
        assert(rawWidth == 640 && rawHeight == height && renderedWidth == 640 && renderedHeight == height);
    }
    info.vo_control.blank_video = 1;
    renderer.RenderFramebuffer(info);
    assert(rawWidth == 640 && renderedHeight == 480); // GL clears its full-sized target.
    info.vo_control.blank_video = 0;
    for (int failure = 1; failure <= 5; ++failure) {
        failAt = failure;
        OpenGLRenderer failed;
        int reportsBefore = dimensionReports;
        bool threw = false;
        try { failed.RenderFramebuffer(info); }
        catch (const std::runtime_error&) { threw = true; }
        assert(threw && !glActive);
        assert(failed.begins == (failure != 5) && failed.ends == failed.begins);
        assert(dimensionReports == reportsBefore);
    }
    std::cout << "PASS GL framebuffer dimensions, CPU decode before timing, and query closure on upload/blit/OSD failures\n";
    failAt = 0;
    BaseVulkanRenderer vkRenderer;
    for (int height : {240, 480}) {
        info.decodedHeight = height;
        vkRenderer.RenderFramebuffer(info);
        assert(!vkActive && vkRenderer.fbCommandPool.submitted && vkRenderer.framebufferRendered);
        assert(rawWidth == 640 && rawHeight == height && renderedWidth == 640 && renderedHeight == height);
    }
    info.vo_control.blank_video = 1;
    vkRenderer.RenderFramebuffer(info);
    assert(rawWidth == 1 && rawHeight == 1 && renderedWidth == 1 && renderedHeight == 1);
    info.vo_control.blank_video = 0; info.fb_r_ctrl.fb_enable = 0;
    vkRenderer.RenderFramebuffer(info);
    assert(rawWidth == 1 && renderedHeight == 1);
    info.fb_r_ctrl.fb_enable = 1;
    for (int failure = 1; failure <= 3; ++failure) {
        failAt = failure;
        BaseVulkanRenderer failed;
        int reportsBefore = dimensionReports;
        bool threw = false;
        try { failed.RenderFramebuffer(info); }
        catch (const RendererException&) { threw = true; }
        assert(threw && !failed.fbCommandPool.submitted && !failed.framebufferRendered);
        assert(failed.context.begins == 1 && failed.context.ends == (failure == 3));
        assert(dimensionReports == reportsBefore);
        vkActive = false; // Simulate renderer teardown; never synthesize commands on unwind.
    }
    std::cout << "PASS Vulkan framebuffer dimensions, blank/disabled video, timing before submit, and renderer-error recovery\n";
}
'''

output = output_directory / 'framebuffer_paths.cpp'
output.write_text(shim + function(gl_header, 'class OpenGLGpuTimingScope\n') + ';\n'
                  + function(gl, 'void OpenGLRenderer::RenderFramebuffer(') + '\n'
                  + function(vulkan, 'void BaseVulkanRenderer::RenderFramebuffer(').replace('vk::CommandBuffer ', 'FakeCommandBuffer ')
                  + tests)
print('Generated:', output)

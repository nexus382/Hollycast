# Portions Copyright 2026 The Hollycast Authors
"""Run production DirectX allocation decisions against counted API adapters.

This exercises the actual C++ method bodies; it is not a Windows GPU test.
Use --baseline-oit to prove the steady-size regression fails against HEAD.
"""
import argparse
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('output_directory', type=Path)
parser.add_argument('--baseline-oit', nargs='?', const='HEAD', metavar='REF')
args = parser.parse_args()
output = args.output_directory.resolve()
output.mkdir(parents=True, exist_ok=True)


def read(path):
    if args.baseline_oit:
        return subprocess.check_output(['git', 'show', f'{args.baseline_oit}:{path}'], cwd=root, text=True)
    return (root / path).read_text()


def function(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    end, depth = brace + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


renderer = read('core/rend/dx11/dx11_renderer.cpp')
oit = read('core/rend/dx11/oit/dx11_oitrenderer.cpp')
dx9 = read('core/rend/dx9/d3d_renderer.cpp')
pieces = [r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <utility>
using u32 = unsigned;
using UINT = unsigned;
using FLOAT = float;
using HRESULT = int;
using DXGI_FORMAT = int;
using D3DFORMAT = int;
constexpr HRESULT S_OK = 0;
#define FAILED(hr) ((hr) < 0)
#define WARN_LOG(...) ((void)0)
#define IID_PPV_ARGS(p) p
inline constexpr int DXGI_FORMAT_R32G8X24_TYPELESS = 1, DXGI_FORMAT_D32_FLOAT_S8X24_UINT = 2,
    DXGI_FORMAT_D24_UNORM_S8_UINT = 3, DXGI_FORMAT_B8G8R8A8_UNORM = 4,
    DXGI_FORMAT_X32_TYPELESS_G8X24_UINT = 5, DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS = 6,
    D3D11_USAGE_DEFAULT = 0, D3D11_BIND_DEPTH_STENCIL = 1,
    D3D11_BIND_SHADER_RESOURCE = 2, D3D11_BIND_RENDER_TARGET = 4,
    D3D11_DSV_DIMENSION_TEXTURE2D = 0, D3D11_SRV_DIMENSION_TEXTURE2D = 0,
    D3DPOOL_SYSTEMMEM = 0, D3D11_MAP_WRITE_DISCARD = 0;
struct D3D11_TEXTURE2D_DESC {
    UINT Width = 0, Height = 0, MipLevels = 1, ArraySize = 1;
    DXGI_FORMAT Format = 0;
    struct { UINT Count = 1, Quality = 0; } SampleDesc;
    UINT Usage = 0, BindFlags = 0, CPUAccessFlags = 0, MiscFlags = 0;
};
struct D3D11_DEPTH_STENCIL_VIEW_DESC { int Format, ViewDimension; };
struct D3D11_SHADER_RESOURCE_VIEW_DESC {
    int Format = 0, ViewDimension = 0;
    struct { UINT MipLevels = 1; } Texture2D;
};
struct D3DSURFACE_DESC { UINT Width, Height; D3DFORMAT Format; };
struct Resource {
    int refs = 1;
    virtual ~Resource() = default;
    void AddRef() { ++refs; }
    void Release() { if (--refs == 0) delete this; }
};
struct ID3D11Texture2D : Resource {
    D3D11_TEXTURE2D_DESC desc;
    void GetDesc(D3D11_TEXTURE2D_DESC* out) { *out = desc; }
};
using ID3D11DepthStencilView = Resource;
using ID3D11ShaderResourceView = Resource;
using ID3D11RenderTargetView = Resource;
struct IDirect3DSurface9 : Resource {
    D3DSURFACE_DESC desc;
    HRESULT GetDesc(D3DSURFACE_DESC* out) { *out = desc; return S_OK; }
};
''', (root / 'core/windows/comptr.h').read_text(), r'''
struct Device {
    int textures = 0, depthTextures = 0, depthViews = 0, surfaces = 0;
    bool fail = false;
    HRESULT CreateTexture2D(const D3D11_TEXTURE2D_DESC* desc, void*, ID3D11Texture2D** out) {
        ++textures;
        if (fail) return -1;
        if (desc->BindFlags & D3D11_BIND_DEPTH_STENCIL) ++depthTextures;
        auto p = new ID3D11Texture2D; p->desc = *desc; *out = p; return S_OK;
    }
    HRESULT CreateDepthStencilView(ID3D11Texture2D*, const D3D11_DEPTH_STENCIL_VIEW_DESC*, Resource** out) {
        ++depthViews; *out = new Resource; return S_OK;
    }
    HRESULT CreateShaderResourceView(ID3D11Texture2D*, const D3D11_SHADER_RESOURCE_VIEW_DESC*, Resource** out) {
        *out = new Resource; return S_OK;
    }
    HRESULT CreateRenderTargetView(ID3D11Texture2D*, void*, Resource** out) {
        *out = new Resource; return S_OK;
    }
    HRESULT CreateOffscreenPlainSurface(UINT w, UINT h, D3DFORMAT fmt, int, IDirect3DSurface9** out, void*) {
        ++surfaces;
        if (fail) return -1;
        auto p = new IDirect3DSurface9; p->desc = {w, h, fmt}; *out = p; return S_OK;
    }
};
struct Context {
    int maps = 0, unmaps = 0, binds = 0;
    int bytes = 0;
    void ClearRenderTargetView(Resource*, FLOAT*) {}
    template<class T> void Map(T, int, int, int, T* out) = delete;
    template<class T> void Map(Resource*, int, int, int, T* out) { ++maps; out->pData = &bytes; }
    void Unmap(Resource*, int) { ++unmaps; }
    void VSSetConstantBuffers(int, int, Resource**) { ++binds; }
};
struct DX11Renderer {
    Device* device;
    Context* deviceContext;
    u32 width = 0, height = 0;
    bool frameRendered = false, frameRenderedOnce = false;
    ComPtr<ID3D11Texture2D> depthTex, fbTex;
    ComPtr<Resource> depthTexView, fbTextureView, fbRenderTarget;
    virtual ~DX11Renderer() = default;
    virtual void resize(int w, int h);
    void createDepthTexAndView(ComPtr<ID3D11Texture2D>&, ComPtr<Resource>&, int, int,
        DXGI_FORMAT = DXGI_FORMAT_D24_UNORM_S8_UINT, UINT = 0);
    void createTexAndRenderTarget(ComPtr<ID3D11Texture2D>&, ComPtr<Resource>&, int, int);
''']
if not args.baseline_oit:
    pieces.append('bool resizeFramebuffer(int, int);')
    pieces.append('HRESULT ensureReadbackTexture(ComPtr<ID3D11Texture2D>&, const D3D11_TEXTURE2D_DESC&);')
pieces += ['};', function(renderer, 'void DX11Renderer::resize('),
           function(renderer, 'void DX11Renderer::createDepthTexAndView('),
           function(renderer, 'void DX11Renderer::createTexAndRenderTarget('), r'''
struct DX11OITRenderer : DX11Renderer {
    u32 maxWidth = 0, maxHeight = 0;
    struct { void resize(u32, u32) {} } buffers;
    ComPtr<ID3D11Texture2D> opaqueTex, multipassTex, depthStencilTex2;
    ComPtr<Resource> opaqueRenderTarget, multipassRenderTarget, multipassTextureView,
        opaqueTextureView, depthStencilView2, stencilView, depthView;
''', function(oit, 'void resizeInternal('),
           function(oit, 'void resize('),
           '};']
if not args.baseline_oit:
    pieces += [function(renderer, 'bool DX11Renderer::resizeFramebuffer('),
               function(renderer, 'HRESULT DX11Renderer::ensureReadbackTexture('), r'''
struct D3DRenderer {
    Device* device;
    HRESULT ensureReadbackSurface(ComPtr<IDirect3DSurface9>&, u32, u32, D3DFORMAT);
};
''', function(dx9, 'HRESULT D3DRenderer::ensureReadbackSurface(')]
    # Execute the actual conditional block; non-OIT sequence values are dead
    # shader outputs, while the shared VS still requires its buffer binding.
    block = function(function(oit, 'void setRenderState('), 'if (!gp->isNaomi2())')
    pieces += [r'''
struct DX11OITShaders { enum Pass { Depth, Color, OIT }; };
struct Poly { bool naomi; bool isNaomi2() const { return naomi; } };
struct Mapped { void* pData; };
template<DX11OITShaders::Pass pass> void sequence(Context* deviceContext, Poly* gp, int polyNumber) {
    Mapped mappedSubres;
    ComPtr<Resource> vtxPolyConstants; vtxPolyConstants.reset(new Resource);
''', block, '}']
pieces += [r'''
int main() {
    Device device; Context context;
    DX11OITRenderer renderer; renderer.device = &device; renderer.deviceContext = &context;
    renderer.resize(640, 480);
    auto depth = renderer.depthTex.get();
    int allocations = device.textures;
    for (int i = 0; i < 120; ++i) renderer.resize(640, 480);
    std::cerr << "Texture allocations during 120 steady OIT frames: " << device.textures - allocations << '\n';
    assert(device.textures == allocations && renderer.depthTex.get() == depth);
    assert(device.depthTextures == 2 && device.depthViews == 2);
    renderer.resize(1280, 960);
    assert(device.depthTextures == 4);
    renderer.resize(640, 480);
    assert(device.depthTextures == 4);
    renderer.resizeInternal(1920, 1080); // RTT growth
    assert(device.depthTextures == 6);
    depth = renderer.depthTex.get(); allocations = device.textures;
    renderer.resize(640, 480); // return from RTT, keep the larger depth allocation
    assert(device.textures == allocations && renderer.depthTex.get() == depth);
    renderer.depthTexView.reset(); renderer.depthTex.reset();
    renderer.resize(640, 480); // missing depth recovery at unchanged framebuffer size
    assert(device.depthTextures == 7);
    assert(renderer.depthTex->desc.Width == 1920 && renderer.depthTex->desc.Height == 1080);
    renderer.depthTexView.reset(); // texture creation can succeed while its view fails
    renderer.resize(640, 480);
    assert(renderer.depthTexView && device.depthTextures == 8);
    std::cout << "PASS production OIT resizing: steady frames, growth, shrink, RTT growth, missing depth\n";
''']
if not args.baseline_oit:
    pieces += [r'''
    DX11Renderer ordinary; ordinary.device = &device; ordinary.deviceContext = &context;
    ordinary.resize(640, 480);
    assert(ordinary.depthTex->desc.Format == DXGI_FORMAT_D24_UNORM_S8_UINT);
    D3D11_TEXTURE2D_DESC desc; desc.Width = 640; desc.Height = 480; desc.Format = 4;
    ComPtr<ID3D11Texture2D> fb, rtt;
    assert(ordinary.ensureReadbackTexture(fb, desc) == S_OK);
    allocations = device.textures;
    for (int i = 0; i < 120; ++i) assert(ordinary.ensureReadbackTexture(fb, desc) == S_OK);
    assert(device.textures == allocations);
    desc.Width = 128; desc.Height = 128;
    assert(ordinary.ensureReadbackTexture(rtt, desc) == S_OK);
    desc.Width = 640; desc.Height = 480;
    assert(ordinary.ensureReadbackTexture(fb, desc) == S_OK && device.textures == allocations + 1);
    desc.Width = 800;
    assert(ordinary.ensureReadbackTexture(fb, desc) == S_OK && device.textures == allocations + 2);
    desc.Format = 5;
    assert(ordinary.ensureReadbackTexture(fb, desc) == S_OK && device.textures == allocations + 3);
    device.fail = true; desc.Height = 600;
    assert(ordinary.ensureReadbackTexture(fb, desc) < 0 && !fb);
    device.fail = false;
    assert(ordinary.ensureReadbackTexture(fb, desc) == S_OK);
    allocations = device.textures;
    ++desc.MipLevels;
    assert(ordinary.ensureReadbackTexture(fb, desc) == S_OK && device.textures == allocations + 1);
    ++desc.ArraySize;
    assert(ordinary.ensureReadbackTexture(fb, desc) == S_OK && device.textures == allocations + 2);
    ++desc.SampleDesc.Count;
    assert(ordinary.ensureReadbackTexture(fb, desc) == S_OK && device.textures == allocations + 3);
    for (UINT* field : { &desc.SampleDesc.Quality, &desc.Usage, &desc.BindFlags,
                        &desc.CPUAccessFlags, &desc.MiscFlags }) {
        allocations = device.textures;
        ++*field;
        assert(ordinary.ensureReadbackTexture(fb, desc) == S_OK && device.textures == allocations + 1);
    }
    D3DRenderer nine; nine.device = &device;
    ComPtr<IDirect3DSurface9> fb9, rtt9;
    assert(nine.ensureReadbackSurface(fb9, 640, 480, 4) == S_OK);
    allocations = device.surfaces;
    for (int i = 0; i < 120; ++i) assert(nine.ensureReadbackSurface(fb9, 640, 480, 4) == S_OK);
    assert(device.surfaces == allocations);
    assert(nine.ensureReadbackSurface(rtt9, 128, 128, 4) == S_OK);
    assert(nine.ensureReadbackSurface(fb9, 640, 480, 4) == S_OK && device.surfaces == allocations + 1);
    assert(nine.ensureReadbackSurface(fb9, 800, 480, 4) == S_OK && device.surfaces == allocations + 2);
    assert(nine.ensureReadbackSurface(fb9, 800, 480, 5) == S_OK && device.surfaces == allocations + 3);
    device.fail = true;
    assert(nine.ensureReadbackSurface(fb9, 800, 600, 5) < 0 && !fb9);
    device.fail = false;
    assert(nine.ensureReadbackSurface(fb9, 800, 600, 5) == S_OK);
    std::cout << "PASS production D3D9/D3D11 staging reuse: separate targets, resize, format, failure retry\n";
    Poly gp{false};
    sequence<DX11OITShaders::Depth>(&context, &gp, 123);
    sequence<DX11OITShaders::Color>(&context, &gp, 456);
    assert(context.maps == 0 && context.unmaps == 0 && context.binds == 2);
    sequence<DX11OITShaders::OIT>(&context, &gp, 789);
    assert(context.maps == 1 && context.unmaps == 1 && context.bytes == 789 && context.binds == 3);
    gp.naomi = true;
    sequence<DX11OITShaders::OIT>(&context, &gp, 0);
    assert(context.maps == 1 && context.binds == 3);
    std::cout << "PASS production sequence upload: OIT only, shared binding, Naomi2 excluded\n";
''']
pieces += ['}']
source = output / 'directx_render_resources.cpp'
source.write_text('\n'.join(pieces))
binary = output / 'directx_render_resources'
subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wno-pragma-once-outside-header',
                '-fsanitize=address,undefined', '-g', str(source), '-o', str(binary)], check=True)
subprocess.run([str(binary)], check=True)

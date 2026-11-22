# Portions Copyright 2026 The Hollycast Authors
"""Exercise the production frame setup after shaders overwrite lookup texture slots."""
from pathlib import Path
import argparse

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('output_directory', type=Path)
output = parser.parse_args().output_directory.resolve()
output.mkdir(parents=True, exist_ok=True)

source = (root / 'core/rend/gles/gles.cpp').read_text(encoding='utf-8')
start = source.index('void OpenGLRenderer::Process(')
brace = source.index('{', start)
depth = 1
end = brace + 1
while depth:
    depth += (source[end] == '{') - (source[end] == '}')
    end += 1
process = source[start:end]

shim = r'''
// Portions Copyright 2026 The Hollycast Authors
#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <stdexcept>
using u8 = std::uint8_t;
using GLuint = unsigned;
using GLenum = unsigned;
constexpr GLenum GL_TEXTURE0 = 0, GL_TEXTURE_2D = 2;
static unsigned activeUnit = 0;
static std::array<GLuint, 8> bindings {};
void glActiveTexture(GLenum unit) { assert(unit < bindings.size()); activeUnit = unit; }
void glBindTexture(GLenum target, GLuint texture) { assert(target == GL_TEXTURE_2D); bindings[activeUnit] = texture; }
namespace config { bool Fog = true; }
namespace i18n { const char *T(const char *text) { return text; } }
struct RendererException : std::runtime_error { using std::runtime_error::runtime_error; };
struct { struct { bool isNaomi2() const { return false; } } platform; } settings;
struct RendContext { bool isRTT = false; int swapInterval = 0; };
struct TA_context { RendContext rend; };
struct { int gl_major = 4; bool prim_restart_fixed_supported = true, prim_restart_supported = true; int single_channel_format = 0; RendContext *rendContext = nullptr; } gl;
struct { void Clear() {} void Cleanup() {} } TexCache;
bool resetTextureCache = false, updateFogTable = true, updatePalette = true;
GLuint fogTextureId = 0, paletteTextureId = 0;
u8 FOG_TABLE[256] {};
void updateFogTexture(u8*, GLenum slot, int) { fogTextureId = 41; glActiveTexture(slot); glBindTexture(GL_TEXTURE_2D, fogTextureId); glActiveTexture(GL_TEXTURE0); }
void updatePaletteTexture(GLenum slot) { paletteTextureId = 42; glActiveTexture(slot); glBindTexture(GL_TEXTURE_2D, paletteTextureId); glActiveTexture(GL_TEXTURE0); }
struct GraphicsContext {
    static GraphicsContext *Instance() { static GraphicsContext context; return &context; }
    void setSwapInterval(int) {}
};
static unsigned fogSlot = 1, paletteSlot = 2, frames = 0;
void ta_parse(TA_context *context, bool) {
    assert(gl.rendContext == &context->rend);
    assert(activeUnit == GL_TEXTURE0);
    if (config::Fog) assert(bindings[fogSlot] == fogTextureId);
    assert(bindings[paletteSlot] == paletteTextureId);
    ++frames;
}
class OpenGLRenderer {
public:
    GLenum getFogTextureSlot() const { return fogSlot; }
    GLenum getPaletteTextureSlot() const { return paletteSlot; }
    void Process(TA_context *context);
};
'''

checks = r'''
int main() {
    for (bool perPixel : { false, true }) {
        fogSlot = perPixel ? 5 : 1;
        paletteSlot = perPixel ? 6 : 2;
        fogTextureId = paletteTextureId = 0;
        updateFogTable = updatePalette = true;
        config::Fog = true;
        bindings.fill(0);
        OpenGLRenderer renderer;
        TA_context context;
        renderer.Process(&context);
        assert(!updateFogTable && !updatePalette);
        // An FX pass samples its backbuffer on these units. The unchanged game
        // tables must be rebound on every later frame, without uploading again.
        for (unsigned frame = 0; frame < 3; ++frame) {
            bindings[fogSlot] = 100 + frame;
            bindings[paletteSlot] = 200 + frame;
            activeUnit = GL_TEXTURE0;
            renderer.Process(&context);
            assert(!updateFogTable && !updatePalette);
        }
        for (bool updateFog : { false, true }) {
            updateFogTable = updateFog;
            updatePalette = !updateFog;
            bindings[fogSlot] = bindings[paletteSlot] = 99;
            renderer.Process(&context);
        }
        config::Fog = false;
        updateFogTable = true;
        bindings[fogSlot] = bindings[paletteSlot] = 99;
        renderer.Process(&context);
        assert(updateFogTable);
        config::Fog = true;
        renderer.Process(&context);
        assert(!updateFogTable);
    }
    assert(frames == 16);
    std::cout << "PASS fog/palette bindings restored after FX across normal and per-pixel rendering, unchanged tables, partial updates and fog toggles\n";
}
'''

target = output / 'gl_lookup_bindings.cpp'
target.write_text(shim + process + checks, encoding='utf-8')
print('Generated:', target)

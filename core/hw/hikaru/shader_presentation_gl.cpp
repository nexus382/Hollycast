// Portions Copyright 2026 The Hollycast Authors
#include "shader_presentation_gl.h"

#include "cfg/option.h"
#include "rend/gles/gles.h"
#include "rend/gles/gl_shader_chain.h"
#include "rend/gles/quad.h"
#include "ui/settings_new.h"

#include <utility>

namespace hikaru {

struct GlShaderPresentation::State
{
    GlShaderChain chain;
    std::unique_ptr<GlFramebuffer> capture;
    std::unique_ptr<GlFramebuffer> shaderInput;
    std::unique_ptr<GlQuadDrawer> quad;
    GLuint drawFramebuffer = 0;
    GLuint readFramebuffer = 0;
    bool shaderRequested = false;
    GLuint shaderDepth = 0;
    int depthWidth = 0;
    int depthHeight = 0;

    ~State()
    {
        releaseDepth();
    }

    void releaseDepth()
    {
        if (shaderDepth) {
            glDeleteTextures(1, &shaderDepth);
            shaderDepth = 0;
        }
    }
};

GlShaderPresentation::GlShaderPresentation() = default;
GlShaderPresentation::~GlShaderPresentation() = default;

uint32_t GlShaderPresentation::begin(uint32_t target, int width, int height)
{
    capturePending = false;
    const bool shaderRequested = postprocess::hasConfiguredShaderSelection();
    if (!shaderRequested && !SettingsNew::shaderLiveTuneActive()) {
        reset();
        return target;
    }

    if (!state) {
        // Hikaru can reach this context without initializing the PVR renderer.
        // The shared chain and quad need the current desktop GL or GLES profile.
        findGLVersion();
        state = std::make_unique<State>();
    } else if (!shaderRequested && state->shaderRequested) {
        state->chain.term();
        state->shaderInput.reset();
        state->releaseDepth();
        glcache.Reset();
    }
    GLint readFramebuffer = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFramebuffer);
    state->drawFramebuffer = target;
    state->readFramebuffer = static_cast<GLuint>(readFramebuffer);
    state->shaderRequested = shaderRequested;

    if (!state->capture || state->capture->getWidth() != width || state->capture->getHeight() != height) {
        GLint maxTextureSize = 0;
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize);
        if (width <= 0 || height <= 0 || width > maxTextureSize || height > maxTextureSize)
            return target;

        glActiveTexture(GL_TEXTURE0);
        glcache.Reset();
        auto capture = std::make_unique<GlFramebuffer>(width, height, false, true);
        const bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        if (complete)
            state->capture = std::move(capture);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, state->drawFramebuffer);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, state->readFramebuffer);
        capture.reset();
        glcache.Reset();

        if (!complete)
            return target;
    }

    capturePending = true;
    return state->capture->getFramebuffer();
}

bool GlShaderPresentation::needsSceneDepth() const
{
    return capturePending && state->shaderRequested && state->chain.needsSceneDepth();
}

void GlShaderPresentation::present(int destY, uint32_t depthTexture)
{
    if (!capturePending)
        return;
    capturePending = false;

    // Valkyrie uses raw GL and has finished compositing the guest and its bars.
    // Keep its lower-left texture orientation when drawing to the window.
    glcache.Reset();
    const int width = state->capture->getWidth();
    const int height = state->capture->getHeight();
    GlFramebuffer* image = state->capture.get();
    bool flipOutput = false;
    if (state->shaderRequested) {
        // Display shaders use a top-origin image on all backends. Normalize
        // Valkyrie's bottom-origin canvas, then flip the filtered result back.
        if (!state->shaderInput || state->shaderInput->getWidth() != width || state->shaderInput->getHeight() != height) {
            glActiveTexture(GL_TEXTURE0);
            auto input = std::make_unique<GlFramebuffer>(width, height, false, true);
            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE)
                state->shaderInput = std::move(input);
            else
                state->shaderInput.reset();
        }
        if (state->shaderInput) {
            glcache.Disable(GL_SCISSOR_TEST);
            state->capture->bind(GL_READ_FRAMEBUFFER);
            state->shaderInput->bind(GL_DRAW_FRAMEBUFFER);
            glBlitFramebuffer(0, 0, width, height, 0, height, width, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            GLuint shaderDepth = 0;
            if (depthTexture) {
                if (!state->shaderDepth || state->depthWidth != width || state->depthHeight != height) {
                    if (!state->shaderDepth)
                        glGenTextures(1, &state->shaderDepth);
                    glActiveTexture(GL_TEXTURE0);
                    glcache.BindTexture(GL_TEXTURE_2D, state->shaderDepth);
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, width, height, 0,
                        GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr);
                    glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                    glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                    glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                    state->depthWidth = width;
                    state->depthHeight = height;
                }
                // The chain sees the same top-origin rows for color and depth.
                glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depthTexture, 0);
                glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, state->shaderDepth, 0);
                if (glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE
                        && glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
                    glBlitFramebuffer(0, 0, width, height, 0, height, width, 0, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
                    shaderDepth = state->shaderDepth;
                }
                glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
                glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
                if (!shaderDepth)
                    state->releaseDepth();
            } else {
                state->releaseDepth();
            }
            postprocess::ShaderFrameInfo frame { width, height, width, height, 0, false,
                static_cast<float>(width) / height };
            bool presentationTransformApplied = false;
            if (auto* output = state->chain.render(state->shaderInput->getTexture(), frame, presentationTransformApplied,
                    shaderDepth, shaderDepth ? width : 0, shaderDepth ? height : 0)) {
                image = output;
                flipOutput = true;
            }
        }
    }

    // Draw a textured quad so the destination may also be a multisampled
    // window framebuffer. GLES cannot blit a single-sample FBO into one.
    if (!state->quad)
        state->quad = std::make_unique<GlQuadDrawer>();
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, state->drawFramebuffer);
    glcache.Disable(GL_SCISSOR_TEST);
    glcache.Disable(GL_BLEND);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glcache.ClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
    glViewport(0, destY, width, height);
    glActiveTexture(GL_TEXTURE0);
    if (glBindSampler != nullptr)
        glBindSampler(0, 0);
    glcache.InvalidateTextureBinding();
    glcache.BindTexture(GL_TEXTURE_2D, image->getTexture());
    const GLint filter = config::LinearInterpolation ? GL_LINEAR : GL_NEAREST;
    glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    state->quad->draw(image->getTexture(), false, flipOutput);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, state->readFramebuffer);
    glcache.Reset();
}

void GlShaderPresentation::reset()
{
    capturePending = false;
    if (state) {
        state.reset();
        glcache.Reset();
    }
}

}

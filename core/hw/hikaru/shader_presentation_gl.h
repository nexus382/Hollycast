// Portions Copyright 2026 The Hollycast Authors
#pragma once

#include <cstdint>
#include <memory>

namespace hikaru {

class GlShaderPresentation
{
public:
    GlShaderPresentation();
    ~GlShaderPresentation();

    // Returns the framebuffer Valkyrie should draw to for this frame.
    uint32_t begin(uint32_t target, int width, int height);
    void present(int destY);
    void reset();

private:
    struct State;
    std::unique_ptr<State> state;
    bool capturePending = false;
};

}

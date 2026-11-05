// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 The Hollycast Authors
#define GL_SILENCE_DEPRECATION
#include <SDL.h>
#ifdef __APPLE__
#include <OpenGL/gl3.h>
#else
#include <glad/gl.h>
#endif
#include <stdio.h>
#include <string.h>

int hikaru_vendor_test_transparency_sorting_pixels(void);

int main(int argc, char **argv)
{
    const int gles = argc == 2 && !strcmp(argv[1], "--gles");
    if (SDL_Init(SDL_INIT_VIDEO)) { fprintf(stderr, "SDL video: %s\n", SDL_GetError()); return 2; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, gles ? SDL_GL_CONTEXT_PROFILE_ES : SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, gles ? 0 : 2);
    SDL_Window *window = SDL_CreateWindow("Hikaru transparency sorting", 0, 0, 16, 16,
        SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!window) { fprintf(stderr, "SDL window: %s\n", SDL_GetError()); SDL_Quit(); return 3; }
    SDL_GLContext context = SDL_GL_CreateContext(window);
    if (!context) { fprintf(stderr, "SDL GL context: %s\n", SDL_GetError()); SDL_DestroyWindow(window); SDL_Quit(); return 4; }
#ifndef __APPLE__
    if (!(gles ? gladLoadGLES2((GLADloadfunc)SDL_GL_GetProcAddress) :
                 gladLoadGL((GLADloadfunc)SDL_GL_GetProcAddress))) return 5;
#endif
    const int passed = hikaru_vendor_test_transparency_sorting_pixels();
    SDL_GL_DeleteContext(context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    if (!passed) { fprintf(stderr, "crossing translucent triangles did not resolve per pixel\n"); return 1; }
    puts("per-pixel transparency sorting: PASS");
    return 0;
}

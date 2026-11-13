// Portions Copyright 2026 The Hollycast Authors
#if defined(USE_OPENGL) && defined(USE_SDL) && !defined(GLES2) && !defined(TARGET_IPHONE)
#include "gtest/gtest.h"
#include "rend/gles/gles.h"
#include "rend/gles/quad.h"
#include <SDL.h>

TEST(RemotePlayCaptureTest, PreservesTheRenderingStateBetweenFrames)
{
	ASSERT_EQ(SDL_InitSubSystem(SDL_INIT_VIDEO), 0) << SDL_GetError();
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
	auto *window = SDL_CreateWindow("Remote Play capture test", 0, 0, 64, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
	ASSERT_NE(window, nullptr) << SDL_GetError();
	auto context = SDL_GL_CreateContext(window);
	ASSERT_NE(context, nullptr) << SDL_GetError();
	ASSERT_NE(gladLoadGL((GLADloadfunc)SDL_GL_GetProcAddress), 0);
	gl.gl_major = 3;
	gl.gl_minor = 2;
	gl.is_gles = false;
	gl.gl_version = "GL3";
	gl.glsl_version_header = "#version 150";
	glcache.Reset();
	gl.quadDrawer = std::make_unique<GlQuadDrawer>();
	gl.ofbo.aspectRatio = 1.f;
	gl.ofbo.framebuffer = std::make_unique<GlFramebuffer>(64, 64, false, true);
	glClearColor(1, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	{
		GlFramebuffer target(64, 64, false, false);
		target.bind();
		glViewport(3, 4, 40, 44);
		glPixelStorei(GL_PACK_ALIGNMENT, 8);
		glPixelStorei(GL_PACK_ROW_LENGTH, 71);
		glPixelStorei(GL_PACK_SKIP_ROWS, 2);
		glPixelStorei(GL_PACK_SKIP_PIXELS, 3);
		glEnable(GL_SCISSOR_TEST);
		GLint beforeFramebuffer, beforeViewport[4], beforePack;
		glGetIntegerv(GL_FRAMEBUFFER_BINDING, &beforeFramebuffer);
		glGetIntegerv(GL_VIEWPORT, beforeViewport);
		glGetIntegerv(GL_PACK_ALIGNMENT, &beforePack);
		OpenGLRenderer renderer;
		std::vector<u8> data;
		int width = 32, height = 0;
		EXPECT_FALSE(renderer.GetLastFrameForRemotePlay(data, width, height));
		// Only the test waits: production capture polls fences without blocking.
		glFinish();
		EXPECT_TRUE(renderer.GetLastFrameForRemotePlay(data, width, height));
		EXPECT_EQ(width, 32);
		EXPECT_EQ(height, 32);
		ASSERT_EQ(data.size(), 32u * 32 * 3);
		for (size_t pixel = 0; pixel < data.size(); pixel += 3)
		{
			EXPECT_EQ(data[pixel], 255);
			EXPECT_EQ(data[pixel + 1], 0);
			EXPECT_EQ(data[pixel + 2], 0);
		}
		GLint afterFramebuffer, afterViewport[4], afterPack;
		glGetIntegerv(GL_FRAMEBUFFER_BINDING, &afterFramebuffer);
		glGetIntegerv(GL_VIEWPORT, afterViewport);
		glGetIntegerv(GL_PACK_ALIGNMENT, &afterPack);
		EXPECT_EQ(beforeFramebuffer, afterFramebuffer);
		EXPECT_EQ(beforePack, afterPack);
		for (int i = 0; i < 4; i++) EXPECT_EQ(beforeViewport[i], afterViewport[i]);
		GLint value;
		glGetIntegerv(GL_PACK_ROW_LENGTH, &value); EXPECT_EQ(value, 71);
		glGetIntegerv(GL_PACK_SKIP_ROWS, &value); EXPECT_EQ(value, 2);
		glGetIntegerv(GL_PACK_SKIP_PIXELS, &value); EXPECT_EQ(value, 3);
		EXPECT_TRUE(glIsEnabled(GL_SCISSOR_TEST));
		EXPECT_EQ(glGetError(), (GLenum)GL_NO_ERROR);
	}
	gl.ofbo.framebuffer.reset();
	gl.quadDrawer.reset();
	glcache.Reset();
	SDL_GL_DeleteContext(context);
	SDL_DestroyWindow(window);
	SDL_QuitSubSystem(SDL_INIT_VIDEO);
}
#endif

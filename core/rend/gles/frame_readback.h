// Portions Copyright 2026 The Hollycast Authors
#pragma once
#include "wsi/gl_context.h"
#if !defined(GLES2)
#include <array>
#include <vector>

// Render-thread-only, bounded GPU readback. Copies with a blit rather than
// drawing a quad, so capturing cannot change the renderer's shaders, texture
// bindings or cached blend/depth state. Never waits for the GPU.
class GlFrameReadback
{
public:
	~GlFrameReadback() { reset(); }
	void reset()
	{
		for (auto& slot : slots)
		{
			if (slot.fence) glDeleteSync(slot.fence);
			if (slot.buffer) glDeleteBuffers(1, &slot.buffer);
			slot = {};
		}
		if (framebuffer) glDeleteFramebuffers(1, &framebuffer);
		if (colorBuffer) glDeleteRenderbuffers(1, &colorBuffer);
		framebuffer = colorBuffer = 0;
		width = height = 0;
	}

	bool capture(GLuint source, int sourceWidth, int sourceHeight, int targetWidth, int targetHeight,
		std::vector<u8>& rgb)
	{
		State restore;
		if (width != targetWidth || height != targetHeight)
		{
			reset();
			width = targetWidth;
			height = targetHeight;
			glGenFramebuffers(1, &framebuffer);
			glGenRenderbuffers(1, &colorBuffer);
			glBindRenderbuffer(GL_RENDERBUFFER, colorBuffer);
			glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, width, height);
			glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
			glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, colorBuffer);
			if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
			{
				reset();
				return false;
			}
			for (auto& slot : slots)
			{
				glGenBuffers(1, &slot.buffer);
				glBindBuffer(GL_PIXEL_PACK_BUFFER, slot.buffer);
				glBufferData(GL_PIXEL_PACK_BUFFER, (size_t)width * height * 4, nullptr, GL_STREAM_READ);
			}
		}

		Slot *newest = nullptr;
		for (auto& slot : slots)
		{
			if (!slot.fence) continue;
			GLenum status = glClientWaitSync(slot.fence, 0, 0);
			if (status == GL_ALREADY_SIGNALED || status == GL_CONDITION_SATISFIED)
			{
				if (newest == nullptr || slot.sequence > newest->sequence) newest = &slot;
				glDeleteSync(slot.fence);
				slot.fence = nullptr;
			}
		}
		bool captured = false;
		if (newest)
		{
			glBindBuffer(GL_PIXEL_PACK_BUFFER, newest->buffer);
			const auto *pixels = static_cast<const u8 *>(glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0,
				(size_t)width * height * 4, GL_MAP_READ_BIT));
			if (pixels)
			{
				rgb.resize((size_t)width * height * 3);
				for (size_t i = 0; i < (size_t)width * height; i++)
					for (size_t c = 0; c < 3; c++) rgb[i * 3 + c] = pixels[i * 4 + c];
				captured = glUnmapBuffer(GL_PIXEL_PACK_BUFFER) == GL_TRUE;
			}
		}

		for (auto& slot : slots)
		{
			if (slot.fence) continue;
			glDisable(GL_SCISSOR_TEST);
			glBindFramebuffer(GL_READ_FRAMEBUFFER, source);
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer);
			glBlitFramebuffer(0, 0, sourceWidth, sourceHeight, 0, 0, width, height,
				GL_COLOR_BUFFER_BIT, GL_LINEAR);
			glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
			glBindBuffer(GL_PIXEL_PACK_BUFFER, slot.buffer);
			glPixelStorei(GL_PACK_ALIGNMENT, 1);
			glPixelStorei(GL_PACK_ROW_LENGTH, 0);
			glPixelStorei(GL_PACK_SKIP_ROWS, 0);
			glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
			glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
			slot.fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
			slot.sequence = ++sequence;
			glFlush();
			break;
		}
		return captured;
	}

private:
	struct State
	{
		GLint read, draw, renderbuffer, packBuffer, alignment, rowLength, skipRows, skipPixels;
		GLboolean scissor;
		State()
		{
			glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
			glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
			glGetIntegerv(GL_RENDERBUFFER_BINDING, &renderbuffer);
			glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &packBuffer);
			glGetIntegerv(GL_PACK_ALIGNMENT, &alignment);
			glGetIntegerv(GL_PACK_ROW_LENGTH, &rowLength);
			glGetIntegerv(GL_PACK_SKIP_ROWS, &skipRows);
			glGetIntegerv(GL_PACK_SKIP_PIXELS, &skipPixels);
			scissor = glIsEnabled(GL_SCISSOR_TEST);
		}
		~State()
		{
			glBindFramebuffer(GL_READ_FRAMEBUFFER, read);
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw);
			glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
			glBindBuffer(GL_PIXEL_PACK_BUFFER, packBuffer);
			glPixelStorei(GL_PACK_ALIGNMENT, alignment);
			glPixelStorei(GL_PACK_ROW_LENGTH, rowLength);
			glPixelStorei(GL_PACK_SKIP_ROWS, skipRows);
			glPixelStorei(GL_PACK_SKIP_PIXELS, skipPixels);
			if (scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
		}
	};
	struct Slot { GLuint buffer = 0; GLsync fence = nullptr; uint64_t sequence = 0; };
	std::array<Slot, 3> slots {};
	GLuint framebuffer = 0, colorBuffer = 0;
	int width = 0, height = 0;
	uint64_t sequence = 0;
};
#endif

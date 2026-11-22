// Portions Copyright 2026 The Hollycast Authors
#include "test_harness_capture.h"
#include "test_harness_protocol.h"
#include "wsi/context.h"
#include "cfg/option.h"
#ifdef USE_OPENGL
#include "wsi/gl_context.h"
#endif
#if defined(USE_DX11) && defined(_WIN32)
#include "rend/dx11/dx11context.h"
#endif
#ifdef USE_VULKAN
#include "rend/vulkan/vulkan_context.h"
#endif
#include <algorithm>
#include <cstring>

namespace test_harness {

static void allocate(std::vector<std::uint8_t>& rgba, int width, int height)
{
	require(width > 0 && height > 0 && width <= 8192 && height <= 8192
		&& std::uint64_t(width) * height <= 16 * 1024 * 1024, "Output dimensions exceed capture limits");
	rgba.resize(std::size_t(width) * height * 4);
}

void captureFrame(std::vector<std::uint8_t>& rgba, int& width, int& height)
{
	width = settings.display.width;
	height = settings.display.height;
	allocate(rgba, width, height);
	const auto context = GraphicsContext::Instance();
#if defined(USE_DX11) && defined(_WIN32)
	if (const auto dx = dynamic_cast<DX11Context *>(context))
	{
		require(bool(dx->getRenderTarget()), "No Direct3D output target");
		ComPtr<ID3D11Resource> resource;
		dx->getRenderTarget()->GetResource(&resource.get());
		ComPtr<ID3D11Texture2D> source;
		require(SUCCEEDED(resource.as(source)), "Direct3D output is not a texture");
		D3D11_TEXTURE2D_DESC desc;
		source->GetDesc(&desc);
		width = int(desc.Width);
		height = int(desc.Height);
		allocate(rgba, width, height);
		const bool bgra = desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
		require(bgra || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
			"Unsupported Direct3D output format");
		desc.Usage = D3D11_USAGE_STAGING;
		desc.BindFlags = desc.MiscFlags = 0;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		ComPtr<ID3D11Texture2D> staging;
		require(SUCCEEDED(dx->getDevice()->CreateTexture2D(&desc, nullptr, &staging.get())), "Cannot allocate capture staging texture");
		dx->getDeviceContext()->CopyResource(staging, source);
		D3D11_MAPPED_SUBRESOURCE mapped;
		require(SUCCEEDED(dx->getDeviceContext()->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)), "Cannot map capture staging texture");
		for (int y = 0; y < height; ++y)
		{
			const auto src = static_cast<const std::uint8_t *>(mapped.pData) + std::size_t(y) * mapped.RowPitch;
			auto dst = rgba.data() + std::size_t(y) * width * 4;
			std::memcpy(dst, src, std::size_t(width) * 4);
			for (int x = 0; x < width; ++x)
			{
				if (bgra)
					std::swap(dst[x * 4], dst[x * 4 + 2]);
				dst[x * 4 + 3] = 255;
			}
		}
		dx->getDeviceContext()->Unmap(staging, 0);
		return;
	}
#endif
#ifdef USE_OPENGL
	if (dynamic_cast<GLGraphicsContext *>(context))
	{
		GLint framebuffer, packBuffer, alignment, rowLength, skipRows, skipPixels, readBuffer;
		glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &framebuffer);
		glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &packBuffer);
		glGetIntegerv(GL_PACK_ALIGNMENT, &alignment);
		glGetIntegerv(GL_PACK_ROW_LENGTH, &rowLength);
		glGetIntegerv(GL_PACK_SKIP_ROWS, &skipRows);
		glGetIntegerv(GL_PACK_SKIP_PIXELS, &skipPixels);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
		glGetIntegerv(GL_READ_BUFFER, &readBuffer);
		glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
		glPixelStorei(GL_PACK_ALIGNMENT, 1);
		glPixelStorei(GL_PACK_ROW_LENGTH, 0);
		glPixelStorei(GL_PACK_SKIP_ROWS, 0);
		glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
		glReadBuffer(GL_BACK);
		glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
		glReadBuffer(readBuffer);
		glPixelStorei(GL_PACK_ALIGNMENT, alignment);
		glPixelStorei(GL_PACK_ROW_LENGTH, rowLength);
		glPixelStorei(GL_PACK_SKIP_ROWS, skipRows);
		glPixelStorei(GL_PACK_SKIP_PIXELS, skipPixels);
		glBindBuffer(GL_PIXEL_PACK_BUFFER, packBuffer);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
		const std::size_t stride = std::size_t(width) * 4;
		for (int y = 0; y < height / 2; ++y)
			std::swap_ranges(rgba.begin() + y * stride, rgba.begin() + (y + 1) * stride, rgba.begin() + (height - 1 - y) * stride);
		for (std::size_t pixel = 3; pixel < rgba.size(); pixel += 4)
			rgba[pixel] = 255;
		return;
	}
#endif
#ifdef USE_VULKAN
	if (const auto vkContext = dynamic_cast<VulkanContext *>(context))
	{
		require(vkContext->CanCaptureOutput() && vkContext->IsValid() && !vkContext->IsRendering(), "Vulkan output is not ready for capture");
		const auto format = vkContext->GetPresentFormat();
		const bool bgra = format == vk::Format::eB8G8R8A8Unorm || format == vk::Format::eB8G8R8A8Srgb;
		require(bgra || format == vk::Format::eR8G8B8A8Unorm || format == vk::Format::eR8G8B8A8Srgb, "Unsupported Vulkan output format");
		const auto device = vkContext->GetDevice();
		device.waitIdle();
		vk::UniqueDeviceMemory memory;
		auto buffer = device.createBufferUnique(vk::BufferCreateInfo({}, rgba.size(), vk::BufferUsageFlagBits::eTransferDst));
		const auto requirements = device.getBufferMemoryRequirements(buffer.get());
		const auto properties = vkContext->GetPhysicalDevice().getMemoryProperties();
		std::uint32_t memoryType = UINT32_MAX;
		for (std::uint32_t i = 0; i < properties.memoryTypeCount; ++i)
			if ((requirements.memoryTypeBits & (1u << i)) != 0
				&& (properties.memoryTypes[i].propertyFlags & (vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent))
					== (vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent))
			{
				memoryType = i;
				break;
			}
		require(memoryType != UINT32_MAX, "No coherent host-visible Vulkan capture memory");
		memory = device.allocateMemoryUnique(vk::MemoryAllocateInfo(requirements.size, memoryType));
		device.bindBufferMemory(buffer.get(), memory.get(), 0);
		auto pool = device.createCommandPoolUnique(vk::CommandPoolCreateInfo(vk::CommandPoolCreateFlagBits::eTransient, vkContext->GetGraphicsQueueFamilyIndex()));
		auto commands = device.allocateCommandBuffersUnique(vk::CommandBufferAllocateInfo(pool.get(), vk::CommandBufferLevel::ePrimary, 1));
		const auto command = commands.front().get();
		const auto images = device.getSwapchainImagesKHR(vkContext->GetSwapChain());
		const auto image = images.at(vkContext->GetCurrentImageIndex());
		command.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
		const vk::ImageSubresourceRange range(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);
		const vk::ImageMemoryBarrier before(vk::AccessFlagBits::eMemoryWrite, vk::AccessFlagBits::eTransferRead,
			vk::ImageLayout::ePresentSrcKHR, vk::ImageLayout::eTransferSrcOptimal,
			VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, image, range);
		command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eTransfer, {}, nullptr, nullptr, before);
		const vk::BufferImageCopy copy(0, 0, 0, vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1),
			vk::Offset3D(), vk::Extent3D(width, height, 1));
		command.copyImageToBuffer(image, vk::ImageLayout::eTransferSrcOptimal, buffer.get(), copy);
		const vk::ImageMemoryBarrier after(vk::AccessFlagBits::eTransferRead, vk::AccessFlagBits::eMemoryRead,
			vk::ImageLayout::eTransferSrcOptimal, vk::ImageLayout::ePresentSrcKHR,
			VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, image, range);
		command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eAllCommands, {}, nullptr, nullptr, after);
		command.end();
		vkContext->SubmitCommandBuffers({command}, {});
		device.waitIdle();
		const void *mapped = device.mapMemory(memory.get(), 0, rgba.size());
		std::memcpy(rgba.data(), mapped, rgba.size());
		device.unmapMemory(memory.get());
		for (std::size_t pixel = 0; pixel < rgba.size(); pixel += 4)
		{
			if (bgra)
				std::swap(rgba[pixel], rgba[pixel + 2]);
			rgba[pixel + 3] = 255;
		}
		return;
	}
#endif
	throw std::runtime_error("Native composited capture is unavailable for this renderer");
}

} // namespace test_harness

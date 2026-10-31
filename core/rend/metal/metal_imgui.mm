// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "metal_common.h"
#include "metal_context.h"
#include "ui/imgui_driver.h"
#include <algorithm>
#include <cstring>
#include <unordered_map>

namespace
{
struct Texture
{
	id<MTLTexture> image;
	bool nearest;
};
struct DrawSnapshot
{
	ImDrawData data;
	std::vector<std::unique_ptr<Texture>> textures;
	std::vector<std::unique_ptr<unsigned char[]>> callbackData;

	~DrawSnapshot()
	{
		for (ImDrawList *list : data.CmdLists)
			IM_DELETE(list);
	}
};
class MetalDriver final : public ImGuiDriver
{
  public:
	MetalDriver()
	{
		id<MTLDevice> device = MetalContext::Instance()->GetDevice();
		NSString *source = @R"(
#include <metal_stdlib>
using namespace metal;
struct V { packed_float2 pos; packed_float2 uv; uchar4 col; };
struct O { float4 pos [[position]]; float2 uv; float4 col; };
vertex O iv(uint i [[vertex_id]], const device V *v [[buffer(0)]], constant float4 &p [[buffer(1)]]) { O o; o.pos=float4((v[i].pos-p.xy)*p.zw+float2(-1,1),0,1); o.uv=v[i].uv; o.col=float4(v[i].col)/255.; return o; }
fragment float4 iff(O in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) { return in.col*t.sample(s,in.uv); }
)";
		NSError *error = nil;
		id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
		if (!library)
			throw FlycastException(metalErrorMessage("Metal ImGui shader compilation failed", error));
		MTLRenderPipelineDescriptor *p = [MTLRenderPipelineDescriptor new];
		p.vertexFunction = [library newFunctionWithName:@"iv"];
		p.fragmentFunction = [library newFunctionWithName:@"iff"];
		p.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
		p.colorAttachments[0].blendingEnabled = YES;
		p.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
		p.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
		p.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
		p.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
		pipeline = [device newRenderPipelineStateWithDescriptor:p error:&error];
		if (!pipeline)
			throw FlycastException(metalErrorMessage("Metal ImGui pipeline initialization failed", error));
		MTLSamplerDescriptor *s = [MTLSamplerDescriptor new];
		s.minFilter = MTLSamplerMinMagFilterLinear;
		s.magFilter = MTLSamplerMinMagFilterLinear;
		linear = [device newSamplerStateWithDescriptor:s];
		s.minFilter = MTLSamplerMinMagFilterNearest;
		s.magFilter = MTLSamplerMinMagFilterNearest;
		nearest = [device newSamplerStateWithDescriptor:s];
		ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures;
	}
	~MetalDriver() override
	{
		if (ImGui::GetCurrentContext())
		{
			for (ImTextureData *t : ImGui::GetPlatformIO().Textures)
				if (t->BackendUserData)
				{
					t->BackendUserData = nullptr;
					t->SetTexID(ImTextureID_Invalid);
					t->SetStatus(ImTextureStatus_Destroyed);
				}
			ImGui::GetIO().BackendFlags &=
				~(ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures);
		}
	}
	void newFrame() override {}
	void renderDrawData(ImDrawData *drawData, bool) override
	{
		if (!drawData)
			return;
		updateAtlasTextures(drawData);
		auto snapshot = std::make_unique<DrawSnapshot>();
		snapshot->data.Valid = true;
		snapshot->data.DisplayPos = drawData->DisplayPos;
		snapshot->data.DisplaySize = drawData->DisplaySize;
		snapshot->data.FramebufferScale = drawData->FramebufferScale;
		std::unordered_map<Texture *, Texture *> textureCopies;
		for (ImDrawList *original : drawData->CmdLists)
		{
			ImDrawList *list = original->CloneOutput();
			for (ImDrawCmd &cmd : list->CmdBuffer)
			{
				if (cmd.UserCallback && cmd.UserCallbackData && cmd.UserCallbackDataSize > 0)
				{
					auto bytes = std::make_unique<unsigned char[]>(cmd.UserCallbackDataSize);
					std::memcpy(bytes.get(), cmd.UserCallbackData, cmd.UserCallbackDataSize);
					cmd.UserCallbackData = bytes.get();
					snapshot->callbackData.push_back(std::move(bytes));
				}
				if (cmd.UserCallback)
					continue;
				Texture *texture = (Texture *)cmd.GetTexID();
				if (!texture)
					continue;
				auto found = textureCopies.find(texture);
				if (found == textureCopies.end())
				{
					auto copy = std::make_unique<Texture>(*texture);
					Texture *stable = copy.get();
					snapshot->textures.push_back(std::move(copy));
					found = textureCopies.emplace(texture, stable).first;
				}
				cmd.TexRef = ImTextureRef((ImTextureID)found->second);
			}
			snapshot->data.CmdLists.push_back(list);
			snapshot->data.CmdListsCount++;
			snapshot->data.TotalVtxCount += list->VtxBuffer.Size;
			snapshot->data.TotalIdxCount += list->IdxBuffer.Size;
		}
		pending.push_back(std::move(snapshot));
	}
	void present() override
	{
		MetalContext::Instance()->PresentLastFrame();
		pending.clear();
		retired.clear();
	}
	ImTextureID getTexture(const std::string &name) override
	{
		auto i = textures.find(name);
		return i == textures.end() ? ImTextureID() : (ImTextureID)i->second.get();
	}
	ImTextureID updateTexture(const std::string &name, const u8 *data, int w, int h, bool point) override
	{
		if (!data || w <= 0 || h <= 0)
			return {};
		auto t = std::make_unique<Texture>(makeTexture(data, w, h, point));
		auto ptr = t.get();
		auto it = textures.find(name);
		if (it != textures.end())
			retired.push_back(std::move(it->second));
		textures[name] = std::move(t);
		return (ImTextureID)ptr;
	}
	void deleteTexture(const std::string &name) override
	{
		auto it = textures.find(name);
		if (it != textures.end())
		{
			retired.push_back(std::move(it->second));
			textures.erase(it);
		}
	}
	void reset() override
	{
		for (auto &entry : textures)
			retired.push_back(std::move(entry.second));
		textures.clear();
		ImGuiDriver::reset();
	}
	void draw(id<MTLRenderCommandEncoder> e, ImDrawData *d, int width, int height)
	{
		if (!d || d->DisplaySize.x <= 0 || d->DisplaySize.y <= 0)
			return;
		[e setRenderPipelineState:pipeline];
		float projection[] = {d->DisplayPos.x, d->DisplayPos.y, 2.f / d->DisplaySize.x, -2.f / d->DisplaySize.y};
		[e setVertexBytes:projection length:sizeof(projection) atIndex:1];
		id<MTLDevice> device = MetalContext::Instance()->GetDevice();
		for (int n = 0; n < d->CmdListsCount; n++)
		{
			const ImDrawList *list = d->CmdLists[n];
			if (!list->VtxBuffer.Size || !list->IdxBuffer.Size)
				continue;
			id<MTLBuffer> vb = [device newBufferWithBytes:list->VtxBuffer.Data
												   length:list->VtxBuffer.Size * sizeof(ImDrawVert)
												  options:MTLResourceStorageModeShared];
			id<MTLBuffer> ib = [device newBufferWithBytes:list->IdxBuffer.Data
												   length:list->IdxBuffer.Size * sizeof(ImDrawIdx)
												  options:MTLResourceStorageModeShared];
			for (const ImDrawCmd &cmd : list->CmdBuffer)
			{
				if (cmd.UserCallback)
				{
					if (cmd.UserCallback != ImDrawCallback_ResetRenderState)
						cmd.UserCallback(list, &cmd);
					continue;
				}
				float x = (cmd.ClipRect.x - d->DisplayPos.x) * d->FramebufferScale.x,
					  y = (cmd.ClipRect.y - d->DisplayPos.y) * d->FramebufferScale.y;
				float z = (cmd.ClipRect.z - d->DisplayPos.x) * d->FramebufferScale.x,
					  w = (cmd.ClipRect.w - d->DisplayPos.y) * d->FramebufferScale.y;
				int x0 = std::max(0, (int)x), y0 = std::max(0, (int)y), x1 = std::min(width, (int)z),
					y1 = std::min(height, (int)w);
				if (x1 <= x0 || y1 <= y0)
					continue;
				[e setScissorRect:MTLScissorRect{(NSUInteger)x0, (NSUInteger)y0, (NSUInteger)(x1 - x0),
												 (NSUInteger)(y1 - y0)}];
				[e setVertexBuffer:vb offset:cmd.VtxOffset * sizeof(ImDrawVert) atIndex:0];
				Texture *t = (Texture *)cmd.GetTexID();
				if (!t || !t->image)
					continue;
				[e setFragmentTexture:t->image atIndex:0];
				[e setFragmentSamplerState:t->nearest ? nearest : linear atIndex:0];
				[e drawIndexedPrimitives:MTLPrimitiveTypeTriangle
							  indexCount:cmd.ElemCount
							   indexType:sizeof(ImDrawIdx) == 2 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32
							 indexBuffer:ib
					   indexBufferOffset:cmd.IdxOffset * sizeof(ImDrawIdx)];
			}
		}
	}
	void drawPending(id<MTLRenderCommandEncoder> encoder, int width, int height)
	{
		for (const auto &snapshot : pending)
			draw(encoder, &snapshot->data, width, height);
	}

  private:
	void updateAtlasTextures(ImDrawData *drawData)
	{
		if (drawData->Textures)
			for (ImTextureData *tex : *drawData->Textures)
			{
				if (tex->Status == ImTextureStatus_WantCreate)
				{
					if (tex->Format != ImTextureFormat_RGBA32)
						continue;
					auto t = std::make_unique<Texture>(makeTexture(tex->Pixels, tex->Width, tex->Height, false));
					tex->BackendUserData = t.get();
					tex->SetTexID((ImTextureID)t.get());
					atlasTextures[tex] = std::move(t);
					tex->SetStatus(ImTextureStatus_OK);
				}
				else if (tex->Status == ImTextureStatus_WantUpdates)
				{
					auto it = atlasTextures.find(tex);
					if (it != atlasTextures.end())
					{
						it->second->image = makeTexture(tex->Pixels, tex->Width, tex->Height, false).image;
						tex->SetStatus(ImTextureStatus_OK);
					}
				}
				else if (tex->Status == ImTextureStatus_WantDestroy)
				{
					auto it = atlasTextures.find(tex);
					if (it != atlasTextures.end())
					{
						retired.push_back(std::move(it->second));
						atlasTextures.erase(it);
					}
					tex->BackendUserData = nullptr;
					tex->SetTexID(ImTextureID_Invalid);
					tex->SetStatus(ImTextureStatus_Destroyed);
				}
			}
	}
	Texture makeTexture(const u8 *data, int w, int h, bool point)
	{
		MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
																						width:w
																					   height:h
																					mipmapped:NO];
		desc.usage = MTLTextureUsageShaderRead;
		desc.storageMode = metalCpuTextureStorageMode(MetalContext::Instance()->GetDevice());
		id<MTLTexture> image = [MetalContext::Instance()->GetDevice() newTextureWithDescriptor:desc];
		[image replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:data bytesPerRow:w * 4];
		return {image, point};
	}
	id<MTLRenderPipelineState> pipeline;
	id<MTLSamplerState> linear, nearest;
	std::vector<std::unique_ptr<DrawSnapshot>> pending;
	std::vector<std::unique_ptr<Texture>> retired;
	std::unordered_map<ImTextureData *, std::unique_ptr<Texture>> atlasTextures;
	std::unordered_map<std::string, std::unique_ptr<Texture>> textures;
};
} // namespace
void metalInitImGui() { imguiDriver.reset(new MetalDriver()); }
void metalDrawImGui(id<MTLRenderCommandEncoder> e, int width, int height)
{
	auto *d = static_cast<MetalDriver *>(imguiDriver.get());
	if (d)
		d->drawPending(e, width, height);
}

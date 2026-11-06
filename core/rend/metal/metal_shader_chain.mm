// Portions Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "metal_shader_chain.h"
#include "cfg/option.h"
#include "log/LogManager.h"
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <map>
#include <stdexcept>

namespace {
using namespace postprocess;

struct FrameUniforms {
	float sourceSize[2], outputSize[2];
	int32_t frameCount, rotation;
	float originalSize[2], viewportSize[2];
	int32_t hasDepth, padding;
	float parameters[MaxShaderParameterComponents];
};
static_assert(offsetof(FrameUniforms, parameters) == 48 && sizeof(FrameUniforms) == 1072,
		"Metal frame data must match the shared std140 shader block");

MTLPixelFormat pixelFormat(ShaderTargetFormat fallback, const std::string& name)
{
	if (name.empty())
		return fallback == ShaderTargetFormat::Float ? MTLPixelFormatRGBA16Float
				: fallback == ShaderTargetFormat::Srgb ? MTLPixelFormatRGBA8Unorm_sRGB : MTLPixelFormatRGBA8Unorm;
	const std::pair<const char *, MTLPixelFormat> formats[] {
		{ "R8_UNORM", MTLPixelFormatR8Unorm }, { "R8G8_UNORM", MTLPixelFormatRG8Unorm },
		{ "R16_UNORM", MTLPixelFormatR16Unorm }, { "R16G16_UNORM", MTLPixelFormatRG16Unorm },
		{ "R16_SFLOAT", MTLPixelFormatR16Float }, { "R16G16_SFLOAT", MTLPixelFormatRG16Float },
		{ "R32_SFLOAT", MTLPixelFormatR32Float }, { "R32G32_SFLOAT", MTLPixelFormatRG32Float },
		{ "R32_SINT", MTLPixelFormatR32Sint }, { "R32_UINT", MTLPixelFormatR32Uint },
		{ "R8G8B8A8_UNORM", MTLPixelFormatRGBA8Unorm }, { "R8G8B8A8_SRGB", MTLPixelFormatRGBA8Unorm_sRGB },
		{ "R16G16B16A16_SFLOAT", MTLPixelFormatRGBA16Float }, { "R16G16B16A16_UNORM", MTLPixelFormatRGBA16Unorm },
		{ "R32G32B32A32_SFLOAT", MTLPixelFormatRGBA32Float },
		{ "R32G32B32A32_SINT", MTLPixelFormatRGBA32Sint }, { "R32G32B32A32_UINT", MTLPixelFormatRGBA32Uint },
		{ "A2B10G10R10_UNORM_PACK32", MTLPixelFormatRGB10A2Unorm },
		{ "B10G11R11_UFLOAT_PACK32", MTLPixelFormatRG11B10Float }
	};
	const std::string canonical = canonicalShaderFormat(name);
	for (const auto& format : formats)
		if (canonical == format.first) return format.second;
	throw std::runtime_error("Metal does not support shader target format '" + name + "'.");
}

MTLTextureType textureType(SlangResourceType type)
{
	switch (type) {
	case SlangResourceType::Sampler3D: case SlangResourceType::Texture3D: case SlangResourceType::Image3D:
		return MTLTextureType3D;
	case SlangResourceType::Sampler2DArray: case SlangResourceType::Texture2DArray: case SlangResourceType::Image2DArray:
		return MTLTextureType2DArray;
	case SlangResourceType::SamplerCube: case SlangResourceType::TextureCube:
		return MTLTextureTypeCube;
	default: return MTLTextureType2D; // The compiler lowers 1D textures to height-one 2D images.
	}
}

bool integerFormat(MTLPixelFormat format)
{
	return format == MTLPixelFormatR32Sint || format == MTLPixelFormatR32Uint
			|| format == MTLPixelFormatRGBA32Sint || format == MTLPixelFormatRGBA32Uint;
}

MTLSamplerAddressMode addressMode(ShaderWrapMode mode)
{
	switch (mode) {
	case ShaderWrapMode::Repeat: return MTLSamplerAddressModeRepeat;
	case ShaderWrapMode::MirroredRepeat: return MTLSamplerAddressModeMirrorRepeat;
	case ShaderWrapMode::ClampToBorder: return MTLSamplerAddressModeClampToZero;
	default: return MTLSamplerAddressModeClampToEdge;
	}
}

MTLBlendFactor blendFactor(ShaderBlendFactor factor)
{
	const MTLBlendFactor values[] { MTLBlendFactorZero, MTLBlendFactorOne,
		MTLBlendFactorSourceColor, MTLBlendFactorOneMinusSourceColor,
		MTLBlendFactorDestinationColor, MTLBlendFactorOneMinusDestinationColor,
		MTLBlendFactorSourceAlpha, MTLBlendFactorOneMinusSourceAlpha,
		MTLBlendFactorDestinationAlpha, MTLBlendFactorOneMinusDestinationAlpha };
	return values[static_cast<unsigned>(factor)];
}

MTLPrimitiveType primitiveType(ShaderPrimitiveTopology topology)
{
	const MTLPrimitiveType values[] { MTLPrimitiveTypePoint, MTLPrimitiveTypeLine,
		MTLPrimitiveTypeLineStrip, MTLPrimitiveTypeTriangle, MTLPrimitiveTypeTriangleStrip };
	return values[static_cast<unsigned>(topology) - 1];
}

id<MTLLibrary> compileLibrary(id<MTLDevice> device, const std::string& source)
{
	NSError *error = nil;
	MTLCompileOptions *options = [MTLCompileOptions new];
	options.languageVersion = MTLLanguageVersion2_1;
	// Preserve the authored arithmetic, including NaN/Inf guards in shader packages.
	options.fastMathEnabled = NO;
	id<MTLLibrary> library = [device newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()]
			options:options error:&error];
	if (!library) throw std::runtime_error(metalErrorMessage("Metal shader compilation failed", error));
	return library;
}

id<MTLFunction> compileFunction(id<MTLDevice> device, const std::string& source, NSString *name)
{
	id<MTLLibrary> library = compileLibrary(device, source);
	id<MTLFunction> function = [library newFunctionWithName:name];
	if (!function) throw std::runtime_error("Metal shader entry point is missing.");
	return function;
}

constexpr char CopySource[] = R"(
#include <metal_stdlib>
using namespace metal;
struct V { float4 position [[position]]; float2 uv; };
vertex V copyVertex(uint i [[vertex_id]], constant uint &rotate [[buffer(0)]]) {
 float2 p[4] = { float2(-1,-1), float2(-1,1), float2(1,-1), float2(1,1) };
 float2 uv[4] = { float2(0,1), float2(0,0), float2(1,1), float2(1,0) };
 V v; v.position = float4(p[i],0,1); v.uv = rotate ? float2(1-uv[i].y,uv[i].x) : uv[i]; return v;
}
fragment float4 copyColor(V v [[stage_in]], texture2d<float> source [[texture(0)]], sampler s [[sampler(0)]]) {
 return source.sample(s,v.uv);
}
fragment float copyDepth(V v [[stage_in]], depth2d<float> source [[texture(0)]], sampler s [[sampler(0)]]) {
 return source.sample(s,v.uv);
}
fragment float4 copySigned(V v [[stage_in]], texture2d<int> source [[texture(0)]]) {
 uint2 size = uint2(source.get_width(),source.get_height());
 return float4(source.read(min(uint2(saturate(v.uv)*float2(size)),size-1)));
}
fragment float4 copyUnsigned(V v [[stage_in]], texture2d<uint> source [[texture(0)]]) {
 uint2 size = uint2(source.get_width(),source.get_height());
 return float4(source.read(min(uint2(saturate(v.uv)*float2(size)),size-1)));
}
)";

struct Target {
	id<MTLTexture> texture = nil;
	uint32_t frame = 0;
	bool valid = false;
	uint32_t layer = InvalidShaderPass;
};
struct Binding {
	ShaderPassInput input;
	uint32_t lookup = InvalidShaderPass;
	id<MTLSamplerState> sampler = nil;
};
struct Program {
	ShaderPass pass;
	std::vector<SlangParameter> parameters;
	std::vector<uint32_t> parameterIndices;
	std::vector<Binding> bindings;
	std::vector<MTLPixelFormat> formats;
	std::vector<bool> mipmaps;
	id<MTLRenderPipelineState> graphics = nil;
	id<MTLComputePipelineState> compute = nil;
	id<MTLDepthStencilState> stencil = nil;
	ShaderPassGeometry geometry;
	uint32_t historyDepth = 1;
	int width = 0, height = 0;
	bool displayGlsl = false, nativeStages = false;
};
} // namespace

struct MetalShaderChain::Impl {
	id<MTLDevice> device;
	id<MTLCommandQueue> queue;
	id<MTLSamplerState> nearest = nil, linear = nil;
	id<MTLFunction> copyVertex = nil;
	std::array<id<MTLFunction>, 4> copyFragments {};
	std::map<std::pair<MTLPixelFormat, unsigned>, id<MTLRenderPipelineState>> copyPipelines;
	std::vector<Program> programs;
	std::vector<id<MTLTexture>> lookups;
	std::vector<ShaderParameter> parameters;
	// Pass -> attachment -> completed-frame ring. Metal's tracked resources and
	// the single ordered queue serialize reuse without per-pass CPU/GPU waits.
	std::vector<std::vector<std::vector<Target>>> targets;
	std::vector<Target> sourceHistory;
	std::map<std::pair<int, int>, Target> stencils;
	Target display, depthCopy, displayDepth, mipSource, finalOutput;
	uint32_t historyDepth = 0, renderedFrames = 0;
	std::array<int, 4> frameSize {};
	std::vector<std::string> configuredPaths;
	uint64_t configuredGeneration = 0, parameterGeneration = 0;
	std::string activePath;

	Impl(id<MTLDevice> device, id<MTLCommandQueue> queue) : device(device), queue(queue) {}
	void resetTargets();
	bool update();
	Program compile(const ShaderChain& chain, size_t index);
	id<MTLSamplerState> sampler(const ShaderPassSampler& config);
	id<MTLTexture> allocate(int width, int height, MTLPixelFormat format, bool mipmaps,
			MTLTextureType type = MTLTextureType2D, uint32_t depth = 1, bool storage = false);
	void ensure(Target& target, int width, int height, MTLPixelFormat format, bool mipmaps,
			MTLTextureType type = MTLTextureType2D, uint32_t depth = 1, bool storage = false);
	void clear(id<MTLCommandBuffer> command, id<MTLTexture> texture);
	void blit(id<MTLCommandBuffer> command, id<MTLTexture> source, id<MTLTexture> destination);
	void copy(id<MTLCommandBuffer> command, id<MTLTexture> source, id<MTLTexture> destination,
			bool rotate = false, const ShaderPresentation *presentation = nullptr, bool sceneDepth = false, bool smooth = false);
	void generateMipmaps(id<MTLCommandBuffer> command, id<MTLTexture> texture);
	bool render(id<MTLTexture> source, id<MTLTexture> depth, ShaderFrameInfo frame,
			id<MTLTexture>& output, bool& transformed, bool& fullPresentation);
};

void MetalShaderChain::Impl::resetTargets()
{
	targets.clear(); sourceHistory.clear(); stencils.clear();
	display = {}; depthCopy = {}; displayDepth = {}; mipSource = {}; finalOutput = {};
}

id<MTLSamplerState> MetalShaderChain::Impl::sampler(const ShaderPassSampler& config)
{
	MTLSamplerDescriptor *desc = [MTLSamplerDescriptor new];
	desc.minFilter = config.minLinear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
	desc.magFilter = config.magLinear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
	desc.mipFilter = !config.useMipmaps ? MTLSamplerMipFilterNotMipmapped
			: config.mipLinear ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNearest;
	desc.sAddressMode = addressMode(config.wrapU); desc.tAddressMode = addressMode(config.wrapV); desc.rAddressMode = addressMode(config.wrapW);
	id<MTLSamplerState> result = [device newSamplerStateWithDescriptor:desc];
	if (!result) throw std::runtime_error("Metal could not create a shader sampler.");
	return result;
}

id<MTLTexture> MetalShaderChain::Impl::allocate(int width, int height, MTLPixelFormat format, bool mipmaps,
		MTLTextureType type, uint32_t depth, bool storage)
{
	if (mipmaps && integerFormat(format))
		throw std::runtime_error("Metal cannot generate filtered mipmaps for integer shader textures.");
	MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:width height:height mipmapped:NO];
	desc.textureType = type;
	desc.depth = type == MTLTextureType3D ? depth : 1;
	desc.arrayLength = type == MTLTextureType2DArray ? depth : 1;
	if (mipmaps) {
		unsigned size = std::max({ (unsigned)width, (unsigned)height, (unsigned)desc.depth });
		while (size > 1) { ++desc.mipmapLevelCount; size >>= 1; }
	}
	desc.storageMode = MTLStorageModePrivate;
	desc.usage = format == MTLPixelFormatStencil8 ? MTLTextureUsageRenderTarget
			: MTLTextureUsageShaderRead | (storage ? MTLTextureUsageShaderWrite : MTLTextureUsageRenderTarget);
	id<MTLTexture> texture = [device newTextureWithDescriptor:desc];
	if (!texture) throw std::runtime_error("Metal could not allocate a shader texture (format, dimensions or device limit).");
	return texture;
}

void MetalShaderChain::Impl::ensure(Target& target, int width, int height, MTLPixelFormat format, bool mipmaps,
		MTLTextureType type, uint32_t depth, bool storage)
{
	id<MTLTexture> t = target.texture;
	if (t && t.width == (NSUInteger)width && t.height == (NSUInteger)height && t.pixelFormat == format
			&& t.textureType == type && t.depth == (type == MTLTextureType3D ? depth : 1)
			&& t.arrayLength == (type == MTLTextureType2DArray ? depth : 1)
			&& ((t.mipmapLevelCount > 1) == (mipmaps && std::max({ width, height, (int)t.depth }) > 1))) return;
	target = { allocate(width, height, format, mipmaps, type, depth, storage) };
}

void MetalShaderChain::Impl::generateMipmaps(id<MTLCommandBuffer> command, id<MTLTexture> texture)
{
	if (texture.mipmapLevelCount <= 1) return;
	id<MTLBlitCommandEncoder> encoder = [command blitCommandEncoder];
	if (!encoder) throw std::runtime_error("Metal could not create a mipmap encoder.");
	[encoder generateMipmapsForTexture:texture];
	[encoder endEncoding];
}

void MetalShaderChain::Impl::clear(id<MTLCommandBuffer> command, id<MTLTexture> texture)
{
	if (texture.textureType == MTLTextureType2D && (texture.usage & MTLTextureUsageRenderTarget)) {
		for (NSUInteger level = 0; level < texture.mipmapLevelCount; ++level) {
			MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
			pass.colorAttachments[0].texture = texture;
			pass.colorAttachments[0].level = level;
			pass.colorAttachments[0].loadAction = MTLLoadActionClear;
			pass.colorAttachments[0].storeAction = MTLStoreActionStore;
			id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
			if (!encoder) throw std::runtime_error("Metal could not clear a shader target.");
			[encoder endEncoding];
		}
		return;
	}
	// Clear every level, including writable textures whose first dispatch may
	// touch only a subset. A private Metal allocation has undefined contents.
	NSUInteger bytes = 4;
	switch (texture.pixelFormat) {
	case MTLPixelFormatR8Unorm: bytes = 1; break;
	case MTLPixelFormatRG8Unorm: case MTLPixelFormatR16Unorm: case MTLPixelFormatR16Float: bytes = 2; break;
	case MTLPixelFormatRG32Float: case MTLPixelFormatRGBA16Float: case MTLPixelFormatRGBA16Unorm: bytes = 8; break;
	case MTLPixelFormatRGBA32Float: case MTLPixelFormatRGBA32Sint: case MTLPixelFormatRGBA32Uint: bytes = 16; break;
	default: break;
	}
	NSUInteger row = (texture.width * bytes + 255) & ~NSUInteger(255);
	NSUInteger image = row * texture.height;
	id<MTLBuffer> zeros = [device newBufferWithLength:image * texture.depth options:MTLResourceStorageModePrivate];
	if (!zeros) throw std::runtime_error("Metal could not allocate shader texture initialization storage.");
	id<MTLBlitCommandEncoder> encoder = [command blitCommandEncoder];
	if (!encoder) throw std::runtime_error("Metal could not create a texture initialization encoder.");
	[encoder fillBuffer:zeros range:NSMakeRange(0, zeros.length) value:0];
	NSUInteger slices = texture.textureType == MTLTextureTypeCube ? 6 : texture.arrayLength;
	for (NSUInteger level = 0; level < texture.mipmapLevelCount; ++level)
		for (NSUInteger slice = 0; slice < slices; ++slice)
			[encoder copyFromBuffer:zeros sourceOffset:0 sourceBytesPerRow:row sourceBytesPerImage:image
				sourceSize:MTLSizeMake(std::max(NSUInteger(1), texture.width >> level),
					std::max(NSUInteger(1), texture.height >> level), std::max(NSUInteger(1), texture.depth >> level))
				toTexture:texture destinationSlice:slice destinationLevel:level destinationOrigin:MTLOriginMake(0,0,0)];
	[encoder endEncoding];
}

void MetalShaderChain::Impl::blit(id<MTLCommandBuffer> command, id<MTLTexture> source, id<MTLTexture> destination)
{
	if (source == destination) return;
	id<MTLBlitCommandEncoder> encoder = [command blitCommandEncoder];
	if (!encoder) throw std::runtime_error("Metal could not create a shader copy encoder.");
	NSUInteger slices = source.textureType == MTLTextureTypeCube ? 6 : source.arrayLength;
	for (NSUInteger slice = 0; slice < slices; ++slice)
		[encoder copyFromTexture:source sourceSlice:slice sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
			sourceSize:MTLSizeMake(source.width, source.height, source.depth) toTexture:destination
			destinationSlice:slice destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
	[encoder endEncoding];
}

void MetalShaderChain::Impl::copy(id<MTLCommandBuffer> command, id<MTLTexture> source, id<MTLTexture> destination,
		bool rotate, const ShaderPresentation *presentation, bool sceneDepth, bool smooth)
{
	// Integer compute outputs need typed reads, followed by a numeric cast to
	// display color. They cannot be bound to a floating-point texture argument.
	const unsigned kind = sceneDepth ? 1 : source.pixelFormat == MTLPixelFormatR32Sint || source.pixelFormat == MTLPixelFormatRGBA32Sint
			? 2 : source.pixelFormat == MTLPixelFormatR32Uint || source.pixelFormat == MTLPixelFormatRGBA32Uint ? 3 : 0;
	const auto key = std::make_pair(destination.pixelFormat, kind);
	auto found = copyPipelines.find(key);
	if (found == copyPipelines.end()) {
		if (!copyVertex) {
			id<MTLLibrary> library = compileLibrary(device, CopySource);
			copyVertex = [library newFunctionWithName:@"copyVertex"];
			copyFragments = { [library newFunctionWithName:@"copyColor"], [library newFunctionWithName:@"copyDepth"],
				[library newFunctionWithName:@"copySigned"], [library newFunctionWithName:@"copyUnsigned"] };
			if (!copyVertex || std::any_of(copyFragments.begin(), copyFragments.end(), [](auto function) { return !function; }))
				throw std::runtime_error("Metal shader copy entry point is missing.");
		}
		MTLRenderPipelineDescriptor *desc = [MTLRenderPipelineDescriptor new];
		desc.vertexFunction = copyVertex; desc.fragmentFunction = copyFragments[kind];
		desc.colorAttachments[0].pixelFormat = destination.pixelFormat;
		NSError *error = nil;
		id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:desc error:&error];
		if (!pipeline) throw std::runtime_error(metalErrorMessage("Metal shader copy pipeline failed", error));
		found = copyPipelines.emplace(key, pipeline).first;
	}
	MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
	pass.colorAttachments[0].texture = destination;
	pass.colorAttachments[0].loadAction = MTLLoadActionClear;
	pass.colorAttachments[0].storeAction = MTLStoreActionStore;
	const auto color = presentation ? presentation->borderColor : std::array<float,4>{0,0,0,0};
	pass.colorAttachments[0].clearColor = MTLClearColorMake(color[0],color[1],color[2],color[3]);
	id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
	if (!encoder) throw std::runtime_error("Metal could not create a shader copy pass.");
	if (presentation)
		[encoder setViewport:MTLViewport{ (double)presentation->x, (double)presentation->y,
			(double)presentation->contentWidth, (double)presentation->contentHeight, 0, 1 }];
	[encoder setRenderPipelineState:found->second];
	uint32_t rotation = rotate ? 1 : 0;
	[encoder setVertexBytes:&rotation length:sizeof(rotation) atIndex:0];
	[encoder setFragmentTexture:source atIndex:0];
	[encoder setFragmentSamplerState:smooth ? linear : nearest atIndex:0];
	[encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
	[encoder endEncoding];
}

Program MetalShaderChain::Impl::compile(const ShaderChain& chain, size_t index)
{
	Program program;
	program.pass = chain.passes[index];
	const auto& pass = program.pass;
	ShaderPassSource loaded;
	std::string error;
	const ShaderPassSource& source = index < chain.sources.size() ? chain.sources[index] : loaded;
	if (&source == &loaded && !loadShaderPassSource(pass, loaded, error)) throw std::runtime_error(error);
	program.displayGlsl = source.displayGlsl;
	program.nativeStages = source.nativeFxStages;
	program.parameters = source.slangParameters;
	if (program.parameters.empty())
		for (const auto& value : chain.parameters)
			if (value.layerIndex == pass.layerIndex) {
				SlangParameter parameter;
				parameter.name = value.name; parameter.type = value.type;
				parameter.defaultValue.values = value.values; parameter.defaultValue.componentCount = value.componentCount;
				program.parameters.push_back(std::move(parameter));
			}
	for (const auto& parameter : program.parameters) {
		const auto found = std::find_if(chain.parameters.begin(), chain.parameters.end(), [&](const auto& value) {
			return value.name == parameter.name && value.layerIndex == pass.layerIndex;
		});
		program.parameterIndices.push_back(found == chain.parameters.end() ? InvalidShaderPass : (uint32_t)(found - chain.parameters.begin()));
	}
	std::vector<ShaderPassInput> inputs = pass.inputs;
	if (std::none_of(inputs.begin(), inputs.end(), [](const auto& input) { return input.bindingName == "Source"; }))
		inputs.push_back({ "Source", ShaderInputKind::Source });
	std::stable_sort(inputs.begin(), inputs.end(), [](const auto& left, const auto& right) {
		return left.bindingName == "Source" && right.bindingName != "Source";
	});
	for (const auto& resource : source.slangResources)
		if (resource.arrayCount != 1) throw std::runtime_error("Metal shader resource arrays are unsupported: " + resource.name);
	if (pass.storageBindings.size() > 8 || pass.colorOutputCount > 8)
		throw std::runtime_error("Metal shader passes support eight color outputs and eight writable textures.");
	for (const auto& input : inputs) {
		Binding binding;
		binding.input = input;
		ShaderPassSampler config;
		config.minLinear = config.magLinear = config.mipLinear = pass.linearFilter;
		config.useMipmaps = pass.useMipmaps;
		config.wrapU = config.wrapV = config.wrapW = pass.wrapMode;
		if (input.kind == ShaderInputKind::LookupTexture) {
			const auto found = std::find_if(chain.textures.begin(), chain.textures.end(), [&](const auto& texture) { return texture.name == input.resourceName; });
			if (found == chain.textures.end()) throw std::runtime_error("Unresolved Metal lookup texture: " + input.resourceName);
			binding.lookup = (uint32_t)(found - chain.textures.begin());
			config.minLinear = config.magLinear = config.mipLinear = found->linearFilter;
			config.useMipmaps = found->useMipmaps;
			config.wrapU = config.wrapV = config.wrapW = found->wrapMode;
		}
		const auto explicitSampler = std::find_if(pass.samplers.begin(), pass.samplers.end(), [&](const auto& value) { return value.bindingName == input.bindingName; });
		if (explicitSampler != pass.samplers.end()) config = *explicitSampler;
		if (input.numericType != ShaderNumericType::Float && (config.minLinear || config.magLinear || config.mipLinear))
			throw std::runtime_error("Integer shader textures require nearest filtering: " + input.bindingName);
		binding.sampler = sampler(config);
		program.bindings.push_back(std::move(binding));
	}
	const size_t outputs = pass.outputs.empty() ? 1 : pass.outputs.size();
	for (const auto& binding : pass.storageBindings)
		if (binding.second >= outputs || binding.second < pass.colorOutputCount)
			throw std::runtime_error("Metal shader storage binding does not reference a writable output.");
	for (size_t output = 0; output < outputs; ++output) {
		const auto config = pass.outputs.empty() ? ShaderPassOutput{} : pass.outputs[output];
		if (output < pass.colorOutputCount && textureType(config.type) != MTLTextureType2D)
			throw std::runtime_error("Metal raster shader outputs must be two-dimensional textures.");
		program.formats.push_back(pixelFormat(pass.outputs.empty() ? pass.targetFormat : config.targetFormat,
				pass.outputs.empty() ? pass.targetFormatName : config.targetFormatName));
	}
	MetalShaderSource metal;
	if (!translateShaderPassToMetal(source, program.parameters, inputs, metal, error)) throw std::runtime_error(error);
	NSError *diagnostic = nil;
	if (pass.compute) {
		id<MTLFunction> function = compileFunction(device, metal.compute, @"hollycast_main");
		program.compute = [device newComputePipelineStateWithFunction:function error:&diagnostic];
		if (!program.compute) throw std::runtime_error(metalErrorMessage("Metal compute pipeline failed", diagnostic));
		const auto local = pass.localSize;
		const MTLSize limits = device.maxThreadsPerThreadgroup;
		if (local[0] == 0 || local[1] == 0 || local[2] == 0 || local[0] > limits.width || local[1] > limits.height || local[2] > limits.depth
				|| (uint64_t)local[0] * local[1] * local[2] > program.compute.maxTotalThreadsPerThreadgroup)
			throw std::runtime_error("The shader compute workgroup exceeds this Metal device's limits.");
	} else {
		MTLRenderPipelineDescriptor *desc = [MTLRenderPipelineDescriptor new];
		desc.vertexFunction = compileFunction(device, metal.vertex, @"hollycast_main");
		desc.fragmentFunction = compileFunction(device, metal.fragment, @"hollycast_main");
		if (!program.nativeStages) {
			MTLVertexDescriptor *vertex = [MTLVertexDescriptor vertexDescriptor];
			vertex.attributes[0].format = vertex.attributes[1].format = MTLVertexFormatFloat2;
			vertex.attributes[0].bufferIndex = vertex.attributes[1].bufferIndex = 1;
			vertex.attributes[1].offset = 2 * sizeof(float);
			vertex.layouts[1].stride = 4 * sizeof(float);
			desc.vertexDescriptor = vertex;
		}
		for (size_t output = 0; output < pass.colorOutputCount; ++output) {
			const auto& state = pass.renderState.attachments[output];
			MTLRenderPipelineColorAttachmentDescriptor *attachment = desc.colorAttachments[output];
			attachment.pixelFormat = program.formats[output];
			attachment.blendingEnabled = pass.renderState.reflected ? state.blendEnabled : pass.blendEnabled;
			attachment.sourceRGBBlendFactor = blendFactor(pass.renderState.reflected ? state.source : pass.sourceBlend);
			attachment.destinationRGBBlendFactor = blendFactor(pass.renderState.reflected ? state.destination : pass.destinationBlend);
			attachment.rgbBlendOperation = (MTLBlendOperation)(pass.renderState.reflected ? state.operation : pass.blendOperation);
			attachment.sourceAlphaBlendFactor = blendFactor(state.sourceAlpha);
			attachment.destinationAlphaBlendFactor = blendFactor(state.destinationAlpha);
			attachment.alphaBlendOperation = (MTLBlendOperation)state.alphaOperation;
			// Metal's color-mask bits run in the reverse order to the shared RGBA mask.
			attachment.writeMask = (MTLColorWriteMask)(((state.writeMask & 1) ? MTLColorWriteMaskRed : 0)
					| ((state.writeMask & 2) ? MTLColorWriteMaskGreen : 0) | ((state.writeMask & 4) ? MTLColorWriteMaskBlue : 0)
					| ((state.writeMask & 8) ? MTLColorWriteMaskAlpha : 0));
		}
		if (pass.renderState.stencilEnabled) desc.stencilAttachmentPixelFormat = MTLPixelFormatStencil8;
		program.graphics = [device newRenderPipelineStateWithDescriptor:desc error:&diagnostic];
		if (!program.graphics) throw std::runtime_error(metalErrorMessage("Metal graphics pipeline failed", diagnostic));
		MTLDepthStencilDescriptor *depth = [MTLDepthStencilDescriptor new];
		if (pass.renderState.stencilEnabled) {
			MTLStencilDescriptor *stencil = [MTLStencilDescriptor new];
			const MTLStencilOperation operations[] { MTLStencilOperationZero, MTLStencilOperationKeep, MTLStencilOperationReplace,
				MTLStencilOperationIncrementClamp, MTLStencilOperationDecrementClamp, MTLStencilOperationInvert,
				MTLStencilOperationIncrementWrap, MTLStencilOperationDecrementWrap };
			stencil.stencilCompareFunction = (MTLCompareFunction)pass.renderState.stencilComparison;
			stencil.stencilFailureOperation = operations[(unsigned)pass.renderState.stencilFail];
			stencil.depthFailureOperation = operations[(unsigned)pass.renderState.stencilDepthFail];
			stencil.depthStencilPassOperation = operations[(unsigned)pass.renderState.stencilPass];
			stencil.readMask = pass.renderState.stencilReadMask; stencil.writeMask = pass.renderState.stencilWriteMask;
			depth.frontFaceStencil = depth.backFaceStencil = stencil;
		}
		program.stencil = [device newDepthStencilStateWithDescriptor:depth];
		if (!program.stencil) throw std::runtime_error("Metal could not create shader depth/stencil state.");
	}
	return program;
}

bool MetalShaderChain::Impl::update()
{
	const auto requested = configuredShaderPaths();
	const uint64_t generation = shaderChainReloadGeneration();
	if (requested == configuredPaths && generation == configuredGeneration) return !programs.empty();
	if (!requested.empty() && !prepareShaderCompilation()) return !programs.empty();
	configuredPaths = requested; configuredGeneration = generation;
	if (requested.empty()) {
		programs.clear(); lookups.clear(); parameters.clear(); resetTargets(); activePath.clear();
		renderedFrames = 0;
		reportShaderChainStatus({}, {});
		return false;
	}
	try {
		std::string error;
		ShaderChain chain;
		if (!loadConfiguredShaderChain(requested, generation, chain, error) || !preflightShaderAssets(chain, error))
			throw std::runtime_error(error);
		applyShaderChainParameterValues(chain, config::PostProcessShaderParameters.get());
		std::vector<Program> candidate;
		const auto requirements = resolveShaderPassOutputMipmaps(chain);
		for (size_t index = 0; index < chain.passes.size(); ++index) {
			try { candidate.push_back(compile(chain, index)); }
			catch (const std::exception& e) { throw std::runtime_error(chain.passes[index].sourcePath + ": " + e.what()); }
			candidate.back().mipmaps = requirements[index];
		}
		if (!nearest) nearest = sampler({});
		if (!linear) { ShaderPassSampler s; s.minLinear = s.magLinear = true; linear = sampler(s); }
		std::vector<id<MTLTexture>> candidateLookups;
		id<MTLCommandBuffer> upload = [queue commandBuffer];
		if (!upload) throw std::runtime_error("Metal could not create a shader upload command buffer.");
		for (const auto& texture : chain.textures) {
			const auto data = loadShaderTextureData(texture, error);
			if (!data) throw std::runtime_error(error);
			const auto type = textureType(data->type);
			const uint32_t slices = type == MTLTextureTypeCube ? 6 : type == MTLTextureType2DArray ? data->layers : 1;
			const uint32_t depth = type == MTLTextureType3D ? data->depth : 1;
			id<MTLTexture> gpu = allocate(data->width, data->height, pixelFormat(ShaderTargetFormat::Default, data->formatName),
					texture.useMipmaps, type, type == MTLTextureType2DArray ? slices : depth);
			const size_t texelBytes = shaderFormatBytes(data->formatName);
			const size_t rawRow = data->width * texelBytes, row = (rawRow + 255) & ~size_t(255), image = row * data->height;
			id<MTLBuffer> staging = [device newBufferWithLength:image * depth * slices options:MTLResourceStorageModeShared];
			if (!staging) throw std::runtime_error("Metal could not allocate lookup texture upload storage.");
			for (size_t layer = 0; layer < depth * slices; ++layer)
				for (size_t y = 0; y < data->height; ++y)
					std::memcpy((uint8_t *)staging.contents + layer * image + y * row,
						data->pixels.data() + (layer * data->height + y) * rawRow, rawRow);
			id<MTLBlitCommandEncoder> encoder = [upload blitCommandEncoder];
			if (!encoder) throw std::runtime_error("Metal could not create a lookup upload encoder.");
			for (uint32_t slice = 0; slice < slices; ++slice)
				[encoder copyFromBuffer:staging sourceOffset:slice * image * depth sourceBytesPerRow:row sourceBytesPerImage:image
					sourceSize:MTLSizeMake(data->width, data->height, depth) toTexture:gpu destinationSlice:slice
					destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
			[encoder endEncoding];
			generateMipmaps(upload, gpu);
			candidateLookups.push_back(gpu);
		}
		// Activation is transactional, including uploads. This one-time wait is
		// covered by the compiling notice; normal frames never wait for the GPU.
		[upload commit]; [upload waitUntilCompleted];
		if (upload.status != MTLCommandBufferStatusCompleted)
			throw std::runtime_error(metalErrorMessage("Metal shader texture upload failed", upload.error));
		uint32_t candidateHistoryDepth = 0;
		for (const auto& pass : chain.passes) {
			for (const auto& input : pass.inputs)
				if (input.kind == ShaderInputKind::History || input.kind == ShaderInputKind::Feedback) {
					const uint32_t count = input.kind == ShaderInputKind::Feedback ? 2 : input.historyOffset + 1;
					if (input.sourcePass == InvalidShaderPass) candidateHistoryDepth = std::max(candidateHistoryDepth, count);
					else candidate.at(input.sourcePass).historyDepth = std::max(candidate.at(input.sourcePass).historyDepth, count);
				}
			for (const auto& output : pass.outputs)
				if (output.previousFrame && output.previousPass != InvalidShaderPass)
					candidate.at(output.previousPass).historyDepth = std::max(2u, candidate.at(output.previousPass).historyDepth);
		}
		programs = std::move(candidate); lookups = std::move(candidateLookups); parameters = std::move(chain.parameters);
		historyDepth = candidateHistoryDepth;
		resetTargets();
		renderedFrames = 0;
		activePath = shaderSelectionLabel(requested);
		reportShaderChainStatus(activePath, {}, parameters, chain.techniques, chain.selectedTechnique);
		parameterGeneration = shaderChainStatusGeneration();
	} catch (const std::exception& error) {
		reportShaderChainStatus(activePath, error.what());
		WARN_LOG(RENDERER, "Metal shader activation failed: %s", error.what());
	}
	return !programs.empty();
}

bool MetalShaderChain::Impl::render(id<MTLTexture> source, id<MTLTexture> depth, ShaderFrameInfo frame,
		id<MTLTexture>& output, bool& transformed, bool& fullPresentation)
{
	output = nil; transformed = fullPresentation = false;
	if (!update() || !source || frame.sourceWidth <= 0 || frame.sourceHeight <= 0) return false;
	prepareShaderPresentation(frame, programs.front().pass.composeDisplayInput);
	const std::array<int,4> size { frame.sourceWidth, frame.sourceHeight, frame.viewportWidth, frame.viewportHeight };
	if (frameSize != size) { resetTargets(); frameSize = size; }
	frame.frameCount = renderedFrames;
	const auto statusGeneration = shaderChainStatusGeneration();
	if (statusGeneration != parameterGeneration) {
		const auto status = getShaderChainStatus();
		if (status.activePath == activePath && status.error.empty()) parameters = status.parameters;
		parameterGeneration = statusGeneration;
	}
	updateShaderHostParameters(parameters, frame);
	std::string error;
	try {
		const bool needsDepth = std::any_of(programs.begin(), programs.end(), [](const auto& program) {
			return std::any_of(program.bindings.begin(), program.bindings.end(), [](const auto& binding) { return binding.input.kind == ShaderInputKind::Depth; });
		});
		if (needsDepth && !depth) throw std::runtime_error("This frame has no scene depth available for the selected shader.");
		const bool needsDisplay = shouldComposeDisplayInput(frame, false) && std::any_of(programs.begin(), programs.end(),
				[](const auto& program) { return program.pass.composeDisplayInput; });
		const bool mipmappedInput = std::any_of(programs.begin(), programs.end(), [](const auto& program) {
			return program.pass.useMipmaps || std::any_of(program.pass.samplers.begin(), program.pass.samplers.end(), [](const auto& sampler) { return sampler.useMipmaps; });
		});
		uint64_t bytes = 0;
		auto budget = [&](int width, int height, ShaderTargetFormat format, const std::string& name, bool mipmaps, uint32_t copies) {
			if (!accumulateShaderTargetBytes(width, height, format, name, mipmaps, copies, bytes, error)) throw std::runtime_error(error);
		};
		if (needsDisplay) budget(frame.viewportWidth, frame.viewportHeight, ShaderTargetFormat::Default, {}, mipmappedInput, 1);
		if (needsDepth) {
			budget(frame.sourceWidth, frame.sourceHeight, ShaderTargetFormat::Default, "R32_SFLOAT", false, 1);
			if (needsDisplay) budget(frame.viewportWidth, frame.viewportHeight, ShaderTargetFormat::Default, "R32_SFLOAT", false, 1);
		}
		if (mipmappedInput) budget(frame.sourceWidth, frame.sourceHeight, ShaderTargetFormat::Default, {}, true, 1);
		const int historyWidth = needsDisplay && programs.front().pass.composeDisplayInput ? frame.viewportWidth : frame.sourceWidth;
		const int historyHeight = needsDisplay && programs.front().pass.composeDisplayInput ? frame.viewportHeight : frame.sourceHeight;
		budget(historyWidth, historyHeight, ShaderTargetFormat::Default, {}, false, historyDepth);
		ShaderFrameInfo sizeFrame = frame;
		bool displayReached = false;
		for (auto& program : programs) {
			if (program.pass.composeDisplayInput && !displayReached) {
				displayReached = true;
				if (needsDisplay) { sizeFrame.sourceWidth = frame.viewportWidth; sizeFrame.sourceHeight = frame.viewportHeight; }
			}
			if (!program.geometry.resolveTargetSize(program.pass, sizeFrame, program.width, program.height, error)) throw std::runtime_error(error);
			for (size_t output = 0; output < program.formats.size(); ++output) {
				const auto config = program.pass.outputs.empty() ? ShaderPassOutput{} : program.pass.outputs[output];
				budget(program.width, program.height, program.pass.outputs.empty() ? program.pass.targetFormat : config.targetFormat,
						program.pass.outputs.empty() ? program.pass.targetFormatName : config.targetFormatName,
						program.mipmaps[output], program.historyDepth * config.depth);
			}
			if (program.pass.renderState.stencilEnabled) budget(program.width, program.height, ShaderTargetFormat::Default, "R8_UNORM", false, 1);
			if (program.pass.updatesSource) { sizeFrame.sourceWidth = program.width; sizeFrame.sourceHeight = program.height; }
		}
		// Readback and presentation consume RGBA8 even when an effect's final target is floating point.
		budget(sizeFrame.sourceWidth, sizeFrame.sourceHeight, ShaderTargetFormat::Default, {}, false, 1);
		id<MTLCommandBuffer> command = [queue commandBuffer];
		if (!command) throw std::runtime_error("Metal could not create a shader command buffer.");
		targets.resize(programs.size());
		for (size_t index = 0; index < programs.size(); ++index) {
			auto& program = programs[index];
			targets[index].resize(program.formats.size());
			for (size_t outputIndex = 0; outputIndex < program.formats.size(); ++outputIndex) {
				const auto config = program.pass.outputs.empty() ? ShaderPassOutput{} : program.pass.outputs[outputIndex];
				auto& ring = targets[index][outputIndex];
				ring.resize(program.historyDepth);
				for (auto& target : ring) {
					ensure(target, program.width, program.height, program.formats[outputIndex], program.mipmaps[outputIndex],
						textureType(config.type), config.depth, outputIndex >= program.pass.colorOutputCount);
					if (!target.valid) clear(command, target.texture);
				}
			}
		}
		id<MTLTexture> effectiveDepth = nil;
		if (needsDepth) {
			ensure(depthCopy, frame.sourceWidth, frame.sourceHeight, MTLPixelFormatR32Float, false);
			copy(command, depth, depthCopy.texture, false, nullptr, depth.pixelFormat == MTLPixelFormatDepth32Float_Stencil8);
			effectiveDepth = depthCopy.texture;
		}
		id<MTLTexture> input = source;
		if (mipmappedInput) {
			ensure(mipSource, frame.sourceWidth, frame.sourceHeight, MTLPixelFormatRGBA8Unorm, true);
			copy(command, source, mipSource.texture); generateMipmaps(command, mipSource.texture); input = mipSource.texture;
		}
		id<MTLTexture> original = input;
		ShaderFrameInfo passFrame = frame;
		bool composed = false;
		displayReached = false;
		auto compose = [&] {
			if (displayReached) return;
			displayReached = true;
			if (!needsDisplay) return;
			ensure(display, frame.viewportWidth, frame.viewportHeight, MTLPixelFormatRGBA8Unorm, mipmappedInput);
			copy(command, input, display.texture, frame.rotationPending, frame.presentationExpanded ? &frame.presentation : nullptr, false, config::LinearInterpolation);
			generateMipmaps(command, display.texture);
			input = original = display.texture;
			passFrame.sourceWidth = frame.viewportWidth; passFrame.sourceHeight = frame.viewportHeight;
			if (needsDepth) {
				ensure(displayDepth, frame.viewportWidth, frame.viewportHeight, MTLPixelFormatR32Float, false);
				auto presentation = frame.presentation;
				presentation.borderColor = {0,0,0,0}; // Scene depth uses reverse Z; borders are far depth.
				copy(command, effectiveDepth, displayDepth.texture, frame.rotationPending, frame.presentationExpanded ? &presentation : nullptr);
				effectiveDepth = displayDepth.texture;
			}
			composed = true;
		};
		if (programs.front().pass.composeDisplayInput) compose();
		if (historyDepth) {
			sourceHistory.resize(historyDepth);
			auto& history = sourceHistory[frame.frameCount % historyDepth];
			ensure(history, (int)original.width, (int)original.height, MTLPixelFormatRGBA8Unorm, false);
			copy(command, original, history.texture);
			history.valid = true; history.frame = frame.frameCount;
		}
		for (size_t index = 0; index < programs.size(); ++index) {
			auto& program = programs[index];
			const auto& pass = program.pass;
			if (pass.composeDisplayInput) compose();
			if (index == 0 || pass.layerIndex != programs[index - 1].pass.layerIndex) original = input;
			const auto current = frame.frameCount % program.historyDepth;
			auto& passTargets = targets[index];
			for (size_t out = 0; out < passTargets.size(); ++out) {
				auto& destination = passTargets[out][current];
				if (!pass.outputs.empty()) {
					const auto& config = pass.outputs[out];
					if (config.previousPass != InvalidShaderPass && (!config.previousFrame || frame.frameCount > 0)) {
						const uint32_t requested = frame.frameCount - (config.previousFrame ? 1 : 0);
						const auto& previous = targets.at(config.previousPass).at(config.previousOutput).at(requested % programs.at(config.previousPass).historyDepth);
						if (previous.valid && previous.frame == requested && previous.texture.width == destination.texture.width
								&& previous.texture.height == destination.texture.height && previous.texture.depth == destination.texture.depth
								&& previous.texture.textureType == destination.texture.textureType && previous.texture.arrayLength == destination.texture.arrayLength
								&& previous.texture.pixelFormat == destination.texture.pixelFormat)
							blit(command, previous.texture, destination.texture);
					}
				}
				if (pass.renderState.clearRenderTargets) clear(command, destination.texture);
			}
			if ((pass.preserveBackbuffer || pass.blendEnabled) && pass.updatesSource && !pass.renderState.clearRenderTargets)
				copy(command, input, passTargets[0][current].texture, false, nullptr, false, pass.linearFilter);
			FrameUniforms uniforms {};
			uniforms.sourceSize[0] = passFrame.sourceWidth; uniforms.sourceSize[1] = passFrame.sourceHeight;
			uniforms.outputSize[0] = program.width; uniforms.outputSize[1] = program.height;
			uniforms.frameCount = pass.frameCountModulo ? frame.frameCount % pass.frameCountModulo : frame.frameCount;
			uniforms.rotation = frame.rotationPending ? 1 : 0;
			uniforms.originalSize[0] = program.displayGlsl ? frame.sourceWidth : original.width;
			uniforms.originalSize[1] = program.displayGlsl ? frame.sourceHeight : original.height;
			uniforms.viewportSize[0] = frame.viewportWidth; uniforms.viewportSize[1] = frame.viewportHeight;
			uniforms.hasDepth = depth ? 1 : 0;
			size_t offset = 0;
			for (size_t p = 0; p < program.parameters.size(); ++p) {
				const auto& parameter = program.parameters[p];
				const auto count = shaderParameterComponentCount(parameter.type);
				if (offset + count > MaxShaderParameterComponents) throw std::runtime_error("Metal shader parameter storage overflow.");
				const auto configured = program.parameterIndices[p];
				const auto& values = configured < parameters.size() ? parameters[configured].values : parameter.defaultValue.values;
				std::copy_n(values.begin(), count, uniforms.parameters + offset); offset += count;
			}
			std::array<id<MTLTexture>,16> reads {};
			for (size_t slot = 0; slot < program.bindings.size(); ++slot) {
				const auto& binding = program.bindings[slot];
				const auto& resource = binding.input;
				id<MTLTexture> texture = nil;
				switch (resource.kind) {
				case ShaderInputKind::Source: texture = input; break;
				case ShaderInputKind::Original: texture = original; break;
				case ShaderInputKind::Depth: texture = effectiveDepth; break;
				case ShaderInputKind::LookupTexture: texture = lookups.at(binding.lookup); break;
				case ShaderInputKind::PassOutput: {
					const auto& target = targets.at(resource.sourcePass).at(resource.sourceOutput).at(frame.frameCount % programs.at(resource.sourcePass).historyDepth);
					if (!target.valid || target.frame != frame.frameCount) throw std::runtime_error("Metal shader input was not rendered in the current frame.");
					texture = target.texture; break;
				}
				case ShaderInputKind::History: case ShaderInputKind::Feedback: {
					const uint32_t age = resource.kind == ShaderInputKind::Feedback ? 1 : resource.historyOffset;
					const auto& ring = resource.sourcePass == InvalidShaderPass ? sourceHistory : targets.at(resource.sourcePass).at(resource.sourceOutput);
					if (ring.empty()) throw std::runtime_error("Metal shader history is unavailable.");
					const uint32_t requested = frame.frameCount - age;
					const auto& previous = ring[requested % ring.size()];
					texture = frame.frameCount >= age && previous.valid && previous.frame == requested ? previous.texture
							: program.nativeStages && resource.sourcePass != InvalidShaderPass ? previous.texture : original;
					break;
				}
				}
				if (!texture) throw std::runtime_error("Metal shader resource is unavailable: " + resource.bindingName);
				reads[slot] = texture;
			}
			if (pass.compute) {
				std::array<uint32_t,3> groups;
				if (!program.geometry.resolveDispatchGroups(pass, passFrame, groups, error)) throw std::runtime_error(error);
				id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
				if (!encoder) throw std::runtime_error("Metal could not create a shader compute encoder.");
				[encoder setComputePipelineState:program.compute];
				[encoder setBytes:&uniforms length:sizeof(uniforms) atIndex:0];
				for (size_t slot = 0; slot < program.bindings.size(); ++slot) {
					[encoder setTexture:reads[slot] atIndex:slot];
					[encoder setSamplerState:program.bindings[slot].sampler atIndex:slot];
				}
				for (size_t slot = 0; slot < pass.storageBindings.size(); ++slot)
					[encoder setTexture:passTargets.at(pass.storageBindings[slot].second)[current].texture atIndex:16 + slot];
				[encoder dispatchThreadgroups:MTLSizeMake(groups[0],groups[1],groups[2])
					threadsPerThreadgroup:MTLSizeMake(pass.localSize[0],pass.localSize[1],pass.localSize[2])];
				[encoder endEncoding];
			} else {
				MTLRenderPassDescriptor *descriptor = [MTLRenderPassDescriptor renderPassDescriptor];
				for (size_t out = 0; out < pass.colorOutputCount; ++out) {
					descriptor.colorAttachments[out].texture = passTargets[out][current].texture;
					descriptor.colorAttachments[out].loadAction = MTLLoadActionLoad;
					descriptor.colorAttachments[out].storeAction = MTLStoreActionStore;
				}
				if (pass.renderState.stencilEnabled) {
					auto& stencil = stencils[{ program.width, program.height }];
					ensure(stencil, program.width, program.height, MTLPixelFormatStencil8, false);
					descriptor.stencilAttachment.texture = stencil.texture;
					descriptor.stencilAttachment.loadAction = !stencil.valid || stencil.frame != frame.frameCount || stencil.layer != pass.layerIndex ? MTLLoadActionClear : MTLLoadActionLoad;
					descriptor.stencilAttachment.storeAction = MTLStoreActionStore;
					stencil.valid = true; stencil.frame = frame.frameCount; stencil.layer = pass.layerIndex;
				}
				id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:descriptor];
				if (!encoder) throw std::runtime_error("Metal could not create a shader render encoder.");
				[encoder setRenderPipelineState:program.graphics];
				[encoder setDepthStencilState:program.stencil];
				[encoder setStencilReferenceValue:pass.renderState.stencilReference];
				[encoder setCullMode:MTLCullModeNone];
				[encoder setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:0];
				[encoder setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:0];
				if (!program.nativeStages) {
					const float vertices[] { -1,-1,0,1, -1,1,0,0, 1,-1,1,1, 1,1,1,0 };
					[encoder setVertexBytes:vertices length:sizeof(vertices) atIndex:1];
				}
				for (size_t slot = 0; slot < program.bindings.size(); ++slot) {
					[encoder setVertexTexture:reads[slot] atIndex:slot]; [encoder setFragmentTexture:reads[slot] atIndex:slot];
					[encoder setVertexSamplerState:program.bindings[slot].sampler atIndex:slot];
					[encoder setFragmentSamplerState:program.bindings[slot].sampler atIndex:slot];
				}
				for (size_t slot = 0; slot < pass.storageBindings.size(); ++slot)
					[encoder setFragmentTexture:passTargets.at(pass.storageBindings[slot].second)[current].texture atIndex:16 + slot];
				[encoder drawPrimitives:program.nativeStages ? primitiveType(pass.renderState.topology) : MTLPrimitiveTypeTriangleStrip
					vertexStart:0 vertexCount:program.nativeStages ? pass.renderState.vertexCount : 4];
				[encoder endEncoding];
			}
			for (size_t out = 0; out < passTargets.size(); ++out) {
				auto& target = passTargets[out][current];
				if (program.mipmaps[out] && pass.renderState.generateMipmaps) generateMipmaps(command, target.texture);
				target.valid = true; target.frame = frame.frameCount;
			}
			if (pass.updatesSource) { input = passTargets[0][current].texture; passFrame.sourceWidth = program.width; passFrame.sourceHeight = program.height; }
		}
		if (input.pixelFormat != MTLPixelFormatRGBA8Unorm) {
			ensure(finalOutput, (int)input.width, (int)input.height, MTLPixelFormatRGBA8Unorm, false);
			copy(command, input, finalOutput.texture); input = finalOutput.texture;
		}
		const std::string path = activePath;
		[command addCompletedHandler:^(id<MTLCommandBuffer> completed) {
			if (completed.status == MTLCommandBufferStatusError && getShaderChainStatusSummary().activePath == path)
				reportShaderFrameStatus(path, metalErrorMessage("Metal shader execution failed", completed.error));
		}];
		[command commit];
		output = input; transformed = composed; fullPresentation = composed && frame.presentationExpanded;
		++renderedFrames;
		reportShaderFrameStatus(activePath, {});
		return true;
	} catch (const std::exception& error) {
		// Discarded command buffers must not leave history entries marked rendered.
		resetTargets();
		reportShaderFrameStatus(activePath, error.what());
		return false;
	}
}

MetalShaderChain::MetalShaderChain(id<MTLDevice> device, id<MTLCommandQueue> queue)
	: impl(std::make_unique<Impl>(device, queue)) {}
MetalShaderChain::~MetalShaderChain() = default;
bool MetalShaderChain::render(id<MTLTexture> source, id<MTLTexture> depth, const ShaderFrameInfo& frame,
		id<MTLTexture>& output, bool& transformed, bool& fullPresentation)
{
	@autoreleasepool { return impl->render(source, depth, frame, output, transformed, fullPresentation); }
}

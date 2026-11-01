/*
	Portions Copyright 2026 The Hollycast Authors

	This file is part of Flycast.

	Flycast is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.
*/
#include "vulkan_shader_chain.h"

#include "cfg/option.h"
#include "compiler.h"
#include "log/LogManager.h"
#include "oslib/storage.h"
#include "stbi.h"
#include "utils.h"
#include "vulkan_context.h"

#include <algorithm>
#include <regex>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace {

constexpr char FramePreamble[] = R"(
layout (push_constant) uniform ShaderFrame
{
	vec4 sizes;
	uvec4 frame;
	vec2 viewportSize;
	uint hasDepth;
} shaderFrame;
layout (set = 0, binding = 16) uniform ShaderParameters
{
	vec4 parameters[64];
} shaderParameters;
#define SourceSize shaderFrame.sizes.xy
#define OutputSize shaderFrame.sizes.zw
#define FrameCount int(shaderFrame.frame.x)
#define Rotation int(shaderFrame.frame.y)
#define OriginalSize vec2(shaderFrame.frame.zw)
#define FinalViewportSize shaderFrame.viewportSize
#define HasDepth int(shaderFrame.hasDepth)
)";

constexpr char FragmentPreamble[] = R"(
layout (location = 0) out vec4 FragColor;
#define gl_FragColor FragColor
)";

constexpr char VertexPreamble[] = R"(
layout (location = 0) in vec3 in_pos;
layout (location = 1) in vec2 in_uv;
#define VertexCoord in_pos
#define TexCoord in_uv
)";

constexpr uint32_t MaxShaderInputs = 16;
constexpr uint32_t MaxShaderStorage = 8;
// Custom vertex stages may sample the same graph resources as fragment stages.
vk::PipelineStageFlags shaderReadStages()
{
	const auto graphics = vk::PipelineStageFlagBits::eVertexShader | vk::PipelineStageFlagBits::eFragmentShader;
	return VulkanContext::Instance()->SupportsGraphicsCompute() ? graphics | vk::PipelineStageFlagBits::eComputeShader : graphics;
}

vk::ShaderStageFlags shaderStages()
{
	const auto graphics = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment;
	return VulkanContext::Instance()->SupportsGraphicsCompute() ? graphics | vk::ShaderStageFlagBits::eCompute : graphics;
}

vk::SamplerAddressMode samplerAddressMode(postprocess::ShaderWrapMode mode)
{
	switch (mode) {
	case postprocess::ShaderWrapMode::Repeat: return vk::SamplerAddressMode::eRepeat;
	case postprocess::ShaderWrapMode::MirroredRepeat: return vk::SamplerAddressMode::eMirroredRepeat;
	case postprocess::ShaderWrapMode::ClampToBorder: return vk::SamplerAddressMode::eClampToBorder;
	case postprocess::ShaderWrapMode::ClampToEdge: return vk::SamplerAddressMode::eClampToEdge;
	}
	return vk::SamplerAddressMode::eClampToEdge;
}

void removeVersionDirective(std::string& source)
{
	const size_t lineEnd = source.find_first_of("\r\n");
	if (source.compare(0, 8, "#version") != 0)
		return;
	source.erase(0, lineEnd == std::string::npos ? source.size() : source.find_first_not_of("\r\n", lineEnd));
}

struct ShaderFrameUniforms {
	float sizes[4];
	uint32_t frameCount;
	uint32_t rotation;
	uint32_t originalWidth;
	uint32_t originalHeight;
	float viewportSize[2];
	uint32_t hasDepth;
};

struct alignas(16) ShaderParameterUniforms {
	float parameters[postprocess::MaxShaderParameterComponents];
};

static_assert(postprocess::MaxShaderParameterComponents == 256,
		"Update the Vulkan shader parameter blocks when changing the shared limit.");
static_assert(sizeof(ShaderFrameUniforms) <= 128,
		"Shader frame metadata must fit Vulkan's guaranteed push-constant size.");

uint32_t parameterComponentCount(postprocess::SlangParameterType type)
{
	return postprocess::shaderParameterComponentCount(type);
}

vk::BlendFactor blendFactor(postprocess::ShaderBlendFactor factor)
{
	switch (factor) {
	case postprocess::ShaderBlendFactor::Zero: return vk::BlendFactor::eZero;
	case postprocess::ShaderBlendFactor::One: return vk::BlendFactor::eOne;
	case postprocess::ShaderBlendFactor::SourceColor: return vk::BlendFactor::eSrcColor;
	case postprocess::ShaderBlendFactor::OneMinusSourceColor: return vk::BlendFactor::eOneMinusSrcColor;
	case postprocess::ShaderBlendFactor::DestinationColor: return vk::BlendFactor::eDstColor;
	case postprocess::ShaderBlendFactor::OneMinusDestinationColor: return vk::BlendFactor::eOneMinusDstColor;
	case postprocess::ShaderBlendFactor::SourceAlpha: return vk::BlendFactor::eSrcAlpha;
	case postprocess::ShaderBlendFactor::OneMinusSourceAlpha: return vk::BlendFactor::eOneMinusSrcAlpha;
	case postprocess::ShaderBlendFactor::DestinationAlpha: return vk::BlendFactor::eDstAlpha;
	case postprocess::ShaderBlendFactor::OneMinusDestinationAlpha: return vk::BlendFactor::eOneMinusDstAlpha;
	}
	return vk::BlendFactor::eOne;
}

vk::BlendOp blendOperation(postprocess::ShaderBlendOperation operation)
{
	switch (operation) {
	case postprocess::ShaderBlendOperation::Add: return vk::BlendOp::eAdd;
	case postprocess::ShaderBlendOperation::Subtract: return vk::BlendOp::eSubtract;
	case postprocess::ShaderBlendOperation::ReverseSubtract: return vk::BlendOp::eReverseSubtract;
	case postprocess::ShaderBlendOperation::Minimum: return vk::BlendOp::eMin;
	case postprocess::ShaderBlendOperation::Maximum: return vk::BlendOp::eMax;
	}
	return vk::BlendOp::eAdd;
}

std::string glslParameterValue(const postprocess::SlangParameter& parameter, uint32_t offset)
{
	// Use the shared type metadata so rectangular matrices consume every packed
	// scalar, just like square matrices and the other render backends.
	std::string result = std::string(postprocess::shaderParameterGlslType(parameter.type)) + "(";
	for (uint32_t component = 0; component < parameterComponentCount(parameter.type); ++component) {
		if (component != 0)
			result += ", ";
		const uint32_t index = offset + component;
		result += "shaderParameters.parameters[" + std::to_string(index / 4) + "][" + std::to_string(index % 4) + "]";
	}
	return result + ")";
}

const char *glslParameterType(postprocess::SlangParameterType type)
{
	return postprocess::shaderParameterGlslType(type);
}

void removeResourceDeclarations(std::string& source, const std::vector<postprocess::SlangParameter>& parameters)
{
	static const std::regex resource(R"(^\s*(?:layout\s*\([^\n]*\)\s*)?(?:uniform\s+)?(?:(?:lowp|mediump|highp)\s+)?(?:[iu]?sampler|texture|[iu]?image)(?:2DArray|1D|2D|3D|Cube)\s+[A-Za-z_][A-Za-z0-9_]*(?:\s*\[\s*[1-9][0-9]*\s*\])?\s*;\s*$)");
	static const std::regex numericUniform(
			R"(^\s*uniform\s+(?:(?:lowp|mediump|highp)\s+)?(?:float|int|uint|bool|[iu]?vec[2-4]|mat(?:[2-4]|[2-4]x[2-4]))\s+[A-Za-z_][A-Za-z0-9_]*\s*;\s*$)",
			std::regex::icase);
	std::string cleaned;
	std::istringstream stream(source);
	for (std::string line; std::getline(stream, line);) {
		const bool parameter = std::any_of(parameters.begin(), parameters.end(), [&](const postprocess::SlangParameter& value) {
			return std::regex_match(line, std::regex("^\\s*uniform\\s+(?:lowp\\s+|mediump\\s+|highp\\s+)?(?:float|int|bool|vec[2-4])\\s+"
					+ value.name + R"(\s*;\s*$)", std::regex::icase));
		});
		if (!std::regex_match(line, resource) && !std::regex_match(line, numericUniform) && !parameter)
			cleaned += line + '\n';
	}
	source = std::move(cleaned);
}

bool collectVaryingLocations(const std::string& source, const char *direction,
		std::unordered_map<std::string, uint32_t>& locations,
		std::unordered_set<uint32_t>& usedLocations, std::string& error)
{
	const std::regex declaration("^\\s*layout\\s*\\(\\s*location\\s*=\\s*([0-9]+)\\s*\\)\\s*(?:flat\\s+)?"
			+ std::string(direction)
			+ R"(\s+(?:(?:lowp|mediump|highp)\s+)?(?:float|int|uint|[iu]?vec[2-4])\s+([A-Za-z_][A-Za-z0-9_]*)\s*;\s*$)");
	std::istringstream lines(source);
	for (std::string line; std::getline(lines, line);) {
		std::smatch match;
		if (!std::regex_match(line, match, declaration))
			continue;
		uint32_t location;
		if (!postprocess::parseShaderUnsigned(match[1].str(), location)) {
			error = "Vulkan shader varying location is invalid.";
			return false;
		}
		const std::string name = match[2].str();
		if (const auto existing = locations.find(name); existing != locations.end() && existing->second != location) {
			error = "Vulkan shader varying '" + name + "' changes explicit location between stages.";
			return false;
		}
		locations[name] = location;
		usedLocations.insert(location);
	}
	return true;
}

void addVaryingLocations(std::string& source, const char *direction,
		std::unordered_map<std::string, uint32_t>& locations,
		std::unordered_set<uint32_t>& usedLocations)
{
	const std::regex declaration("^(\\s*)(?:(flat)\\s+)?" + std::string(direction)
			+ R"(\s+(?:(?:lowp|mediump|highp)\s+)?(?:float|int|uint|[iu]?vec[2-4])\s+([A-Za-z_][A-Za-z0-9_]*)\s*;\s*$)");
	std::istringstream lines(source);
	std::string normalized;
	uint32_t nextLocation = 1;
	for (std::string line; std::getline(lines, line);) {
		std::smatch match;
		if (std::regex_match(line, match, declaration)) {
			const std::string name = match[3].str();
			auto location = locations.find(name);
			if (location == locations.end()) {
				while (usedLocations.count(nextLocation) != 0)
					++nextLocation;
				location = locations.emplace(name, nextLocation).first;
				usedLocations.insert(nextLocation++);
			}
			const size_t content = line.find_first_not_of(" \t");
			line = match[1].str() + "layout (location = " + std::to_string(location->second) + ") "
					+ line.substr(content);
		}
		normalized += line + '\n';
	}
	source = std::move(normalized);
}

vk::Format resolveTargetFormat(postprocess::ShaderTargetFormat targetFormat, const std::string& targetFormatName, std::string& error)
{
	if (!targetFormatName.empty()) {
		if (targetFormatName == "R8_UNORM") return vk::Format::eR8Unorm;
		if (targetFormatName == "R16_UNORM") return vk::Format::eR16Unorm;
		if (targetFormatName == "R16_SFLOAT") return vk::Format::eR16Sfloat;
		if (targetFormatName == "R32_SFLOAT") return vk::Format::eR32Sfloat;
		if (targetFormatName == "R32_SINT") return vk::Format::eR32Sint;
		if (targetFormatName == "R32_UINT") return vk::Format::eR32Uint;
		if (targetFormatName == "R8G8_UNORM") return vk::Format::eR8G8Unorm;
		if (targetFormatName == "R16G16_UNORM") return vk::Format::eR16G16Unorm;
		if (targetFormatName == "R16G16_SFLOAT") return vk::Format::eR16G16Sfloat;
		if (targetFormatName == "R32G32_SFLOAT") return vk::Format::eR32G32Sfloat;
		if (targetFormatName == "R32G32B32A32_SINT") return vk::Format::eR32G32B32A32Sint;
		if (targetFormatName == "R32G32B32A32_UINT") return vk::Format::eR32G32B32A32Uint;
		if (targetFormatName == "A2B10G10R10_UNORM_PACK32") return vk::Format::eA2B10G10R10UnormPack32;
		if (targetFormatName == "B10G11R11_UFLOAT_PACK32") return vk::Format::eB10G11R11UfloatPack32;
		if (targetFormatName == "R8G8B8A8_UNORM")
			return vk::Format::eR8G8B8A8Unorm;
		if (targetFormatName == "R8G8B8A8_SRGB")
			return vk::Format::eR8G8B8A8Srgb;
		if (targetFormatName == "R16G16B16A16_SFLOAT")
			return vk::Format::eR16G16B16A16Sfloat;
		if (targetFormatName == "R16G16B16A16_UNORM")
			return vk::Format::eR16G16B16A16Unorm;
		if (targetFormatName == "R32G32B32A32_SFLOAT")
			return vk::Format::eR32G32B32A32Sfloat;
		error = "Vulkan post-processing does not support target format '" + targetFormatName + "'.";
		return vk::Format::eUndefined;
	}
	if (targetFormat == postprocess::ShaderTargetFormat::Float)
		return vk::Format::eR16G16B16A16Sfloat;
	if (targetFormat == postprocess::ShaderTargetFormat::Srgb)
		return vk::Format::eR8G8B8A8Srgb;
	return vk::Format::eR8G8B8A8Unorm;
}

std::string fragmentPreamble(const std::vector<postprocess::SlangParameter>& parameters,
		const std::vector<postprocess::ShaderPassInput>& bindings, uint32_t outputCount, bool nativeStages = false,
		bool includeTexCoord = true)
{
	std::string preamble = "layout (set = 0, binding = 0) uniform sampler2D Source;\n";
	for (size_t index = 0; index < bindings.size(); ++index)
		preamble += "layout (set = 0, binding = " + std::to_string(index + 1)
				+ ") uniform " + std::string(postprocess::shaderResourceGlslType(bindings[index].type, bindings[index].numericType)) + " " + bindings[index].bindingName + ";\n";
	preamble += FramePreamble;
	if (!nativeStages) {
		if (includeTexCoord)
			preamble += "layout (location = 0) in vec2 vTexCoord;\n";
		preamble += FragmentPreamble;
		preamble += "#define FxOutput0 FragColor\n";
		for (uint32_t index = 1; index < outputCount; ++index)
			preamble += "layout (location = " + std::to_string(index) + ") out vec4 FxOutput" + std::to_string(index) + ";\n";
	}
	uint32_t parameterOffset = 0;
	for (const postprocess::SlangParameter& parameter : parameters) {
		if (parameterOffset + parameterComponentCount(parameter.type) > postprocess::MaxShaderParameterComponents)
			return {};
		preamble += std::string(glslParameterType(parameter.type)) + " " + parameter.name + " = "
				+ glslParameterValue(parameter, parameterOffset) + ";\n";
		parameterOffset += parameterComponentCount(parameter.type);
	}
	return preamble;
}

std::string vertexPreamble(const std::vector<postprocess::SlangParameter>& parameters,
		const std::vector<postprocess::ShaderPassInput>& bindings, bool retroArchGlsl, bool nativeStages,
		bool includeTexCoord)
{
	std::string preamble = "layout (set = 0, binding = 0) uniform sampler2D Source;\n";
	for (size_t index = 0; index < bindings.size(); ++index)
		preamble += "layout (set = 0, binding = " + std::to_string(index + 1)
				+ ") uniform " + std::string(postprocess::shaderResourceGlslType(bindings[index].type, bindings[index].numericType)) + " " + bindings[index].bindingName + ";\n";
	preamble += FramePreamble;
	if (!nativeStages) {
		preamble += VertexPreamble;
		if (includeTexCoord)
			preamble += "layout (location = 0) out vec2 vTexCoord;\n";
	}
	if (retroArchGlsl && !nativeStages)
		preamble += "#undef VertexCoord\n#define VertexCoord vec4(in_pos, 1.0)\n";
	uint32_t parameterOffset = 0;
	for (const postprocess::SlangParameter& parameter : parameters) {
		if (parameterOffset + parameterComponentCount(parameter.type) > postprocess::MaxShaderParameterComponents)
			return {};
		preamble += std::string(glslParameterType(parameter.type)) + " " + parameter.name + " = "
				+ glslParameterValue(parameter, parameterOffset) + ";\n";
		parameterOffset += parameterComponentCount(parameter.type);
	}
	return preamble;
}

std::string storageDeclarations(const std::string& source, const postprocess::ShaderPassSource& pass)
{
	std::string result;
	for (uint32_t i = 0; i < pass.storageBindings.size(); ++i) {
		const auto& name = pass.storageBindings[i].first;
		std::smatch match;
		if (!std::regex_search(source, match, std::regex("layout\\s*\\(([^)]*)\\)\\s*uniform\\s+([iu]?image(?:1D|2DArray|2D|3D))\\s+" + name + "\\s*;"))) continue;
		std::string format = std::regex_replace(match[1].str(), std::regex(R"((?:set|binding)\s*=\s*[0-9]+\s*,?\s*)"), "");
		while (!format.empty() && (format.back() == ',' || std::isspace(static_cast<unsigned char>(format.back())))) format.pop_back();
		result += "layout(set=0, binding=" + std::to_string(MaxShaderInputs + 1 + i) + (format.empty() ? "" : ", " + format) + ") uniform " + match[2].str() + " " + name + ";\n";
	}
	return result;
}

} // namespace

bool postprocess::buildVulkanShaderSources(const ShaderPassSource& source,
		const std::vector<SlangParameter>& parameters, const std::vector<ShaderPassInput>& bindings,
		uint32_t outputCount, std::string& fragmentSource, std::string& vertexSource, std::string& error)
{
	std::string fragment = source.fragment;
	removeVersionDirective(fragment);
	removeResourceDeclarations(fragment, parameters);
	fragment = storageDeclarations(source.fragment, source) + fragment;
	std::string vertex = source.vertex;
	std::unordered_map<std::string, uint32_t> varyingLocations;
	std::unordered_set<uint32_t> usedVaryingLocations { 0 };
	if (!vertex.empty()) {
		removeVersionDirective(vertex);
		removeResourceDeclarations(vertex, parameters);
		vertex = storageDeclarations(source.vertex, source) + vertex;
		if (source.nativeFxStages) {
			vertex = std::regex_replace(vertex, std::regex(R"(\bgl_VertexID\b)"), "gl_VertexIndex");
			// Native effect coordinates use top-left UVs; the renderer's positive
			// Vulkan viewport maps UV zero to clip-space minus one.
			vertex = "#define main HollycastEffectVertexMain\n" + vertex
					+ "\n#undef main\nvoid main() { HollycastEffectVertexMain(); gl_Position.y = -gl_Position.y; }\n";
		}
	}
	// Native effect stages already carry their complete stage interface, including
	// location zero. Preserve it instead of reserving that location for vTexCoord.
	if (!source.nativeFxStages) {
		if ((!vertex.empty() && !collectVaryingLocations(vertex, "out", varyingLocations, usedVaryingLocations, error))
				|| !collectVaryingLocations(fragment, "in", varyingLocations, usedVaryingLocations, error))
			return false;
		if (!vertex.empty())
			addVaryingLocations(vertex, "out", varyingLocations, usedVaryingLocations);
		addVaryingLocations(fragment, "in", varyingLocations, usedVaryingLocations);
	}
	// Slang stages may own location zero with a differently named varying.
	// Keep the convenience UV only when the shader uses it or leaves zero free.
	const bool customLocationZero = std::any_of(varyingLocations.begin(), varyingLocations.end(),
			[](const auto& varying) { return varying.second == 0 && varying.first != "vTexCoord"; });
	const std::regex texCoordName(R"(\bvTexCoord\b)");
	const bool includeTexCoord = !customLocationZero || std::regex_search(vertex, texCoordName)
			|| std::regex_search(fragment, texCoordName);
	const std::string fragmentHeader = fragmentPreamble(parameters, bindings, outputCount, source.nativeFxStages, includeTexCoord);
	if (fragmentHeader.empty()) {
		error = "Vulkan shader chains may use at most "
				+ std::to_string(postprocess::MaxShaderParameterComponents) + " parameter components per pass.";
		return false;
	}
	fragmentSource = VulkanSource().addSource(fragmentHeader).addSource(fragment).generate();
	vertexSource.clear();
	if (!vertex.empty()) {
		const std::string vertexHeader = vertexPreamble(parameters, bindings, source.retroArchGlsl, source.nativeFxStages, includeTexCoord);
		if (vertexHeader.empty()) {
			error = "Vulkan shader chains may use at most "
					+ std::to_string(postprocess::MaxShaderParameterComponents) + " parameter components per pass.";
			return false;
		}
		vertexSource = VulkanSource().addSource(vertexHeader).addSource(vertex).generate();
	}
	error.clear();
	return true;
}

bool postprocess::buildVulkanComputeSource(const ShaderPassSource& source, const std::vector<SlangParameter>& parameters,
		const std::vector<ShaderPassInput>& bindings, std::string& compute, std::string& error)
{
	if (source.compute.empty()) { error = "Compute entry point is empty."; return false; }
	std::string body = source.compute; removeVersionDirective(body); removeResourceDeclarations(body, parameters);
	compute = VulkanSource().addSource(fragmentPreamble(parameters, bindings, 0, true, false))
			.addSource(storageDeclarations(source.compute, source)).addSource(body).generate();
	return true;
}

VulkanShaderChain::~VulkanShaderChain()
{
	term();
}

void VulkanShaderChain::ensureResources()
{
	if (renderPass)
		return;

	VulkanContext *context = VulkanContext::Instance();
	renderPass = createRenderPass({ vk::Format::eR8G8B8A8Unorm });

	const uint32_t storageLimit = std::min(MaxShaderStorage, context->GetPhysicalDevice().getProperties().limits.maxPerStageDescriptorStorageImages);
	std::vector<vk::DescriptorSetLayoutBinding> bindings(MaxShaderInputs + 1 + storageLimit);
	for (uint32_t index = 0; index < MaxShaderInputs; ++index)
		bindings[index] = vk::DescriptorSetLayoutBinding(index, vk::DescriptorType::eCombinedImageSampler, 1,
				shaderStages());
	bindings[MaxShaderInputs] = vk::DescriptorSetLayoutBinding(MaxShaderInputs,
			vk::DescriptorType::eUniformBuffer, 1,
			shaderStages());
	for (uint32_t i = 0; i < storageLimit; ++i) bindings[MaxShaderInputs + 1 + i] = vk::DescriptorSetLayoutBinding(MaxShaderInputs + 1 + i, vk::DescriptorType::eStorageImage, 1, shaderStages());
	descriptorSetLayout = context->GetDevice().createDescriptorSetLayoutUnique(vk::DescriptorSetLayoutCreateInfo({}, bindings));
	const vk::PushConstantRange pushConstants(shaderStages(), 0, sizeof(ShaderFrameUniforms));
	pipelineLayout = context->GetDevice().createPipelineLayoutUnique(vk::PipelineLayoutCreateInfo({}, descriptorSetLayout.get(), pushConstants));
	linearSampler = context->GetDevice().createSamplerUnique(vk::SamplerCreateInfo({}, vk::Filter::eLinear, vk::Filter::eLinear,
			vk::SamplerMipmapMode::eNearest, vk::SamplerAddressMode::eClampToEdge, vk::SamplerAddressMode::eClampToEdge,
			vk::SamplerAddressMode::eClampToEdge, 0.f, false, 1.f, false, vk::CompareOp::eNever, 0.f, 0.f));
	nearestSampler = context->GetDevice().createSamplerUnique(vk::SamplerCreateInfo({}, vk::Filter::eNearest, vk::Filter::eNearest,
			vk::SamplerMipmapMode::eNearest, vk::SamplerAddressMode::eClampToEdge, vk::SamplerAddressMode::eClampToEdge,
			vk::SamplerAddressMode::eClampToEdge, 0.f, false, 1.f, false, vk::CompareOp::eNever, 0.f, 0.f));
	// A view with only level zero must not be sampled at a higher LOD. Mipmapped
	// targets use the matching unrestricted sampler below.
	mipLinearSampler = context->GetDevice().createSamplerUnique(vk::SamplerCreateInfo({}, vk::Filter::eLinear, vk::Filter::eLinear,
			vk::SamplerMipmapMode::eLinear, vk::SamplerAddressMode::eClampToEdge, vk::SamplerAddressMode::eClampToEdge,
			vk::SamplerAddressMode::eClampToEdge, 0.f, false, 1.f, false, vk::CompareOp::eNever, 0.f, vk::LodClampNone));
	mipNearestSampler = context->GetDevice().createSamplerUnique(vk::SamplerCreateInfo({}, vk::Filter::eNearest, vk::Filter::eNearest,
			vk::SamplerMipmapMode::eNearest, vk::SamplerAddressMode::eClampToEdge, vk::SamplerAddressMode::eClampToEdge,
			vk::SamplerAddressMode::eClampToEdge, 0.f, false, 1.f, false, vk::CompareOp::eNever, 0.f, vk::LodClampNone));
	quadBuffer = std::make_unique<QuadBuffer>();
	copyFragmentShader = ShaderCompiler::Compile(vk::ShaderStageFlagBits::eFragment,
			VulkanSource().addSource(fragmentPreamble({}, {}, 1)).addSource("void main() { gl_FragColor = texture(Source, vTexCoord); }").generate());
	const postprocess::ShaderPass copyPass;
	copyPipeline = createPipeline({}, copyFragmentShader.get(), renderPass.get(), copyPass, 1);
	copyRotatePipeline = createPipeline(VulkanContext::Instance()->GetShaderManager()->GetQuadVertexShader(true),
			copyFragmentShader.get(), renderPass.get(), copyPass, 1);
}

vk::UniqueRenderPass VulkanShaderChain::createRenderPass(const std::vector<vk::Format>& formats, bool preserveContents, bool stencil, bool clear) const
{
	VulkanContext *context = VulkanContext::Instance();
	std::vector<vk::AttachmentDescription> attachments;
	std::vector<vk::AttachmentReference> colorAttachments;
	attachments.reserve(formats.size() + (stencil ? 1u : 0u));
	colorAttachments.reserve(formats.size());
	for (size_t index = 0; index < formats.size(); ++index) {
		attachments.emplace_back(vk::AttachmentDescription({}, formats[index], vk::SampleCountFlagBits::e1,
				clear ? vk::AttachmentLoadOp::eClear : preserveContents ? vk::AttachmentLoadOp::eLoad : vk::AttachmentLoadOp::eDontCare,
				vk::AttachmentStoreOp::eStore,
				vk::AttachmentLoadOp::eDontCare, vk::AttachmentStoreOp::eDontCare,
				vk::ImageLayout::eColorAttachmentOptimal, vk::ImageLayout::eShaderReadOnlyOptimal));
		colorAttachments.emplace_back(static_cast<uint32_t>(index), vk::ImageLayout::eColorAttachmentOptimal);
	}
	vk::AttachmentReference stencilAttachment(static_cast<uint32_t>(formats.size()), vk::ImageLayout::eDepthStencilAttachmentOptimal);
	if (stencil) attachments.emplace_back(vk::AttachmentDescription({}, context->GetDepthFormat(), vk::SampleCountFlagBits::e1,
			vk::AttachmentLoadOp::eDontCare, vk::AttachmentStoreOp::eDontCare, vk::AttachmentLoadOp::eLoad, vk::AttachmentStoreOp::eStore,
			vk::ImageLayout::eDepthStencilAttachmentOptimal, vk::ImageLayout::eDepthStencilAttachmentOptimal));
	const vk::SubpassDescription subpass({}, vk::PipelineBindPoint::eGraphics, 0, nullptr,
			static_cast<uint32_t>(colorAttachments.size()), colorAttachments.data(),
			nullptr, stencil ? &stencilAttachment : nullptr, 0, nullptr);
	const vk::SubpassDependency dependency(0, VK_SUBPASS_EXTERNAL,
			vk::PipelineStageFlagBits::eColorAttachmentOutput | vk::PipelineStageFlagBits::eLateFragmentTests,
			shaderReadStages() | vk::PipelineStageFlagBits::eEarlyFragmentTests,
			vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eDepthStencilAttachmentWrite,
			vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eDepthStencilAttachmentRead | vk::AccessFlagBits::eDepthStencilAttachmentWrite);
	return context->GetDevice().createRenderPassUnique(vk::RenderPassCreateInfo({}, static_cast<uint32_t>(attachments.size()),
			attachments.data(), 1, &subpass, 1, &dependency));
}

vk::UniquePipeline VulkanShaderChain::createPipeline(vk::ShaderModule vertexShader, vk::ShaderModule fragmentShader,
		vk::RenderPass renderPass, const postprocess::ShaderPass& pass, size_t colorAttachmentCount) const
{
	VulkanContext *context = VulkanContext::Instance();
	const vk::PipelineVertexInputStateCreateInfo vertexInput = GetQuadInputStateCreateInfo(true);
	const vk::PrimitiveTopology topologies[] { vk::PrimitiveTopology::ePointList, vk::PrimitiveTopology::ePointList,
		vk::PrimitiveTopology::eLineList, vk::PrimitiveTopology::eLineStrip, vk::PrimitiveTopology::eTriangleList, vk::PrimitiveTopology::eTriangleStrip };
	const vk::PipelineInputAssemblyStateCreateInfo inputAssembly({}, pass.renderState.reflected
			? topologies[static_cast<unsigned>(pass.renderState.topology)] : vk::PrimitiveTopology::eTriangleStrip);
	const vk::PipelineViewportStateCreateInfo viewportState({}, 1, nullptr, 1, nullptr);
	vk::PipelineRasterizationStateCreateInfo rasterization;
	rasterization.lineWidth = 1.f;
	const vk::PipelineMultisampleStateCreateInfo multisample;
	vk::PipelineDepthStencilStateCreateInfo depthStencil;
	depthStencil.stencilTestEnable = pass.renderState.stencilEnabled;
	const vk::StencilOp operations[] { vk::StencilOp::eZero, vk::StencilOp::eKeep, vk::StencilOp::eReplace,
		vk::StencilOp::eIncrementAndClamp, vk::StencilOp::eDecrementAndClamp, vk::StencilOp::eInvert, vk::StencilOp::eIncrementAndWrap, vk::StencilOp::eDecrementAndWrap };
	const auto& state = pass.renderState;
	depthStencil.front = depthStencil.back = vk::StencilOpState(operations[static_cast<unsigned>(state.stencilFail)],
			operations[static_cast<unsigned>(state.stencilPass)], operations[static_cast<unsigned>(state.stencilDepthFail)],
			static_cast<vk::CompareOp>(state.stencilComparison), state.stencilReadMask, state.stencilWriteMask, state.stencilReference);
	std::vector<vk::PipelineColorBlendAttachmentState> colorBlends;
	for (size_t i = 0; i < colorAttachmentCount; ++i) {
		const auto& a = state.attachments[i];
		colorBlends.emplace_back(state.reflected ? a.blendEnabled : pass.blendEnabled,
				blendFactor(state.reflected ? a.source : pass.sourceBlend), blendFactor(state.reflected ? a.destination : pass.destinationBlend),
				blendOperation(state.reflected ? a.operation : pass.blendOperation), blendFactor(a.sourceAlpha), blendFactor(a.destinationAlpha),
				blendOperation(a.alphaOperation), static_cast<vk::ColorComponentFlags>(a.writeMask));
	}

	const vk::PipelineColorBlendStateCreateInfo blendState({}, false, vk::LogicOp::eNoOp, colorBlends);
	const std::array<vk::DynamicState, 2> dynamicStates = { vk::DynamicState::eViewport, vk::DynamicState::eScissor };
	const vk::PipelineDynamicStateCreateInfo dynamicState({}, dynamicStates);
	const std::array<vk::PipelineShaderStageCreateInfo, 2> stages = {
		vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eVertex,
				vertexShader ? vertexShader : VulkanContext::Instance()->GetShaderManager()->GetQuadVertexShader(false), "main"),
		vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eFragment, fragmentShader, "main"),
	};
	return context->GetDevice().createGraphicsPipelineUnique(context->GetPipelineCache(),
			vk::GraphicsPipelineCreateInfo({}, stages, &vertexInput, &inputAssembly, nullptr, &viewportState,
					&rasterization, &multisample, &depthStencil, &blendState, &dynamicState, pipelineLayout.get(), renderPass)).value;
}

bool VulkanShaderChain::compileCandidate(const postprocess::ShaderChain& chain, std::vector<Program>& candidate,
		std::string& error)
{
	VulkanContext *context = VulkanContext::Instance();
	candidate.reserve(chain.passes.size());
	for (size_t passIndex = 0; passIndex < chain.passes.size(); ++passIndex) {
		const postprocess::ShaderPass& pass = chain.passes[passIndex];
		postprocess::ShaderPassSource loadedSource;
		const postprocess::ShaderPassSource *passSource = passIndex < chain.sources.size()
				? &chain.sources[passIndex] : &loadedSource;
		if (passSource == &loadedSource && !postprocess::loadShaderPassSource(pass, loadedSource, error))
			return false;
		std::vector<postprocess::SlangParameter> typedParameters = passSource->slangParameters;
		if (typedParameters.empty())
			for (const postprocess::ShaderParameter& parameter : chain.parameters) {
				if (parameter.layerIndex != pass.layerIndex)
					continue;
				postprocess::SlangParameter typed;
				typed.name = parameter.name;
				typed.type = parameter.type;
				typed.defaultValue.values = parameter.values;
				typed.defaultValue.componentCount = parameter.componentCount;
				typedParameters.push_back(std::move(typed));
			}
		std::vector<postprocess::ShaderPassOutput> outputs = pass.outputs;
		if (outputs.empty())
			outputs.push_back({ pass.alias, pass.targetFormat, pass.targetFormatName, false });
		if (passSource->outputCount + passSource->storageOutputs.size() != outputs.size()) {
			error = "Vulkan post-processing shader output count does not match its configured render targets.";
			return false;
		}
		const vk::PhysicalDeviceLimits& limits = context->GetPhysicalDevice().getProperties().limits;
		if (pass.colorOutputCount > limits.maxColorAttachments || pass.colorOutputCount > limits.maxFragmentOutputAttachments) {
			error = "Vulkan post-processing shader uses more color targets than the device supports.";
			return false;
		}
		const auto features = context->GetPhysicalDevice().getFeatures();
		if (pass.storageBindings.size() > MaxShaderStorage || pass.storageBindings.size() > limits.maxPerStageDescriptorStorageImages
				|| (!pass.compute && !pass.storageBindings.empty() && !features.fragmentStoresAndAtomics)) {
			error = "Shader storage images exceed this Vulkan device's supported stage or binding limits.";
			return false;
		}
		if (pass.compute && (!context->SupportsGraphicsCompute()
				|| pass.localSize[0] > limits.maxComputeWorkGroupSize[0] || pass.localSize[1] > limits.maxComputeWorkGroupSize[1]
				|| pass.localSize[2] > limits.maxComputeWorkGroupSize[2]
				|| static_cast<uint64_t>(pass.localSize[0]) * pass.localSize[1] * pass.localSize[2] > limits.maxComputeWorkGroupInvocations)) {
			error = "Shader workgroup size exceeds this Vulkan queue's compute capabilities.";
			return false;
		}
		if (!features.independentBlend && pass.renderState.reflected) {
			const auto& first = pass.renderState.attachments[0];
			for (uint32_t i = 1; i < pass.colorOutputCount; ++i) {
				const auto& a = pass.renderState.attachments[i];
				if (a.blendEnabled != first.blendEnabled || a.source != first.source || a.destination != first.destination
						|| a.sourceAlpha != first.sourceAlpha || a.destinationAlpha != first.destinationAlpha
						|| a.operation != first.operation || a.alphaOperation != first.alphaOperation || a.writeMask != first.writeMask) {
					error = "This Vulkan device does not support independent attachment blend states.";
					return false;
				}
			}
		}
		Program program;
		program.pass = pass;
		program.displayGlsl = passSource->displayGlsl;
		program.nativeFxStages = passSource->nativeFxStages;
		if (pass.wrapMode != postprocess::ShaderWrapMode::ClampToEdge) {
			vk::SamplerCreateInfo samplerInfo;
			samplerInfo.magFilter = pass.linearFilter ? vk::Filter::eLinear : vk::Filter::eNearest;
			samplerInfo.minFilter = samplerInfo.magFilter;
			samplerInfo.mipmapMode = vk::SamplerMipmapMode::eNearest;
			samplerInfo.addressModeU = samplerAddressMode(pass.wrapMode);
			samplerInfo.addressModeV = samplerInfo.addressModeW = samplerInfo.addressModeU;
			samplerInfo.borderColor = vk::BorderColor::eFloatTransparentBlack;
			try {
				program.sampler = context->GetDevice().createSamplerUnique(samplerInfo);
				samplerInfo.mipmapMode = pass.linearFilter ? vk::SamplerMipmapMode::eLinear : vk::SamplerMipmapMode::eNearest;
				samplerInfo.maxLod = vk::LodClampNone;
				program.mipSampler = context->GetDevice().createSamplerUnique(samplerInfo);
			} catch (const vk::SystemError& exception) {
				error = "Vulkan could not create shader pass samplers: " + std::string(exception.what());
				return false;
			}
		}
		program.parameters = std::move(typedParameters);
		for (const postprocess::SlangParameter& parameter : program.parameters) {
			const auto configured = std::find_if(chain.parameters.begin(), chain.parameters.end(), [&](const postprocess::ShaderParameter& value) {
				return value.name == parameter.name && value.layerIndex == pass.layerIndex;
			});
			program.parameterIndices.push_back(configured == chain.parameters.end() ? postprocess::InvalidShaderPass
					: static_cast<uint32_t>(std::distance(chain.parameters.begin(), configured)));
		}
		program.outputs.reserve(outputs.size());
		std::vector<vk::Format> targetFormats;
		targetFormats.reserve(outputs.size());
		for (const postprocess::ShaderPassOutput& output : outputs) {
			const vk::Format targetFormat = resolveTargetFormat(output.targetFormat, output.targetFormatName, error);
			if (targetFormat == vk::Format::eUndefined)
				return false;
			const vk::FormatFeatureFlags formatFeatures = context->GetPhysicalDevice().getFormatProperties(targetFormat).optimalTilingFeatures;
			if (!(formatFeatures & (program.outputs.size() >= pass.colorOutputCount ? vk::FormatFeatureFlagBits::eStorageImage : vk::FormatFeatureFlagBits::eColorAttachment)) || !(formatFeatures & vk::FormatFeatureFlagBits::eSampledImage)) {
				error = "Vulkan does not support a requested post-processing target format.";
				return false;
			}
			const bool supportsMipmaps = (formatFeatures & vk::FormatFeatureFlagBits::eBlitSrc)
					&& (formatFeatures & vk::FormatFeatureFlagBits::eBlitDst)
					&& (formatFeatures & vk::FormatFeatureFlagBits::eSampledImageFilterLinear);
			if (output.useMipmaps && !supportsMipmaps) {
				error = "Vulkan post-processing mipmaps require linear blit support for every requested target format.";
				return false;
			}
			program.outputs.push_back({ output, targetFormat, output.useMipmaps, supportsMipmaps });
			if (program.outputs.size() <= pass.colorOutputCount) targetFormats.push_back(targetFormat);
		}
		std::vector<postprocess::ShaderPassInput> shaderBindings;
		for (const postprocess::SlangResource& resource : passSource->slangResources) {
			if (resource.arrayCount != 1) {
				error = "Vulkan post-processing does not support arrays of sampled resource bindings.";
				return false;
			}
			// Binding zero is the canonical Source name. SourceTexture and FX
			// SourceSampler still need their declared binding preserved.
			if (resource.convention == postprocess::SlangResourceConvention::Source && resource.name == "Source")
				continue;
			auto input = std::find_if(pass.inputs.begin(), pass.inputs.end(), [&](const postprocess::ShaderPassInput& candidateInput) {
				return candidateInput.bindingName == resource.name;
			});
			if (input == pass.inputs.end()) {
				error = "Vulkan post-processing could not resolve Slang resource '" + resource.name + "'.";
				return false;
			}
			shaderBindings.push_back(*input);
		}
		if (shaderBindings.size() + 1 > MaxShaderInputs) {
			error = "Vulkan post-processing supports at most 16 sampled inputs per pass.";
			return false;
		}
		std::string source;
		std::string vertexSource;
		if (!(pass.compute ? postprocess::buildVulkanComputeSource(*passSource, program.parameters, shaderBindings, source, error) : postprocess::buildVulkanShaderSources(*passSource, program.parameters, shaderBindings, pass.colorOutputCount, source, vertexSource, error)))
			return false;
		for (size_t index = 0; index < shaderBindings.size(); ++index) {
			uint32_t lookupIndex = postprocess::InvalidShaderPass;
			if (shaderBindings[index].kind == postprocess::ShaderInputKind::LookupTexture) {
				const auto lookup = std::find_if(chain.textures.begin(), chain.textures.end(), [&](const postprocess::ShaderPresetTexture& texture) {
					return texture.name == shaderBindings[index].resourceName;
				});
				if (lookup == chain.textures.end()) {
					error = "Vulkan lookup texture '" + shaderBindings[index].resourceName + "' was not resolved.";
					return false;
				}
				lookupIndex = static_cast<uint32_t>(std::distance(chain.textures.begin(), lookup));
			}
			Binding binding;
			binding.input = shaderBindings[index];
			binding.descriptorBinding = static_cast<uint32_t>(index + 1);
			binding.lookupIndex = lookupIndex;
			const auto samplerConfig = std::find_if(pass.samplers.begin(), pass.samplers.end(), [&](const auto& sampler) {
				return sampler.bindingName == shaderBindings[index].bindingName;
			});
			if (samplerConfig != pass.samplers.end()
					&& shaderBindings[index].kind != postprocess::ShaderInputKind::LookupTexture) {
				vk::SamplerCreateInfo samplerInfo;
				samplerInfo.magFilter = samplerConfig->linearFilter ? vk::Filter::eLinear : vk::Filter::eNearest;
				samplerInfo.minFilter = samplerInfo.magFilter;
				samplerInfo.mipmapMode = vk::SamplerMipmapMode::eNearest;
				samplerInfo.addressModeU = samplerAddressMode(samplerConfig->wrapMode);
				samplerInfo.addressModeV = samplerInfo.addressModeW = samplerInfo.addressModeU;
				samplerInfo.borderColor = vk::BorderColor::eFloatTransparentBlack;
				samplerInfo.maxLod = 0.f;
				try {
					binding.sampler = context->GetDevice().createSamplerUnique(samplerInfo);
					samplerInfo.mipmapMode = samplerConfig->linearFilter
							? vk::SamplerMipmapMode::eLinear : vk::SamplerMipmapMode::eNearest;
					samplerInfo.maxLod = vk::LodClampNone;
					binding.mipSampler = context->GetDevice().createSamplerUnique(samplerInfo);
				}
				catch (const vk::SystemError& exception) {
					error = "Vulkan could not create ReShade sampler '" + samplerConfig->bindingName
							+ "': " + exception.what();
					return false;
				}
				binding.useMipmaps = samplerConfig->useMipmaps;
			}
			program.bindings.push_back(std::move(binding));
		}
		program.targetWasInitialized.reserve(program.outputs.size());
		program.targetBarriers.reserve(program.outputs.size());
		program.imageInfos.reserve(program.bindings.size() + 1 + pass.storageBindings.size());
		program.descriptorWrites.reserve(program.bindings.size() + 1 + pass.storageBindings.size());
		program.clearColors.assign(program.outputs.size(),
				vk::ClearValue(vk::ClearColorValue(std::array<uint32_t, 4> { 0, 0, 0, 0 })));
		program.blendBarriers.reserve(program.outputs.size());
		if (!vertexSource.empty()) {
			if (!ShaderCompiler::TryCompile(vk::ShaderStageFlagBits::eVertex,
					vertexSource, program.vertexShader, error))
				return false;
		}
		if (!ShaderCompiler::TryCompile(pass.compute ? vk::ShaderStageFlagBits::eCompute : vk::ShaderStageFlagBits::eFragment, source, pass.compute ? program.computeShader : program.fragmentShader, error))
			return false;
		try {
			if (pass.compute) {
				const auto queues = context->GetPhysicalDevice().getQueueFamilyProperties();
				if (!(queues[context->GetGraphicsQueueFamilyIndex()].queueFlags & vk::QueueFlagBits::eCompute)) { error = "The active Vulkan graphics queue does not support compute."; return false; }
				program.pipeline = context->GetDevice().createComputePipelineUnique(context->GetPipelineCache(),
						vk::ComputePipelineCreateInfo({}, vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eCompute, program.computeShader.get(), "main"), pipelineLayout.get())).value;
			} else {
			program.renderPass = createRenderPass(targetFormats, pass.preserveBackbuffer || program.pass.blendEnabled || !pass.updatesSource, pass.renderState.stencilEnabled, pass.renderState.clearRenderTargets);
			program.pipeline = createPipeline(program.vertexShader.get(), program.fragmentShader.get(), program.renderPass.get(),
					program.pass, pass.colorOutputCount);
			if ((program.pass.preserveBackbuffer || program.pass.blendEnabled) && program.pass.updatesSource && !pass.renderState.clearRenderTargets)
				program.blendCopyPipeline = createPipeline({}, copyFragmentShader.get(), program.renderPass.get(),
						postprocess::ShaderPass {}, pass.colorOutputCount);
			}
		} catch (const vk::SystemError& exception) {
			error = exception.what();
			return false;
		}
		candidate.push_back(std::move(program));
	}
	return true;
}

bool VulkanShaderChain::updateConfiguredChain()
{
	const std::vector<std::string> requestedPaths = postprocess::configuredShaderPaths();
	const std::string requestedLabel = postprocess::shaderSelectionLabel(requestedPaths);
	const uint64_t generation = postprocess::shaderChainReloadGeneration();
	if (requestedPaths == configuredPaths && generation == configuredGeneration)
		return !programs.empty();
	auto releaseActiveResources = [&] {
		if (programs.empty() && targets.empty() && sourceTargets.empty() && displayTargets.empty()
				&& lookupTextures.empty() && descriptorSets.empty() && sourceDescriptorSets.empty()
				&& displayDescriptorSets.empty() && depthRotateTargets.empty() && depthRotateDescriptorSets.empty())
			return;
		// A live reload can retire resources referenced by any swapchain image.
		// Waiting only on this rare transition keeps normal frames stall-free.
		VulkanContext::Instance()->GetDevice().waitIdle();
		targets.clear();
		stencilTargets.clear();
		sourceTargets.clear();
		displayTargets.clear();
		depthRotateTargets.clear();
		lookupTextures.clear();
		descriptorSets.clear();
		sourceDescriptorSets.clear();
		displayDescriptorSets.clear();
		depthRotateDescriptorSets.clear();
		depthRotatePipeline.reset();
		depthRotateRenderPass.reset();
		programs.clear();
	};
	// Present the loading notice before compilation blocks this render thread.
	if (!requestedPaths.empty() && !postprocess::prepareShaderCompilation())
		return !programs.empty();
	configuredPaths = requestedPaths;
	configuredGeneration = generation;
	if (requestedPaths.empty()) {
		releaseActiveResources();
		parameters.clear();
		activePath.clear();
		postprocess::reportShaderChainStatus({}, {});
		return false;
	}
	std::string error;
	postprocess::ShaderChain chain;
	if (!postprocess::loadConfiguredShaderChain(requestedPaths, generation, chain, error)) {
		postprocess::reportShaderChainStatus(activePath, error);
		WARN_LOG(RENDERER, "Post-processing shader '%s' was not loaded: %s", requestedLabel.c_str(), error.c_str());
		return !programs.empty();
	}
	postprocess::applyShaderChainParameterValues(chain, config::PostProcessShaderParameters.get());
	std::vector<Program> candidate;
	try {
		ensureResources();
		if (!compileCandidate(chain, candidate, error)) {
			postprocess::reportShaderChainStatus(activePath, error);
			WARN_LOG(RENDERER, "Post-processing shader '%s' was not activated: %s", requestedLabel.c_str(), error.c_str());
			return !programs.empty();
		}
	} catch (const std::exception& exception) {
		error = exception.what();
		postprocess::reportShaderChainStatus(activePath, error);
		WARN_LOG(RENDERER, "Post-processing shader '%s' was not activated: %s", requestedLabel.c_str(), error.c_str());
		return !programs.empty();
	}
	for (const Program& program : candidate)
		for (const Binding& binding : program.bindings)
			if (binding.input.sourcePass != postprocess::InvalidShaderPass
					&& binding.input.kind != postprocess::ShaderInputKind::LookupTexture
					&& (binding.input.sourcePass >= candidate.size()
							|| binding.input.sourceOutput >= candidate[binding.input.sourcePass].outputs.size())) {
				error = "Vulkan post-processing input references an unavailable render target output.";
				postprocess::reportShaderChainStatus(activePath, error);
				return !programs.empty();
			}
	const std::vector<std::vector<bool>> mipmapRequirements = postprocess::resolveShaderPassOutputMipmaps(chain);
	for (size_t passIndex = 0; passIndex < candidate.size(); ++passIndex)
		for (size_t outputIndex = 0; outputIndex < candidate[passIndex].outputs.size(); ++outputIndex)
			candidate[passIndex].outputs[outputIndex].needsMipmaps = mipmapRequirements[passIndex][outputIndex];
	for (const Program& program : candidate)
		for (const Program::Output& output : program.outputs)
			if (output.needsMipmaps && !output.supportsMipmaps) {
				error = "Vulkan post-processing mipmaps require linear blit support for every sampled render target.";
				postprocess::reportShaderChainStatus(activePath, error);
				return !programs.empty();
			}
	std::vector<LookupTexture> candidateLookupTextures;
	candidateLookupTextures.reserve(chain.textures.size());
	for (const postprocess::ShaderPresetTexture& texture : chain.textures)
		candidateLookupTextures.push_back({ texture });
	if (!validateLookupTextures(candidateLookupTextures, error)) {
		postprocess::reportShaderChainStatus(activePath, error);
		WARN_LOG(RENDERER, "Post-processing shader '%s' was not activated: %s", requestedLabel.c_str(), error.c_str());
		return !programs.empty();
	}
	releaseActiveResources();
	programs = std::move(candidate);
	renderedFrames = 0;
	parameters = std::move(chain.parameters);
	sourceHistoryDepth = 1;
	for (const postprocess::ShaderPass& pass : chain.passes)
		for (const postprocess::ShaderPassInput& input : pass.inputs) {
			const uint32_t requiredDepth = input.kind == postprocess::ShaderInputKind::History
					? input.historyOffset + 1
					: input.kind == postprocess::ShaderInputKind::Feedback ? 2u : 1u;
			if (requiredDepth == 1)
				continue;
			if (input.sourcePass == postprocess::InvalidShaderPass)
				sourceHistoryDepth = std::max(sourceHistoryDepth, requiredDepth);
			else
				programs[input.sourcePass].historyDepth = std::max(programs[input.sourcePass].historyDepth, requiredDepth);
		}
	for (const auto& program : programs)
		for (const auto& output : program.pass.outputs)
			if (output.previousFrame && output.previousPass != postprocess::InvalidShaderPass)
				programs[output.previousPass].historyDepth = std::max(2u, programs[output.previousPass].historyDepth);
	lookupTextures = std::move(candidateLookupTextures);
	activePath = requestedLabel;
	postprocess::reportShaderChainStatus(activePath, {}, parameters,
			chain.techniques, chain.selectedTechnique);
	parameterGeneration = postprocess::shaderChainStatusGeneration();
	return true;
}

bool VulkanShaderChain::ensureTarget(Target& target, const vk::Extent2D& extent, vk::Format format,
		bool mipmapped, bool storage, postprocess::SlangResourceType type, uint32_t depth, uint32_t layers)
{
	const uint32_t maxDimension = VulkanContext::Instance()->GetPhysicalDevice().getProperties().limits.maxImageDimension2D;
	if (extent.width == 0 || extent.height == 0 || extent.width > maxDimension || extent.height > maxDimension)
		throw std::runtime_error("Vulkan post-processing target exceeds the device texture-size limit.");
	uint32_t mipLevels = 1;
	for (uint32_t dimension = std::max({extent.width, extent.height, depth}); mipmapped && dimension > 1; dimension >>= 1)
		++mipLevels;
	if (target.image && target.extent == extent && target.format == format && target.mipLevels == mipLevels && target.type == type && target.depth == depth && target.layers == layers && target.storage == storage)
		return false;
	target.framebuffer.reset();
	target.sampleView.reset();
	target.renderView.reset();
	target.image.reset();
	target.allocation = {};

	VulkanContext *context = VulkanContext::Instance();
	vk::ImageUsageFlags usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc;
	usage |= storage ? vk::ImageUsageFlagBits::eStorage : vk::ImageUsageFlagBits::eColorAttachment;
	const bool volume = type == postprocess::SlangResourceType::Sampler3D || type == postprocess::SlangResourceType::Texture3D;
	const bool line = type == postprocess::SlangResourceType::Sampler1D || type == postprocess::SlangResourceType::Texture1D;
	const bool cube = type == postprocess::SlangResourceType::SamplerCube || type == postprocess::SlangResourceType::TextureCube;
	const vk::ImageType imageType = volume ? vk::ImageType::e3D : line ? vk::ImageType::e1D : vk::ImageType::e2D;
	const bool array = type == postprocess::SlangResourceType::Sampler2DArray || type == postprocess::SlangResourceType::Texture2DArray;
	const vk::ImageViewType viewType = volume ? vk::ImageViewType::e3D : line ? vk::ImageViewType::e1D : cube ? vk::ImageViewType::eCube : array ? vk::ImageViewType::e2DArray : vk::ImageViewType::e2D;
	if (mipLevels > 1)
		usage |= vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst;
	target.image = context->GetDevice().createImageUnique(vk::ImageCreateInfo(cube ? vk::ImageCreateFlagBits::eCubeCompatible : vk::ImageCreateFlags(), imageType,
			format, vk::Extent3D(extent.width, line ? 1u : extent.height, volume ? depth : 1u), mipLevels, layers, vk::SampleCountFlagBits::e1,
			vk::ImageTiling::eOptimal, usage));
	VmaAllocationCreateInfo allocationInfo = { VmaAllocationCreateFlags(), VmaMemoryUsage::VMA_MEMORY_USAGE_GPU_ONLY };
	target.allocation = context->GetAllocator().AllocateForImage(target.image.get(), allocationInfo);
	const vk::ImageSubresourceRange renderRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, layers);
	target.renderView = context->GetDevice().createImageViewUnique(vk::ImageViewCreateInfo({}, target.image.get(),
			viewType, format, {}, renderRange));
	const vk::ImageSubresourceRange sampleRange(vk::ImageAspectFlagBits::eColor, 0, mipLevels, 0, layers);
	target.sampleView = context->GetDevice().createImageViewUnique(vk::ImageViewCreateInfo({}, target.image.get(),
			viewType, format, {}, sampleRange));
	target.extent = extent; target.type = type; target.depth = depth; target.layers = layers; target.storage = storage;
	target.format = format;
	target.mipLevels = mipLevels;
	target.renderedFrame = 0;
	target.initialized = false;
	return true;
}

bool VulkanShaderChain::ensureTargetSet(TargetSet& targetSet, const Program& program, const vk::Extent2D& extent)
{
	bool changed = targetSet.outputs.size() != program.outputs.size();
	if (changed)
		targetSet.outputs.resize(program.outputs.size());
	for (size_t index = 0; index < program.outputs.size(); ++index)
		changed = ensureTarget(targetSet.outputs[index], extent, program.outputs[index].format,
				program.outputs[index].needsMipmaps, index >= program.pass.colorOutputCount, program.outputs[index].config.type,
				program.outputs[index].config.type == postprocess::SlangResourceType::Sampler3D ? program.outputs[index].config.depth : 1,
				program.outputs[index].config.type == postprocess::SlangResourceType::Sampler2DArray ? program.outputs[index].config.depth : 1) || changed;
	if (!changed && (program.pass.compute || targetSet.framebuffer))
		return false;
	targetSet.renderedFrame = 0;
	targetSet.initialized = false;
	targetSet.framebuffer.reset();
	if (program.pass.compute) return true;
	std::vector<vk::ImageView> attachments;
	for (uint32_t i = 0; i < program.pass.colorOutputCount; ++i) attachments.push_back(targetSet.outputs[i].renderView.get());
	if (program.pass.renderState.stencilEnabled) {
		auto *context = VulkanContext::Instance();
		const size_t imageIndex = context->GetCurrentImageIndex();
		if (stencilTargets.size() <= imageIndex) stencilTargets.resize(imageIndex + 1);
		auto& stencil = stencilTargets[imageIndex][{extent.width, extent.height}];
		if (!stencil.image) {
			stencil.image = context->GetDevice().createImageUnique(vk::ImageCreateInfo({}, vk::ImageType::e2D, context->GetDepthFormat(), vk::Extent3D(extent, 1), 1, 1,
					vk::SampleCountFlagBits::e1, vk::ImageTiling::eOptimal, vk::ImageUsageFlagBits::eDepthStencilAttachment | vk::ImageUsageFlagBits::eTransferDst));
			VmaAllocationCreateInfo info { {}, VMA_MEMORY_USAGE_GPU_ONLY };
			stencil.allocation = context->GetAllocator().AllocateForImage(stencil.image.get(), info);
			stencil.view = context->GetDevice().createImageViewUnique(vk::ImageViewCreateInfo({}, stencil.image.get(), vk::ImageViewType::e2D, context->GetDepthFormat(), {},
					vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil, 0, 1, 0, 1)));
		}
		attachments.push_back(stencil.view.get());
	}
	targetSet.framebuffer = VulkanContext::Instance()->GetDevice().createFramebufferUnique(vk::FramebufferCreateInfo({},
			program.renderPass.get(), attachments, extent.width, extent.height, 1));
	return true;
}

const VulkanShaderChain::Target *VulkanShaderChain::findSourceHistoryTarget(uint32_t frameCount) const
{
	// Resources stay partitioned by swapchain image for flight safety, so frame
	// metadata—not the current partition—identifies the requested history slot.
	for (const std::vector<Target>& partition : sourceTargets)
		for (const Target& target : partition)
			if (target.initialized && target.renderedFrame == frameCount)
				return &target;
	return nullptr;
}

const VulkanShaderChain::TargetSet *VulkanShaderChain::findPassHistoryTarget(uint32_t passIndex,
		uint32_t frameCount) const
{
	for (const std::vector<std::vector<TargetSet>>& partition : targets) {
		if (passIndex >= partition.size())
			continue;
		for (const TargetSet& targetSet : partition[passIndex])
			if (targetSet.initialized && targetSet.renderedFrame == frameCount)
				return &targetSet;
	}
	return nullptr;
}

void VulkanShaderChain::generateMipmaps(vk::CommandBuffer commandBuffer, const Target& target, bool existingContents) const
{
	if (target.mipLevels == 1)
		return;
	const vk::ImageSubresourceRange baseRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, target.layers);
	vk::ImageMemoryBarrier baseBarrier(target.storage ? vk::AccessFlagBits::eShaderWrite : vk::AccessFlagBits::eColorAttachmentWrite, vk::AccessFlagBits::eTransferRead,
			vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageLayout::eTransferSrcOptimal,
			VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, target.image.get(), baseRange);
	commandBuffer.pipelineBarrier(target.storage ? shaderReadStages() : vk::PipelineStageFlags(vk::PipelineStageFlagBits::eColorAttachmentOutput), vk::PipelineStageFlagBits::eTransfer,
			{}, nullptr, nullptr, baseBarrier);

	uint32_t sourceWidth = target.extent.width;
	uint32_t sourceHeight = target.extent.height;
	uint32_t sourceDepth = target.type == postprocess::SlangResourceType::Sampler3D ? target.depth : 1u;
	for (uint32_t level = 1; level < target.mipLevels; ++level) {
		const vk::ImageSubresourceRange destinationRange(vk::ImageAspectFlagBits::eColor, level, 1, 0, target.layers);
		const vk::ImageMemoryBarrier destinationBarrier(existingContents ? vk::AccessFlagBits::eShaderRead : vk::AccessFlags(),
				vk::AccessFlagBits::eTransferWrite, existingContents ? vk::ImageLayout::eShaderReadOnlyOptimal : vk::ImageLayout::eUndefined,
				vk::ImageLayout::eTransferDstOptimal, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, target.image.get(), destinationRange);
		commandBuffer.pipelineBarrier(existingContents ? shaderReadStages() : vk::PipelineStageFlagBits::eTopOfPipe,
				vk::PipelineStageFlagBits::eTransfer, {}, nullptr, nullptr, destinationBarrier);
		const uint32_t destinationWidth = std::max(sourceWidth >> 1, 1u);
		const uint32_t destinationHeight = std::max(sourceHeight >> 1, 1u);
		const uint32_t destinationDepth = std::max(sourceDepth >> 1, 1u);
		const vk::ImageBlit blit(vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, level - 1, 0, target.layers),
				{ { vk::Offset3D(), vk::Offset3D(static_cast<int>(sourceWidth), static_cast<int>(sourceHeight), static_cast<int>(sourceDepth)) } },
				vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, level, 0, target.layers),
				{ { vk::Offset3D(), vk::Offset3D(static_cast<int>(destinationWidth), static_cast<int>(destinationHeight), static_cast<int>(destinationDepth)) } });
		commandBuffer.blitImage(target.image.get(), vk::ImageLayout::eTransferSrcOptimal, target.image.get(),
				vk::ImageLayout::eTransferDstOptimal, blit, vk::Filter::eLinear);
		const vk::ImageSubresourceRange sourceRange(vk::ImageAspectFlagBits::eColor, level - 1, 1, 0, target.layers);
		const vk::ImageMemoryBarrier sourceBarrier(vk::AccessFlagBits::eTransferRead, vk::AccessFlagBits::eShaderRead,
				vk::ImageLayout::eTransferSrcOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
				VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, target.image.get(), sourceRange);
		commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, shaderReadStages(),
				{}, nullptr, nullptr, sourceBarrier);
		baseBarrier = vk::ImageMemoryBarrier(vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eTransferRead,
				vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eTransferSrcOptimal,
				VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, target.image.get(), destinationRange);
		commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer,
				{}, nullptr, nullptr, baseBarrier);
		sourceWidth = destinationWidth;
		sourceHeight = destinationHeight; sourceDepth = destinationDepth;
	}
	const vk::ImageSubresourceRange lastRange(vk::ImageAspectFlagBits::eColor, target.mipLevels - 1, 1, 0, target.layers);
	const vk::ImageMemoryBarrier lastBarrier(vk::AccessFlagBits::eTransferRead, vk::AccessFlagBits::eShaderRead,
			vk::ImageLayout::eTransferSrcOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
			VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, target.image.get(), lastRange);
	commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, shaderReadStages(),
			{}, nullptr, nullptr, lastBarrier);
}

bool VulkanShaderChain::ensureLookupTextures(vk::CommandBuffer commandBuffer, std::string& error)
{
	return ensureLookupTextures(commandBuffer, lookupTextures, error);
}

bool VulkanShaderChain::validateLookupTextures(std::vector<LookupTexture>& textures, std::string& error) const
{
	uint64_t bytes = 0;
	for (auto& lookup : textures) {
		const auto decoded = postprocess::loadShaderTextureData(lookup.config, error);
		if (!decoded) return false;
		const auto& data = *decoded;
		// Upload storage remains alive until the chain retires, so account for it.
		if (data.pixels.size() > (postprocess::MaxShaderChainTargetBytes - bytes) / 2) { error = "Shader lookup GPU/upload storage exceeds the chain budget."; return false; }
		bytes += data.pixels.size() * 2;
		lookup.config.decoded = decoded;
	}
	return true;
}

bool VulkanShaderChain::ensureLookupTextures(vk::CommandBuffer commands, std::vector<LookupTexture>& textures, std::string& error)
{
	auto *context = VulkanContext::Instance();
	for (auto& lookup : textures) {
		if (lookup.image.image) continue;
		const auto decoded = postprocess::loadShaderTextureData(lookup.config, error);
		if (!decoded) return false;
		const auto& data = *decoded;
		if (lookup.config.numericType != postprocess::ShaderNumericType::Float && (lookup.config.linearFilter || lookup.config.useMipmaps)) { error = "Integer textures require nearest filtering without generated mipmaps."; return false; }
		try {
			auto& image = lookup.image;
			image.format = resolveTargetFormat(postprocess::ShaderTargetFormat::Default, data.formatName, error);
			if (image.format == vk::Format::eUndefined) return false;
			const auto features = context->GetPhysicalDevice().getFormatProperties(image.format).optimalTilingFeatures;
			if (!(features & vk::FormatFeatureFlagBits::eSampledImage) || (lookup.config.linearFilter && !(features & vk::FormatFeatureFlagBits::eSampledImageFilterLinear))) { error = "Vulkan does not support the requested lookup texture format/filter."; return false; }
			const bool volume = data.type == postprocess::SlangResourceType::Sampler3D || data.type == postprocess::SlangResourceType::Texture3D;
			const bool line = data.type == postprocess::SlangResourceType::Sampler1D || data.type == postprocess::SlangResourceType::Texture1D;
			const bool cube = data.type == postprocess::SlangResourceType::SamplerCube || data.type == postprocess::SlangResourceType::TextureCube;
			image.extent = vk::Extent2D(data.width, data.height); image.depth = data.depth; image.layers = data.layers; image.type = data.type;
			image.mipLevels = 1; if (lookup.config.useMipmaps) for (uint32_t n = std::max({data.width, data.height, data.depth}); n > 1; n >>= 1) ++image.mipLevels;
			if (image.mipLevels > 1 && (!(features & vk::FormatFeatureFlagBits::eBlitSrc) || !(features & vk::FormatFeatureFlagBits::eBlitDst) || !(features & vk::FormatFeatureFlagBits::eSampledImageFilterLinear))) { error = "Vulkan cannot generate the requested lookup mip chain."; return false; }
			const vk::ImageType type = volume ? vk::ImageType::e3D : line ? vk::ImageType::e1D : vk::ImageType::e2D;
			const vk::ImageViewType view = volume ? vk::ImageViewType::e3D : line ? vk::ImageViewType::e1D : cube ? vk::ImageViewType::eCube : (data.type == postprocess::SlangResourceType::Sampler2DArray || data.type == postprocess::SlangResourceType::Texture2DArray) ? vk::ImageViewType::e2DArray : vk::ImageViewType::e2D;
			image.image = context->GetDevice().createImageUnique(vk::ImageCreateInfo(cube ? vk::ImageCreateFlagBits::eCubeCompatible : vk::ImageCreateFlags(), type, image.format,
					vk::Extent3D(data.width, data.height, data.depth), image.mipLevels, data.layers, vk::SampleCountFlagBits::e1, vk::ImageTiling::eOptimal,
					vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc));
			VmaAllocationCreateInfo allocationInfo { {}, VMA_MEMORY_USAGE_GPU_ONLY };
			image.allocation = context->GetAllocator().AllocateForImage(image.image.get(), allocationInfo);
			const vk::ImageSubresourceRange range(vk::ImageAspectFlagBits::eColor, 0, image.mipLevels, 0, data.layers);
			image.sampleView = context->GetDevice().createImageViewUnique(vk::ImageViewCreateInfo({}, image.image.get(), view, image.format, {}, range));
			lookup.staging = std::make_unique<BufferData>(data.pixels.size(), vk::BufferUsageFlagBits::eTransferSrc);
			lookup.staging->upload(static_cast<u32>(data.pixels.size()), data.pixels.data());
			commands.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe, vk::PipelineStageFlagBits::eTransfer, {}, nullptr, nullptr,
					vk::ImageMemoryBarrier({}, vk::AccessFlagBits::eTransferWrite, vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, image.image.get(), range));
			const vk::BufferImageCopy copy(0, 0, 0, vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, data.layers), {}, vk::Extent3D(data.width, data.height, data.depth));
			commands.copyBufferToImage(lookup.staging->buffer.get(), image.image.get(), vk::ImageLayout::eTransferDstOptimal, copy);
			vk::Extent3D extent(data.width, data.height, data.depth);
			for (uint32_t level = 1; level < image.mipLevels; ++level) {
				const vk::ImageSubresourceRange previous(vk::ImageAspectFlagBits::eColor, level - 1, 1, 0, data.layers);
				commands.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer, {}, nullptr, nullptr,
						vk::ImageMemoryBarrier(vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eTransferRead, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eTransferSrcOptimal, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, image.image.get(), previous));
				const vk::Extent3D next(std::max(1u, extent.width / 2), std::max(1u, extent.height / 2), std::max(1u, extent.depth / 2));
				const std::array<vk::Offset3D,2> from { vk::Offset3D(), vk::Offset3D(extent.width, extent.height, extent.depth) };
				const std::array<vk::Offset3D,2> to { vk::Offset3D(), vk::Offset3D(next.width, next.height, next.depth) };
				const vk::ImageBlit blit(vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, level - 1, 0, data.layers), from,
						vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, level, 0, data.layers), to);
				commands.blitImage(image.image.get(), vk::ImageLayout::eTransferSrcOptimal, image.image.get(), vk::ImageLayout::eTransferDstOptimal, blit, vk::Filter::eLinear);
				commands.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, shaderReadStages(), {}, nullptr, nullptr,
						vk::ImageMemoryBarrier(vk::AccessFlagBits::eTransferRead, vk::AccessFlagBits::eShaderRead, vk::ImageLayout::eTransferSrcOptimal, vk::ImageLayout::eShaderReadOnlyOptimal, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, image.image.get(), previous));
				extent = next;
			}
			commands.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, shaderReadStages(), {}, nullptr, nullptr,
					vk::ImageMemoryBarrier(vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eShaderRead, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, image.image.get(), vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, image.mipLevels - 1, 1, 0, data.layers)));
			vk::SamplerCreateInfo sampler;
			sampler.magFilter = sampler.minFilter = lookup.config.linearFilter ? vk::Filter::eLinear : vk::Filter::eNearest;
			sampler.mipmapMode = lookup.config.linearFilter ? vk::SamplerMipmapMode::eLinear : vk::SamplerMipmapMode::eNearest;
			sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = samplerAddressMode(lookup.config.wrapMode);
			sampler.maxLod = static_cast<float>(image.mipLevels - 1);
			sampler.borderColor = lookup.config.numericType == postprocess::ShaderNumericType::Float ? vk::BorderColor::eFloatTransparentBlack : vk::BorderColor::eIntTransparentBlack;
			lookup.sampler = context->GetDevice().createSamplerUnique(sampler);
			lookup.config.decoded.reset();
		} catch (const std::exception& exception) { error = exception.what(); return false; }
	}
	return true;
}

bool VulkanShaderChain::render(vk::Image sourceImage, vk::ImageView sourceView, const vk::Extent2D& extent,
		const postprocess::ShaderFrameInfo& frame, vk::ImageView& outputView,
		bool& presentationTransformApplied, vk::Image depthImage, vk::ImageView depthView)
{
	try {
		return renderInternal(sourceImage, sourceView, extent, frame, outputView, presentationTransformApplied, depthImage, depthView);
	} catch (const std::exception& exception) {
		try {
			postprocess::reportShaderFrameStatus(activePath, exception.what());
			WARN_LOG(RENDERER, "Vulkan post-processing failed: %s", exception.what());
		} catch (...) {
			// PresentFrame is noexcept; diagnostics must never turn recovery into termination.
		}
		return false;
	} catch (...) {
		return false;
	}
}

bool VulkanShaderChain::renderInternal(vk::Image sourceImage, vk::ImageView sourceView, const vk::Extent2D& extent,
		const postprocess::ShaderFrameInfo& inputFrame, vk::ImageView& outputView,
		bool& presentationTransformApplied, vk::Image depthImage, vk::ImageView depthView)
{
	postprocess::ShaderFrameInfo frame = inputFrame;
	const std::array<int,4> currentStencilSize { frame.sourceWidth, frame.sourceHeight, frame.viewportWidth, frame.viewportHeight };
	if (stencilFrameSize != currentStencilSize) {
		if (!targets.empty() || !sourceTargets.empty() || !displayTargets.empty()) {
			VulkanContext::Instance()->GetDevice().waitIdle();
			targets.clear(); sourceTargets.clear(); displayTargets.clear(); depthRotateTargets.clear();
			stencilTargets.clear();
		}
		stencilFrameSize = currentStencilSize;
	}

	presentationTransformApplied = false;
	bool composedDisplayInput = false;
	if (!updateConfiguredChain() || !sourceImage || !sourceView || extent.width == 0 || extent.height == 0)
		return false;
	frame.frameCount = renderedFrames;
	VulkanContext *context = VulkanContext::Instance();
	vk::CommandBuffer commandBuffer = context->GetCurrentCommandBuffer();
	const bool needsDepth = std::any_of(programs.begin(), programs.end(), [](const Program& program) {
		return std::any_of(program.bindings.begin(), program.bindings.end(), [](const Binding& binding) {
			return binding.input.kind == postprocess::ShaderInputKind::Depth;
		});
	});
	if (needsDepth && (!depthImage || !depthView)) {
		postprocess::reportShaderFrameStatus(activePath, "This frame has no scene depth available for the selected shader.");
		return false;
	}
	// Restore the renderer's depth layout on every exit, including a failed pass.
	// The borrowed image remains owned by the scene renderer throughout the draw.
	struct DepthReadScope {
		vk::CommandBuffer commands;
		vk::Image image;
		DepthReadScope(vk::CommandBuffer commands, vk::Image image) : commands(commands), image(image)
		{
			if (!image)
				return;
			const vk::ImageMemoryBarrier barrier(vk::AccessFlagBits::eDepthStencilAttachmentWrite,
					vk::AccessFlagBits::eShaderRead, vk::ImageLayout::eDepthStencilAttachmentOptimal,
					vk::ImageLayout::eDepthStencilReadOnlyOptimal, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
					image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil, 0, 1, 0, 1));
			commands.pipelineBarrier(vk::PipelineStageFlagBits::eEarlyFragmentTests | vk::PipelineStageFlagBits::eLateFragmentTests,
					shaderReadStages(), {}, nullptr, nullptr, barrier);
		}
		~DepthReadScope()
		{
			if (!image)
				return;
			const vk::ImageMemoryBarrier barrier(vk::AccessFlagBits::eShaderRead,
					vk::AccessFlagBits::eDepthStencilAttachmentRead | vk::AccessFlagBits::eDepthStencilAttachmentWrite,
					vk::ImageLayout::eDepthStencilReadOnlyOptimal, vk::ImageLayout::eDepthStencilAttachmentOptimal,
					VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, image,
					vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil, 0, 1, 0, 1));
			commands.pipelineBarrier(shaderReadStages(),
					vk::PipelineStageFlagBits::eEarlyFragmentTests | vk::PipelineStageFlagBits::eLateFragmentTests,
					{}, nullptr, nullptr, barrier);
		}
	} depthRead(commandBuffer, needsDepth ? depthImage : vk::Image {});
	const size_t imageIndex = static_cast<size_t>(context->GetCurrentImageIndex());
	if (targets.size() <= imageIndex)
		targets.resize(imageIndex + 1);
	if (targets[imageIndex].size() < programs.size())
		targets[imageIndex].resize(programs.size());
	if (sourceTargets.size() <= imageIndex)
		sourceTargets.resize(imageIndex + 1);
	std::string lookupError;
	if (!ensureLookupTextures(commandBuffer, lookupError)) {
		postprocess::reportShaderFrameStatus(activePath, lookupError);
		WARN_LOG(RENDERER, "Post-processing lookup texture failed: %s", lookupError.c_str());
		return false;
	}
	const vk::ImageMemoryBarrier sourceBarrier(vk::AccessFlagBits::eColorAttachmentWrite, vk::AccessFlagBits::eShaderRead,
			vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
			VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, sourceImage,
			vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
	commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput, shaderReadStages(),
			{}, nullptr, nullptr, sourceBarrier);
	if (descriptorSets.size() <= imageIndex)
		descriptorSets.resize(imageIndex + 1);
	if (sourceDescriptorSets.size() <= imageIndex)
		sourceDescriptorSets.resize(imageIndex + 1);
	if (descriptorSets[imageIndex].size() < programs.size())
		descriptorSets[imageIndex].resize(programs.size());
	postprocess::ShaderFrameInfo passFrame = frame;
	const uint64_t statusGeneration = postprocess::shaderChainStatusGeneration();
	if (statusGeneration != parameterGeneration) {
		const postprocess::ShaderChainStatus status = postprocess::getShaderChainStatus();
		parameterGeneration = statusGeneration;
		if (status.activePath == activePath && status.error.empty())
			parameters = status.parameters;
	}
	postprocess::updateShaderHostParameters(parameters, frame);
	uint64_t targetBytes = 0;
	std::string targetBudgetError;
	const uint32_t swapchainCopies = std::max(1u, context->GetSwapChainSize());
	postprocess::ShaderFrameInfo budgetFrame = frame;
	const auto displayBoundary = std::find_if(programs.begin(), programs.end(), [](const Program& program) {
		return program.pass.composeDisplayInput;
	});
	const bool rotateDepth = needsDepth && frame.rotationPending && displayBoundary != programs.end()
			&& postprocess::shouldComposeDisplayInput(frame, false);
	// R32F retains scene-depth precision; it uses four bytes per pixel, just like
	// the default color format used by the shared target budget calculation.
	if (rotateDepth && !postprocess::accumulateShaderTargetBytes(extent.height, extent.width,
			postprocess::ShaderTargetFormat::Default, {}, false, swapchainCopies, targetBytes, targetBudgetError)) {
		postprocess::reportShaderFrameStatus(activePath, targetBudgetError);
		return false;
	}
	if (displayBoundary != programs.end() && postprocess::shouldComposeDisplayInput(frame, false)) {
		if (!postprocess::accumulateShaderTargetBytes(frame.viewportWidth, frame.viewportHeight,
				postprocess::ShaderTargetFormat::Default, {}, displayBoundary->pass.useMipmaps,
				swapchainCopies, targetBytes, targetBudgetError)) {
			postprocess::reportShaderFrameStatus(activePath, targetBudgetError);
			return false;
		}
		if (displayBoundary == programs.begin()) {
			budgetFrame.sourceWidth = frame.viewportWidth;
			budgetFrame.sourceHeight = frame.viewportHeight;
		}
	}
	if ((programs.front().pass.useMipmaps || sourceHistoryDepth > 1)
			&& !postprocess::accumulateShaderTargetBytes(budgetFrame.sourceWidth, budgetFrame.sourceHeight,
					postprocess::ShaderTargetFormat::Default, {}, programs.front().pass.useMipmaps,
					sourceHistoryDepth * swapchainCopies, targetBytes, targetBudgetError)) {
		postprocess::reportShaderFrameStatus(activePath, targetBudgetError);
		return false;
	}
	bool budgetDisplaySpaceReached = false;
	for (Program& program : programs) {
		if (program.pass.composeDisplayInput && !budgetDisplaySpaceReached) {
			budgetDisplaySpaceReached = true;
			if (postprocess::shouldComposeDisplayInput(frame, false)) {
				budgetFrame.sourceWidth = frame.viewportWidth;
				budgetFrame.sourceHeight = frame.viewportHeight;
			}
		}
		int width;
		int height;
		if (!postprocess::resolveTargetSize(program.pass, budgetFrame, width, height, targetBudgetError)) {
			postprocess::reportShaderFrameStatus(activePath, targetBudgetError);
			return false;
		}
		program.targetWidth = static_cast<uint32_t>(width);
		program.targetHeight = static_cast<uint32_t>(height);
		for (const Program::Output& output : program.outputs)
			if (!postprocess::accumulateShaderTargetBytes(width, height, output.config.targetFormat,
					output.config.targetFormatName, output.needsMipmaps,
					program.historyDepth * swapchainCopies * output.config.depth, targetBytes, targetBudgetError)) {
				postprocess::reportShaderFrameStatus(activePath, targetBudgetError);
				return false;
			}
		if (program.pass.renderState.stencilEnabled
				&& !postprocess::accumulateShaderTargetBytes(width, height, postprocess::ShaderTargetFormat::Default,
						"R32_UINT", false, swapchainCopies, targetBytes, targetBudgetError)) {
			postprocess::reportShaderFrameStatus(activePath, targetBudgetError);
			return false;
		}

		if (program.pass.updatesSource) { budgetFrame.sourceWidth = width; budgetFrame.sourceHeight = height; }
	}
	vk::ImageView inputView = sourceView;
	vk::ImageView originalView = sourceView;
	vk::ImageView effectiveDepthView = depthView;
	vk::ImageLayout effectiveDepthLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal;
	vk::Extent2D inputExtent = extent;
	vk::Extent2D originalExtent = extent;
	bool inputHasMipmaps = false;
	bool displaySpaceReached = false;
	if (displayTargets.size() <= imageIndex)
		displayTargets.resize(imageIndex + 1);
	if (displayDescriptorSets.size() <= imageIndex)
		displayDescriptorSets.resize(imageIndex + 1);
	auto composeDisplayInput = [&](bool needsMipmaps) {
		if (displaySpaceReached)
			return true;
		displaySpaceReached = true;
		if (!postprocess::shouldComposeDisplayInput(frame, false))
			return true;
		const vk::Extent2D displayExtent(static_cast<uint32_t>(frame.viewportWidth),
				static_cast<uint32_t>(frame.viewportHeight));
		Target& displayTarget = displayTargets[imageIndex];
		try {
			const bool allocated = ensureTarget(displayTarget, displayExtent,
					vk::Format::eR8G8B8A8Unorm, needsMipmaps);
			const bool initialized = displayTarget.initialized && !allocated;
			const vk::ImageMemoryBarrier targetBarrier(initialized ? vk::AccessFlagBits::eShaderRead : vk::AccessFlags(),
					vk::AccessFlagBits::eColorAttachmentWrite,
					initialized ? vk::ImageLayout::eShaderReadOnlyOptimal : vk::ImageLayout::eUndefined,
					vk::ImageLayout::eColorAttachmentOptimal, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
					displayTarget.image.get(), vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
			commandBuffer.pipelineBarrier(initialized ? shaderReadStages() : vk::PipelineStageFlagBits::eTopOfPipe,
					vk::PipelineStageFlagBits::eColorAttachmentOutput, {}, nullptr, nullptr, targetBarrier);
			if (!displayDescriptorSets[imageIndex])
				displayDescriptorSets[imageIndex] = std::move(context->GetDevice().allocateDescriptorSetsUnique(
						vk::DescriptorSetAllocateInfo(context->GetDescriptorPool(), descriptorSetLayout.get())).front());
			const vk::Sampler sampler = config::LinearInterpolation ? linearSampler.get() : nearestSampler.get();
			const vk::DescriptorImageInfo imageInfo(sampler, inputView, vk::ImageLayout::eShaderReadOnlyOptimal);
			context->GetDevice().updateDescriptorSets(vk::WriteDescriptorSet(displayDescriptorSets[imageIndex].get(), 0, 0,
					vk::DescriptorType::eCombinedImageSampler, imageInfo), nullptr);
			if (!displayTarget.framebuffer) {
				const std::array<vk::ImageView, 1> attachments = { displayTarget.renderView.get() };
				displayTarget.framebuffer = context->GetDevice().createFramebufferUnique(
						vk::FramebufferCreateInfo({}, renderPass.get(), attachments,
							displayExtent.width, displayExtent.height, 1));
			}
			const vk::ClearValue clearColor(vk::ClearColorValue(std::array<float, 4> { 0.f, 0.f, 0.f, 1.f }));
			commandBuffer.beginRenderPass(vk::RenderPassBeginInfo(renderPass.get(), displayTarget.framebuffer.get(),
					vk::Rect2D({}, displayExtent), clearColor), vk::SubpassContents::eInline);
			commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics,
					frame.rotationPending ? copyRotatePipeline.get() : copyPipeline.get());
			commandBuffer.setViewport(0, vk::Viewport(0.f, 0.f, static_cast<float>(displayExtent.width),
					static_cast<float>(displayExtent.height), 0.f, 1.f));
			commandBuffer.setScissor(0, vk::Rect2D({}, displayExtent));
			commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipelineLayout.get(), 0,
					displayDescriptorSets[imageIndex].get(), nullptr);
			quadBuffer->Update();
			quadBuffer->Bind(commandBuffer);
			quadBuffer->Draw(commandBuffer);
			commandBuffer.endRenderPass();
			if (needsMipmaps)
				generateMipmaps(commandBuffer, displayTarget, initialized);
			displayTarget.renderedFrame = frame.frameCount;
			displayTarget.initialized = true;
			if (rotateDepth) {
				// Apply the exact vertex transform used for color at the same graph
				// boundary. Earlier raw-input passes still see the original depth.
				if (!depthRotateRenderPass) {
					const auto features = context->GetPhysicalDevice().getFormatProperties(vk::Format::eR32Sfloat).optimalTilingFeatures;
					const vk::FormatFeatureFlags required = vk::FormatFeatureFlagBits::eColorAttachment | vk::FormatFeatureFlagBits::eSampledImage;
					if ((features & required) != required)
						throw std::runtime_error("Vulkan rotated depth requires sampled R32F color-attachment support.");
					depthRotateRenderPass = createRenderPass({ vk::Format::eR32Sfloat });
				}
				if (!depthRotatePipeline)
					depthRotatePipeline = createPipeline(context->GetShaderManager()->GetQuadVertexShader(true),
							copyFragmentShader.get(), depthRotateRenderPass.get(), postprocess::ShaderPass {}, 1);
				if (depthRotateTargets.size() <= imageIndex)
					depthRotateTargets.resize(imageIndex + 1);
				if (depthRotateDescriptorSets.size() <= imageIndex)
					depthRotateDescriptorSets.resize(imageIndex + 1);
				Target& depthTarget = depthRotateTargets[imageIndex];
				const vk::Extent2D depthExtent(extent.height, extent.width);
				ensureTarget(depthTarget, depthExtent, vk::Format::eR32Sfloat, false);
				const vk::ImageMemoryBarrier depthBarrier(depthTarget.initialized ? vk::AccessFlagBits::eShaderRead : vk::AccessFlags(),
						vk::AccessFlagBits::eColorAttachmentWrite,
						depthTarget.initialized ? vk::ImageLayout::eShaderReadOnlyOptimal : vk::ImageLayout::eUndefined,
						vk::ImageLayout::eColorAttachmentOptimal, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
						depthTarget.image.get(), vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
				commandBuffer.pipelineBarrier(depthTarget.initialized ? shaderReadStages() : vk::PipelineStageFlagBits::eTopOfPipe,
						vk::PipelineStageFlagBits::eColorAttachmentOutput, {}, nullptr, nullptr, depthBarrier);
				if (!depthRotateDescriptorSets[imageIndex])
					depthRotateDescriptorSets[imageIndex] = std::move(context->GetDevice().allocateDescriptorSetsUnique(
							vk::DescriptorSetAllocateInfo(context->GetDescriptorPool(), descriptorSetLayout.get())).front());
				const vk::DescriptorImageInfo depthInfo(nearestSampler.get(), depthView, vk::ImageLayout::eDepthStencilReadOnlyOptimal);
				context->GetDevice().updateDescriptorSets(vk::WriteDescriptorSet(depthRotateDescriptorSets[imageIndex].get(), 0, 0,
						vk::DescriptorType::eCombinedImageSampler, depthInfo), nullptr);
				if (!depthTarget.framebuffer) {
					const std::array<vk::ImageView, 1> attachments = { depthTarget.renderView.get() };
					depthTarget.framebuffer = context->GetDevice().createFramebufferUnique(vk::FramebufferCreateInfo({},
							depthRotateRenderPass.get(), attachments, depthExtent.width, depthExtent.height, 1));
				}
				commandBuffer.beginRenderPass(vk::RenderPassBeginInfo(depthRotateRenderPass.get(), depthTarget.framebuffer.get(),
						vk::Rect2D({}, depthExtent), clearColor), vk::SubpassContents::eInline);
				commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, depthRotatePipeline.get());
				commandBuffer.setViewport(0, vk::Viewport(0.f, 0.f, static_cast<float>(depthExtent.width),
						static_cast<float>(depthExtent.height), 0.f, 1.f));
				commandBuffer.setScissor(0, vk::Rect2D({}, depthExtent));
				commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipelineLayout.get(), 0,
						depthRotateDescriptorSets[imageIndex].get(), nullptr);
				quadBuffer->Bind(commandBuffer);
				quadBuffer->Draw(commandBuffer);
				commandBuffer.endRenderPass();
				depthTarget.initialized = true;
				depthTarget.renderedFrame = frame.frameCount;
				effectiveDepthView = depthTarget.sampleView.get();
				effectiveDepthLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
			}
		} catch (const std::exception& exception) {
			postprocess::reportShaderFrameStatus(activePath, exception.what());
			WARN_LOG(RENDERER, "Post-processing display composition failed: %s", exception.what());
			return false;
		}
		inputView = displayTarget.sampleView.get();
		originalView = inputView;
		inputExtent = displayExtent;
		originalExtent = displayExtent;
		inputHasMipmaps = needsMipmaps;
		passFrame.sourceWidth = frame.viewportWidth;
		passFrame.sourceHeight = frame.viewportHeight;
		composedDisplayInput = true;
		return true;
	};
	if (programs.front().pass.composeDisplayInput
			&& !composeDisplayInput(programs.front().pass.useMipmaps))
		return false;

	const size_t currentSourceTarget = frame.frameCount % sourceHistoryDepth;
	const bool sourceNeedsMipmaps = programs.front().pass.useMipmaps && !inputHasMipmaps;
	if (sourceNeedsMipmaps || sourceHistoryDepth > 1) {
		if (sourceTargets[imageIndex].size() != sourceHistoryDepth)
			sourceTargets[imageIndex].resize(sourceHistoryDepth);
		Target& sourceTarget = sourceTargets[imageIndex][currentSourceTarget];
		bool sourceWasInitialized = false;
		try {
			const bool allocated = ensureTarget(sourceTarget, inputExtent,
					vk::Format::eR8G8B8A8Unorm, sourceNeedsMipmaps);
			sourceWasInitialized = sourceTarget.initialized && !allocated;
		} catch (const std::exception& exception) {
			postprocess::reportShaderFrameStatus(activePath, exception.what());
			WARN_LOG(RENDERER, "Post-processing source target allocation failed: %s", exception.what());
			return false;
		}
		const vk::ImageMemoryBarrier targetBarrier(sourceWasInitialized ? vk::AccessFlagBits::eShaderRead : vk::AccessFlags(),
				vk::AccessFlagBits::eColorAttachmentWrite,
				sourceWasInitialized ? vk::ImageLayout::eShaderReadOnlyOptimal : vk::ImageLayout::eUndefined,
				vk::ImageLayout::eColorAttachmentOptimal, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
				sourceTarget.image.get(), vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
		commandBuffer.pipelineBarrier(sourceWasInitialized ? shaderReadStages() : vk::PipelineStageFlagBits::eTopOfPipe,
				vk::PipelineStageFlagBits::eColorAttachmentOutput, {}, nullptr, nullptr, targetBarrier);
		if (!sourceDescriptorSets[imageIndex]) {
			sourceDescriptorSets[imageIndex] = std::move(context->GetDevice().allocateDescriptorSetsUnique(
					vk::DescriptorSetAllocateInfo(context->GetDescriptorPool(), descriptorSetLayout.get())).front());
		}
		const vk::DescriptorImageInfo imageInfo(nearestSampler.get(), inputView, vk::ImageLayout::eShaderReadOnlyOptimal);
		context->GetDevice().updateDescriptorSets(vk::WriteDescriptorSet(sourceDescriptorSets[imageIndex].get(), 0, 0,
				vk::DescriptorType::eCombinedImageSampler, imageInfo), nullptr);
		const vk::ClearValue clearColor(vk::ClearColorValue(std::array<float, 4> { 0.f, 0.f, 0.f, 1.f }));
		if (!sourceTarget.framebuffer) {
			const std::array<vk::ImageView, 1> sourceAttachments = { sourceTarget.renderView.get() };
			sourceTarget.framebuffer = context->GetDevice().createFramebufferUnique(
					vk::FramebufferCreateInfo({}, renderPass.get(), sourceAttachments,
							inputExtent.width, inputExtent.height, 1));
		}
		commandBuffer.beginRenderPass(vk::RenderPassBeginInfo(renderPass.get(), sourceTarget.framebuffer.get(),
				vk::Rect2D({}, inputExtent), clearColor), vk::SubpassContents::eInline);
		commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, copyPipeline.get());
		commandBuffer.setViewport(0, vk::Viewport(0.f, 0.f, static_cast<float>(inputExtent.width),
				static_cast<float>(inputExtent.height), 0.f, 1.f));
		commandBuffer.setScissor(0, vk::Rect2D({}, inputExtent));
		commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipelineLayout.get(), 0, sourceDescriptorSets[imageIndex].get(), nullptr);
		ShaderFrameUniforms uniforms {};
		uniforms.sizes[0] = static_cast<float>(inputExtent.width);
		uniforms.sizes[1] = static_cast<float>(inputExtent.height);
		uniforms.sizes[2] = static_cast<float>(inputExtent.width);
		uniforms.sizes[3] = static_cast<float>(inputExtent.height);
		uniforms.frameCount = frame.frameCount;
		uniforms.rotation = frame.rotationPending ? 1u : 0u;
		uniforms.originalWidth = originalExtent.width;
		uniforms.originalHeight = originalExtent.height;
		commandBuffer.pushConstants(pipelineLayout.get(), shaderStages(),
				0, sizeof(uniforms), &uniforms);
		quadBuffer->Update();
		quadBuffer->Bind(commandBuffer);
		quadBuffer->Draw(commandBuffer);
		commandBuffer.endRenderPass();
		if (sourceNeedsMipmaps)
			generateMipmaps(commandBuffer, sourceTarget, sourceWasInitialized);
		sourceTarget.renderedFrame = frame.frameCount;
		sourceTarget.initialized = true;
		if (sourceNeedsMipmaps) {
			inputView = sourceTarget.sampleView.get();
			inputHasMipmaps = sourceNeedsMipmaps;
		}
	}
	for (size_t index = 0; index < programs.size(); ++index) {
		Program& program = programs[index];
		if (program.pass.composeDisplayInput && !composeDisplayInput(program.pass.useMipmaps))
			return false;
		if (index == 0 || program.pass.layerIndex != programs[index - 1].pass.layerIndex) {
			originalView = inputView;
			originalExtent = inputExtent;
		}
		const int outputWidth = static_cast<int>(program.targetWidth);
		const int outputHeight = static_cast<int>(program.targetHeight);
		std::string error;
		const vk::Extent2D targetExtent(static_cast<uint32_t>(outputWidth), static_cast<uint32_t>(outputHeight));
		auto& targetHistory = targets[imageIndex][index];
		if (targetHistory.size() != program.historyDepth)
			targetHistory.resize(program.historyDepth);
		const size_t currentTarget = frame.frameCount % program.historyDepth;
		TargetSet& targetSet = targetHistory[currentTarget];
		try {
			// History and feedback sample a previous target set. Allocate every
			// attachment now so the first-frame fallback never exposes undefined memory.
			for (TargetSet& historyTarget : targetHistory)
				ensureTargetSet(historyTarget, program, targetExtent);
		} catch (const std::exception& exception) {
			postprocess::reportShaderFrameStatus(activePath, exception.what());
			WARN_LOG(RENDERER, "Post-processing target allocation failed: %s", exception.what());
			return false;
		}
		program.targetWasInitialized.clear();
		for (const Target& target : targetSet.outputs)
			program.targetWasInitialized.push_back(target.initialized);
		for (const auto& target : targetSet.outputs) {
			if (target.initialized || target.mipLevels == 1) continue;
			const vk::ImageSubresourceRange range(vk::ImageAspectFlagBits::eColor, 1, target.mipLevels - 1, 0, target.layers);
			commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe, vk::PipelineStageFlagBits::eTransfer, {}, nullptr, nullptr,
					vk::ImageMemoryBarrier({}, vk::AccessFlagBits::eTransferWrite, vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
							VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, target.image.get(), range));
			commandBuffer.clearColorImage(target.image.get(), vk::ImageLayout::eTransferDstOptimal, vk::ClearColorValue(std::array<uint32_t, 4>{}), range);
			commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, shaderReadStages(), {}, nullptr, nullptr,
					vk::ImageMemoryBarrier(vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eShaderRead,
							vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, target.image.get(), range));
		}
		program.targetBarriers.clear();
		bool hasInitializedTarget = false;
		for (size_t outputIndex = 0; outputIndex < targetSet.outputs.size(); ++outputIndex) {
			const Target& target = targetSet.outputs[outputIndex];
			const bool initialized = program.targetWasInitialized[outputIndex];
			hasInitializedTarget = hasInitializedTarget || initialized;
			program.targetBarriers.emplace_back(initialized ? vk::AccessFlagBits::eShaderRead : vk::AccessFlags(),
					(outputIndex >= program.pass.colorOutputCount ? vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite : vk::AccessFlags(vk::AccessFlagBits::eColorAttachmentRead | vk::AccessFlagBits::eColorAttachmentWrite)),
					initialized ? vk::ImageLayout::eShaderReadOnlyOptimal : vk::ImageLayout::eUndefined,
					(outputIndex >= program.pass.colorOutputCount ? vk::ImageLayout::eGeneral : vk::ImageLayout::eColorAttachmentOptimal), VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
					target.image.get(), vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, target.layers));
		}
		commandBuffer.pipelineBarrier(hasInitializedTarget ? shaderReadStages() : vk::PipelineStageFlagBits::eTopOfPipe,
				vk::PipelineStageFlagBits::eColorAttachmentOutput | shaderReadStages(), {}, nullptr, nullptr, program.targetBarriers);
		for (size_t i = 0; i < targetSet.outputs.size(); ++i) {
			if (program.targetWasInitialized[i] && !program.pass.renderState.clearRenderTargets) continue;
			const bool storage = i >= program.pass.colorOutputCount;
			if (!storage && !(program.pass.preserveBackbuffer || program.pass.blendEnabled) && program.pass.updatesSource && !program.pass.renderState.clearRenderTargets) continue;
			const auto& image = targetSet.outputs[i];
			const auto layout = storage ? vk::ImageLayout::eGeneral : vk::ImageLayout::eColorAttachmentOptimal;
			const auto access = storage ? vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite : vk::AccessFlagBits::eColorAttachmentWrite;
			const auto stages = storage ? shaderReadStages() : vk::PipelineStageFlags(vk::PipelineStageFlagBits::eColorAttachmentOutput);
			const vk::ImageSubresourceRange range(vk::ImageAspectFlagBits::eColor, 0, 1, 0, image.layers);
			commandBuffer.pipelineBarrier(stages, vk::PipelineStageFlagBits::eTransfer, {}, nullptr, nullptr,
					vk::ImageMemoryBarrier(access, vk::AccessFlagBits::eTransferWrite, layout, vk::ImageLayout::eTransferDstOptimal, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, image.image.get(), range));
			commandBuffer.clearColorImage(image.image.get(), vk::ImageLayout::eTransferDstOptimal, vk::ClearColorValue(std::array<uint32_t,4>{0,0,0,0}), range);
			commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, stages, {}, nullptr, nullptr,
					vk::ImageMemoryBarrier(vk::AccessFlagBits::eTransferWrite, access, vk::ImageLayout::eTransferDstOptimal, layout, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, image.image.get(), range));
		}
		for (size_t outputIndex = 0; outputIndex < program.pass.outputs.size(); ++outputIndex) {
			const auto& config = program.pass.outputs[outputIndex];
			if (config.previousPass == postprocess::InvalidShaderPass || (config.previousFrame && frame.frameCount == 0))
				continue;
			const uint32_t requestedFrame = frame.frameCount - (config.previousFrame ? 1u : 0u);
			const TargetSet *previous = config.previousFrame ? findPassHistoryTarget(config.previousPass, requestedFrame)
					: &targets[imageIndex][config.previousPass][requestedFrame % programs[config.previousPass].historyDepth];
			if (!previous || !previous->initialized || previous->renderedFrame != requestedFrame)
				continue;
			const auto& source = previous->outputs[config.previousOutput];
			const auto& destination = targetSet.outputs[outputIndex];
			if (source.extent != destination.extent || source.depth != destination.depth || source.type != destination.type || source.format != destination.format)
				continue; // A resized texture starts from its cleared initial contents.
			const bool storage = outputIndex >= program.pass.colorOutputCount;
			const auto layout = storage ? vk::ImageLayout::eGeneral : vk::ImageLayout::eColorAttachmentOptimal;
			const auto stages = storage ? shaderReadStages() : vk::PipelineStageFlags(vk::PipelineStageFlagBits::eColorAttachmentOutput);
			const auto access = storage ? vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite
					: vk::AccessFlags(vk::AccessFlagBits::eColorAttachmentRead | vk::AccessFlagBits::eColorAttachmentWrite);
			const vk::ImageSubresourceRange range(vk::ImageAspectFlagBits::eColor, 0, 1, 0, source.layers);
			const std::array<vk::ImageMemoryBarrier, 2> before {
				vk::ImageMemoryBarrier(vk::AccessFlagBits::eShaderRead, vk::AccessFlagBits::eTransferRead,
						vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageLayout::eTransferSrcOptimal,
						VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, source.image.get(), range),
				vk::ImageMemoryBarrier(access, vk::AccessFlagBits::eTransferWrite, layout, vk::ImageLayout::eTransferDstOptimal,
						VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, destination.image.get(), range)
			};
			commandBuffer.pipelineBarrier(shaderReadStages() | stages, vk::PipelineStageFlagBits::eTransfer, {}, nullptr, nullptr, before);
			const vk::ImageSubresourceLayers layers(vk::ImageAspectFlagBits::eColor, 0, 0, source.layers);
			commandBuffer.copyImage(source.image.get(), vk::ImageLayout::eTransferSrcOptimal, destination.image.get(), vk::ImageLayout::eTransferDstOptimal,
					vk::ImageCopy(layers, {}, layers, {}, vk::Extent3D(source.extent, source.depth)));
			const std::array<vk::ImageMemoryBarrier, 2> after {
				vk::ImageMemoryBarrier(vk::AccessFlagBits::eTransferRead, vk::AccessFlagBits::eShaderRead,
						vk::ImageLayout::eTransferSrcOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
						VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, source.image.get(), range),
				vk::ImageMemoryBarrier(vk::AccessFlagBits::eTransferWrite, access, vk::ImageLayout::eTransferDstOptimal, layout,
						VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, destination.image.get(), range)
			};
			commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, shaderReadStages() | stages, {}, nullptr, nullptr, after);
		}
		if (program.pass.renderState.stencilEnabled) {
			auto& stencil = stencilTargets[imageIndex][{targetExtent.width, targetExtent.height}];
			const vk::ImageSubresourceRange range(vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil, 0, 1, 0, 1);
			if (!stencil.initialized || stencil.frame != frame.frameCount || stencil.layerIndex != program.pass.layerIndex) {
				commandBuffer.pipelineBarrier(stencil.initialized ? vk::PipelineStageFlagBits::eLateFragmentTests : vk::PipelineStageFlagBits::eTopOfPipe,
						vk::PipelineStageFlagBits::eTransfer, {}, nullptr, nullptr,
						vk::ImageMemoryBarrier(stencil.initialized ? vk::AccessFlagBits::eDepthStencilAttachmentWrite : vk::AccessFlags(), vk::AccessFlagBits::eTransferWrite,
								stencil.initialized ? vk::ImageLayout::eDepthStencilAttachmentOptimal : vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
								VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, stencil.image.get(), range));
				commandBuffer.clearDepthStencilImage(stencil.image.get(), vk::ImageLayout::eTransferDstOptimal, vk::ClearDepthStencilValue(1.f, 0), range);
				commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eEarlyFragmentTests | vk::PipelineStageFlagBits::eLateFragmentTests, {}, nullptr, nullptr,
						vk::ImageMemoryBarrier(vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eDepthStencilAttachmentRead | vk::AccessFlagBits::eDepthStencilAttachmentWrite,
								vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eDepthStencilAttachmentOptimal, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, stencil.image.get(), range));
				stencil.frame = frame.frameCount; stencil.layerIndex = program.pass.layerIndex; stencil.initialized = true;
			} else commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eLateFragmentTests, vk::PipelineStageFlagBits::eEarlyFragmentTests | vk::PipelineStageFlagBits::eLateFragmentTests, {}, nullptr, nullptr,
					vk::ImageMemoryBarrier(vk::AccessFlagBits::eDepthStencilAttachmentWrite, vk::AccessFlagBits::eDepthStencilAttachmentRead | vk::AccessFlagBits::eDepthStencilAttachmentWrite,
							vk::ImageLayout::eDepthStencilAttachmentOptimal, vk::ImageLayout::eDepthStencilAttachmentOptimal, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, stencil.image.get(), range));
		}
		if (!descriptorSets[imageIndex][index]) {
			descriptorSets[imageIndex][index] = std::move(context->GetDevice().allocateDescriptorSetsUnique(
					vk::DescriptorSetAllocateInfo(context->GetDescriptorPool(), descriptorSetLayout.get())).front());
		}
		const vk::Sampler sampler = program.sampler
				? (inputHasMipmaps ? program.mipSampler.get() : program.sampler.get())
				: program.pass.linearFilter
				? (inputHasMipmaps ? mipLinearSampler.get() : linearSampler.get())
				: (inputHasMipmaps ? mipNearestSampler.get() : nearestSampler.get());
		program.imageInfos.clear();
		program.descriptorWrites.clear();
		auto bindImage = [&](uint32_t binding, vk::Sampler imageSampler, vk::ImageView view,
				vk::ImageLayout layout = vk::ImageLayout::eShaderReadOnlyOptimal) {
			program.imageInfos.emplace_back(imageSampler, view, layout);
			program.descriptorWrites.emplace_back(descriptorSets[imageIndex][index].get(), binding, 0, 1,
					vk::DescriptorType::eCombinedImageSampler, &program.imageInfos.back());
		};
		bindImage(0, sampler, inputView);
		for (const Binding& binding : program.bindings) {
			vk::ImageView view {};
			vk::Sampler resourceSampler = sampler;
			bool resourceHasMipmaps = false;
			switch (binding.input.kind) {
			case postprocess::ShaderInputKind::Source:
				view = inputView;
				resourceHasMipmaps = inputHasMipmaps;
				break;
			case postprocess::ShaderInputKind::Original:
				view = originalView;
				break;
			case postprocess::ShaderInputKind::Depth:
				view = effectiveDepthView;
				resourceSampler = nearestSampler.get();
				break;
			case postprocess::ShaderInputKind::PassOutput: {
				const Program& sourceProgram = programs[binding.input.sourcePass];
				const size_t sourceTarget = frame.frameCount % sourceProgram.historyDepth;
				view = targets[imageIndex][binding.input.sourcePass][sourceTarget].outputs[binding.input.sourceOutput].sampleView.get();
				resourceHasMipmaps = programs[binding.input.sourcePass].outputs[binding.input.sourceOutput].needsMipmaps;
				break;
			}
			case postprocess::ShaderInputKind::History:
			case postprocess::ShaderInputKind::Feedback: {
				const uint32_t offset = binding.input.kind == postprocess::ShaderInputKind::Feedback ? 1 : binding.input.historyOffset;
				const bool requestedFrameAvailable = frame.frameCount >= offset;
				const uint32_t requestedFrame = requestedFrameAvailable ? frame.frameCount - offset : 0;
				if (binding.input.sourcePass == postprocess::InvalidShaderPass) {
					const Target *history = requestedFrameAvailable ? findSourceHistoryTarget(requestedFrame) : nullptr;
					view = history != nullptr ? history->sampleView.get() : originalView;
					resourceHasMipmaps = history != nullptr && history->mipLevels > 1;
					break;
				}
				const TargetSet *historySet = requestedFrameAvailable
						? findPassHistoryTarget(binding.input.sourcePass, requestedFrame) : nullptr;
				const Target *history = historySet != nullptr && binding.input.sourceOutput < historySet->outputs.size()
						? &historySet->outputs[binding.input.sourceOutput] : nullptr;
				// Startup or a missing exact frame falls back to Original deterministically.
				if ((!history || !history->initialized) && program.nativeFxStages) {
					const auto& producer = programs[binding.input.sourcePass];
					auto& ring = targets[imageIndex][binding.input.sourcePass];
					if (ring.size() != producer.historyDepth) ring.resize(producer.historyDepth);
					auto& fallback = ring[(frame.frameCount + producer.historyDepth - 1) % producer.historyDepth];
					ensureTargetSet(fallback, producer, vk::Extent2D(producer.targetWidth, producer.targetHeight));
					auto& image = fallback.outputs[binding.input.sourceOutput];
					if (!image.initialized) {
						const vk::ImageSubresourceRange range(vk::ImageAspectFlagBits::eColor, 0, image.mipLevels, 0, image.layers);
						commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe, vk::PipelineStageFlagBits::eTransfer, {}, nullptr, nullptr,
								vk::ImageMemoryBarrier({}, vk::AccessFlagBits::eTransferWrite, vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
										VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, image.image.get(), range));
						commandBuffer.clearColorImage(image.image.get(), vk::ImageLayout::eTransferDstOptimal, vk::ClearColorValue(std::array<uint32_t, 4>{}), range);
						commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, shaderReadStages(), {}, nullptr, nullptr,
								vk::ImageMemoryBarrier(vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eShaderRead, vk::ImageLayout::eTransferDstOptimal,
										vk::ImageLayout::eShaderReadOnlyOptimal, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, image.image.get(), range));
						image.initialized = true;
					}
					history = &image;
				}
				view = history != nullptr && history->initialized ? history->sampleView.get() : originalView;
				resourceHasMipmaps = history != nullptr && history->initialized
						&& programs[binding.input.sourcePass].outputs[binding.input.sourceOutput].needsMipmaps;
				break;
			}
			case postprocess::ShaderInputKind::LookupTexture: {
				if (binding.lookupIndex >= lookupTextures.size()
						|| !lookupTextures[binding.lookupIndex].image.image || !lookupTextures[binding.lookupIndex].sampler) {
					error = "Vulkan lookup texture '" + binding.input.resourceName + "' is unavailable.";
					postprocess::reportShaderFrameStatus(activePath, error);
					return false;
				}
				view = lookupTextures[binding.lookupIndex].image.sampleView.get();
				resourceSampler = lookupTextures[binding.lookupIndex].sampler.get();
				break;
			}
			}
			if (!view) {
				error = "Vulkan shader resource '" + binding.input.bindingName + "' is unavailable.";
				postprocess::reportShaderFrameStatus(activePath, error);
				return false;
			}
			if (binding.input.kind != postprocess::ShaderInputKind::LookupTexture
					&& binding.input.kind != postprocess::ShaderInputKind::Depth) {
				if (binding.sampler)
					resourceSampler = binding.useMipmaps && resourceHasMipmaps
							? binding.mipSampler.get() : binding.sampler.get();
				else
					resourceSampler = program.sampler
							? (resourceHasMipmaps ? program.mipSampler.get() : program.sampler.get())
							: program.pass.linearFilter
							? (resourceHasMipmaps ? mipLinearSampler.get() : linearSampler.get())
							: (resourceHasMipmaps ? mipNearestSampler.get() : nearestSampler.get());
			}
			bindImage(binding.descriptorBinding, resourceSampler, view,
					binding.input.kind == postprocess::ShaderInputKind::Depth
							? effectiveDepthLayout : vk::ImageLayout::eShaderReadOnlyOptimal);
		}
		ShaderParameterUniforms parameterUniforms {};
		uint32_t parameterOffset = 0;
		for (size_t parameterIndex = 0; parameterIndex < program.parameters.size(); ++parameterIndex) {
			const postprocess::SlangParameter& parameter = program.parameters[parameterIndex];
			const uint32_t components = parameterComponentCount(parameter.type);
			std::copy_n(parameter.defaultValue.values.begin(), components,
					parameterUniforms.parameters + parameterOffset);
			const uint32_t configuredIndex = program.parameterIndices[parameterIndex];
			if (configuredIndex < parameters.size())
				std::copy_n(parameters[configuredIndex].values.begin(), components,
						parameterUniforms.parameters + parameterOffset);
			parameterOffset += components;
		}
		try {
			if (program.parameterBuffers.size() <= imageIndex)
				program.parameterBuffers.resize(imageIndex + 1);
			if (!program.parameterBuffers[imageIndex])
				program.parameterBuffers[imageIndex] = std::make_unique<BufferData>(
						sizeof(ShaderParameterUniforms), vk::BufferUsageFlagBits::eUniformBuffer);
			program.parameterBuffers[imageIndex]->upload(sizeof(parameterUniforms), &parameterUniforms);
		} catch (const std::exception& exception) {
			postprocess::reportShaderFrameStatus(activePath, exception.what());
			WARN_LOG(RENDERER, "Post-processing parameter-buffer allocation failed: %s", exception.what());
			return false;
		}
		for (uint32_t i = 0; i < program.pass.storageBindings.size(); ++i) {
			const auto& binding = program.pass.storageBindings[i];
			program.imageInfos.emplace_back(vk::Sampler(), targetSet.outputs[binding.second].renderView.get(), vk::ImageLayout::eGeneral);
			program.descriptorWrites.emplace_back(descriptorSets[imageIndex][index].get(), MaxShaderInputs + 1 + i, 0, 1, vk::DescriptorType::eStorageImage, &program.imageInfos.back());
		}
		context->GetDevice().updateDescriptorSets(program.descriptorWrites, nullptr);
		const vk::DescriptorBufferInfo parameterInfo(program.parameterBuffers[imageIndex]->buffer.get(),
				0, sizeof(ShaderParameterUniforms));
		context->GetDevice().updateDescriptorSets(vk::WriteDescriptorSet(descriptorSets[imageIndex][index].get(),
				MaxShaderInputs, 0, vk::DescriptorType::eUniformBuffer, nullptr, parameterInfo), nullptr);
		if ((program.pass.preserveBackbuffer || program.pass.blendEnabled) && program.pass.updatesSource && !program.pass.renderState.clearRenderTargets) {
			// ReShade blends against the current backbuffer. Populate the private
			// target first, then load those pixels for the blended pass.
			commandBuffer.beginRenderPass(vk::RenderPassBeginInfo(program.renderPass.get(), targetSet.framebuffer.get(),
					vk::Rect2D({}, targetExtent), program.clearColors), vk::SubpassContents::eInline);
			commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, program.blendCopyPipeline.get());
			commandBuffer.setViewport(0, vk::Viewport(0.f, 0.f, static_cast<float>(outputWidth),
					static_cast<float>(outputHeight), 0.f, 1.f));
			commandBuffer.setScissor(0, vk::Rect2D({}, targetExtent));
			commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipelineLayout.get(), 0,
					descriptorSets[imageIndex][index].get(), nullptr);
			quadBuffer->Update();
			quadBuffer->Bind(commandBuffer);
			quadBuffer->Draw(commandBuffer);
			commandBuffer.endRenderPass();
			program.blendBarriers.clear();
			for (uint32_t i = 0; i < program.pass.colorOutputCount; ++i)
				program.blendBarriers.emplace_back(vk::AccessFlagBits::eColorAttachmentWrite,
						vk::AccessFlagBits::eColorAttachmentRead | vk::AccessFlagBits::eColorAttachmentWrite,
						vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageLayout::eColorAttachmentOptimal,
						VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, targetSet.outputs[i].image.get(),
						vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
			commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
					vk::PipelineStageFlagBits::eColorAttachmentOutput, {}, nullptr, nullptr, program.blendBarriers);
		}
		if (!program.pass.compute) {
		commandBuffer.beginRenderPass(vk::RenderPassBeginInfo(program.renderPass.get(), targetSet.framebuffer.get(),
				vk::Rect2D({}, targetExtent), program.clearColors), vk::SubpassContents::eInline);
		commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, program.pipeline.get());
		commandBuffer.setViewport(0, vk::Viewport(0.f, 0.f, static_cast<float>(outputWidth), static_cast<float>(outputHeight), 0.f, 1.f));
		commandBuffer.setScissor(0, vk::Rect2D({}, targetExtent));
		commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipelineLayout.get(), 0, descriptorSets[imageIndex][index].get(), nullptr);
		} else {
			commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, program.pipeline.get());
			commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute, pipelineLayout.get(), 0, descriptorSets[imageIndex][index].get(), nullptr);
		}

		ShaderFrameUniforms uniforms {};
		uniforms.sizes[0] = static_cast<float>(passFrame.sourceWidth);
		uniforms.sizes[1] = static_cast<float>(passFrame.sourceHeight);
		uniforms.sizes[2] = static_cast<float>(outputWidth);
		uniforms.sizes[3] = static_cast<float>(outputHeight);
		uniforms.frameCount = program.pass.frameCountModulo == 0 ? frame.frameCount : frame.frameCount % program.pass.frameCountModulo;
		uniforms.rotation = frame.rotationPending ? 1u : 0u;
		uniforms.originalWidth = program.displayGlsl ? static_cast<uint32_t>(frame.sourceWidth) : originalExtent.width;
		uniforms.originalHeight = program.displayGlsl ? static_cast<uint32_t>(frame.sourceHeight) : originalExtent.height;
		uniforms.viewportSize[0] = static_cast<float>(frame.viewportWidth);
		uniforms.viewportSize[1] = static_cast<float>(frame.viewportHeight);
		uniforms.hasDepth = depthView ? 1u : 0u;
		commandBuffer.pushConstants(pipelineLayout.get(), shaderStages(),
				0, sizeof(uniforms), &uniforms);
		if (program.pass.compute) {
			std::array<uint32_t, 3> groups;
			if (!postprocess::resolveShaderDispatchGroups(program.pass, passFrame, groups, error)) {
				postprocess::reportShaderFrameStatus(activePath, error); return false;
			}
			commandBuffer.dispatch(groups[0], groups[1], groups[2]);
		} else {
		quadBuffer->Update();
		quadBuffer->Bind(commandBuffer);
		if (program.nativeFxStages)
			commandBuffer.draw(program.pass.renderState.vertexCount, 1, 0, 0);
		else
			quadBuffer->Draw(commandBuffer);
		commandBuffer.endRenderPass();
		}
		for (const auto& binding : program.pass.storageBindings) {
			const auto& image = targetSet.outputs[binding.second];
			commandBuffer.pipelineBarrier(shaderReadStages(), shaderReadStages(), {}, nullptr, nullptr,
					vk::ImageMemoryBarrier(vk::AccessFlagBits::eShaderWrite, vk::AccessFlagBits::eShaderRead, vk::ImageLayout::eGeneral, vk::ImageLayout::eShaderReadOnlyOptimal,
							VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, image.image.get(), vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, image.layers)));
		}

		for (size_t outputIndex = 0; outputIndex < targetSet.outputs.size(); ++outputIndex) {
			Target& target = targetSet.outputs[outputIndex];
			if (program.outputs[outputIndex].needsMipmaps && program.pass.renderState.generateMipmaps)
				generateMipmaps(commandBuffer, target, true);
			target.initialized = true;
		}
		targetSet.renderedFrame = frame.frameCount;
		targetSet.initialized = true;
		if (program.pass.updatesSource) {
		inputView = targetSet.outputs[0].sampleView.get();
		inputExtent = targetExtent;
		inputHasMipmaps = program.outputs[0].needsMipmaps;
		passFrame.sourceWidth = outputWidth;
		passFrame.sourceHeight = outputHeight;
		}
	}
	outputView = inputView;
	presentationTransformApplied = composedDisplayInput;
	++renderedFrames;
	postprocess::reportShaderFrameStatus(activePath, {});
	return true;
}

void VulkanShaderChain::term()
{
	sourceTargets.clear();
	displayTargets.clear();
	depthRotateTargets.clear();
	targets.clear();
	stencilTargets.clear();
	sourceDescriptorSets.clear();
	displayDescriptorSets.clear();
	depthRotateDescriptorSets.clear();
	descriptorSets.clear();
	quadBuffer.reset();
	programs.clear();
	parameters.clear();
	depthRotatePipeline.reset();
	depthRotateRenderPass.reset();
	copyRotatePipeline.reset();
	copyPipeline.reset();
	copyFragmentShader.reset();
	mipNearestSampler.reset();
	mipLinearSampler.reset();
	nearestSampler.reset();
	linearSampler.reset();
	pipelineLayout.reset();
	descriptorSetLayout.reset();
	renderPass.reset();
	configuredPaths.clear();
	configuredGeneration = 0;
	parameterGeneration = 0;
	activePath.clear();
	sourceHistoryDepth = 1;
	postprocess::reportShaderChainStatus({}, {});
}

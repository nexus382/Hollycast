/*
	Portions Copyright 2026 The Hollycast Authors

	This file is part of Flycast.

	Flycast is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.
*/
#include "shader_chain.h"
#include "reshade_fx.h"
#include "effect_codegen.hpp"
#include "effect_parser.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <memory>
#include <regex>
#include <sstream>
#include <string_view>
#include <unordered_map>

namespace postprocess {
namespace {

std::string lowercase(std::string value)
{
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return value;
}

std::string glslType(const std::string& type)
{
	const std::string lower = lowercase(type);
	if (lower == "float2") return "vec2";
	if (lower == "float3") return "vec3";
	if (lower == "float4") return "vec4";
	if (lower == "float2x2") return "mat2";
	if (lower == "float3x3") return "mat3";
	if (lower == "float4x4") return "mat4";
	if (lower == "uint2") return "uvec2";
	if (lower == "uint3") return "uvec3";
	if (lower == "uint4") return "uvec4";
	if (lower == "int2") return "ivec2";
	if (lower == "int3") return "ivec3";
	if (lower == "int4") return "ivec4";
	if (lower == "bool2") return "bvec2";
	if (lower == "bool3") return "bvec3";
	if (lower == "bool4") return "bvec4";
	const auto matrix = lower.find('x');
	if (matrix != std::string::npos && matrix >= 1 && matrix + 2 == lower.size()
			&& lower[matrix - 1] >= '2' && lower[matrix - 1] <= '4' && lower[matrix + 1] >= '2' && lower[matrix + 1] <= '4')
		return "mat" + std::string(1, lower[matrix - 1]) + "x" + lower[matrix + 1];
	return type;
}


uint32_t componentCount(SlangParameterType type)
{
	return postprocess::shaderParameterComponentCount(type);
}

bool matchesConstructor(const std::string& name, SlangParameterType type)
{
	const std::string value = lowercase(name);
	switch (type) {
	case SlangParameterType::Float: return value == "float";
	case SlangParameterType::Int: return value == "int" || value == "uint";
	case SlangParameterType::Bool: return value == "bool";
	case SlangParameterType::Float2: return value == "float2" || value == "vec2" || value == "int2" || value == "uint2" || value == "bool2" || value == "bvec2";
	case SlangParameterType::Float3: return value == "float3" || value == "vec3" || value == "int3" || value == "uint3" || value == "bool3" || value == "bvec3";
	case SlangParameterType::Float4: return value == "float4" || value == "vec4" || value == "int4" || value == "uint4" || value == "bool4" || value == "bvec4";
	case SlangParameterType::Float2x2: return value == "float2x2" || value == "mat2";
	case SlangParameterType::Float3x3: return value == "float3x3" || value == "mat3";
	case SlangParameterType::Float4x4: return value == "float4x4" || value == "mat4";
	case SlangParameterType::Float2x3: return value == "float2x3" || value == "mat2x3";
	case SlangParameterType::Float2x4: return value == "float2x4" || value == "mat2x4";
	case SlangParameterType::Float3x2: return value == "float3x2" || value == "mat3x2";
	case SlangParameterType::Float3x4: return value == "float3x4" || value == "mat3x4";
	case SlangParameterType::Float4x2: return value == "float4x2" || value == "mat4x2";
	case SlangParameterType::Float4x3: return value == "float4x3" || value == "mat4x3";

	}
	return false;
}

bool parseValues(const std::string& expression, SlangParameterType type, SlangParameterValue& result)
{
	result = {};
	result.componentCount = componentCount(type);
	if (expression.empty())
		return true;
	auto trim = [](std::string_view value) {
		const size_t first = value.find_first_not_of(" \t\r\n");
		if (first == std::string_view::npos)
			return std::string();
		return std::string(value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1));
	};
	std::string values = trim(expression);
	const std::string lower = lowercase(values);
	if (type == SlangParameterType::Bool && (lower == "true" || lower == "false")) {
		result.values[0] = lower == "true" ? 1.f : 0.f;
		return true;
	}
	const size_t open = values.find('(');
	if (open != std::string::npos) {
		if (values.back() != ')' || values.find('(', open + 1) != std::string::npos
				|| !matchesConstructor(trim(std::string_view(values).substr(0, open)), type))
			return false;
		values = values.substr(open + 1, values.size() - open - 2);
	}
	else if (values.find(')') != std::string::npos) {
		return false;
	}
	uint32_t index = 0;
	for (size_t begin = 0;;) {
		const size_t separator = values.find(',', begin);
		const std::string token = trim(std::string_view(values).substr(begin,
				separator == std::string::npos ? std::string::npos : separator - begin));
		if (token.empty() || index >= result.componentCount)
			return false;
		float value;
		if (!(token == "true" ? (value = 1.f, true) : token == "false" ? (value = 0.f, true) : parseShaderFloat(token, value))
				|| (type == SlangParameterType::Int && std::floor(value) != value)
				|| (type == SlangParameterType::Bool && value != 0.f && value != 1.f))
			return false;
		result.values[index++] = value;
		if (separator == std::string::npos)
			break;
		begin = separator + 1;
	}
	if (index == 1 && result.componentCount > 1)
		std::fill_n(result.values.begin() + 1, result.componentCount - 1, result.values[0]);
	return index == result.componentCount || index == 1;
}

std::vector<std::string> splitChoiceLabels(const std::string& values)
{
	std::vector<std::string> labels;
	for (size_t begin = 0; begin < values.size();) {
		const size_t end = values.find('\0', begin);
		const size_t length = (end == std::string::npos ? values.size() : end) - begin;
		if (length != 0)
			labels.emplace_back(values.substr(begin, length));
		if (end == std::string::npos)
			break;
		begin = end + 1;
	}
	return labels;
}

std::string samplerTexture(const ReShadeSampler& sampler)
{
	std::smatch match;
	if (std::regex_search(sampler.definition, match,
			std::regex(R"(\bTexture\s*=\s*(?:[A-Za-z_][A-Za-z0-9_]*\s*::\s*)*([A-Za-z_][A-Za-z0-9_]*))",
					std::regex::icase)))
		return match[1].str();
	return sampler.name;
}

bool isSourceTexture(const ReShadeEffect& effect, const std::string& texture, const std::string& sampler)
{
	const std::string lowerTexture = lowercase(texture);
	const std::string lowerSampler = lowercase(sampler);
	if (lowerTexture == "source" || lowerTexture == "sourcetex" || lowerTexture == "backbuffer"
			|| lowerTexture == "backbuffertex" || lowerSampler == "source" || lowerSampler == "sourcesampler"
			|| lowerSampler == "backbuffer")
		return true;
	const auto resource = std::find_if(effect.textures.begin(), effect.textures.end(), [&](const ReShadeTexture& value) {
		return value.name == texture;
	});
	if (resource == effect.textures.end())
		return false;
	return std::any_of(resource->annotations.begin(), resource->annotations.end(), [](const ReShadeAnnotation& annotation) {
		return lowercase(annotation.name) == "source" && lowercase(annotation.value) == "color";
	});
}

size_t renderTargetProducer(const reshadefx::effect_module& module, size_t techniqueIndex, size_t consumer,
		const std::string& texture)
{
	const auto& passes = module.techniques[techniqueIndex].passes;
	size_t previous = InvalidShaderPass, final = InvalidShaderPass;
	for (size_t passIndex = 0; passIndex < passes.size(); ++passIndex) {
		const auto& pass = passes[passIndex];
		bool writes = std::find(std::begin(pass.render_target_names), std::end(pass.render_target_names), texture) != std::end(pass.render_target_names);
		for (const auto& storage : pass.storage_bindings)
			writes = writes || (storage.index < module.storages.size() && module.storages[storage.index].texture_name == texture);
		if (writes) {
			final = passIndex;
			if (passIndex < consumer) previous = passIndex;
		}
	}
	return previous != InvalidShaderPass ? previous : final;
}

bool parseUiBound(const std::string& expression, SlangParameterType type, SlangParameterValue& result)
{
	if (parseValues(expression, type, result))
		return true;
	std::string compact = expression;
	compact.erase(std::remove_if(compact.begin(), compact.end(),
			[](unsigned char c) { return std::isspace(c); }), compact.end());
	float sign = 1.f;
	if (!compact.empty() && compact.front() == '-') {
		sign = -1.f;
		compact.erase(0, 1);
	}
	if (compact != "OutputSize.x" && compact != "OutputSize.y" && compact != "FinalViewportSize.x" && compact != "FinalViewportSize.y"
			&& !(type == SlangParameterType::Float2 && (compact == "float2(OutputSize.x,OutputSize.y)" || compact == "float2(FinalViewportSize.x,FinalViewportSize.y)")))
		return false;
	// Runtime-size UI bounds cannot be evaluated while loading a package. Use
	// the same supported dimension limit as target allocation, with the original
	// sign preserved for pixel offsets that can move in either direction.
	result = {};
	result.componentCount = componentCount(type);
	std::fill_n(result.values.begin(), result.componentCount, sign * static_cast<float>(MaxShaderTargetDimension));
	return true;
}
bool reShadeHostUniformExpression(const ReShadeUniform& uniform, bool& hostProvided,
		std::string& expression, std::string& error)
{
	const auto annotation = std::find_if(uniform.annotations.begin(), uniform.annotations.end(),
			[](const ReShadeAnnotation& value) { return lowercase(value.name) == "source"; });
	if (annotation == uniform.annotations.end()) {
		hostProvided = false;
		expression.clear();
		return true;
	}
	hostProvided = true;
	const std::string source = lowercase(annotation->value);
	const std::string type = glslType(uniform.typeName);
	const bool scalar = type == "float" || type == "int" || type == "uint" || type == "bool";
	const auto requireScalar = [&]() {
		if (scalar)
			return true;
		error = "ReShade FX host uniform '" + uniform.name + "' requires a scalar type.";
		return false;
	};
	const auto requireVec2 = [&]() {
		if (type == "vec2")
			return true;
		error = "ReShade FX host uniform '" + uniform.name + "' requires a float2 type.";
		return false;
	};
	if (source == "zero")
		expression = type + "(0.0)";
	else if (source == "has_depth" || source == "bufready_depth")
		expression = type + "(HasDepth)";
	else if (source == "framecount") {
		if (!requireScalar()) return false;
		expression = type + "(FrameCount)";
	}
	else if (source == "bufferwidth" || source == "viewportwidth") {
		if (!requireScalar()) return false;
		expression = type + "(OutputSize.x)";
	}
	else if (source == "bufferheight" || source == "viewportheight") {
		if (!requireScalar()) return false;
		expression = type + "(OutputSize.y)";
	}
	else if (source == "internalwidth" || source == "nativewidth") {
		if (!requireScalar()) return false;
		expression = type + "(OriginalSize.x)";
	}
	else if (source == "internalheight" || source == "nativeheight") {
		if (!requireScalar()) return false;
		expression = type + "(OriginalSize.y)";
	}
	else if (source == "upscale_multiplier") {
		if (!requireScalar()) return false;
		expression = type + "(SourceSize.x / max(OriginalSize.x, 1.0))";
	}
	else if (source == "viewportx" || source == "viewporty") {
		if (!requireScalar()) return false;
		expression = type + "(0.0)";
	}
	else if (source == "viewportsize") {
		if (!requireVec2()) return false;
		expression = "OutputSize";
	}
	else if (source == "buffer_to_viewport_ratio") {
		if (!requireVec2()) return false;
		expression = "vec2(1.0)";
	}
	else if (source == "internal_pixel_size" || source == "native_pixel_size") {
		if (!requireVec2()) return false;
		expression = "OutputSize / max(OriginalSize, vec2(1.0))";
	}
	else if (source == "normalized_internal_pixel_size" || source == "normalized_native_pixel_size") {
		if (!requireVec2()) return false;
		expression = "vec2(1.0) / max(OriginalSize, vec2(1.0))";
	}
	else {
		error = "ReShade FX uniform '" + uniform.name + "' uses unsupported host source '" + annotation->value + "'.";
		return false;
	}
	return true;
}


bool fail(std::string& error, const std::string& message)
{
	error = message;
	return false;
}

struct SourceReplacement {
	size_t begin;
	size_t end;
	std::string text;
};

bool prepareCompilerSource(const ReShadeEffect& effect, std::string& source, std::string& error)
{
	std::vector<SourceReplacement> replacements;
	for (const auto& range : effect.annotationRanges)
		replacements.push_back({ range.first, range.second, {} });
	for (const ReShadeTexture& texture : effect.textures) {
		// Target allocation belongs to the render graph. Keep dimensions as runtime
		// expressions there, but use valid placeholders in the language compiler's
		// texture declarations, whose properties require constant expressions.
		static const std::regex dimensions(R"(\b(Width|Height|Depth|MipLevels)\s*=\s*([^;]+);)", std::regex::icase);
		for (std::sregex_iterator it(texture.definition.begin(), texture.definition.end(), dimensions), end;
				it != end; ++it) {
			const size_t begin = texture.sourceBegin + static_cast<size_t>((*it).position(2));
			replacements.push_back({ begin, begin + static_cast<size_t>((*it).length(2)), "1" });
		}
	}
	for (const ReShadeSampler& sampler : effect.samplers) {
		// Older packages use the combined D3D filter spelling. The render graph
		// already retains it; spell out the equivalent ReShade language properties.
		static const std::regex filter(R"(\bFilter\s*=\s*([A-Za-z_]+)\s*;)", std::regex::icase);
		static constexpr const char *modes[] = {
			"MIN_MAG_MIP_POINT", "MIN_MAG_POINT_MIP_LINEAR",
			"MIN_POINT_MAG_LINEAR_MIP_POINT", "MIN_POINT_MAG_MIP_LINEAR",
			"MIN_LINEAR_MAG_MIP_POINT", "MIN_LINEAR_MAG_POINT_MIP_LINEAR",
			"MIN_MAG_LINEAR_MIP_POINT", "MIN_MAG_MIP_LINEAR"
		};
		for (std::sregex_iterator it(sampler.definition.begin(), sampler.definition.end(), filter), end; it != end; ++it) {
			const std::string value = lowercase((*it)[1].str());
			unsigned mode = 0;
			for (; mode < 8 && lowercase(modes[mode]) != value; ++mode) {}
			if (mode == 8)
				return fail(error, "ReShade FX sampler '" + sampler.name + "' has an unsupported combined filter.");
			const size_t begin = sampler.sourceBegin + static_cast<size_t>((*it).position());
			const std::string text = std::string("MinFilter = ") + ((mode & 4) ? "LINEAR" : "POINT")
					+ "; MagFilter = " + ((mode & 2) ? "LINEAR" : "POINT")
					+ "; MipFilter = " + ((mode & 1) ? "LINEAR" : "POINT") + ";";
			replacements.push_back({ begin, begin + static_cast<size_t>((*it).length()), text });
		}
	}
	bool needsDefaultVertex = false;
	for (const ReShadeTechnique& technique : effect.techniques)
		for (const ReShadePass& pass : technique.passes) {
			static const std::regex viewport(R"(\b(ViewportWidth|ViewportHeight|DispatchSizeX|DispatchSizeY)\s*=\s*([^;]+);)", std::regex::icase);
			for (std::sregex_iterator it(pass.stateBlock.begin(), pass.stateBlock.end(), viewport), end; it != end; ++it) {
				const size_t begin = pass.bodyBegin + static_cast<size_t>((*it).position(2));
				replacements.push_back({begin, begin + static_cast<size_t>((*it).length(2)), "1"});
			}

			// Preserve graph-supported legacy spellings while feeding the language
			// compiler its canonical render-state enum names.
			static const std::regex blendState(R"(\b(?:SrcBlend|DestBlend|BlendOp)\s*=\s*([A-Za-z_]+)\s*;)", std::regex::icase);
			for (std::sregex_iterator it(pass.stateBlock.begin(), pass.stateBlock.end(), blendState), end; it != end; ++it) {
				std::string canonical = (*it)[1].str();
				if (canonical.find('_') == std::string::npos)
					continue;
				std::transform(canonical.begin(), canonical.end(), canonical.begin(),
						[](unsigned char c) { return static_cast<char>(std::toupper(c)); });
				canonical.erase(std::remove(canonical.begin(), canonical.end(), '_'), canonical.end());
				if (canonical == "REVERSESUBTRACT")
					canonical = "REVSUBTRACT";
				const size_t begin = pass.bodyBegin + static_cast<size_t>((*it).position(1));
				replacements.push_back({ begin, begin + static_cast<size_t>((*it).length(1)), std::move(canonical) });
			}
			if (pass.vertexShader.empty() && !pass.pixelShader.empty()) {
				replacements.push_back({ pass.bodyBegin, pass.bodyBegin, "\nVertexShader = HollyPostProcessVS;\n" });
				needsDefaultVertex = true;
			}
		}
	std::sort(replacements.begin(), replacements.end(), [](const SourceReplacement& left, const SourceReplacement& right) {
		return left.begin > right.begin;
	});
	source = effect.preprocessedSource;
	size_t next = source.size();
	for (const SourceReplacement& replacement : replacements) {
		if (replacement.begin > replacement.end || replacement.end > next)
			return fail(error, "ReShade FX contains overlapping metadata declarations.");
		source.replace(replacement.begin, replacement.end - replacement.begin, replacement.text);
		next = replacement.begin;
	}
	std::string host;
	for (const char *name : { "OutputSize", "SourceSize", "OriginalSize", "FinalViewportSize" })
		if (std::none_of(effect.uniforms.begin(), effect.uniforms.end(),
				[&](const ReShadeUniform& uniform) { return uniform.name == name; }))
			host += std::string("uniform float2 ") + name + ";\n";
	if (std::none_of(effect.uniforms.begin(), effect.uniforms.end(),
			[](const ReShadeUniform& uniform) { return uniform.name == "FrameCount"; }))
		host += "uniform uint FrameCount;\n";
	if (needsDefaultVertex)
		host +=
			"void HollyPostProcessVS(uint id : SV_VertexID, out float4 position : SV_Position, out float2 uv : TEXCOORD0)\n"
			"{ uv = float2(id == 2 ? 2.0 : 0.0, id == 1 ? 2.0 : 0.0);"
			" position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0); }\n";
	source.insert(0, host);
	return true;
}

SlangParameterType reflectedParameterType(const reshadefx::type& type)
{
	if (type.cols > 1) {
		static const SlangParameterType matrices[3][3] {
			{ SlangParameterType::Float2x2, SlangParameterType::Float2x3, SlangParameterType::Float2x4 },
			{ SlangParameterType::Float3x2, SlangParameterType::Float3x3, SlangParameterType::Float3x4 },
			{ SlangParameterType::Float4x2, SlangParameterType::Float4x3, SlangParameterType::Float4x4 }
		};
		return matrices[type.rows - 2][type.cols - 2];
	}
	if (type.rows > 1) return type.rows == 2 ? SlangParameterType::Float2 : type.rows == 3 ? SlangParameterType::Float3 : SlangParameterType::Float4;
	return type.base == reshadefx::type::t_bool ? SlangParameterType::Bool
			: type.is_integral() ? SlangParameterType::Int : SlangParameterType::Float;
}

bool reflectParameters(const ReShadeEffect& effect, const reshadefx::effect_module& module,
		ShaderPassSource& compiled, std::unordered_map<std::string, std::string>& expressions, std::string& error)
{
	uint32_t components = 0;
	for (const auto& uniform : effect.uniforms) {
		const auto value = std::find_if(module.uniforms.begin(), module.uniforms.end(), [&](const auto& item) { return item.name == uniform.name; });
		if (value == module.uniforms.end()) return fail(error, "FX uniform was not reflected: " + uniform.name);
		if (!value->type.is_numeric() || value->type.rows > 4 || value->type.cols > 4
				|| (value->type.cols > 1 && value->type.rows < 2))
			return fail(error, "FX uniform has an invalid numeric shape: " + uniform.name);
		std::string hostSource;
		uint32_t hostKey = 0;
		bool hostToggle = false, hostPress = false;
		std::string uiType, uiItems, label = uniform.name;
		for (const auto& annotation : uniform.annotations) {
			const auto name = lowercase(annotation.name);
			if (name == "source") hostSource = lowercase(annotation.value);
			else if (name == "keycode" || name == "button") {
				std::string_view key = annotation.value;
				const int base = key.size() > 2 && key[0] == '0' && (key[1] == 'x' || key[1] == 'X') ? 16 : 10;
				if (base == 16) key.remove_prefix(2);
				const auto parsed = std::from_chars(key.data(), key.data() + key.size(), hostKey, base);
				if (parsed.ec != std::errc() || parsed.ptr != key.data() + key.size() || hostKey > 255)
					return fail(error, "Invalid host input key: " + uniform.name);
			}
			else if (name == "mode") {
				const auto mode = lowercase(annotation.value);
				if (!mode.empty() && mode != "toggle" && mode != "press") return fail(error, "Unknown host input mode: " + uniform.name);
				hostToggle = mode == "toggle"; hostPress = mode == "press";
			}
			else if (name == "ui_label") label = annotation.value;
			else if (name == "ui_type") uiType = lowercase(annotation.value);
			else if (name == "ui_items") uiItems = annotation.value;
		}
		const bool dynamic = hostSource == "timer" || hostSource == "frametime" || hostSource == "random" || hostSource == "date"
				|| hostSource == "key" || hostSource == "mousepoint" || hostSource == "mousedelta" || hostSource == "mousebutton" || hostSource == "mousewheel";
		if (!dynamic) {
			bool provided = false;
			std::string expression;
			if (!reShadeHostUniformExpression(uniform, provided, expression, error)) return false;
			if (provided) { expressions.emplace(uniform.name, std::move(expression)); continue; }
		}
		if (dynamic) {
			const bool pair = hostSource == "mousepoint" || hostSource == "mousedelta" || hostSource == "mousewheel";
			const uint32_t expected = hostSource == "date" ? 4u : pair ? 2u : 1u;
			if (value->type.is_array() || value->type.cols != 1 || value->type.rows != expected)
				return fail(error, "Host uniform has the wrong shape for source '" + hostSource + "': " + uniform.name);
			if (hostSource == "mousebutton" && hostKey > 4)
				return fail(error, "Host mouse-button indices must be in the range 0..4.");
		}
		const bool array = value->type.is_array();
		const uint32_t count = array ? value->type.array_length : 1;
		const auto type = reflectedParameterType(value->type);
		const uint32_t size = componentCount(type);
		if (count == 0 || count > MaxShaderParameterComponents / size || components + count * size > MaxShaderParameterComponents)
			return fail(error, "FX uniforms exceed the 256-component parameter budget.");
		std::string expression = array ? glslType(uniform.typeName) + "[" + std::to_string(count) + "](" : "";
		for (uint32_t element = 0; element < count; ++element) {
			SlangParameter parameter;
			parameter.type = type;
			parameter.name = !array ? uniform.name : "HollycastArray_" + uniform.name + "_" + std::to_string(element);
			parameter.label = label + (!array ? "" : "[" + std::to_string(element) + "]");
			parameter.defaultValue.componentCount = parameter.minimum.componentCount = parameter.maximum.componentCount = parameter.step.componentCount = size;
			const reshadefx::constant *initial = &value->initializer_value;
			if (value->type.is_array()) initial = element < value->initializer_value.array_data.size() ? &value->initializer_value.array_data[element] : nullptr;
			for (uint32_t c = 0; initial && c < size; ++c) {
				parameter.defaultValue.values[c] = value->type.base == reshadefx::type::t_int ? static_cast<float>(initial->as_int[c])
						: value->type.is_integral() ? static_cast<float>(initial->as_uint[c]) : initial->as_float[c];
			}
			parameter.minimum = parameter.maximum = parameter.defaultValue;
			if (value->type.base == reshadefx::type::t_bool) {
				for (uint32_t c = 0; c < size; ++c) {
					parameter.minimum.values[c] = 0;
					parameter.maximum.values[c] = parameter.step.values[c] = 1;
				}
			}
			for (const auto& annotation : uniform.annotations) {
				const auto name = lowercase(annotation.name);
				if (name == "ui_min" && !parseUiBound(annotation.value, type, parameter.minimum)) return fail(error, "Invalid ui_min: " + uniform.name);
				if (name == "ui_max" && !parseUiBound(annotation.value, type, parameter.maximum)) return fail(error, "Invalid ui_max: " + uniform.name);
				if (name == "ui_step" && !parseValues(annotation.value, type, parameter.step)) return fail(error, "Invalid ui_step: " + uniform.name);
			}
			if (value->type.is_integral()) for (uint32_t c = 0; c < size; ++c) if (parameter.step.values[c] == 0) parameter.step.values[c] = 1;
			if (!dynamic && !validateSlangParameter(parameter, error)) return false;
			ShaderParameter visible;
			visible.name = parameter.name; visible.label = parameter.label; visible.type = type; visible.componentCount = size;
			visible.value = parameter.defaultValue.values[0]; visible.minimum = parameter.minimum.values[0];
			visible.maximum = parameter.maximum.values[0]; visible.step = parameter.step.values[0];
			visible.values = parameter.defaultValue.values; visible.minimumValues = parameter.minimum.values;
			visible.maximumValues = parameter.maximum.values; visible.stepValues = parameter.step.values;
			if (dynamic) { visible.hostSource = hostSource; visible.hostKey = hostKey; visible.hostToggle = hostToggle; visible.hostPress = hostPress; }
			if (hostSource == "random" || hostSource == "mousewheel") {
				visible.minimum = 0; visible.maximum = hostSource == "random" ? 32767.f : 0.f;
				for (const auto& annotation : uniform.annotations) {
					if (lowercase(annotation.name) == "min" && !parseShaderFloat(annotation.value, visible.minimum)) return fail(error, "Invalid host-source minimum: " + uniform.name);
					if (lowercase(annotation.name) == "max" && !parseShaderFloat(annotation.value, visible.maximum)) return fail(error, "Invalid host-source maximum: " + uniform.name);
				}
				if (visible.minimum > visible.maximum) return fail(error, "Host-source minimum exceeds its maximum: " + uniform.name);
			}
			if (size == 1 && type == SlangParameterType::Int && (uiType == "combo" || uiType == "radio")) {
				visible.choiceLabels = splitChoiceLabels(uiItems);
				if (!visible.choiceLabels.empty()) { visible.minimum = visible.minimumValues[0] = 0; visible.maximum = visible.maximumValues[0] = static_cast<float>(visible.choiceLabels.size() - 1); visible.step = visible.stepValues[0] = 1; }
			}
			std::string elementExpression = parameter.name;
			if (value->type.is_integral() && value->type.rows > 1 && value->type.cols == 1) elementExpression = glslType(uniform.typeName) + "(" + elementExpression + ")";
			else if (value->type.base == reshadefx::type::t_uint && value->type.cols == 1) elementExpression = "uint(" + elementExpression + ")";
			if (element) expression += ", ";
			expression += elementExpression;
			compiled.parameters.push_back(std::move(visible)); compiled.slangParameters.push_back(std::move(parameter));
		}
		if (array) expression += ")";
		expressions.emplace(uniform.name, std::move(expression)); components += count * size;
	}
	return true;
}

std::string normalizeStage(std::string stage, const reshadefx::effect_module& module,
		const ReShadeEffect& effect, const std::unordered_map<std::string, std::string>& hostExpressions,
		const ShaderPassSource& compiled)
{
	stage = std::regex_replace(stage, std::regex(R"(^#version[^\n]*\n)"), "");
	stage = std::regex_replace(stage,
			std::regex(R"(layout\s*\([^)]*\)\s*uniform\s+_Globals\s*\{[^}]*\}\s*;)"), "");
	std::string declarations;
	for (const auto& parameter : compiled.slangParameters)
		declarations += std::string("uniform ") + shaderParameterGlslType(parameter.type) + " " + parameter.name + ";\n";
	for (const reshadefx::uniform& uniform : module.uniforms) {
		const auto host = hostExpressions.find(uniform.name);
		std::string expression;
		if (host != hostExpressions.end()) {
			expression = host->second;
		}
		else if (uniform.name == "OutputSize" || uniform.name == "SourceSize" || uniform.name == "OriginalSize"
				|| uniform.name == "FrameCount" || uniform.name == "FinalViewportSize") {
			expression = uniform.name;
		}
		else {
			const auto reflected = std::find_if(effect.uniforms.begin(), effect.uniforms.end(),
					[&](const ReShadeUniform& candidate) { return candidate.name == uniform.name; });
			if (reflected == effect.uniforms.end())
				continue;
			std::string storageType = glslType(reflected->typeName);
			expression = uniform.name;
			if (uniform.type.is_integral() && uniform.type.is_vector()) {
				storageType = "vec" + std::to_string(uniform.type.rows);
				expression = glslType(reflected->typeName) + "(" + uniform.name + ")";
			}
			else if (uniform.type.base == reshadefx::type::t_uint) {
				storageType = "int";
				expression = "uint(" + uniform.name + ")";
			}
			declarations += "uniform " + storageType + " " + uniform.name + ";\n";
		}
		declarations += "#define " + uniform.unique_name + " (" + expression + ")\n";
	}
	for (const reshadefx::sampler& sampler : module.samplers) {
		const std::regex declaration("layout\\s*\\([^)]*\\)\\s*uniform\\s+[iu]?sampler[123]D\\s+" + sampler.unique_name + "\\s*;");
		if (!std::regex_search(stage, declaration))
			continue;
		stage = std::regex_replace(stage, declaration, "");
		const auto resource = std::find_if(compiled.slangResources.begin(), compiled.slangResources.end(), [&](const auto& r) { return r.name == sampler.name; });
		if (resource != compiled.slangResources.end())
			declarations += "uniform " + std::string(shaderResourceGlslType(resource->type, resource->numericType)) + " " + sampler.name + ";\n";
		declarations += "#define " + sampler.unique_name + " " + sampler.name + "\n";
	}
	return declarations + stage;
}

} // namespace

bool compileReShadeFxPass(const std::string& effectText, size_t techniqueIndex, size_t passIndex,
		ShaderPassSource& source, std::string& error)
{
	ReShadeEffect effect;
	if (!parseReShadeEffect(effectText, effect, error))
		return false;
	if (techniqueIndex >= effect.techniques.size() || passIndex >= effect.techniques[techniqueIndex].passes.size())
		return fail(error, "ReShade FX selected an invalid technique pass.");

	std::string compilerSource;
	if (!prepareCompilerSource(effect, compilerSource, error))
		return false;
	std::unique_ptr<reshadefx::codegen> compiler(reshadefx::create_codegen_glsl(false, false, false));
	reshadefx::parser parser;
	if (!parser.parse(std::move(compilerSource), compiler.get()))
		return fail(error, "ReShade FX compilation failed:\n" + parser.errors());
	const reshadefx::effect_module& module = compiler->module();
	if (techniqueIndex >= module.techniques.size() || passIndex >= module.techniques[techniqueIndex].passes.size())
		return fail(error, "ReShade FX compiler returned inconsistent technique metadata.");
	const reshadefx::pass& pass = module.techniques[techniqueIndex].passes[passIndex];


	ShaderPassSource compiled;
	compiled.nativeFxStages = true;
	compiled.displayName = effect.techniques[techniqueIndex].name;
	compiled.updatesSource = pass.cs_entry_point.empty() && pass.render_target_names[0].empty();
	compiled.colorOutputCount = pass.cs_entry_point.empty() ? 1u : 0u;
	const auto& passText = effect.techniques[techniqueIndex].passes[passIndex].stateBlock;
	std::smatch dispatch;
	if (!pass.cs_entry_point.empty()) {
		for (uint32_t axis = 0; axis < 3; ++axis) {
			const std::string stateName = std::string("DispatchSize") + "XYZ"[axis];
			if (std::regex_search(passText, dispatch, std::regex(stateName + R"(\s*=\s*([^;]+))", std::regex::icase)))
				compiled.dispatchExpressions[axis] = dispatch[1].str();
			else compiled.dispatchExpressions[axis] = "1";
		}
	}
	auto& state = compiled.renderState;
	state.reflected = true;
	state.clearRenderTargets = pass.clear_render_targets;
	state.srgbWrite = pass.srgb_write_enable;
	state.generateMipmaps = pass.generate_mipmaps;
	state.topology = static_cast<ShaderPrimitiveTopology>(pass.topology);
	state.vertexCount = pass.num_vertices;
	if (state.vertexCount == 0 || state.vertexCount > 1048576
			|| static_cast<unsigned>(state.topology) < 1 || static_cast<unsigned>(state.topology) > 5)
		return fail(error, "FX pass has an invalid vertex count or primitive topology.");
	if (static_cast<unsigned>(pass.stencil_comparison_func) > 7
			|| static_cast<unsigned>(pass.stencil_pass_op) > 7 || static_cast<unsigned>(pass.stencil_fail_op) > 7
			|| static_cast<unsigned>(pass.stencil_depth_fail_op) > 7)
		return fail(error, "FX pass contains an invalid stencil state.");
	state.stencilEnabled = pass.stencil_enable;
	state.stencilReadMask = pass.stencil_read_mask; state.stencilWriteMask = pass.stencil_write_mask;
	state.stencilReference = pass.stencil_reference_value;
	state.stencilComparison = static_cast<ShaderComparison>(pass.stencil_comparison_func);
	state.stencilPass = static_cast<ShaderStencilOperation>(pass.stencil_pass_op);
	state.stencilFail = static_cast<ShaderStencilOperation>(pass.stencil_fail_op);
	state.stencilDepthFail = static_cast<ShaderStencilOperation>(pass.stencil_depth_fail_op);
	for (size_t i = 0; i < state.attachments.size(); ++i) {
		if (static_cast<unsigned>(pass.source_color_blend_factor[i]) > 9 || static_cast<unsigned>(pass.dest_color_blend_factor[i]) > 9
				|| static_cast<unsigned>(pass.source_alpha_blend_factor[i]) > 9 || static_cast<unsigned>(pass.dest_alpha_blend_factor[i]) > 9
				|| static_cast<unsigned>(pass.color_blend_op[i]) < 1 || static_cast<unsigned>(pass.color_blend_op[i]) > 5
				|| static_cast<unsigned>(pass.alpha_blend_op[i]) < 1 || static_cast<unsigned>(pass.alpha_blend_op[i]) > 5)
			return fail(error, "FX pass contains an invalid attachment blend state.");
		auto& attachment = state.attachments[i];
		attachment.blendEnabled = pass.blend_enable[i];
		attachment.source = static_cast<ShaderBlendFactor>(pass.source_color_blend_factor[i]);
		attachment.destination = static_cast<ShaderBlendFactor>(pass.dest_color_blend_factor[i]);
		attachment.sourceAlpha = static_cast<ShaderBlendFactor>(pass.source_alpha_blend_factor[i]);
		attachment.destinationAlpha = static_cast<ShaderBlendFactor>(pass.dest_alpha_blend_factor[i]);
		attachment.operation = static_cast<ShaderBlendOperation>(static_cast<int>(pass.color_blend_op[i]) - 1);
		attachment.alphaOperation = static_cast<ShaderBlendOperation>(static_cast<int>(pass.alpha_blend_op[i]) - 1);
		attachment.writeMask = pass.render_target_write_mask[i] & 15;
	}

	std::unordered_map<std::string, std::string> hostExpressions;
	if (!reflectParameters(effect, module, compiled, hostExpressions, error))
		return false;

	for (const reshadefx::sampler_binding& binding : pass.sampler_bindings) {
		if (binding.index >= module.samplers.size())
			return fail(error, "ReShade FX compiler returned an invalid sampler binding.");
		const reshadefx::sampler& sampler = module.samplers[binding.index];

		const auto metadata = std::find_if(effect.samplers.begin(), effect.samplers.end(),
				[&](const ReShadeSampler& candidate) { return candidate.name == sampler.name; });
		if (metadata == effect.samplers.end())
			return fail(error, "ReShade FX compiler returned an unknown sampler.");
		const std::string texture = samplerTexture(*metadata);
		const auto textureInfo = std::find_if(module.textures.begin(), module.textures.end(),
				[&](const reshadefx::texture& candidate) { return candidate.unique_name == sampler.texture_name; });
		SlangResource resource;
		resource.type = sampler.type.texture_dimension() == 1 ? SlangResourceType::Sampler1D
				: sampler.type.texture_dimension() == 3 ? SlangResourceType::Sampler3D : SlangResourceType::Sampler2D;
		resource.numericType = sampler.type.is_integral() ? (sampler.type.is_signed() ? ShaderNumericType::SignedInteger : ShaderNumericType::UnsignedInteger) : ShaderNumericType::Float;
		resource.name = sampler.name;
		resource.alias = texture;
		if (textureInfo != module.textures.end() && lowercase(textureInfo->semantic) == "depth")
			resource.convention = SlangResourceConvention::Depth;
		else if ((textureInfo != module.textures.end() && lowercase(textureInfo->semantic) == "color")
				|| isSourceTexture(effect, texture, sampler.name))
			resource.convention = SlangResourceConvention::Source;
		else {
			const size_t producer = renderTargetProducer(module, techniqueIndex, passIndex, sampler.texture_name);
			resource.convention = producer != InvalidShaderPass && producer >= passIndex
					? SlangResourceConvention::Feedback : SlangResourceConvention::PassAlias;
		}
		compiled.slangResources.push_back(std::move(resource));
	}

	std::string assembly;
	if (!pass.cs_entry_point.empty()) {
		if (!compiler->assemble_code_for_entry_point(pass.cs_entry_point, compiled.compute, assembly, error)) return false;
		compiled.compute = normalizeStage(std::move(compiled.compute), module, effect, hostExpressions, compiled);
		std::smatch local;
		const std::regex size(R"(local_size_x\s*=\s*([0-9]+)\s*,\s*local_size_y\s*=\s*([0-9]+)\s*,\s*local_size_z\s*=\s*([0-9]+))");
		if (!std::regex_search(compiled.compute, local, size)) return fail(error, "Compute entry point has no local workgroup size.");
		for (uint32_t i = 0; i < 3; ++i) if (!parseShaderUnsigned(local[i + 1].str(), compiled.localSize[i]) || compiled.localSize[i] == 0) return fail(error, "Invalid compute workgroup size.");
		compiled.outputCount = 0;
	} else {
		if (pass.ps_entry_point.empty() || pass.vs_entry_point.empty()
				|| !compiler->assemble_code_for_entry_point(pass.ps_entry_point, compiled.fragment, assembly, error)
				|| !compiler->assemble_code_for_entry_point(pass.vs_entry_point, compiled.vertex, assembly, error))
			return fail(error, "FX could not generate the selected graphics entry points.");
		compiled.fragment = normalizeStage(std::move(compiled.fragment), module, effect, hostExpressions, compiled);
		compiled.vertex = normalizeStage(std::move(compiled.vertex), module, effect, hostExpressions, compiled);
		static const std::regex output(R"(layout\s*\(\s*location\s*=\s*([0-9]+)\s*\)\s*(?:(?:flat|noperspective|centroid)\s+)*out\b)");
		compiled.outputCount = 0;
		for (std::sregex_iterator it(compiled.fragment.begin(), compiled.fragment.end(), output), end; it != end; ++it)
			compiled.outputCount = std::max(compiled.outputCount, static_cast<uint32_t>(std::stoul((*it)[1].str()) + 1));
		if (compiled.outputCount == 0 || compiled.outputCount > 8) return fail(error, "FX color outputs exceed the attachment limit.");
		compiled.colorOutputCount = compiled.outputCount;
	}
	for (const auto& binding : pass.storage_bindings) {
		if (binding.index >= module.storages.size()) return fail(error, "Invalid FX storage binding.");
		const auto& storage = module.storages[binding.index];
		if (storage.level != 0)
			return fail(error, "Writable FX views at nonzero mip levels are not supported by the current render graph.");
		const auto texture = std::find_if(module.textures.begin(), module.textures.end(), [&](const auto& item) { return item.unique_name == storage.texture_name; });
		if (texture == module.textures.end()) return fail(error, "Unknown FX storage texture.");
		ShaderPassOutput output;
		output.alias = texture->name;
		output.type = texture->type == reshadefx::texture_type::texture_3d ? SlangResourceType::Sampler3D
				: texture->type == reshadefx::texture_type::texture_1d ? SlangResourceType::Sampler1D : SlangResourceType::Sampler2D;
		const auto metadata = std::find_if(effect.textures.begin(), effect.textures.end(), [&](const auto& item) { return item.name == texture->name; });
		if (metadata == effect.textures.end()) return fail(error, "Missing FX storage metadata.");
		std::smatch property;
		if (std::regex_search(metadata->definition, property, std::regex(R"(\bFormat\s*=\s*(\w+))"))) output.targetFormatName = canonicalShaderFormat(property[1].str());
		if (output.targetFormatName.empty()) output.targetFormatName = "R8G8B8A8_UNORM";
		if (std::regex_search(metadata->definition, property, std::regex(R"(\bDepth\s*=\s*([0-9]+))"))) parseShaderUnsigned(property[1].str(), output.depth);
		std::string width = "1", height = "1";
		if (std::regex_search(metadata->definition, property, std::regex(R"(\bWidth\s*=\s*([^;]+))"))) width = property[1].str();
		if (std::regex_search(metadata->definition, property, std::regex(R"(\bHeight\s*=\s*([^;]+))"))) height = property[1].str();
		if (compiled.storageOutputs.empty() && compiled.widthExpression.empty() && compiled.heightExpression.empty()) {
			compiled.widthExpression = width; compiled.heightExpression = height;
		}
		else if (compiled.widthExpression != width || compiled.heightExpression != height)
			return fail(error, "Writable images within an FX pass must use matching base dimensions.");
		if (std::any_of(compiled.storageOutputs.begin(), compiled.storageOutputs.end(), [&](const auto& previous) { return previous.alias == output.alias; }))
			return fail(error, "An FX pass cannot bind overlapping writable views of the same image.");
		const uint32_t slot = static_cast<uint32_t>(compiled.storageOutputs.size());
		compiled.storageBindings.emplace_back(storage.unique_name, slot);
		if (output.depth == 0 || output.depth > MaxShaderTargetDimension) return fail(error, "Invalid storage texture depth.");
		compiled.storageOutputs.push_back(std::move(output));
	}
	if (!compiled.compute.empty() && compiled.storageOutputs.empty()) {
		compiled.storageOutputs.emplace_back();
		compiled.widthExpression = compiled.heightExpression = "1";
	}
	source = std::move(compiled);
	error.clear();
	return true;
}

} // namespace postprocess

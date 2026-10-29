/*
	Portions Copyright 2026 The Hollycast Authors

	This file is part of Flycast.

	Flycast is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.
*/
#include "slang_frontend.h"
#include "reshade_fx.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <locale>
#include <regex>
#include <sstream>
#include <string_view>
#include <utility>

namespace postprocess {

bool parseShaderFloat(std::string_view text, float& value)
{
	std::istringstream stream { std::string(text) };
	stream.imbue(std::locale::classic());
	float parsed;
	stream >> std::noskipws >> parsed;
	if (!stream.eof() || stream.fail() || !std::isfinite(parsed))
		return false;
	value = parsed;
	return true;
}

bool parseShaderUnsigned(std::string_view text, uint32_t& value)
{
	if (text.empty())
		return false;
	uint64_t parsed = 0;
	for (const unsigned char character : text) {
		if (!std::isdigit(character))
			return false;
		const uint32_t digit = character - '0';
		if (parsed > (std::numeric_limits<uint32_t>::max() - digit) / 10u)
			return false;
		parsed = parsed * 10 + digit;
	}
	value = static_cast<uint32_t>(parsed);
	return true;
}

uint32_t shaderParameterComponentCount(SlangParameterType type)
{
	switch (type) {
	case SlangParameterType::Float2: return 2;
	case SlangParameterType::Float3: return 3;
	case SlangParameterType::Float4: return 4;
	case SlangParameterType::Float2x2: return 4;
	case SlangParameterType::Float3x3: return 9;
	case SlangParameterType::Float4x4: return 16;
	case SlangParameterType::Float2x3: case SlangParameterType::Float3x2: return 6;
	case SlangParameterType::Float2x4: case SlangParameterType::Float4x2: return 8;
	case SlangParameterType::Float3x4: case SlangParameterType::Float4x3: return 12;
	default: return 1;
	}
}
const char *shaderParameterGlslType(SlangParameterType type)
{
	switch (type) {
	case SlangParameterType::Int: return "int";
	case SlangParameterType::Bool: return "bool";
	case SlangParameterType::Float2: return "vec2";
	case SlangParameterType::Float3: return "vec3";
	case SlangParameterType::Float4: return "vec4";
	case SlangParameterType::Float2x2: return "mat2";
	case SlangParameterType::Float3x3: return "mat3";
	case SlangParameterType::Float4x4: return "mat4";
	case SlangParameterType::Float2x3: return "mat2x3";
	case SlangParameterType::Float2x4: return "mat2x4";
	case SlangParameterType::Float3x2: return "mat3x2";
	case SlangParameterType::Float3x4: return "mat3x4";
	case SlangParameterType::Float4x2: return "mat4x2";
	case SlangParameterType::Float4x3: return "mat4x3";
	default: return "float";
	}
}
const char *shaderResourceGlslType(SlangResourceType type, ShaderNumericType numeric)
{
	static const char *names[][3] = {
		{ "sampler1D", "isampler1D", "usampler1D" }, { "sampler2D", "isampler2D", "usampler2D" },
		{ "sampler2DArray", "isampler2DArray", "usampler2DArray" }, { "sampler3D", "isampler3D", "usampler3D" },
		{ "samplerCube", "isamplerCube", "usamplerCube" },
		{ "sampler1D", "isampler1D", "usampler1D" }, { "sampler2D", "isampler2D", "usampler2D" },
		{ "sampler2DArray", "isampler2DArray", "usampler2DArray" }, { "sampler3D", "isampler3D", "usampler3D" },
		{ "samplerCube", "isamplerCube", "usamplerCube" },
		{ "image1D", "iimage1D", "uimage1D" }, { "image2D", "iimage2D", "uimage2D" },
		{ "image2DArray", "iimage2DArray", "uimage2DArray" }, { "image3D", "iimage3D", "uimage3D" }
	};
	return names[static_cast<size_t>(type)][static_cast<size_t>(numeric)];
}
bool isShaderStorageResource(SlangResourceType type)
{
	return type >= SlangResourceType::Image1D;
}

namespace {

constexpr size_t MaxShaderTextSize = 2 * 1024 * 1024;

bool fail(std::string& error, const std::string& message)
{
	error = message;
	return false;
}

std::string trim(std::string value)
{
	const size_t first = value.find_first_not_of(" \t\r\n");
	if (first == std::string::npos)
		return {};
	const size_t last = value.find_last_not_of(" \t\r\n");
	return value.substr(first, last - first + 1);
}

bool startsWith(const std::string& value, std::string_view prefix)
{
	return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

bool endsWith(const std::string& value, std::string_view suffix)
{
	return value.size() >= suffix.size() && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool isIdentifier(const std::string& value)
{
	if (value.empty() || !(std::isalpha(static_cast<unsigned char>(value[0])) || value[0] == '_'))
		return false;
	return std::all_of(value.begin() + 1, value.end(), [](unsigned char c) { return std::isalnum(c) || c == '_'; });
}

std::string stripComments(const std::string& line, bool& inBlockComment)
{
	std::string result;
	bool quoted = false;
	bool escaped = false;
	for (size_t index = 0; index < line.size(); ++index) {
		if (inBlockComment) {
			if (line[index] == '*' && index + 1 < line.size() && line[index + 1] == '/') {
				inBlockComment = false;
				++index;
			}
			continue;
		}
		if (!quoted && line[index] == '/' && index + 1 < line.size()) {
			if (line[index + 1] == '/')
				break;
			if (line[index + 1] == '*') {
				inBlockComment = true;
				++index;
				continue;
			}
		}
		result += line[index];
		if (line[index] == '"' && !escaped)
			quoted = !quoted;
		escaped = line[index] == '\\' && !escaped;
		if (line[index] != '\\')
			escaped = false;
	}
	return result;
}

bool tokenize(const std::string& text, std::vector<std::string>& tokens, std::string& error)
{
	for (size_t index = 0; index < text.size();) {
		while (index < text.size() && std::isspace(static_cast<unsigned char>(text[index])))
			++index;
		if (index == text.size())
			break;
		if (text[index] == '"') {
			std::string token;
			bool escaped = false;
			++index;
			for (; index < text.size(); ++index) {
				if (text[index] == '"' && !escaped)
					break;
				token += text[index];
				escaped = text[index] == '\\' && !escaped;
				if (text[index] != '\\')
					escaped = false;
			}
			if (index == text.size())
				return fail(error, "Unterminated quoted Slang pragma value.");
			tokens.push_back(std::move(token));
			++index;
			continue;
		}
		const size_t begin = index;
		int parentheses = 0;
		for (; index < text.size(); ++index) {
			if (text[index] == '(')
				++parentheses;
			else if (text[index] == ')') {
				if (parentheses == 0)
					return fail(error, "Unbalanced parentheses in Slang pragma value.");
				--parentheses;
			}
			else if (parentheses == 0 && std::isspace(static_cast<unsigned char>(text[index])))
				break;
		}
		if (parentheses != 0)
			return fail(error, "Unbalanced parentheses in Slang pragma value.");
		tokens.push_back(text.substr(begin, index - begin));
	}
	return true;
}

bool parseParameterType(const std::string& token, SlangParameterType& type)
{
	if (token == "float") type = SlangParameterType::Float;
	else if (token == "int") type = SlangParameterType::Int;
	else if (token == "bool") type = SlangParameterType::Bool;
	else if (token == "float2" || token == "vec2") type = SlangParameterType::Float2;
	else if (token == "float3" || token == "vec3") type = SlangParameterType::Float3;
	else if (token == "float4" || token == "vec4") type = SlangParameterType::Float4;
	else if (token == "float2x2" || token == "mat2") type = SlangParameterType::Float2x2;
	else if (token == "float3x3" || token == "mat3") type = SlangParameterType::Float3x3;
	else if (token == "float4x4" || token == "mat4") type = SlangParameterType::Float4x4;
	else if (token == "float2x3" || token == "mat2x3") type = SlangParameterType::Float2x3;
	else if (token == "float2x4" || token == "mat2x4") type = SlangParameterType::Float2x4;
	else if (token == "float3x2" || token == "mat3x2") type = SlangParameterType::Float3x2;
	else if (token == "float3x4" || token == "mat3x4") type = SlangParameterType::Float3x4;
	else if (token == "float4x2" || token == "mat4x2") type = SlangParameterType::Float4x2;
	else if (token == "float4x3" || token == "mat4x3") type = SlangParameterType::Float4x3;
	else return false;
	return true;
}

uint32_t componentCount(SlangParameterType type)
{
	return shaderParameterComponentCount(type);
}

bool parseScalar(const std::string& text, SlangParameterType type, float& value)
{
	if (type == SlangParameterType::Bool) {
		if (text == "true" || text == "1") { value = 1.f; return true; }
		if (text == "false" || text == "0") { value = 0.f; return true; }
		return false;
	}
	float parsed;
	if (!parseShaderFloat(text, parsed))
		return false;
	if (type == SlangParameterType::Int && std::floor(parsed) != parsed)
		return false;
	value = parsed;
	return true;
}

bool parseParameterValue(std::string text, SlangParameterType type, SlangParameterValue& value)
{
	value.componentCount = componentCount(type);
	const size_t opening = text.find('(');
	if (opening != std::string::npos) {
		if (text.back() != ')')
			return false;
		text = text.substr(opening + 1, text.size() - opening - 2);
	}
	std::stringstream stream(text);
	std::string component;
	uint32_t index = 0;
	while (std::getline(stream, component, ',')) {
		if (index >= value.componentCount || !parseScalar(trim(component), type, value.values[index]))
			return false;
		++index;
	}
	return index == value.componentCount;
}

bool parseParameter(const std::string& pragma, SlangParameter& parameter, std::string& error)
{
	std::vector<std::string> tokens;
	if (!tokenize(pragma, tokens, error))
		return false;
	if (tokens.size() < 7 || tokens[0] != "#pragma" || tokens[1] != "parameter")
		return fail(error, "Malformed Slang #pragma parameter declaration.");
	parameter = {};
	size_t index = 2;
	SlangParameterType type;
	if (parseParameterType(tokens[index], type)) {
		parameter.type = type;
		++index;
	}
	if ((tokens.size() != index + 5 && tokens.size() != index + 6) || !isIdentifier(tokens[index]))
		return fail(error, "Slang #pragma parameter needs a name, label, default, minimum, maximum, and optional step.");
	parameter.name = tokens[index++];
	parameter.label = tokens[index++];
	if (parameter.label.empty()
			|| !parseParameterValue(tokens[index++], parameter.type, parameter.defaultValue)
			|| !parseParameterValue(tokens[index++], parameter.type, parameter.minimum)
			|| !parseParameterValue(tokens[index++], parameter.type, parameter.maximum))
		return fail(error, "Slang #pragma parameter has an invalid typed value.");
	if (index < tokens.size()) {
		if (!parseParameterValue(tokens[index], parameter.type, parameter.step))
			return fail(error, "Slang #pragma parameter has an invalid step.");
	}
	else {
		parameter.step.componentCount = componentCount(parameter.type);
		for (uint32_t component = 0; component < parameter.step.componentCount; ++component) {
			const float range = parameter.maximum.values[component] - parameter.minimum.values[component];
			parameter.step.values[component] = parameter.type == SlangParameterType::Bool ? 1.f
					: parameter.type == SlangParameterType::Int ? std::max(1.f, std::floor(range * 0.1f)) : range * 0.1f;
		}
	}
	return validateSlangParameter(parameter, error);
}

bool parseResourceType(const std::string& text, SlangResourceType& type)
{
	if (text == "sampler1D") type = SlangResourceType::Sampler1D;
	else if (text == "sampler2D") type = SlangResourceType::Sampler2D;
	else if (text == "sampler2DArray") type = SlangResourceType::Sampler2DArray;
	else if (text == "sampler3D") type = SlangResourceType::Sampler3D;
	else if (text == "samplerCube") type = SlangResourceType::SamplerCube;
	else if (text == "texture1D") type = SlangResourceType::Texture1D;
	else if (text == "texture2D") type = SlangResourceType::Texture2D;
	else if (text == "texture2DArray") type = SlangResourceType::Texture2DArray;
	else if (text == "texture3D") type = SlangResourceType::Texture3D;
	else if (text == "textureCube") type = SlangResourceType::TextureCube;
	else if (text == "image1D") type = SlangResourceType::Image1D;
	else if (text == "image2D") type = SlangResourceType::Image2D;
	else if (text == "image2DArray") type = SlangResourceType::Image2DArray;
	else if (text == "image3D") type = SlangResourceType::Image3D;
	else return false;
	return true;
}

void classifyResource(SlangResource& resource)
{
	// Slang commonly uses a Texture suffix for the GLSL binding while the
	// preset alias names the underlying render target. Keep the exact binding
	// name above, but resolve graph conventions from the logical name.
	std::string logicalName = resource.name;
	if (endsWith(logicalName, "Texture"))
		logicalName.resize(logicalName.size() - std::string_view("Texture").size());
	else if (endsWith(logicalName, "Sampler"))
		logicalName.resize(logicalName.size() - std::string_view("Sampler").size());
	if (logicalName == "Source") {
		resource.convention = SlangResourceConvention::Source;
		return;
	}
	if (logicalName == "Original") {
		resource.convention = SlangResourceConvention::Original;
		return;
	}
	if (endsWith(logicalName, "Feedback")) {
		resource.convention = SlangResourceConvention::Feedback;
		resource.alias = logicalName.substr(0, logicalName.size() - std::string_view("Feedback").size());
		return;
	}
	std::smatch indexedFeedback;
	if (std::regex_match(logicalName, indexedFeedback, std::regex(R"(^PassFeedback([0-9]+)$)"))) {
		resource.convention = SlangResourceConvention::Feedback;
		resource.alias = logicalName;
		return;
	}
	std::smatch historyMatch;
	if (std::regex_match(logicalName, historyMatch, std::regex(R"(^(.+)History([0-9]+)?$)"))) {
		resource.convention = SlangResourceConvention::History;
		resource.alias = historyMatch[1].str();
		if (historyMatch[2].matched) {
			const unsigned long offset = std::strtoul(historyMatch[2].str().c_str(), nullptr, 10);
			resource.historyOffset = static_cast<uint32_t>(std::min<unsigned long>(offset, UINT32_MAX));
		}
		return;
	}
	resource.convention = SlangResourceConvention::PassAlias;
	resource.alias = std::move(logicalName);
}

bool parseResource(const std::string& line, SlangResource& resource)
{
	static const std::regex declaration(R"(^\s*(?:layout\s*\(([^)]*)\)\s*)?(?:uniform\s+)?(?:(?:lowp|mediump|highp)\s+)?((?:[iu]?sampler|texture|[iu]?image)(?:2DArray|1D|2D|3D|Cube))\s+([A-Za-z_][A-Za-z0-9_]*)(?:\s*\[\s*([1-9][0-9]*)\s*\])?\s*;\s*$)");
	std::smatch match;
	if (!std::regex_match(line, match, declaration)) return false;
	std::string resourceType = match[2].str();
	if (resourceType.rfind("usampler", 0) == 0 || resourceType.rfind("isampler", 0) == 0
			|| resourceType.rfind("uimage", 0) == 0 || resourceType.rfind("iimage", 0) == 0) {
		resource.numericType = resourceType.front() == 'u' ? ShaderNumericType::UnsignedInteger : ShaderNumericType::SignedInteger;
		resourceType.erase(0, 1);
	}
	if (!parseResourceType(resourceType, resource.type)) return false;
	resource.name = match[3].str();
	if (match[4].matched) {
		const std::string countText = match[4].str();
		uint32_t count;
		if (!parseShaderUnsigned(countText, count))
			return false;
		resource.arrayCount = count;
	}
	else {
		resource.arrayCount = 1;
	}
	if (match[1].matched) {
		std::smatch bindingMatch;
		const std::string layout = match[1].str();
		std::smatch format;
		if (std::regex_search(layout, format, std::regex(R"(\b(rgba|rg|r)(8|16|32)(f|ui|i)?\b)"))) {
			resource.formatName = format[0].str();
			std::transform(resource.formatName.begin(), resource.formatName.end(), resource.formatName.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
			if (resource.formatName.size() > 2 && resource.formatName.substr(resource.formatName.size()-2) == "UI") resource.formatName.pop_back();
		}

		if (std::regex_search(layout, bindingMatch, std::regex(R"(\bset\s*=\s*([0-9]+))", std::regex::icase))) {
			if (!parseShaderUnsigned(bindingMatch[1].str(), resource.bindingSet))
				return false;
				resource.hasExplicitBinding = true;
		}
		if (std::regex_search(layout, bindingMatch, std::regex(R"(\bbinding\s*=\s*([0-9]+))", std::regex::icase))) {
			if (!parseShaderUnsigned(bindingMatch[1].str(), resource.binding))
				return false;
				resource.hasExplicitBinding = true;
		}
	}
	classifyResource(resource);
	return true;
}

bool looksLikeResourceDeclaration(const std::string& line)
{
	// Sampling calls may start a continuation line inside a shader function.
	// A resource declaration needs a type followed by its identifier.
	static const std::regex resourceStart(R"(^\s*(?:layout\s*\([^)]*\)\s*)?(?:uniform\s+)?(?:(?:lowp|mediump|highp)\s+)?(?:[iu]?sampler|texture|[iu]?image)[A-Za-z0-9_]*\s+[A-Za-z_])");
	return std::regex_search(line, resourceStart);
}

bool lowerStageInterface(std::string& source, bool vertex, SlangShaderIR& shader, std::string& error)
{
	if (source.empty())
		return true;
	std::string preprocessed;
	if (!preprocessShaderSource(source, { { "__VERSION__", "450" }, { "PARAMETER_UNIFORM", "1" } },
			preprocessed, error))
		return false;
	// Macro expansion has already happened. Keeping the definitions would expand
	// normalized host symbols a second time in the backend compiler.
	std::istringstream lines(preprocessed);
	std::string normalized;
	bool inComment = false;
	for (std::string line; std::getline(lines, line);) {
		const std::string code = trim(stripComments(line, inComment));
		if (std::regex_search(code, std::regex(R"(^#\s*(?:define|undef)\b)")))
			continue;
		SlangResource resource;
		if (parseResource(code, resource)) {
			const auto existing = std::find_if(shader.resources.begin(), shader.resources.end(),
					[&](const SlangResource& value) { return value.name == resource.name; });
			if (existing == shader.resources.end())
				shader.resources.push_back(std::move(resource));
			else if (existing->type != resource.type || existing->numericType != resource.numericType
					|| existing->formatName != resource.formatName || existing->arrayCount != resource.arrayCount
					|| (existing->hasExplicitBinding && resource.hasExplicitBinding
							&& (existing->bindingSet != resource.bindingSet || existing->binding != resource.binding)))
				return fail(error, "Slang stages declare incompatible bindings for '" + resource.name + "'.");
			else if (resource.hasExplicitBinding)
				*existing = std::move(resource);
		}
		else if (looksLikeResourceDeclaration(code))
			return fail(error, "Malformed or unsupported Slang resource declaration.");
		normalized += line + '\n';
	}
	source = std::move(normalized);

	// Presets describe resources by semantic names, regardless of whether an
	// author placed them in a uniform block or a push-constant block. Flatten
	// those names into the common host interface before any API compiles them.
	const std::regex block(R"((?:layout\s*\([^)]*\)\s*)?uniform\s+[A-Za-z_][A-Za-z0-9_]*\s*\{([^{}]*)\}\s*([A-Za-z_][A-Za-z0-9_]*)?\s*;)");
	const std::regex member(R"(^\s*(?:layout\s*\([^)]*\)\s*)?(float|int|uint|bool|vec[234]|mat[234])\s+([A-Za-z_][A-Za-z0-9_]*)\s*$)");
	std::vector<std::pair<std::string, std::string>> replacements;
	std::smatch match;
	while (std::regex_search(source, match, block)) {
		std::string declarations;
		std::istringstream blockLines(match[1].str());
		bool blockComment = false;
		for (std::string line; std::getline(blockLines, line);)
			declarations += stripComments(line, blockComment) + '\n';
		std::istringstream members(declarations);
		for (std::string declaration; std::getline(members, declaration, ';');) {
			if (trim(declaration).empty())
				continue;
			std::smatch field;
			if (!std::regex_match(declaration, field, member))
				return fail(error, "Unsupported Slang uniform-block member: " + trim(declaration));
			const std::string type = field[1].str();
			const std::string name = field[2].str();
			std::string expression;
			if (name == "MVP" || name == "MVPMatrix")
				expression = "mat4(1.0)";
			else if (name == "SourceSize" || name == "OriginalSize" || name == "OutputSize" || name == "FinalViewportSize")
				expression = type == "vec4" ? "vec4(" + name + ", vec2(1.0) / " + name + ")" : name;
			else if (name == "FrameCount" || name == "Rotation")
				expression = type + "(" + name + ")";
			else if (name == "FrameDirection")
				expression = type + "(1)";
			else if (name == "OriginalAspect")
				expression = "(OriginalSize.x / OriginalSize.y)";
			else if (name == "OriginalAspectRotated")
				expression = "((Rotation % 2) == 0 ? OriginalSize.x / OriginalSize.y : OriginalSize.y / OriginalSize.x)";
			else if (std::any_of(shader.parameters.begin(), shader.parameters.end(),
					[&](const SlangParameter& parameter) { return parameter.name == name; }))
				expression = name;
			else if (type == "vec4" && endsWith(name, "Size")) {
				const std::string textureName = name.substr(0, name.size() - 4);
				if (std::none_of(shader.resources.begin(), shader.resources.end(),
						[&](const SlangResource& resource) { return resource.name == textureName; })) {
					SlangResource resource;
					resource.name = textureName;
					classifyResource(resource);
					shader.resources.push_back(std::move(resource));
				}
				const std::string dimensions = "vec2(textureSize(" + textureName + ", 0))";
				expression = "vec4(" + dimensions + ", vec2(1.0) / " + dimensions + ")";
			}
			else
				return fail(error, "Unsupported Slang uniform semantic '" + name + "'.");
			const std::string binding = match[2].matched ? match[2].str() + "\\s*\\.\\s*" + name : name;
			replacements.emplace_back(binding, "(" + expression + ")");
		}
		source.erase(match.position(), match.length());
	}
	for (const auto& replacement : replacements)
		source = std::regex_replace(source, std::regex("\\b" + replacement.first + "\\b"), replacement.second);
	if (vertex) {
		const std::regex input(R"((?:layout\s*\([^)]*\)\s*)?in\s+(vec[234])\s+(Position|VertexCoord|TexCoord)\s*;)");
		while (std::regex_search(source, match, input)) {
			const std::string name = match[2].str();
			const std::string type = match[1].str();
			source.erase(match.position(), match.length());
			const std::string value = name == "TexCoord" ? "in_uv"
					: type == "vec4" ? "vec4(in_pos, 1.0)" : type == "vec2" ? "in_pos.xy" : "in_pos";
			source = std::regex_replace(source, std::regex("\\b" + name + "\\b"), "(" + value + ")");
		}
	}
	return true;
}

} // namespace

bool validateSlangParameter(const SlangParameter& parameter, std::string& error)
{
	const uint32_t components = componentCount(parameter.type);
	if (parameter.name.empty() || parameter.label.empty()
			|| parameter.defaultValue.componentCount != components
			|| parameter.minimum.componentCount != components
			|| parameter.maximum.componentCount != components
			|| parameter.step.componentCount != components)
		return fail(error, "Shader parameter metadata has inconsistent typed component counts.");

	for (uint32_t index = 0; index < components; ++index) {
		const float defaultValue = parameter.defaultValue.values[index];
		const float minimum = parameter.minimum.values[index];
		const float maximum = parameter.maximum.values[index];
		const float step = parameter.step.values[index];
		if (!std::isfinite(defaultValue) || !std::isfinite(minimum) || !std::isfinite(maximum)
				|| !std::isfinite(step) || minimum > maximum || defaultValue < minimum
				|| defaultValue > maximum || step < 0.f)
			return fail(error, "Shader parameter '" + parameter.name + "' has an invalid default, range, or step.");
		if (parameter.type == SlangParameterType::Int
				&& (std::floor(defaultValue) != defaultValue || std::floor(minimum) != minimum
						|| std::floor(maximum) != maximum || std::floor(step) != step))
			return fail(error, "Integer shader parameter '" + parameter.name + "' requires integral metadata.");
		if (parameter.type == SlangParameterType::Bool
				&& ((defaultValue != 0.f && defaultValue != 1.f) || (minimum != 0.f && minimum != 1.f)
						|| (maximum != 0.f && maximum != 1.f) || (step != 0.f && step != 1.f)))
			return fail(error, "Boolean shader parameter '" + parameter.name + "' requires Boolean metadata.");
	}
	error.clear();
	return true;
}

bool parseSlangFrontend(const std::string& shaderText, SlangShaderIR& shader, std::string& error)
{
	shader = {};
	error.clear();
	if (shaderText.empty() || shaderText.size() > MaxShaderTextSize || shaderText.find('\0') != std::string::npos)
		return fail(error, "Slang source is empty, too large, or contains NUL bytes.");

	SlangShaderIR parsed;
	std::string commonSource;
	std::string vertexSource;
	std::string fragmentSource;
	std::string computeSource;
	bool inBlockComment = false;
	bool versionSeen = false;
	bool stageSeen = false;
	bool vertexSeen = false;
	bool fragmentSeen = false;
	bool computeSeen = false;
	enum class Stage { None, Vertex, Fragment, Compute } stage = Stage::None;

	std::istringstream stream(shaderText);
	std::string rawLine;
	while (std::getline(stream, rawLine)) {
		const std::string line = trim(stripComments(rawLine, inBlockComment));
		if (line.empty())
			continue;
		if (startsWith(line, "#version")) {
			if (versionSeen || stageSeen || !commonSource.empty() || !vertexSource.empty() || !fragmentSource.empty())
				return fail(error, "Slang #version must appear once before declarations and stages.");
			versionSeen = true;
			commonSource += rawLine + '\n';
			continue;
		}
		if (!versionSeen)
			return fail(error, "Slang source must start with #version.");
		if (startsWith(line, "#pragma parameter")) {
			SlangParameter parameter;
			if (!parseParameter(line, parameter, error))
				return false;
			const auto existing = std::find_if(parsed.parameters.begin(), parsed.parameters.end(),
					[&](const SlangParameter& value) { return value.name == parameter.name; });
			if (existing != parsed.parameters.end()) {
				if (existing->label != parameter.label || existing->type != parameter.type
						|| existing->defaultValue.values != parameter.defaultValue.values
						|| existing->minimum.values != parameter.minimum.values
						|| existing->maximum.values != parameter.maximum.values
						|| existing->step.values != parameter.step.values)
					return fail(error, "Conflicting Slang parameter '" + parameter.name + "'.");
				continue;
			}
			parsed.parameters.push_back(std::move(parameter));
			continue;
		}
		if (startsWith(line, "#pragma stage")) {
			std::vector<std::string> tokens;
			if (!tokenize(line, tokens, error))
				return false;
			if (tokens.size() != 3 || tokens[0] != "#pragma" || tokens[1] != "stage")
				return fail(error, "Malformed Slang #pragma stage declaration.");
			stageSeen = true;
			if (tokens[2] == "vertex" && !vertexSeen && !computeSeen) {
				stage = Stage::Vertex;
				vertexSeen = true;
			}
			else if (tokens[2] == "fragment" && !fragmentSeen && !computeSeen) {
				stage = Stage::Fragment;
				fragmentSeen = true;
			}
			else if (tokens[2] == "compute" && !computeSeen && !vertexSeen && !fragmentSeen) { stage = Stage::Compute; computeSeen = true; }
			else
				return fail(error, "Slang must declare graphics stages or a single compute stage.");
			continue;
		}
		if (startsWith(line, "#pragma name") || startsWith(line, "#pragma alias")) {
			std::vector<std::string> tokens;
			if (!tokenize(line, tokens, error) || tokens.size() != 3 || !parsed.displayName.empty())
				return fail(error, "Malformed or duplicate Slang #pragma name declaration.");
			parsed.displayName = tokens[2];
			continue;
		}
		if (startsWith(line, "#pragma format")) {
			std::vector<std::string> tokens;
			if (!tokenize(line, tokens, error) || tokens.size() != 3 || !parsed.format.empty())
				return fail(error, "Malformed or duplicate Slang #pragma format declaration.");
			parsed.format = tokens[2];
			continue;
		}

		std::string *destination = stage == Stage::Vertex ? &vertexSource : stage == Stage::Fragment ? &fragmentSource : stage == Stage::Compute ? &computeSource : &commonSource;
		*destination += rawLine + '\n';
	}
	if (inBlockComment)
		return fail(error, "Unterminated block comment in Slang source.");
	if (!versionSeen)
		return fail(error, "Slang source must start with #version.");
	if (!stageSeen) {
		parsed.fragment = { commonSource, true };
	}
	else if (computeSeen && !vertexSeen && !fragmentSeen) { parsed.compute = { commonSource + computeSource, true }; }
	else if (!fragmentSeen || fragmentSource.empty())
		return fail(error, "Slang source must declare a non-empty fragment stage.");
	else {
		parsed.vertex = { vertexSeen ? commonSource + vertexSource : std::string(), vertexSeen };
		parsed.fragment = { commonSource + fragmentSource, true };
	}
	if (!lowerStageInterface(parsed.vertex.source, true, parsed, error)
			|| !lowerStageInterface(parsed.fragment.source, false, parsed, error)
			|| !lowerStageInterface(parsed.compute.source, false, parsed, error))
		return false;
	shader = std::move(parsed);
	return true;
}

} // namespace postprocess

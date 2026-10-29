/*
	Portions Copyright 2026 The Hollycast Authors

	This file is part of Flycast.

	Flycast is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.
*/
#include "shader_chain.h"

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <glslang/SPIRV/GlslangToSpv.h>
#include <spirv_hlsl.hpp>
#include <spirv_parser.hpp>

#include <algorithm>
#include <map>
#include <regex>
#include <set>
#include <sstream>

namespace postprocess {
namespace {

struct CompilerLifetime {
	bool initialized = glslang::InitializeProcess();
	~CompilerLifetime()
	{
		if (initialized)
			glslang::FinalizeProcess();
	}
};

// Match Dx11ShaderChain::ShaderFrameUniforms, including the padding before the
// packed parameter array. std140 and HLSL both align each vec4 to 16 bytes.
constexpr char FrameUniforms[] = R"(
layout(std140, set = 1, binding = 0) uniform ShaderFrame
{
	vec2 SourceSize;
	vec2 OutputSize;
	int FrameCount;
	int Rotation;
	vec2 OriginalSize;
	vec2 FinalViewportSize;
	int HasDepth;
	int FramePadding;
	vec4 Parameters[64];
};
)";

uint32_t componentCount(SlangParameterType type)
{
	return postprocess::shaderParameterComponentCount(type);
}

const char *parameterType(SlangParameterType type)
{
	return postprocess::shaderParameterGlslType(type);
}

std::string parameterValue(const SlangParameter& parameter, uint32_t offset)
{
	std::string value = std::string(parameterType(parameter.type)) + "(";
	for (uint32_t component = 0; component < componentCount(parameter.type); ++component) {
		if (component != 0)
			value += ", ";
		const uint32_t index = offset + component;
		value += "Parameters[" + std::to_string(index / 4) + "][" + std::to_string(index % 4) + "]";
	}
	return value + ")";
}

// Keep host resource declarations separate from shader syntax. Unknown uniforms
// remain in the source and are rejected by the compiler instead of becoming zero.
void removeHostDeclarations(std::string& source, const std::set<std::string>& names)
{
	static const std::regex declaration(
		R"((?:layout\s*\([^\n;{}]*\)\s*)?uniform\s+(?:(?:lowp|mediump|highp)\s+)?\w+\s+(\w+)\s*;)");
	std::string result;
	size_t offset = 0;
	for (std::sregex_iterator it(source.begin(), source.end(), declaration), end; it != end; ++it) {
		if (names.count((*it)[1].str()) == 0)
			continue;
		result.append(source, offset, it->position() - offset);
		offset = it->position() + it->length();
	}
	result.append(source, offset, std::string::npos);
	source = std::move(result);
}

// A shared alphabetic assignment preserves varying semantics when vertex and
// fragment declarations use different orders. Explicit locations stay intact.
void assignVaryingLocations(std::string& source, const std::string& companion, const char *direction)
{
	static const std::regex declaration(
		R"((^|\n)([ \t]*)(?:layout\s*\(\s*location\s*=\s*([0-9]+)\s*\)\s*)?((?:(?:flat|noperspective|centroid)\s+)?(?:in|out)\s+\w+\s+(\w+)\s*;))");
	std::map<std::string, uint32_t> locations;
	std::set<uint32_t> used { 0 };
	const auto collect = [&](const std::string& text, const char *stageDirection) {
		for (std::sregex_iterator it(text.begin(), text.end(), declaration), end; it != end; ++it) {
			if (!std::regex_search((*it)[4].str(), std::regex("\\b" + std::string(stageDirection) + "\\s")))
				continue;
			const std::string name = (*it)[5].str();
			if (name == "vTexCoord")
				locations[name] = 0;
			else if ((*it)[3].matched) {
				uint32_t location;
				if (parseShaderUnsigned((*it)[3].str(), location)) {
					locations[name] = location;
					used.insert(location);
				}
			}
			else
				locations.emplace(name, InvalidShaderPass);
		}
	};
	collect(source, direction);
	collect(companion, std::string(direction) == "in" ? "out" : "in");
	uint32_t next = 1;
	for (auto& entry : locations)
		if (entry.second == InvalidShaderPass) {
			while (used.count(next) != 0)
				++next;
			entry.second = next;
			used.insert(next++);
		}
	std::string result;
	size_t offset = 0;
	const std::regex requested("\\b" + std::string(direction) + "\\s");
	for (std::sregex_iterator it(source.begin(), source.end(), declaration), end; it != end; ++it) {
		if ((*it)[3].matched || !std::regex_search((*it)[4].str(), requested))
			continue;
		result.append(source, offset, it->position() - offset);
		result += (*it)[1].str() + (*it)[2].str() + "layout(location = "
				+ std::to_string(locations.at((*it)[5].str())) + ") " + (*it)[4].str();
		offset = it->position() + it->length();
	}
	result.append(source, offset, std::string::npos);
	source = std::move(result);
}

bool compileStage(const std::string& body, const std::string& companion,
		const std::vector<SlangParameter>& parameters, const std::vector<ShaderPassInput>& inputs,
		bool vertex, bool nativeStages, bool retroArchGlsl, uint32_t outputs,
		std::string& hlsl, std::string& error, const ShaderPassSource *computeSource = nullptr)
{
	hlsl.clear();
	if ((!computeSource && outputs == 0) || outputs > 8 || inputs.size() > 16) {
		error = "DirectX shader chains support up to 16 textures and eight color outputs per pass.";
		return false;
	}
	if (!initializeShaderCompiler()) {
		error = "Unable to initialize the shader compiler.";
		return false;
	}
	std::string source = std::regex_replace(body, std::regex(R"(^\s*#version[^\r\n]*[\r\n]*)"), "");
	std::string header = "#version 450\n#define gl_VertexID gl_VertexIndex\n"
			"#define texture2D texture\n#define texture2DLod textureLod\n#define texture2DProj textureProj\n"
			"#define texture2DGrad textureGrad\n#define texture2DGradEXT textureGrad\n";
	header += FrameUniforms;
	std::set<std::string> hostNames { "SourceSize", "OutputSize", "OriginalSize", "FinalViewportSize", "FrameCount", "Rotation", "HasDepth" };
	std::vector<std::string> resources { "Source" };
	for (const ShaderPassInput& input : inputs)
		if (std::find(resources.begin(), resources.end(), input.bindingName) == resources.end())
			resources.push_back(input.bindingName);
	if (resources.size() > 16) {
		error = "DirectX shader chains support up to 16 textures per pass, including Source.";
		return false;
	}
	for (size_t index = 0; index < resources.size(); ++index) {
		hostNames.insert(resources[index]);
		const auto input = std::find_if(inputs.begin(), inputs.end(), [&](const auto& item) { return item.bindingName == resources[index]; });
		const std::string type = input == inputs.end() ? "sampler2D" : shaderResourceGlslType(input->type, input->numericType);
		header += "layout(set = 0, binding = " + std::to_string(index) + ") uniform " + type + " " + resources[index] + ";\n";
	}
	uint32_t parameterOffset = 0;
	std::string initializeParameters;
	for (const SlangParameter& parameter : parameters) {
		if (parameterOffset + componentCount(parameter.type) > MaxShaderParameterComponents) {
			error = "Shader parameter storage exceeds 256 components.";
			return false;
		}
		hostNames.insert(parameter.name);
		header += std::string(parameterType(parameter.type)) + " " + parameter.name + ";\n";
		initializeParameters += parameter.name + " = " + parameterValue(parameter, parameterOffset) + ";\n";
		parameterOffset += componentCount(parameter.type);
	}
	if (computeSource) {
		uint32_t storageSlot = 0;
		for (const auto& binding : computeSource->storageBindings) {
			const std::regex declaration("layout\\s*\\(([^)]*)\\)\\s*uniform\\s+([iu]?image(?:1D|2DArray|2D|3D))\\s+" + binding.first + "\\s*;");
			std::smatch match;
			if (!std::regex_search(source, match, declaration)) { error = "Missing compute storage declaration: " + binding.first; return false; }
			std::string layout = std::regex_replace(match[1].str(), std::regex(R"((?:set|binding)\s*=\s*[0-9]+\s*,?\s*)"), "");
			while (!layout.empty() && (layout.back() == ',' || std::isspace(static_cast<unsigned char>(layout.back())))) layout.pop_back();
			header += "layout(set=2, binding=" + std::to_string(storageSlot++) + (layout.empty() ? "" : ", " + layout) + ") uniform " + match[2].str() + " " + binding.first + ";\n";
			source = std::regex_replace(source, declaration, "");
		}
	}
	removeHostDeclarations(source, hostNames);
	if (!nativeStages) {
		const std::string direction = vertex ? "out" : "in";
		const bool customLocationZero = std::regex_search(source, std::regex(
				"layout\\s*\\([^)]*\\blocation\\s*=\\s*0\\b[^)]*\\)\\s*(?:(?:flat|noperspective|centroid)\\s+)?"
				+ direction + "\\s+\\w+\\s+\\w+"));
		// A fully specified stage interface owns location zero. Do not add the
		// convenience UV varying when the shader uses its own varying there.
		const bool defaultTexCoord = !customLocationZero
				|| std::regex_search(source, std::regex(R"(\bvTexCoord\b)"));
		if (vertex) {
			// The quad has two position components. Expose the established vec3
			// portable contract, or the vec4 position expected by GLSL presets.
			source = std::regex_replace(source, std::regex(R"((?:layout\s*\([^\n]*\)\s*)?in\s+vec[234]\s+(?:VertexCoord|TexCoord|in_pos|in_uv|Position|aPosition|aTexCoord)\s*;)"), "");
			header += "layout(location=0) in vec2 HollycastPosition;\nlayout(location=1) in vec2 in_uv;\n#define in_pos vec3(HollycastPosition, 1.0)\n";
			const bool position4 = retroArchGlsl || std::regex_search(source, std::regex(R"(gl_Position\s*=\s*VertexCoord\s*;)"));
			header += position4 ? "#define VertexCoord vec4(in_pos, 1.0)\n" : "#define VertexCoord in_pos\n";
			header += "#define TexCoord in_uv\n#define Position VertexCoord\n#define aPosition VertexCoord\n#define aTexCoord in_uv\n";
			source = std::regex_replace(source, std::regex(R"((?:layout\s*\([^\n]*\)\s*)?out\s+vec2\s+vTexCoord\s*;)"), "");
			if (defaultTexCoord) {
				header += "layout(location=0) out vec2 vTexCoord;\n";
				initializeParameters += "vTexCoord = in_uv;\n";
			}
		}
		else {
			source = std::regex_replace(source, std::regex(R"((?:layout\s*\([^\n]*\)\s*)?in\s+vec2\s+vTexCoord\s*;)"), "");
			source = std::regex_replace(source, std::regex(R"((?:layout\s*\([^\n]*\)\s*)?out\s+vec4\s+(?:FragColor|FxOutput0)\s*;)"), "");
			if (defaultTexCoord)
				header += "layout(location=0) in vec2 vTexCoord;\n";
			header += "layout(location=0) out vec4 FragColor;\n#define gl_FragColor FragColor\n#define FxOutput0 FragColor\n";
			for (uint32_t index = 1; index < outputs; ++index)
				header += "layout(location=" + std::to_string(index) + ") out vec4 FxOutput" + std::to_string(index) + ";\n";
		}
		assignVaryingLocations(source, companion, vertex ? "out" : "in");
	}
	if (!initializeParameters.empty()) {
		const std::regex mainFunction(R"(\bvoid\s+main\s*\(\s*(?:void\s*)?\))");
		if (!std::regex_search(source, mainFunction)) {
			error = "Shader has no main entry point.";
			return false;
		}
		source = std::regex_replace(source, mainFunction, "void HollycastShaderMain()");
		source += "\nvoid main() {\n" + initializeParameters + "HollycastShaderMain();\n}\n";
	}
	const std::string glsl = header + source;
	const char *text = glsl.c_str();
	const EShLanguage stage = computeSource && !computeSource->compute.empty() ? EShLangCompute : vertex ? EShLangVertex : EShLangFragment;
	glslang::TShader shader(stage);
	shader.setStrings(&text, 1);
	shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 450);
	shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
	shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
	const EShMessages messages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);
	if (!shader.parse(GetDefaultResources(), 450, false, messages)) {
		error = shader.getInfoLog();
		return false;
	}
	glslang::TProgram program;
	program.addShader(&shader);
	if (!program.link(messages)) {
		error = program.getInfoLog();
		return false;
	}
	std::vector<uint32_t> spirv;
	glslang::GlslangToSpv(*program.getIntermediate(stage), spirv);
	try {
		spirv_cross::Parser parser(std::move(spirv));
		parser.parse();
		auto& ir = parser.get_parsed_ir();
		std::vector<std::pair<uint32_t, uint32_t>> undefinedReturns;
		ir.for_each_typed_id<spirv_cross::SPIRBlock>([&](uint32_t id, auto& block) {
			// FX lowering expands loop conditions into SSA statements. FXC may
			// then attempt an unbounded unroll of texture-gradient loops. Retain
			// dynamic flow control unless the effect supplied an explicit hint.
			// https://learn.microsoft.com/windows/win32/direct3dhlsl/dx-graphics-hlsl-for
			if (nativeStages && !vertex && block.merge == spirv_cross::SPIRBlock::MergeLoop
					&& block.hint == spirv_cross::SPIRBlock::HintNone)
				block.hint = spirv_cross::SPIRBlock::HintDontUnroll;
			if (block.terminator == spirv_cross::SPIRBlock::Return && block.return_value
					&& ir.ids[block.return_value].get_type() == spirv_cross::TypeUndef)
				undefinedReturns.emplace_back(id, ir.get<spirv_cross::SPIRUndef>(block.return_value).basetype);
		});
		// GLSL permits an undefined fallthrough return, while FXC requires a
		// value on every path. Define only those undefined results as zero.
		// Allocate after iteration because the IR locks its ID list while visiting.
		for (const auto& entry : undefinedReturns) {
			const uint32_t constant = ir.increase_bound_by(1);
			ir.make_constant_null(constant, entry.second, true);
			ir.get<spirv_cross::SPIRBlock>(entry.first).return_value = constant;
		}
		spirv_cross::CompilerHLSL compiler(std::move(ir));
		spirv_cross::CompilerHLSL::Options options;
		options.shader_model = 50;
		compiler.set_hlsl_options(options);
		if (computeSource && computeSource->compute.empty())
			for (uint32_t i = 0; i < computeSource->storageBindings.size(); ++i) {
				spirv_cross::HLSLResourceBinding binding;
				binding.stage = spv::ExecutionModelFragment; binding.desc_set = 2; binding.binding = i;
				binding.uav.register_binding = outputs + i;
				compiler.add_hlsl_resource_binding(binding);
			}

		if (vertex && !nativeStages) {
			compiler.add_vertex_attribute_remap({ 0, "POSITION" });
			compiler.add_vertex_attribute_remap({ 1, "TEXCOORD0" });
		}
		hlsl = compiler.compile();
	} catch (const spirv_cross::CompilerError& exception) {
		error = exception.what();
		return false;
	}
	error.clear();
	return true;
}

} // namespace

bool initializeShaderCompiler()
{
	static const CompilerLifetime compiler;
	return compiler.initialized;
}

bool translateGlslFragmentToHlsl(const std::string& source, std::string& hlsl, std::string& error)
{
	return translateGlslFragmentToHlsl(source, std::vector<SlangParameter> {}, hlsl, error);
}

bool translateGlslFragmentToHlsl(const std::string& source, const std::vector<ShaderParameter>& parameters,
		std::string& hlsl, std::string& error)
{
	std::vector<SlangParameter> typed;
	for (const ShaderParameter& parameter : parameters) {
		SlangParameter value;
		value.name = parameter.name;
		value.type = parameter.type;
		typed.push_back(std::move(value));
	}
	return translateGlslFragmentToHlsl(source, typed, hlsl, error);
}

bool translateGlslFragmentToHlsl(const std::string& source, const std::vector<SlangParameter>& parameters,
		std::string& hlsl, std::string& error, uint32_t outputCount)
{
	return compileStage(source, {}, parameters, {}, false, false, false, outputCount, hlsl, error);
}

bool translateGlslVertexToHlsl(const std::string& source, const std::vector<SlangParameter>& parameters,
		std::string& hlsl, std::string& error)
{
	return compileStage(source, {}, parameters, {}, true, false, false, 1, hlsl, error);
}

bool translateGlslGraphFragmentToHlsl(const ShaderPassSource& source,
		const std::vector<SlangParameter>& parameters, const std::vector<ShaderPassInput>& inputs,
		uint32_t outputCount, std::string& hlsl, std::string& error)
{
	return compileStage(source.fragment, source.vertex, parameters, inputs, false,
			source.nativeFxStages, source.retroArchGlsl, outputCount, hlsl, error, source.storageBindings.empty() ? nullptr : &source);
}

bool translateGlslGraphVertexToHlsl(const std::string& source,
		const std::vector<SlangParameter>& parameters, const std::vector<ShaderPassInput>& inputs,
		std::string& hlsl, std::string& error, bool nativeFxStages, bool retroArchGlsl)
{
	return compileStage(source, {}, parameters, inputs, true, nativeFxStages, retroArchGlsl, 1, hlsl, error);
}

bool translateGlslComputeToHlsl(const ShaderPassSource& source,
		const std::vector<SlangParameter>& parameters, const std::vector<ShaderPassInput>& inputs,
		std::string& hlsl, std::string& error)
{
	return compileStage(source.compute, {}, parameters, inputs, false, true, false, 0, hlsl, error, &source);
}

} // namespace postprocess

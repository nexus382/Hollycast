/*
	Portions Copyright 2026 The Hollycast Authors

	This file is part of Flycast.

	Flycast is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.
*/
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <functional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace postprocess {

enum class ReShadeValueType {
	Unknown,
	Bool,
	Int,
	UInt,
	Int2,
	Int3,
	Int4,
	UInt2,
	UInt3,
	UInt4,
	Float,
	Float2,
	Float3,
	Float4,
	Float2x2,
	Float3x3,
	Float4x4,
};

struct ReShadeAnnotation {
	std::string name;
	std::string value;
};

struct ReShadeUniform {
	ReShadeValueType valueType = ReShadeValueType::Unknown;
	std::string typeName;
	std::string name;
	std::string defaultValue;
	std::vector<ReShadeAnnotation> annotations;
};

struct ReShadeTexture {
	std::string typeName;
	std::string name;
	std::string definition;
	std::vector<ReShadeAnnotation> annotations;
	size_t sourceBegin = 0;
	size_t sourceEnd = 0;
};

struct ReShadeSampler {
	std::string typeName;
	std::string name;
	std::string definition;
	std::vector<ReShadeAnnotation> annotations;
	size_t sourceBegin = 0;
	size_t sourceEnd = 0;
};

struct ReShadeStruct {
	std::string name;
	std::string body;
};

struct ReShadeFunction {
	std::string returnType;
	std::string name;
	std::string signature;
	std::string body;
	size_t line = 0;
};

struct ReShadePass {
	std::string name;
	std::string vertexShader;
	std::string pixelShader;
	std::string computeShader;
	std::string stateBlock;
	size_t line = 0;
	size_t bodyBegin = 0;
};

struct ReShadeTechnique {
	std::string name;
	std::vector<ReShadePass> passes;
	size_t line = 0;
};

// This is a syntax IR, not a compiler. It deliberately preserves declarations
// and bodies so later backend emitters can lower one validated representation.
struct ReShadeEffect {
	// Preserve complete namespaced source for the language compiler. Metadata
	// extraction only records host-owned annotations and resource declarations.
	std::string preprocessedSource;
	std::vector<std::pair<size_t, size_t>> annotationRanges;
	std::vector<std::string> preprocessorDirectives;
	std::vector<ReShadeUniform> uniforms;
	std::vector<ReShadeTexture> textures;
	std::vector<ReShadeSampler> samplers;
	std::vector<ReShadeSampler> storages;
	std::vector<ReShadeStruct> structs;
	std::vector<std::string> globals;
	std::vector<ReShadeFunction> functions;
	std::vector<ReShadeTechnique> techniques;
	std::optional<size_t> selectedTechnique;
	std::optional<size_t> selectedPass;
};

// The package loader resolves only active includes and enforces its storage boundary.
using ShaderIncludeResolver = std::function<bool(const std::string& parent, const std::string& name,
		std::string& path, std::string& text, std::string& error)>;
bool preprocessShaderSource(const std::string& source,
		const std::unordered_map<std::string, std::string>& initialDefines,
		std::string& output, std::string& error, const ShaderIncludeResolver& includeResolver = {},
		const std::string& sourcePath = {}, bool preserveSource = false, bool stageScopes = false);

// Compiles all selected passes with a single effect compiler.
struct ShaderPassSource;
bool compileReShadeFxTechnique(const ReShadeEffect& effect, size_t techniqueIndex,
		std::vector<ShaderPassSource>& sources, std::string& error);

// Lexes and parses FX declarations after preprocessing inside the selected package.
bool parseReShadeEffect(const std::string& source, ReShadeEffect& effect, std::string& error,
		const ShaderIncludeResolver& includeResolver = {}, const std::string& sourcePath = {});

} // namespace postprocess

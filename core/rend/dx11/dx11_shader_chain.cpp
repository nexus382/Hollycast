/*
	Portions Copyright 2026 The Hollycast Authors

	This file is part of Flycast.

	Flycast is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.
*/
#include "dx11_shader_chain.h"

#include "cfg/option.h"
#include "dx11_quad.h"
#include "dx11context.h"
#include "log/LogManager.h"
#include "oslib/storage.h"
#include "stbi.h"

#include <algorithm>
#include <array>
#include <cstddef>

namespace {

bool isSampler2D(postprocess::SlangResourceType type)
{
	return type == postprocess::SlangResourceType::Sampler2D || type == postprocess::SlangResourceType::Texture2D;
}

uint32_t parameterComponentCount(postprocess::SlangParameterType type)
{
	return postprocess::shaderParameterComponentCount(type);
}

D3D11_TEXTURE_ADDRESS_MODE textureAddressMode(postprocess::ShaderWrapMode mode)
{
	switch (mode) {
	case postprocess::ShaderWrapMode::Repeat: return D3D11_TEXTURE_ADDRESS_WRAP;
	case postprocess::ShaderWrapMode::MirroredRepeat: return D3D11_TEXTURE_ADDRESS_MIRROR;
	case postprocess::ShaderWrapMode::ClampToBorder: return D3D11_TEXTURE_ADDRESS_BORDER;
	case postprocess::ShaderWrapMode::ClampToEdge: return D3D11_TEXTURE_ADDRESS_CLAMP;
	}
	return D3D11_TEXTURE_ADDRESS_CLAMP;
}

D3D11_BLEND blendFactor(postprocess::ShaderBlendFactor factor, bool alpha = false)
{
	// DirectX restricts alpha blend factors to their alpha-channel spellings.
	if (alpha) {
		switch (factor) {
		case postprocess::ShaderBlendFactor::SourceColor: factor = postprocess::ShaderBlendFactor::SourceAlpha; break;
		case postprocess::ShaderBlendFactor::OneMinusSourceColor: factor = postprocess::ShaderBlendFactor::OneMinusSourceAlpha; break;
		case postprocess::ShaderBlendFactor::DestinationColor: factor = postprocess::ShaderBlendFactor::DestinationAlpha; break;
		case postprocess::ShaderBlendFactor::OneMinusDestinationColor: factor = postprocess::ShaderBlendFactor::OneMinusDestinationAlpha; break;
		default: break;
		}
	}
	switch (factor) {
	case postprocess::ShaderBlendFactor::Zero: return D3D11_BLEND_ZERO;
	case postprocess::ShaderBlendFactor::One: return D3D11_BLEND_ONE;
	case postprocess::ShaderBlendFactor::SourceColor: return D3D11_BLEND_SRC_COLOR;
	case postprocess::ShaderBlendFactor::OneMinusSourceColor: return D3D11_BLEND_INV_SRC_COLOR;
	case postprocess::ShaderBlendFactor::DestinationColor: return D3D11_BLEND_DEST_COLOR;
	case postprocess::ShaderBlendFactor::OneMinusDestinationColor: return D3D11_BLEND_INV_DEST_COLOR;
	case postprocess::ShaderBlendFactor::SourceAlpha: return D3D11_BLEND_SRC_ALPHA;
	case postprocess::ShaderBlendFactor::OneMinusSourceAlpha: return D3D11_BLEND_INV_SRC_ALPHA;
	case postprocess::ShaderBlendFactor::DestinationAlpha: return D3D11_BLEND_DEST_ALPHA;
	case postprocess::ShaderBlendFactor::OneMinusDestinationAlpha: return D3D11_BLEND_INV_DEST_ALPHA;
	}
	return D3D11_BLEND_ONE;
}

D3D11_BLEND_OP blendOperation(postprocess::ShaderBlendOperation operation)
{
	switch (operation) {
	case postprocess::ShaderBlendOperation::Add: return D3D11_BLEND_OP_ADD;
	case postprocess::ShaderBlendOperation::Subtract: return D3D11_BLEND_OP_SUBTRACT;
	case postprocess::ShaderBlendOperation::ReverseSubtract: return D3D11_BLEND_OP_REV_SUBTRACT;
	case postprocess::ShaderBlendOperation::Minimum: return D3D11_BLEND_OP_MIN;
	case postprocess::ShaderBlendOperation::Maximum: return D3D11_BLEND_OP_MAX;
	}
	return D3D11_BLEND_OP_ADD;
}

bool resolveTargetFormat(const postprocess::ShaderPass& pass, DXGI_FORMAT& format, std::string& error)
{
	if (pass.targetFormatName.empty()) {
		format = pass.targetFormat == postprocess::ShaderTargetFormat::Float ? DXGI_FORMAT_R16G16B16A16_FLOAT
				: pass.targetFormat == postprocess::ShaderTargetFormat::Srgb ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
				: DXGI_FORMAT_B8G8R8A8_UNORM;
		return true;
	}
	if (pass.targetFormatName == "R8_UNORM") { format = DXGI_FORMAT_R8_UNORM; return true; }
	if (pass.targetFormatName == "R16_UNORM") { format = DXGI_FORMAT_R16_UNORM; return true; }
	if (pass.targetFormatName == "R16_SFLOAT") { format = DXGI_FORMAT_R16_FLOAT; return true; }
	if (pass.targetFormatName == "R32_SFLOAT") { format = DXGI_FORMAT_R32_FLOAT; return true; }
	if (pass.targetFormatName == "R32_SINT") { format = DXGI_FORMAT_R32_SINT; return true; }
	if (pass.targetFormatName == "R32_UINT") { format = DXGI_FORMAT_R32_UINT; return true; }
	if (pass.targetFormatName == "R8G8_UNORM") { format = DXGI_FORMAT_R8G8_UNORM; return true; }
	if (pass.targetFormatName == "R16G16_UNORM") { format = DXGI_FORMAT_R16G16_UNORM; return true; }
	if (pass.targetFormatName == "R16G16_SFLOAT") { format = DXGI_FORMAT_R16G16_FLOAT; return true; }
	if (pass.targetFormatName == "R32G32_SFLOAT") { format = DXGI_FORMAT_R32G32_FLOAT; return true; }
	if (pass.targetFormatName == "R32G32B32A32_SINT") { format = DXGI_FORMAT_R32G32B32A32_SINT; return true; }
	if (pass.targetFormatName == "R32G32B32A32_UINT") { format = DXGI_FORMAT_R32G32B32A32_UINT; return true; }
	if (pass.targetFormatName == "A2B10G10R10_UNORM_PACK32") { format = DXGI_FORMAT_R10G10B10A2_UNORM; return true; }
	if (pass.targetFormatName == "B10G11R11_UFLOAT_PACK32") { format = DXGI_FORMAT_R11G11B10_FLOAT; return true; }
	if (pass.targetFormatName == "R8G8B8A8_UNORM") format = DXGI_FORMAT_R8G8B8A8_UNORM;
	else if (pass.targetFormatName == "R8G8B8A8_SRGB") format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	else if (pass.targetFormatName == "R16G16B16A16_SFLOAT" || pass.targetFormatName == "RGBA16F") format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	else if (pass.targetFormatName == "R16G16B16A16_UNORM") format = DXGI_FORMAT_R16G16B16A16_UNORM;
	else if (pass.targetFormatName == "R32G32B32A32_SFLOAT") format = DXGI_FORMAT_R32G32B32A32_FLOAT;
	else {
		error = "DirectX 11 does not support post-processing target format '" + pass.targetFormatName + "'.";
		return false;
	}
	return true;
}

} // namespace
void Dx11ShaderChain::init(const ComPtr<ID3D11Device>& device, const ComPtr<ID3D11DeviceContext>& deviceContext, Quad *quad)
{
	this->device = device;
	this->deviceContext = deviceContext;
	this->quad = quad;
}

bool Dx11ShaderChain::ensurePassSamplers(std::string& error)
{
	if (linearSampler && nearestSampler && mipLinearSampler && mipNearestSampler)
		return true;
	const auto create = [&](D3D11_FILTER filter, float maxLod, ComPtr<ID3D11SamplerState>& sampler) {
		D3D11_SAMPLER_DESC description {};
		description.Filter = filter;
		description.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
		description.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		description.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		description.ComparisonFunc = D3D11_COMPARISON_NEVER;
		description.MinLOD = 0.f;
		description.MaxLOD = maxLod;
		return SUCCEEDED(device->CreateSamplerState(&description, &sampler.get()));
	};
	if (!create(D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT, 0.f, linearSampler)
			|| !create(D3D11_FILTER_MIN_MAG_MIP_POINT, 0.f, nearestSampler)
			|| !create(D3D11_FILTER_MIN_MAG_MIP_LINEAR, D3D11_FLOAT32_MAX, mipLinearSampler)
			|| !create(D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_FLOAT32_MAX, mipNearestSampler)) {
		linearSampler.reset();
		nearestSampler.reset();
		mipLinearSampler.reset();
		mipNearestSampler.reset();
		error = "DirectX 11 could not create post-processing sampler states.";
		return false;
	}
	return true;
}

bool Dx11ShaderChain::compileCandidate(const postprocess::ShaderChain& chain, std::vector<Program>& candidate,
		std::string& error) const
{
	const pD3DCompile compiler = DX11Context::Instance()->getCompiler();
	if (compiler == nullptr) {
		error = "DirectX 11 shader compiler is unavailable.";
		return false;
	}
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
		std::vector<postprocess::ShaderPassInput> inputs = pass.inputs;
		if (std::none_of(inputs.begin(), inputs.end(), [](const postprocess::ShaderPassInput& input) { return input.bindingName == "Source"; }))
			inputs.push_back({ "Source", postprocess::ShaderInputKind::Source, {}, postprocess::InvalidShaderPass });
		std::stable_sort(inputs.begin(), inputs.end(), [](const postprocess::ShaderPassInput& left, const postprocess::ShaderPassInput& right) {
			return left.bindingName == "Source" && right.bindingName != "Source";
		});
		if (inputs.front().bindingName != "Source" || inputs.size() > D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT) {
			error = inputs.front().bindingName != "Source"
					? "DirectX 11 could not assign Source to texture slot zero."
					: "DirectX 11 shader chains exceed the device resource-slot limit.";
			return false;
		}
		for (const postprocess::SlangResource& resource : passSource->slangResources)
			if (resource.arrayCount != 1) {
				error = "DirectX 11 does not support arrays of sampled resources; '" + resource.name + "' is unsupported.";
				return false;
			}
		const auto level = device->GetFeatureLevel();
		const uint32_t shaderModel = level >= D3D_FEATURE_LEVEL_11_0 ? 50 : level >= D3D_FEATURE_LEVEL_10_1 ? 41 : 40;
		if (shaderModel < 50 && !pass.storageBindings.empty()) {
			error = "Writable shader images require DirectX feature level 11.";
			return false;
		}
		std::string hlslSource;
		const uint32_t outputCount = pass.outputs.empty() ? 1u : static_cast<uint32_t>(pass.outputs.size());
		if (!(pass.compute ? postprocess::translateGlslComputeToHlsl(*passSource, typedParameters, inputs, hlslSource, error)
				: postprocess::translateGlslGraphFragmentToHlsl(*passSource, typedParameters, inputs, pass.colorOutputCount, hlslSource, error, shaderModel)))
			return false;
		if (pass.storageBindings.size() > D3D11_PS_CS_UAV_REGISTER_COUNT
				|| (!pass.compute && pass.colorOutputCount + pass.storageBindings.size() > D3D11_PS_CS_UAV_REGISTER_COUNT)) {
			error = "DirectX 11 writable image bindings exceed the available UAV slots.";
			return false;
		}
		if (pass.compute && (device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0
				|| static_cast<uint64_t>(pass.localSize[0]) * pass.localSize[1] * pass.localSize[2] > 1024
				|| pass.localSize[0] > 1024 || pass.localSize[1] > 1024 || pass.localSize[2] > 64)) {
			error = "This compute workgroup requires DirectX feature level 11 and valid local dimensions.";
			return false;
		}
		ComPtr<ID3DBlob> shaderBlob;
		ComPtr<ID3DBlob> errorBlob;
		const HRESULT result = compiler(hlslSource.data(), hlslSource.size(), pass.sourcePath.c_str(), nullptr, nullptr,
				"main", pass.compute ? "cs_5_0" : shaderModel == 50 ? "ps_5_0" : shaderModel == 41 ? "ps_4_1" : "ps_4_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &shaderBlob.get(), &errorBlob.get());
		if (FAILED(result)) {
			error = errorBlob ? static_cast<const char *>(errorBlob->GetBufferPointer()) : "DirectX 11 HLSL compilation failed.";
			return false;
		}
		Program program;
		program.pass = pass;
		program.displayGlsl = passSource->displayGlsl;
		program.nativeFxStages = passSource->nativeFxStages;
		program.parameters = std::move(typedParameters);
		for (const postprocess::SlangParameter& parameter : program.parameters) {
			const auto configured = std::find_if(chain.parameters.begin(), chain.parameters.end(), [&](const postprocess::ShaderParameter& value) {
				return value.name == parameter.name && value.layerIndex == pass.layerIndex;
			});
			program.parameterIndices.push_back(configured == chain.parameters.end() ? postprocess::InvalidShaderPass
					: static_cast<uint32_t>(std::distance(chain.parameters.begin(), configured)));
		}
		program.outputFormats.reserve(outputCount);
		for (uint32_t output = 0; output < outputCount; ++output) {
			postprocess::ShaderPass outputPass = pass;
			if (!pass.outputs.empty()) {
				outputPass.targetFormat = pass.outputs[output].targetFormat;
				outputPass.targetFormatName = pass.outputs[output].targetFormatName;
			}
			DXGI_FORMAT targetFormat;
			if (!resolveTargetFormat(outputPass, targetFormat, error))
				return false;
			program.outputFormats.push_back(targetFormat);
		}
		for (size_t slot = 0; slot < inputs.size(); ++slot) {
			uint32_t lookupIndex = postprocess::InvalidShaderPass;
			if (inputs[slot].kind == postprocess::ShaderInputKind::LookupTexture) {
				const auto lookup = std::find_if(chain.textures.begin(), chain.textures.end(), [&](const postprocess::ShaderPresetTexture& texture) {
					return texture.name == inputs[slot].resourceName;
				});
				if (lookup == chain.textures.end()) {
					error = "DirectX 11 lookup texture '" + inputs[slot].resourceName + "' was not resolved.";
					return false;
				}
				lookupIndex = static_cast<uint32_t>(std::distance(chain.textures.begin(), lookup));
			}
			ResourceBinding binding;
			binding.input = inputs[slot];
			binding.slot = static_cast<UINT>(slot);
			binding.lookupIndex = lookupIndex;
			const auto samplerConfig = std::find_if(pass.samplers.begin(), pass.samplers.end(), [&](const auto& sampler) {
				return sampler.bindingName == inputs[slot].bindingName;
			});
			if (inputs[slot].kind != postprocess::ShaderInputKind::LookupTexture
					&& (samplerConfig != pass.samplers.end() || pass.wrapMode != postprocess::ShaderWrapMode::ClampToEdge)) {
				const bool linear = samplerConfig != pass.samplers.end() ? samplerConfig->linearFilter : pass.linearFilter;
				const auto wrap = samplerConfig != pass.samplers.end() ? samplerConfig->wrapMode : pass.wrapMode;
				D3D11_SAMPLER_DESC description {};
				description.Filter = linear
						? D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT : D3D11_FILTER_MIN_MAG_MIP_POINT;
				description.AddressU = textureAddressMode(wrap);
				description.AddressV = description.AddressW = description.AddressU;
				description.ComparisonFunc = D3D11_COMPARISON_NEVER;
				description.MaxLOD = 0.f;
				if (FAILED(device->CreateSamplerState(&description, &binding.sampler.get()))) {
					error = "DirectX 11 could not create shader sampler '" + inputs[slot].bindingName + "'.";
					return false;
				}
				description.Filter = linear
						? D3D11_FILTER_MIN_MAG_MIP_LINEAR : D3D11_FILTER_MIN_MAG_MIP_POINT;
				description.MaxLOD = D3D11_FLOAT32_MAX;
				if (FAILED(device->CreateSamplerState(&description, &binding.mipSampler.get()))) {
					error = "DirectX 11 could not create mipmapped shader sampler '" + inputs[slot].bindingName + "'.";
					return false;
				}
				binding.useMipmaps = samplerConfig == pass.samplers.end() || samplerConfig->useMipmaps;
			}
			program.resources.push_back(std::move(binding));
		}
		program.resourceViews.resize(program.resources.size());
		program.samplers.resize(program.resources.size());
		if (FAILED(pass.compute ? device->CreateComputeShader(shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize(), nullptr, &program.computeShader.get())
				: device->CreatePixelShader(shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize(), nullptr, &program.pixelShader.get()))) {
			error = "DirectX 11 pixel shader creation failed.";
			return false;
		}
		if (!pass.compute) {
			D3D11_BLEND_DESC blendDescription {};
			blendDescription.IndependentBlendEnable = TRUE;
			for (uint32_t output = 0; output < pass.colorOutputCount; ++output) {
				D3D11_RENDER_TARGET_BLEND_DESC& renderTarget = blendDescription.RenderTarget[output];
				const auto& attachment = pass.renderState.attachments[output];
				renderTarget.BlendEnable = pass.renderState.reflected ? attachment.blendEnabled : pass.blendEnabled;
				renderTarget.SrcBlend = blendFactor(pass.renderState.reflected ? attachment.source : pass.sourceBlend);
				renderTarget.DestBlend = blendFactor(pass.renderState.reflected ? attachment.destination : pass.destinationBlend);
				renderTarget.BlendOp = blendOperation(pass.renderState.reflected ? attachment.operation : pass.blendOperation);
				renderTarget.SrcBlendAlpha = blendFactor(attachment.sourceAlpha, true);
				renderTarget.DestBlendAlpha = blendFactor(attachment.destinationAlpha, true);
				renderTarget.BlendOpAlpha = blendOperation(attachment.alphaOperation);
				renderTarget.RenderTargetWriteMask = attachment.writeMask;
			}
			if (FAILED(device->CreateBlendState(&blendDescription, &program.blendState.get()))) {
				error = "DirectX 11 post-processing blend-state creation failed.";
				return false;
			}
		}
		if (pass.renderState.stencilEnabled) {
			D3D11_DEPTH_STENCIL_DESC desc {}; desc.DepthEnable = FALSE; desc.StencilEnable = TRUE;
			desc.StencilReadMask = pass.renderState.stencilReadMask; desc.StencilWriteMask = pass.renderState.stencilWriteMask;
			const D3D11_STENCIL_OP operations[] { D3D11_STENCIL_OP_ZERO, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_REPLACE,
				D3D11_STENCIL_OP_INCR_SAT, D3D11_STENCIL_OP_DECR_SAT, D3D11_STENCIL_OP_INVERT, D3D11_STENCIL_OP_INCR, D3D11_STENCIL_OP_DECR };
			desc.FrontFace.StencilFunc = static_cast<D3D11_COMPARISON_FUNC>(static_cast<unsigned>(pass.renderState.stencilComparison) + 1);
			desc.FrontFace.StencilPassOp = operations[static_cast<unsigned>(pass.renderState.stencilPass)];
			desc.FrontFace.StencilFailOp = operations[static_cast<unsigned>(pass.renderState.stencilFail)];
			desc.FrontFace.StencilDepthFailOp = operations[static_cast<unsigned>(pass.renderState.stencilDepthFail)];
			desc.BackFace = desc.FrontFace;
			if (FAILED(device->CreateDepthStencilState(&desc, &program.stencilState.get()))) { error = "Unable to create FX stencil state."; return false; }
		}
		if (!pass.compute) {
			// Translate the default vertex stage too: the ordinary quad exports
			// SV_POSITION before TEXCOORD0, while translated fragment stages read
			// TEXCOORD0 first. D3D11 links signature registers by order, so mixing
			// those stages can sample a single pixel or produce a black frame.
			const std::string vertexSource = passSource->vertex.empty()
					? "void main() { gl_Position = vec4(in_pos.xy, 0.0, 1.0); }"
					: passSource->vertex;
			std::string hlslVertex;
			if (!postprocess::translateGlslGraphVertexToHlsl(vertexSource, program.parameters, inputs,
					hlslVertex, error, passSource->nativeFxStages, passSource->retroArchGlsl, shaderModel))
				return false;
			ComPtr<ID3DBlob> vertexBlob;
			ComPtr<ID3DBlob> vertexErrorBlob;
			const HRESULT vertexResult = compiler(hlslVertex.data(), hlslVertex.size(), pass.sourcePath.c_str(), nullptr, nullptr,
					"main", shaderModel == 50 ? "vs_5_0" : shaderModel == 41 ? "vs_4_1" : "vs_4_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &vertexBlob.get(), &vertexErrorBlob.get());
			if (FAILED(vertexResult)) {
				error = vertexErrorBlob ? static_cast<const char *>(vertexErrorBlob->GetBufferPointer()) : "DirectX 11 vertex shader compilation failed.";
				return false;
			}
			if (FAILED(device->CreateVertexShader(vertexBlob->GetBufferPointer(), vertexBlob->GetBufferSize(), nullptr, &program.vertexShader.get()))) {
				error = "DirectX 11 vertex shader creation failed.";
				return false;
			}
		}
		candidate.push_back(std::move(program));
	}
	const std::vector<std::vector<bool>> mipmapRequirements = postprocess::resolveShaderPassOutputMipmaps(chain);
	for (size_t passIndex = 0; passIndex < candidate.size(); ++passIndex)
		candidate[passIndex].outputNeedsMipmaps = mipmapRequirements[passIndex];
	return true;
}

bool Dx11ShaderChain::updateConfiguredChain()
{
	const std::vector<std::string> requestedPaths = postprocess::configuredShaderPaths();
	const std::string requestedLabel = postprocess::shaderSelectionLabel(requestedPaths);
	const uint64_t generation = postprocess::shaderChainReloadGeneration();
	if (requestedPaths == configuredPaths && generation == configuredGeneration)
		return !programs.empty();
	// Present the loading notice before compilation blocks this render thread.
	if (!requestedPaths.empty() && !postprocess::prepareShaderCompilation())
		return !programs.empty();
	configuredPaths = requestedPaths;
	configuredGeneration = generation;
	if (requestedPaths.empty()) {
		programs.clear();
		parameters.clear();
		targets.clear();
		sourceHistory.clear();
		lookupTextures.clear();
		displayInput = {};
		depthInput = {};
		mipmapSource = {};
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
	if (!ensurePassSamplers(error)) {
		postprocess::reportShaderChainStatus(activePath, error);
		WARN_LOG(RENDERER, "Post-processing shader '%s' was not activated: %s", requestedLabel.c_str(), error.c_str());
		return !programs.empty();
	}
	std::vector<Program> candidate;
	if (!compileCandidate(chain, candidate, error)) {
		postprocess::reportShaderChainStatus(activePath, error);
		WARN_LOG(RENDERER, "Post-processing shader '%s' was not activated: %s", requestedLabel.c_str(), error.c_str());
		return !programs.empty();
	}
	std::vector<LookupTexture> candidateLookupTextures;
	candidateLookupTextures.reserve(chain.textures.size());
	for (const postprocess::ShaderPresetTexture& texture : chain.textures)
		candidateLookupTextures.push_back({ texture });
	if (!ensureLookupTextures(candidateLookupTextures, error)) {
		postprocess::reportShaderChainStatus(activePath, error);
		WARN_LOG(RENDERER, "Post-processing shader '%s' was not activated: %s", requestedLabel.c_str(), error.c_str());
		return !programs.empty();
	}
	programs = std::move(candidate);
	renderedFrames = 0;
	parameters = std::move(chain.parameters);
	sourceHistoryDepth = 1;
	needsSourceHistory = false;
	for (const postprocess::ShaderPass& pass : chain.passes)
		for (const postprocess::ShaderPassInput& input : pass.inputs) {
			if (input.kind != postprocess::ShaderInputKind::History
					&& input.kind != postprocess::ShaderInputKind::Feedback)
				continue;
			const uint32_t requiredDepth = input.kind == postprocess::ShaderInputKind::Feedback
					? 2u : input.historyOffset + 1;
			if (input.sourcePass == postprocess::InvalidShaderPass) {
				sourceHistoryDepth = std::max(sourceHistoryDepth, requiredDepth);
				needsSourceHistory = true;
			}
			else {
				programs[input.sourcePass].historyDepth =
						std::max(programs[input.sourcePass].historyDepth, requiredDepth);
			}
		}
	for (const auto& program : programs)
		for (const auto& output : program.pass.outputs)
			if (output.previousFrame && output.previousPass != postprocess::InvalidShaderPass)
				programs[output.previousPass].historyDepth = std::max(2u, programs[output.previousPass].historyDepth);
	targets.clear();
	stencilTargets.clear();
	sourceHistory.clear();
	lookupTextures = std::move(candidateLookupTextures);
	activePath = requestedLabel;
	postprocess::reportShaderChainStatus(activePath, {}, parameters,
			chain.techniques, chain.selectedTechnique);
	parameterGeneration = postprocess::shaderChainStatusGeneration();
	return true;
}

bool Dx11ShaderChain::ensureLookupTextures(std::string& error)
{
	return ensureLookupTextures(lookupTextures, error);
}

bool Dx11ShaderChain::ensureLookupTextures(std::vector<LookupTexture>& textures, std::string& error)
{
	uint64_t bytes = 0;
	for (auto& lookup : textures) {
		if (lookup.view) continue;
		const auto decoded = postprocess::loadShaderTextureData(lookup.config, error);
		if (!decoded) return false;
		const auto& data = *decoded;
		if (data.pixels.size() > postprocess::MaxShaderChainTargetBytes - bytes) { error = "Shader lookup images exceed the chain budget."; return false; }
		bytes += data.pixels.size();
		postprocess::ShaderPass formatPass; formatPass.targetFormatName = data.formatName;
		DXGI_FORMAT format;
		if (!resolveTargetFormat(formatPass, format, error)) return false;
		if (lookup.config.numericType != postprocess::ShaderNumericType::Float && (lookup.config.linearFilter || lookup.config.useMipmaps)) {
			error = "Integer shader textures require nearest filtering without generated mipmaps."; return false;
		}
		const UINT pixelBytes = postprocess::shaderFormatBytes(data.formatName);
		const bool mipmaps = lookup.config.useMipmaps;
		const UINT binds = D3D11_BIND_SHADER_RESOURCE | (mipmaps ? D3D11_BIND_RENDER_TARGET : 0);
		const UINT misc = mipmaps ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;
		ComPtr<ID3D11Resource> resource;
		if (data.type == postprocess::SlangResourceType::Sampler3D || data.type == postprocess::SlangResourceType::Texture3D) {
			D3D11_TEXTURE3D_DESC desc { data.width, data.height, data.depth, mipmaps ? 0u : 1u, format, D3D11_USAGE_DEFAULT, binds, 0, misc };
			ComPtr<ID3D11Texture3D> texture;
			if (FAILED(device->CreateTexture3D(&desc, nullptr, &texture.get()))) { error = "Unable to allocate 3D shader lookup: " + lookup.config.path; return false; }
			texture.as(resource);
		} else if (data.type == postprocess::SlangResourceType::Sampler1D || data.type == postprocess::SlangResourceType::Texture1D) {
			D3D11_TEXTURE1D_DESC desc { data.width, mipmaps ? 0u : 1u, 1, format, D3D11_USAGE_DEFAULT, binds, 0, misc };
			ComPtr<ID3D11Texture1D> texture;
			if (FAILED(device->CreateTexture1D(&desc, nullptr, &texture.get()))) { error = "Unable to allocate 1D shader lookup."; return false; }
			texture.as(resource);
		} else {
			D3D11_TEXTURE2D_DESC desc {};
			desc.Width = data.width; desc.Height = data.height; desc.MipLevels = mipmaps ? 0 : 1;
			desc.ArraySize = data.layers; desc.Format = format; desc.SampleDesc.Count = 1;
			desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = binds; desc.MiscFlags = misc;
			if (data.type == postprocess::SlangResourceType::SamplerCube || data.type == postprocess::SlangResourceType::TextureCube) desc.MiscFlags |= D3D11_RESOURCE_MISC_TEXTURECUBE;
			ComPtr<ID3D11Texture2D> texture;
			if (FAILED(device->CreateTexture2D(&desc, nullptr, &texture.get()))) { error = "Unable to allocate shader lookup image."; return false; }
			texture.as(resource);
		}
		UINT levels = 1; if (mipmaps) for (UINT n = std::max({data.width, data.height, data.depth}); n > 1; n >>= 1) ++levels;
		for (UINT layer = 0; layer < data.layers; ++layer)
			deviceContext->UpdateSubresource(resource, layer * levels, nullptr,
					data.pixels.data() + static_cast<size_t>(layer) * data.width * data.height * pixelBytes,
					data.width * pixelBytes, data.width * data.height * pixelBytes);
		const bool array = data.type == postprocess::SlangResourceType::Sampler2DArray || data.type == postprocess::SlangResourceType::Texture2DArray;
		D3D11_SHADER_RESOURCE_VIEW_DESC view {};
		view.Format = format;
		view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
		view.Texture2DArray.MipLevels = levels;
		view.Texture2DArray.ArraySize = data.layers;
		if (FAILED(device->CreateShaderResourceView(resource, array ? &view : nullptr, &lookup.view.get()))) { error = "Unable to create typed shader lookup view."; return false; }
		if (mipmaps) deviceContext->GenerateMips(lookup.view);
		D3D11_SAMPLER_DESC sampler {};
		sampler.Filter = lookup.config.linearFilter ? D3D11_FILTER_MIN_MAG_MIP_LINEAR : D3D11_FILTER_MIN_MAG_MIP_POINT;
		sampler.AddressU = sampler.AddressV = sampler.AddressW = textureAddressMode(lookup.config.wrapMode);
		sampler.MaxLOD = mipmaps ? D3D11_FLOAT32_MAX : 0;
		if (FAILED(device->CreateSamplerState(&sampler, &lookup.sampler.get()))) { error = "Unable to create shader lookup sampler."; return false; }
		lookup.config.decoded.reset();
	}
	return true;
}

bool Dx11ShaderChain::ensureTarget(Target& target, int width, int height, DXGI_FORMAT format, bool mipmapped, bool storage, postprocess::SlangResourceType type, uint32_t depth)
{
	if (storage && (!target.resource() || target.width != width || target.height != height || target.format != format || target.depth != depth || target.type != type || target.mipmapped != mipmapped)) {
		target = {}; target.width = width; target.height = height; target.format = format; target.depth = depth; target.type = type; target.mipmapped = mipmapped;
		const UINT flags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | (mipmapped ? D3D11_BIND_RENDER_TARGET : 0);
		const UINT levels = mipmapped ? 0u : 1u;
		const UINT misc = mipmapped ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0u;
		HRESULT result;
		if (type == postprocess::SlangResourceType::Sampler3D) {
			D3D11_TEXTURE3D_DESC d { static_cast<UINT>(width), static_cast<UINT>(height), depth, levels, format, D3D11_USAGE_DEFAULT, flags, 0, misc };
			result = device->CreateTexture3D(&d, nullptr, &target.volume.get());
		} else if (type == postprocess::SlangResourceType::Sampler1D) {
			D3D11_TEXTURE1D_DESC d { static_cast<UINT>(width), levels, 1, format, D3D11_USAGE_DEFAULT, flags, 0, misc };
			result = device->CreateTexture1D(&d, nullptr, &target.line.get());
		} else {
			D3D11_TEXTURE2D_DESC d {}; d.Width = width; d.Height = height; d.MipLevels = levels; d.ArraySize = type == postprocess::SlangResourceType::Sampler2DArray ? depth : 1; d.SampleDesc.Count = 1; d.Format = format; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = flags; d.MiscFlags = misc;
			result = device->CreateTexture2D(&d, nullptr, &target.texture.get());
		}
		// Default views collapse a one-layer array to Texture2D; preserve its declared type.
		D3D11_SHADER_RESOURCE_VIEW_DESC srv {};
		D3D11_UNORDERED_ACCESS_VIEW_DESC uav {};
		const bool array = type == postprocess::SlangResourceType::Sampler2DArray;
		if (array) {
			srv.Format = uav.Format = format;
			srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
			srv.Texture2DArray.MipLevels = mipmapped ? UINT(-1) : 1;
			srv.Texture2DArray.ArraySize = depth;
			uav.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
			uav.Texture2DArray.ArraySize = depth;
		}
		if (FAILED(result) || FAILED(device->CreateShaderResourceView(target.resource(), array ? &srv : nullptr, &target.shaderView.get()))
				|| FAILED(device->CreateUnorderedAccessView(target.resource(), array ? &uav : nullptr, &target.storage.get()))) { target = {}; return false; }
		const UINT zero[4] {}; deviceContext->ClearUnorderedAccessViewUint(target.storage, zero);
		if (mipmapped) deviceContext->GenerateMips(target.shaderView);
	}
	if (storage) return static_cast<bool>(target.storage);
	if (target.texture && target.width == width && target.height == height && target.format == format && target.mipmapped == mipmapped)
		return true;
	target.shaderView.reset();
	target.view.reset();
	target.texture.reset();
	target.initialized = false;
	D3D11_TEXTURE2D_DESC description {};
	description.Width = width;
	description.Height = height;
	description.MipLevels = mipmapped ? 0 : 1;
	description.ArraySize = 1;
	description.Format = format;
	description.SampleDesc.Count = 1;
	description.Usage = D3D11_USAGE_DEFAULT;
	description.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	description.MiscFlags = mipmapped ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;
	D3D11_RENDER_TARGET_VIEW_DESC renderTargetDescription {};
	renderTargetDescription.Format = description.Format;
	renderTargetDescription.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
	renderTargetDescription.Texture2D.MipSlice = 0;
	D3D11_SHADER_RESOURCE_VIEW_DESC shaderResourceDescription {};
	shaderResourceDescription.Format = description.Format;
	shaderResourceDescription.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	shaderResourceDescription.Texture2D.MostDetailedMip = 0;
	shaderResourceDescription.Texture2D.MipLevels = mipmapped ? UINT(-1) : 1;
	if (FAILED(device->CreateTexture2D(&description, nullptr, &target.texture.get())) ||
			FAILED(device->CreateRenderTargetView(target.texture, &renderTargetDescription, &target.view.get())) ||
			FAILED(device->CreateShaderResourceView(target.texture, &shaderResourceDescription, &target.shaderView.get()))) {
		target.shaderView.reset();
		target.view.reset();
		target.texture.reset();
		target.width = 0;
		target.height = 0;
		target.format = DXGI_FORMAT_UNKNOWN;
		target.mipmapped = false;
		return false;
	}
	target.width = width;
	target.height = height;
	target.format = format;
	target.mipmapped = mipmapped;
	// The alternate target is sampled by history/feedback before its first render.
	// Define it instead of exposing driver-dependent texture contents on that frame.
	static constexpr float clearColor[] = { 0.f, 0.f, 0.f, 0.f };
	deviceContext->ClearRenderTargetView(target.view, clearColor);
	return true;
}

void Dx11ShaderChain::unbindPixelShaderSource(UINT count) const
{
	std::array<ID3D11ShaderResourceView *, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> nullViews {};
	count = std::min(count, static_cast<UINT>(nullViews.size()));
	deviceContext->PSSetShaderResources(0, count, nullViews.data());
	deviceContext->VSSetShaderResources(0, count, nullViews.data());
}

bool Dx11ShaderChain::prepareMipmappedSource(ComPtr<ID3D11ShaderResourceView>& sourceView, int width, int height,
		ComPtr<ID3D11ShaderResourceView>& mipmappedView)
{
	if (!ensureTarget(mipmapSource, width, height, DXGI_FORMAT_B8G8R8A8_UNORM, true))
		return false;

	// The emulator framebuffer has one mip level. Blit it into an internal,
	// mip-capable target before generation instead of issuing GenerateMips on a
	// resource that cannot legally provide a complete mip chain.
	D3D11_VIEWPORT viewport {};
	viewport.Width = static_cast<float>(width);
	viewport.Height = static_cast<float>(height);
	viewport.MinDepth = 0.f;
	viewport.MaxDepth = 1.f;
	unbindPixelShaderSource();
	deviceContext->RSSetViewports(1, &viewport);
	deviceContext->OMSetRenderTargets(1, &mipmapSource.view.get(), nullptr);
	deviceContext->OMSetBlendState(nullptr, nullptr, 0xffffffff);
	quad->draw(sourceView, DX11Context::Instance()->getSamplers().getSampler(false));
	unbindPixelShaderSource();
	deviceContext->GenerateMips(mipmapSource.shaderView);
	mipmappedView = mipmapSource.shaderView;
	return true;
}

bool Dx11ShaderChain::render(ComPtr<ID3D11ShaderResourceView>& sourceView, const postprocess::ShaderFrameInfo& inputFrame,
		ComPtr<ID3D11ShaderResourceView>& outputView, bool& presentationTransformApplied,
		const ComPtr<ID3D11ShaderResourceView>& depthView)
{
	postprocess::ShaderFrameInfo frame = inputFrame;
	const std::array<int,4> currentStencilSize { frame.sourceWidth, frame.sourceHeight, frame.viewportWidth, frame.viewportHeight };
	if (stencilFrameSize != currentStencilSize) { stencilTargets.clear(); stencilFrameSize = currentStencilSize; }

	static_assert(offsetof(ShaderFrameUniforms, parameters) == 48, "DX11 shader parameter layout must match the compiler block");
	static_assert(sizeof(ShaderFrameUniforms) <= Quad::ConstantBufferBytes,
			"DX11 post-processing uniforms must fit the shared quad constant buffer");
	presentationTransformApplied = false;
	bool composedDisplayInput = false;
	if (!updateConfiguredChain() || !sourceView || frame.sourceWidth <= 0 || frame.sourceHeight <= 0)
		return false;
	frame.frameCount = renderedFrames;
	const bool needsDepth = std::any_of(programs.begin(), programs.end(), [](const Program& program) {
		return std::any_of(program.resources.begin(), program.resources.end(), [](const ResourceBinding& resource) {
			return resource.input.kind == postprocess::ShaderInputKind::Depth;
		});
	});
	if (needsDepth && !depthView) {
		postprocess::reportShaderFrameStatus(activePath, "This frame has no scene depth available for the selected shader.");
		return false;
	}
	std::string lookupError;
	if (!ensureLookupTextures(lookupError)) {
		postprocess::reportShaderFrameStatus(activePath, lookupError);
		WARN_LOG(RENDERER, "%s", lookupError.c_str());
		return false;
	}
	// External renderer state is unknown at chain entry. Clear it once; later
	// passes only unbind the slots that HollyCast itself populated.
	unbindPixelShaderSource(D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT);
	ComPtr<ID3D11ShaderResourceView> effectiveDepthView = depthView;
	uint64_t depthTargetBytes = 0;
	bool depthInputRotated = false;
	if (needsDepth) {
		ComPtr<ID3D11Resource> resource;
		ComPtr<ID3D11Texture2D> texture;
		depthView->GetResource(&resource.get());
		if (!resource || FAILED(resource.as(texture))) {
			postprocess::reportShaderFrameStatus(activePath, "Scene depth is not a two-dimensional texture.");
			return false;
		}
		D3D11_TEXTURE2D_DESC description {};
		texture->GetDesc(&description);
		if (description.Width < static_cast<UINT>(frame.sourceWidth) || description.Height < static_cast<UINT>(frame.sourceHeight)) {
			postprocess::reportShaderFrameStatus(activePath, "Scene depth is smaller than the current color frame.");
			return false;
		}
		const bool rotateDepth = frame.rotationPending && std::any_of(programs.begin(), programs.end(),
				[](const Program& program) { return program.pass.composeDisplayInput; });
		if (description.Width != static_cast<UINT>(frame.sourceWidth)
				|| description.Height != static_cast<UINT>(frame.sourceHeight) || rotateDepth) {
			// OIT retains oversized depth allocations. Copy only the current scene
			// rectangle, keeping its float precision and display rotation intact.
			const int width = rotateDepth ? frame.sourceHeight : frame.sourceWidth;
			const int height = rotateDepth ? frame.sourceWidth : frame.sourceHeight;
			std::string error;
			if (!postprocess::accumulateShaderTargetBytes(width, height,
					postprocess::ShaderTargetFormat::Default, {}, false, 1, depthTargetBytes, error)) {
				postprocess::reportShaderFrameStatus(activePath, error);
				return false;
			}
			if (!depthCopyShader) {
				static constexpr char source[] = R"(
Texture2D<float> Source : register(t0);
SamplerState SourceSampler : register(s0);
cbuffer DepthCrop : register(b0) { float2 UVScale; float RotateDepth; float Padding; };
float main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET
{
	float2 coordinates = RotateDepth != 0.0 ? float2(1.0 - uv.y, uv.x) : uv;
	return Source.SampleLevel(SourceSampler, coordinates * UVScale, 0.0);
}
)";
				const pD3DCompile compiler = DX11Context::Instance()->getCompiler();
				ComPtr<ID3DBlob> shaderBlob;
				ComPtr<ID3DBlob> errorBlob;
				if (!compiler || FAILED(compiler(source, sizeof(source) - 1, "DepthCrop", nullptr, nullptr,
						"main", "ps_4_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &shaderBlob.get(), &errorBlob.get()))
						|| FAILED(device->CreatePixelShader(shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize(),
								nullptr, &depthCopyShader.get()))) {
					postprocess::reportShaderFrameStatus(activePath, "DirectX 11 could not compile the scene-depth copy shader.");
					return false;
				}
			}
			if (!ensureTarget(depthInput, width, height, DXGI_FORMAT_R32_FLOAT, false)) {
				postprocess::reportShaderFrameStatus(activePath, "DirectX 11 could not allocate the scene-depth copy target.");
				return false;
			}
			const D3D11_VIEWPORT viewport { 0.f, 0.f, static_cast<float>(width), static_cast<float>(height), 0.f, 1.f };
			const float crop[] = { static_cast<float>(frame.sourceWidth) / description.Width,
					static_cast<float>(frame.sourceHeight) / description.Height, rotateDepth ? 1.f : 0.f, 0.f };
			deviceContext->RSSetViewports(1, &viewport);
			deviceContext->OMSetRenderTargets(1, &depthInput.view.get(), nullptr);
			deviceContext->OMSetBlendState(nullptr, nullptr, 0xffffffff);
			quad->drawWithPixelShader(effectiveDepthView, nearestSampler, depthCopyShader, crop, sizeof(crop));
			unbindPixelShaderSource();
			depthInputRotated = rotateDepth;
			if (!rotateDepth)
				effectiveDepthView = depthInput.shaderView;
		}
		else
			depthInput = {};
	}
	else
		depthInput = {};
	postprocess::ShaderFrameInfo passFrame = frame;
	const uint64_t statusGeneration = postprocess::shaderChainStatusGeneration();
	if (statusGeneration != parameterGeneration) {
		const postprocess::ShaderChainStatus status = postprocess::getShaderChainStatus();
		parameterGeneration = statusGeneration;
		if (status.activePath == activePath && status.error.empty())
			parameters = status.parameters;
	}
	postprocess::updateShaderHostParameters(parameters, frame);
	ComPtr<ID3D11ShaderResourceView> inputView = sourceView;
	ComPtr<ID3D11ShaderResourceView> originalView = sourceView;
	bool inputHasMipChain = false;
	bool displaySpaceReached = false;
	int originalWidth = passFrame.sourceWidth;
	int originalHeight = passFrame.sourceHeight;
	auto composeDisplayInput = [&]() {
		if (displaySpaceReached)
			return true;
		displaySpaceReached = true;
		if (!postprocess::shouldComposeDisplayInput(frame, false))
			return true;
		if (!ensureTarget(displayInput, frame.viewportWidth, frame.viewportHeight, DXGI_FORMAT_B8G8R8A8_UNORM, false))
			return false;
		D3D11_VIEWPORT viewport {};
		viewport.Width = static_cast<float>(frame.viewportWidth);
		viewport.Height = static_cast<float>(frame.viewportHeight);
		viewport.MinDepth = 0.f;
		viewport.MaxDepth = 1.f;
		unbindPixelShaderSource();
		deviceContext->RSSetViewports(1, &viewport);
		deviceContext->OMSetRenderTargets(1, &displayInput.view.get(), nullptr);
		deviceContext->OMSetBlendState(nullptr, nullptr, 0xffffffff);
		quad->draw(inputView, DX11Context::Instance()->getSamplers().getSampler(config::LinearInterpolation),
				nullptr, -1.f, -1.f, 2.f, 2.f, frame.rotationPending);
		unbindPixelShaderSource();
		inputView = displayInput.shaderView;
		originalView = inputView;
		inputHasMipChain = false;
		passFrame.sourceWidth = frame.viewportWidth;
		passFrame.sourceHeight = frame.viewportHeight;
		originalWidth = passFrame.sourceWidth;
		originalHeight = passFrame.sourceHeight;
		if (depthInputRotated)
			effectiveDepthView = depthInput.shaderView;
		composedDisplayInput = true;
		return true;
	};
	if (programs.front().pass.composeDisplayInput && !composeDisplayInput())
		return false;
	if (targets.size() != programs.size())
		targets.resize(programs.size());
	postprocess::ShaderFrameInfo sizeFrame = frame;
	bool sizeDisplaySpaceReached = false;
	uint64_t targetBytes = depthTargetBytes;
	std::string targetBudgetError;
	if (std::any_of(programs.begin(), programs.end(), [](const Program& program) {
			return program.pass.composeDisplayInput;
		}) && postprocess::shouldComposeDisplayInput(frame, false)
			&& !postprocess::accumulateShaderTargetBytes(frame.viewportWidth, frame.viewportHeight,
					postprocess::ShaderTargetFormat::Default, {}, false, 1, targetBytes, targetBudgetError)) {
		postprocess::reportShaderFrameStatus(activePath, targetBudgetError);
		WARN_LOG(RENDERER, "%s", targetBudgetError.c_str());
		return false;
	}
	if (programs.front().pass.useMipmaps
			&& !postprocess::accumulateShaderTargetBytes(passFrame.sourceWidth, passFrame.sourceHeight,
					postprocess::ShaderTargetFormat::Default, {}, true, 1, targetBytes, targetBudgetError)) {
		postprocess::reportShaderFrameStatus(activePath, targetBudgetError);
		WARN_LOG(RENDERER, "%s", targetBudgetError.c_str());
		return false;
	}
	for (size_t index = 0; index < programs.size(); ++index) {
		Program& program = programs[index];
		if (program.pass.composeDisplayInput && !sizeDisplaySpaceReached) {
			sizeDisplaySpaceReached = true;
			if (postprocess::shouldComposeDisplayInput(frame, false)) {
				sizeFrame.sourceWidth = frame.viewportWidth;
				sizeFrame.sourceHeight = frame.viewportHeight;
			}
		}
		std::string error;
		if (!postprocess::resolveTargetSize(program.pass, sizeFrame,
				program.targetWidth, program.targetHeight, error)) {
			postprocess::reportShaderFrameStatus(activePath, error);
			WARN_LOG(RENDERER, "%s", error.c_str());
			return false;
		}
		if (program.pass.updatesSource) { sizeFrame.sourceWidth = program.targetWidth; sizeFrame.sourceHeight = program.targetHeight; }
		const uint32_t outputCount = program.pass.outputs.empty()
				? 1u : static_cast<uint32_t>(program.pass.outputs.size());
		for (uint32_t output = 0; output < outputCount; ++output) {
			const postprocess::ShaderPassOutput config = program.pass.outputs.empty()
					? postprocess::ShaderPassOutput {} : program.pass.outputs[output];
			const postprocess::ShaderTargetFormat format = program.pass.outputs.empty()
					? program.pass.targetFormat : config.targetFormat;
			const std::string& formatName = program.pass.outputs.empty()
					? program.pass.targetFormatName : config.targetFormatName;
			if (!postprocess::accumulateShaderTargetBytes(program.targetWidth, program.targetHeight,
					format, formatName, program.outputNeedsMipmaps[output], program.historyDepth * (program.pass.outputs.empty() ? 1u : program.pass.outputs[output].depth),
					targetBytes, targetBudgetError)) {
				postprocess::reportShaderFrameStatus(activePath, targetBudgetError);
				WARN_LOG(RENDERER, "%s", targetBudgetError.c_str());
				return false;
			}
		}
		if (program.pass.renderState.stencilEnabled
				&& !postprocess::accumulateShaderTargetBytes(program.targetWidth, program.targetHeight,
						postprocess::ShaderTargetFormat::Default, "R32_UINT", false, 1, targetBytes, targetBudgetError)) {
			postprocess::reportShaderFrameStatus(activePath, targetBudgetError); return false;
		}
		PassTargets& passTargets = targets[index];
		if (passTargets.size() != outputCount)
			passTargets.resize(outputCount);
		for (uint32_t output = 0; output < outputCount; ++output) {
			TargetRing& targetRing = passTargets[output];
			if (targetRing.size() != program.historyDepth)
				targetRing.resize(program.historyDepth);
			if (std::any_of(targetRing.begin(), targetRing.end(), [&](Target& target) {
				return !ensureTarget(target, program.targetWidth, program.targetHeight,
						program.outputFormats[output], program.outputNeedsMipmaps[output], output >= program.pass.colorOutputCount,
					program.pass.outputs.empty() ? postprocess::SlangResourceType::Sampler2D : program.pass.outputs[output].type,
					program.pass.outputs.empty() ? 1u : program.pass.outputs[output].depth);
			})) {
				error = "DirectX 11 post-processing target creation failed.";
				postprocess::reportShaderFrameStatus(activePath, error);
				WARN_LOG(RENDERER, "%s", error.c_str());
				return false;
			}
		}
	}
	if (needsSourceHistory) {
		if (!postprocess::accumulateShaderTargetBytes(passFrame.sourceWidth, passFrame.sourceHeight,
				postprocess::ShaderTargetFormat::Default, {}, false, sourceHistoryDepth,
				targetBytes, targetBudgetError)) {
			postprocess::reportShaderFrameStatus(activePath, targetBudgetError);
			WARN_LOG(RENDERER, "%s", targetBudgetError.c_str());
			return false;
		}
		if (sourceHistory.size() != sourceHistoryDepth)
			sourceHistory.resize(sourceHistoryDepth);
		if (std::any_of(sourceHistory.begin(), sourceHistory.end(), [&](Target& target) {
			return !ensureTarget(target, passFrame.sourceWidth, passFrame.sourceHeight, DXGI_FORMAT_B8G8R8A8_UNORM, false);
		})) {
			const std::string error = "DirectX 11 post-processing source history target creation failed.";
			postprocess::reportShaderFrameStatus(activePath, error);
			WARN_LOG(RENDERER, "%s", error.c_str());
			return false;
		}
		D3D11_VIEWPORT historyViewport {};
		historyViewport.Width = static_cast<float>(passFrame.sourceWidth);
		historyViewport.Height = static_cast<float>(passFrame.sourceHeight);
		historyViewport.MinDepth = 0.f;
		historyViewport.MaxDepth = 1.f;
		unbindPixelShaderSource();
		deviceContext->RSSetViewports(1, &historyViewport);
		const size_t currentSourceTarget = frame.frameCount % sourceHistoryDepth;
		deviceContext->OMSetRenderTargets(1, &sourceHistory[currentSourceTarget].view.get(), nullptr);
		deviceContext->OMSetBlendState(nullptr, nullptr, 0xffffffff);
		quad->draw(originalView, DX11Context::Instance()->getSamplers().getSampler(false));
		unbindPixelShaderSource();
		sourceHistory[currentSourceTarget].renderedFrame = frame.frameCount;
		sourceHistory[currentSourceTarget].initialized = true;
	}
	for (size_t index = 0; index < programs.size(); ++index) {
		Program& program = programs[index];
		if (program.pass.composeDisplayInput && !composeDisplayInput())
			return false;
		if (index == 0 || program.pass.layerIndex != programs[index - 1].pass.layerIndex) {
			originalView = inputView;
			originalWidth = passFrame.sourceWidth;
			originalHeight = passFrame.sourceHeight;
		}
		const size_t currentTarget = frame.frameCount % program.historyDepth;
		const int outputWidth = program.targetWidth;
		const int outputHeight = program.targetHeight;
		std::string error;
		const uint32_t outputCount = program.pass.outputs.empty() ? 1u : static_cast<uint32_t>(program.pass.outputs.size());
		PassTargets& passTargets = targets[index];
		Target& target = passTargets[0][currentTarget];
		if (program.pass.useMipmaps && !inputHasMipChain) {
			if (!prepareMipmappedSource(inputView, passFrame.sourceWidth, passFrame.sourceHeight, inputView)) {
				error = "DirectX 11 post-processing mip source creation failed.";
				postprocess::reportShaderFrameStatus(activePath, error);
				WARN_LOG(RENDERER, "%s", error.c_str());
				return false;
			}
			inputHasMipChain = true;
		}
		D3D11_VIEWPORT viewport {};
		viewport.Width = static_cast<float>(outputWidth);
		viewport.Height = static_cast<float>(outputHeight);
		viewport.MinDepth = 0.f;
		viewport.MaxDepth = 1.f;
		deviceContext->RSSetViewports(1, &viewport);
		unbindPixelShaderSource();
		ID3D11DepthStencilView *stencilView = nullptr;
		if (program.pass.renderState.stencilEnabled) {
			auto& stencil = stencilTargets[{ outputWidth, outputHeight }];
			if (!stencil.view) {
				D3D11_TEXTURE2D_DESC desc {}; desc.Width = outputWidth; desc.Height = outputHeight; desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
				desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
				if (FAILED(device->CreateTexture2D(&desc, nullptr, &stencil.texture.get())) || FAILED(device->CreateDepthStencilView(stencil.texture, nullptr, &stencil.view.get()))) {
					postprocess::reportShaderFrameStatus(activePath, "Unable to allocate FX stencil target."); return false;
				}
			}
			if (!stencil.initialized || stencil.frame != frame.frameCount || stencil.layerIndex != program.pass.layerIndex) {
				deviceContext->ClearDepthStencilView(stencil.view, D3D11_CLEAR_STENCIL, 1.f, 0); stencil.frame = frame.frameCount; stencil.layerIndex = program.pass.layerIndex; stencil.initialized = true;
			}
			stencilView = stencil.view;
		}
		std::array<ID3D11RenderTargetView *, 8> renderTargets {};
		for (uint32_t output = 0; output < program.pass.colorOutputCount; ++output)
			renderTargets[output] = passTargets[output][currentTarget].view.get();
		deviceContext->OMSetRenderTargets(program.pass.colorOutputCount, renderTargets.data(), stencilView);
		if ((program.pass.preserveBackbuffer || program.pass.blendEnabled) && program.pass.updatesSource && !program.pass.renderState.clearRenderTargets) {
			// ReShade blends against the current backbuffer. Seed HollyCast's
			// private target before enabling the pass blend state.
			deviceContext->OMSetBlendState(nullptr, nullptr, 0xffffffff);
			quad->draw(inputView, DX11Context::Instance()->getSamplers().getSampler(program.pass.linearFilter));
			unbindPixelShaderSource();
			deviceContext->OMSetRenderTargets(program.pass.colorOutputCount, renderTargets.data(), stencilView);
		}
		// Keep persistent named textures consistent across writers and frame rings.
		deviceContext->OMSetRenderTargets(0, nullptr, nullptr);
		for (uint32_t output = 0; output < outputCount; ++output) {
			auto& destination = passTargets[output][currentTarget];
			if (!program.pass.outputs.empty()) {
				const auto& config = program.pass.outputs[output];
				if (config.previousPass != postprocess::InvalidShaderPass && (!config.previousFrame || frame.frameCount != 0)) {
					const uint32_t requestedFrame = frame.frameCount - (config.previousFrame ? 1u : 0u);
					auto& source = targets[config.previousPass][config.previousOutput][requestedFrame % programs[config.previousPass].historyDepth];
					if (source.initialized && source.renderedFrame == requestedFrame && source.width == destination.width
							&& source.height == destination.height && source.depth == destination.depth && source.type == destination.type && source.format == destination.format)
						deviceContext->CopySubresourceRegion(destination.resource(), 0, 0, 0, 0, source.resource(), 0, nullptr);
				}
			}
			if (program.pass.renderState.clearRenderTargets) {
				if (destination.storage) { const UINT zero[4] {}; deviceContext->ClearUnorderedAccessViewUint(destination.storage, zero); }
				else { const float zero[4] {}; deviceContext->ClearRenderTargetView(destination.view, zero); }
			}
		}
		deviceContext->OMSetRenderTargets(program.pass.colorOutputCount, renderTargets.data(), stencilView);
		deviceContext->OMSetBlendState(program.blendState, nullptr, 0xffffffff);
		ShaderFrameUniforms uniforms {};
		uniforms.sourceSize[0] = static_cast<float>(passFrame.sourceWidth);
		uniforms.sourceSize[1] = static_cast<float>(passFrame.sourceHeight);
		uniforms.outputSize[0] = static_cast<float>(outputWidth);
		uniforms.outputSize[1] = static_cast<float>(outputHeight);
		uniforms.frameCount = static_cast<int>(program.pass.frameCountModulo == 0 ? frame.frameCount : frame.frameCount % program.pass.frameCountModulo);
		uniforms.rotation = frame.rotationPending ? 1 : 0;
		uniforms.originalSize[0] = static_cast<float>(program.displayGlsl ? frame.sourceWidth : originalWidth);
		uniforms.originalSize[1] = static_cast<float>(program.displayGlsl ? frame.sourceHeight : originalHeight);
		uniforms.finalViewportSize[0] = static_cast<float>(frame.viewportWidth);
		uniforms.finalViewportSize[1] = static_cast<float>(frame.viewportHeight);
		uniforms.hasDepth = depthView ? 1 : 0;
		uint32_t parameterOffset = 0;
		for (size_t parameterIndex = 0; parameterIndex < program.parameters.size(); ++parameterIndex) {
			const postprocess::SlangParameter& parameter = program.parameters[parameterIndex];
			const uint32_t components = parameterComponentCount(parameter.type);
			if (parameterOffset + components > std::size(uniforms.parameters)) {
				error = "DirectX 11 shader parameter storage overflow.";
				postprocess::reportShaderFrameStatus(activePath, error);
				WARN_LOG(RENDERER, "%s", error.c_str());
				return false;
			}
			std::copy_n(parameter.defaultValue.values.begin(), components, uniforms.parameters + parameterOffset);
			const uint32_t configuredIndex = program.parameterIndices[parameterIndex];
			if (configuredIndex < parameters.size())
				std::copy_n(parameters[configuredIndex].values.begin(), components, uniforms.parameters + parameterOffset);
			parameterOffset += components;
		}
		for (size_t resourceIndex = 0; resourceIndex < program.resources.size(); ++resourceIndex) {
			const ResourceBinding& binding = program.resources[resourceIndex];
			ComPtr<ID3D11ShaderResourceView> view;
			ComPtr<ID3D11SamplerState> sampler;
			bool resourceHasMipmaps = false;
			if ((binding.input.kind == postprocess::ShaderInputKind::PassOutput
						|| binding.input.kind == postprocess::ShaderInputKind::History
						|| binding.input.kind == postprocess::ShaderInputKind::Feedback)
					&& binding.input.sourcePass != postprocess::InvalidShaderPass
					&& (binding.input.sourcePass >= targets.size()
							|| binding.input.sourceOutput >= targets[binding.input.sourcePass].size())) {
				error = "DirectX 11 shader graph references an invalid pass output.";
				postprocess::reportShaderFrameStatus(activePath, error);
				WARN_LOG(RENDERER, "%s", error.c_str());
				return false;
			}
			switch (binding.input.kind) {
			case postprocess::ShaderInputKind::Source:
				view = inputView;
				resourceHasMipmaps = inputHasMipChain;
				break;
			case postprocess::ShaderInputKind::Original:
				view = originalView;
				break;
			case postprocess::ShaderInputKind::Depth:
				view = effectiveDepthView;
				break;
			case postprocess::ShaderInputKind::PassOutput: {
				const uint32_t sourceDepth = programs[binding.input.sourcePass].historyDepth;
				const Target& sourceTarget = targets[binding.input.sourcePass][binding.input.sourceOutput]
						[frame.frameCount % sourceDepth];
				if (!sourceTarget.initialized || sourceTarget.renderedFrame != frame.frameCount) {
					error = "DirectX 11 shader pass input was not rendered for the current frame.";
					postprocess::reportShaderFrameStatus(activePath, error);
					WARN_LOG(RENDERER, "%s", error.c_str());
					return false;
				}
				view = sourceTarget.shaderView;
				resourceHasMipmaps = programs[binding.input.sourcePass].outputNeedsMipmaps[binding.input.sourceOutput];
				break;
			}
			case postprocess::ShaderInputKind::History:
			case postprocess::ShaderInputKind::Feedback:
			{
				const uint32_t offset = binding.input.kind == postprocess::ShaderInputKind::Feedback ? 1 : binding.input.historyOffset;
				if (frame.frameCount < offset) {
					view = program.nativeFxStages && binding.input.sourcePass != postprocess::InvalidShaderPass
							? targets[binding.input.sourcePass][binding.input.sourceOutput][programs[binding.input.sourcePass].historyDepth - 1].shaderView : originalView;
					break;
				}
				const uint32_t requestedFrame = frame.frameCount - offset;
				if (binding.input.sourcePass == postprocess::InvalidShaderPass) {
					const Target& history = sourceHistory[requestedFrame % sourceHistoryDepth];
					view = history.initialized && history.renderedFrame == requestedFrame
							? history.shaderView : originalView;
					resourceHasMipmaps = false;
				}
				else {
					const uint32_t sourceDepth = programs[binding.input.sourcePass].historyDepth;
					const Target& history = targets[binding.input.sourcePass][binding.input.sourceOutput]
							[requestedFrame % sourceDepth];
					view = history.initialized && history.renderedFrame == requestedFrame
							? history.shaderView : program.nativeFxStages ? history.shaderView : originalView;
					resourceHasMipmaps = history.initialized && history.renderedFrame == requestedFrame
							&& programs[binding.input.sourcePass].outputNeedsMipmaps[binding.input.sourceOutput];
				}
				break;
			}
			case postprocess::ShaderInputKind::LookupTexture:
				if (binding.lookupIndex < lookupTextures.size()) {
					view = lookupTextures[binding.lookupIndex].view;
					sampler = lookupTextures[binding.lookupIndex].sampler;
				}
				break;
			}
			if (!view) {
				error = "DirectX 11 shader resource '" + binding.input.bindingName + "' is unavailable.";
				postprocess::reportShaderFrameStatus(activePath, error);
				WARN_LOG(RENDERER, "%s", error.c_str());
				return false;
			}
			program.resourceViews[resourceIndex] = view;
			if (!sampler && binding.sampler)
				sampler = binding.useMipmaps && resourceHasMipmaps ? binding.mipSampler : binding.sampler;
			program.samplers[resourceIndex] = sampler ? sampler
					: program.pass.linearFilter
							? (resourceHasMipmaps ? mipLinearSampler : linearSampler)
							: (resourceHasMipmaps ? mipNearestSampler : nearestSampler);
		}
		std::vector<ID3D11UnorderedAccessView *> writes;
		for (const auto& binding : program.pass.storageBindings) writes.push_back(passTargets[binding.second][currentTarget].storage);
		if (program.pass.compute) {
			if (!computeConstants) {
				D3D11_BUFFER_DESC desc {}; desc.ByteWidth = sizeof(uniforms); desc.Usage = D3D11_USAGE_DYNAMIC; desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER; desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
				if (FAILED(device->CreateBuffer(&desc, nullptr, &computeConstants.get()))) { postprocess::reportShaderFrameStatus(activePath, "Unable to allocate compute uniforms."); return false; }
			}
			D3D11_MAPPED_SUBRESOURCE mapped {};
			if (FAILED(deviceContext->Map(computeConstants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return false;
			std::memcpy(mapped.pData, &uniforms, sizeof(uniforms)); deviceContext->Unmap(computeConstants, 0);
			deviceContext->CSSetConstantBuffers(0, 1, &computeConstants.get());
			std::vector<ID3D11ShaderResourceView *> views; for (const auto& view : program.resourceViews) views.push_back(view);
			std::vector<ID3D11SamplerState *> samplers; for (const auto& sampler : program.samplers) samplers.push_back(sampler);
			deviceContext->CSSetShaderResources(0, static_cast<UINT>(views.size()), views.data());
			deviceContext->CSSetSamplers(0, static_cast<UINT>(samplers.size()), samplers.data());
			deviceContext->CSSetUnorderedAccessViews(0, static_cast<UINT>(writes.size()), writes.data(), nullptr);
			deviceContext->CSSetShader(program.computeShader, nullptr, 0);
			std::array<uint32_t, 3> groups;
			if (!postprocess::resolveShaderDispatchGroups(program.pass, passFrame, groups, error)) {
				postprocess::reportShaderFrameStatus(activePath, error); return false;
			}
			deviceContext->Dispatch(groups[0], groups[1], groups[2]);
			std::fill(writes.begin(), writes.end(), nullptr);
			deviceContext->CSSetUnorderedAccessViews(0, static_cast<UINT>(writes.size()), writes.data(), nullptr);
			std::fill(views.begin(), views.end(), nullptr); deviceContext->CSSetShaderResources(0, static_cast<UINT>(views.size()), views.data());
			deviceContext->CSSetShader(nullptr, nullptr, 0);
		} else {
			if (!writes.empty()) deviceContext->OMSetRenderTargetsAndUnorderedAccessViews(program.pass.colorOutputCount, renderTargets.data(), stencilView,
					program.pass.colorOutputCount, static_cast<UINT>(writes.size()), writes.data(), nullptr);
			quad->drawWithShaders(program.resourceViews, program.samplers, program.vertexShader.get(), program.pixelShader,
					&uniforms, sizeof(uniforms), program.nativeFxStages ? program.pass.renderState.vertexCount : 4u,
					program.stencilState, program.pass.renderState.stencilReference,
					program.nativeFxStages ? static_cast<D3D11_PRIMITIVE_TOPOLOGY>(program.pass.renderState.topology) : D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP,
					program.nativeFxStages);
			if (!writes.empty()) { std::fill(writes.begin(), writes.end(), nullptr); deviceContext->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr,
					program.pass.colorOutputCount, static_cast<UINT>(writes.size()), writes.data(), nullptr); }
		}

		unbindPixelShaderSource(static_cast<UINT>(program.resourceViews.size()));
		for (uint32_t output = 0; output < outputCount; ++output) {
			if (program.outputNeedsMipmaps[output] && program.pass.renderState.generateMipmaps)
				deviceContext->GenerateMips(passTargets[output][currentTarget].shaderView);
			passTargets[output][currentTarget].renderedFrame = frame.frameCount;
			passTargets[output][currentTarget].initialized = true;
		}
		if (program.pass.updatesSource) {
		inputView = target.shaderView;
		inputHasMipChain = program.outputNeedsMipmaps[0];
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

void Dx11ShaderChain::term()
{
	targets.clear();
	stencilTargets.clear();
	displayInput = {};
	depthInput = {};
	depthCopyShader.reset();
	computeConstants.reset();
	mipmapSource = {};
	sourceHistory.clear();
	programs.clear();
	lookupTextures.clear();
	parameters.clear();
	linearSampler.reset();
	nearestSampler.reset();
	mipLinearSampler.reset();
	mipNearestSampler.reset();
	configuredPaths.clear();
	configuredGeneration = 0;
	parameterGeneration = 0;
	activePath.clear();
	sourceHistoryDepth = 1;
	needsSourceHistory = false;
	quad = nullptr;
	deviceContext.reset();
	device.reset();
	postprocess::reportShaderChainStatus({}, {});
}

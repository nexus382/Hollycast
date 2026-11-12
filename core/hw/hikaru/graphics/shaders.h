// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-3.0-or-later
// Portions Copyright 2026 The Hollycast Authors
#pragma once
#include <string>
#include <cstddef>
#include <array>
#include <cstdint>
namespace hikaru::graphics {
std::string metalShaderSource();
inline constexpr std::size_t MeshShaderSpecializationCount = 25;
struct Draw;
using MeshShaderSpecialization = std::array<int32_t,MeshShaderSpecializationCount>;
MeshShaderSpecialization meshShaderSpecialization(const Draw& draw, bool specialize = true);
// Pass switches are CPU-known and independent of the shared light/material
// specialization. The generic shader oracle leaves all branches dynamic.
enum Dx11PassFlag : unsigned {
    Dx11FamilyEnabled=1u<<0, Dx11FamilyOther=1u<<1, Dx11RawMetadata=1u<<2,
    Dx11PeelEnabled=1u<<3, Dx11PeelAfterFirst=1u<<4,
    Dx11WeighByAlpha=1u<<5, Dx11ForceCoverage=1u<<6, Dx11CommonDepth=1u<<7, Dx11SeparateDepth=1u<<8
};
std::string dx11ShaderSource(bool bitmap, bool familyResolve, bool palette, const MeshShaderSpecialization& specialization, bool packed = false, unsigned passFlags = 0, bool peelComposite = false);
std::string vulkanVertexSource(bool bitmap, bool palette = false);
std::string vulkanFragmentSource(bool bitmap, bool familyResolve = false, bool palette = false, unsigned textureSlots = 1, bool peelComposite = false);
}

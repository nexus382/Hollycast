// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

inline constexpr const char *metalModifierShaderSource = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct U { float4x4 matrix; float4 clip, values; uint4 flags; };
struct V { float4 position [[position]]; float z; };
vertex V modifierVertex(uint i [[vertex_id]], const device packed_float3 *vertices [[buffer(0)]],
                        constant U& u [[buffer(1)]]) {
    float4 p = u.matrix * float4(float3(vertices[i]), 1.0);
    if (u.flags.y) {
        float invZ = 1.0 / p.z;
        return { float4(p.xy * invZ, invZ, invZ), invZ };
    }
    return { float4(p.xy, 0.0, 1.0), p.z };
}
vertex V shadowVertex(uint i [[vertex_id]]) {
    float2 positions[4] = { float2(-1,-1), float2(1,-1), float2(-1,1), float2(1,1) };
    return { float4(positions[i],0,1), 0 };
}
struct F { float4 color [[color(0)]]; float depth [[depth(any)]]; };
fragment F modifierFragment(V in [[stage_in]], constant U& u [[buffer(1)]]) {
    if (u.flags.x && all(in.position.xy >= u.clip.xy) && all(in.position.xy < u.clip.zw))
        discard_fragment();
    float z = u.flags.y ? 1.0 / max(in.z, 1e-20) : in.z;
    return { float4(0,0,0,u.values.x), log2(1.0 + max(100000.0 * z, -0.999999)) / 34.0 };
}
)MSL";

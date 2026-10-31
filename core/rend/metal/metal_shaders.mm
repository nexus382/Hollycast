// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "metal_shaders.h"
#include "metal_reconstruction.h"

// The vertex descriptor defines the byte layout of the shared PVR Vertex.
// Uniforms are float4/uint4 aligned on both sides of the API boundary.
const char *metalRegularShaderSource = R"MSL(
#include <metal_stdlib>
using namespace metal;
)MSL" METAL_RECONSTRUCTION_SOURCE R"MSL(
struct Uniforms {
    float4x4 matrix;
    float4 clip;
    float4 fogColor;
    float4 fogVert;
    float4 fogClampMin;
    float4 fogClampMax;
    float4 values; // alpha reference, trilinear weight, fog density, mip LOD bias
    uint4 flags; // texture, use alpha, ignore texture alpha, shading instruction
    uint4 flags2; // offset, punch-through, clip-inside, fog mode
    uint4 flags3; // color clamp, bump map, palette mode, palette base
    uint4 flags4; // gouraud, reconstruction mode, native depth interpolation
};
struct VertexIn {
    float3 position [[attribute(0)]];
    float4 color [[attribute(1)]];
    float4 offset [[attribute(2)]];
    float2 uv [[attribute(3)]];
};
struct Varyings {
    float4 position [[position]];
    float4 color;
    float4 offset;
    float4 flatColor [[flat]];
    float4 flatOffset [[flat]];
    float3 uv;
};
Varyings pvrMakeVertex(VertexIn in, constant Uniforms& u) {
    Varyings out;
    float4 p = u.matrix * float4(in.position, 1.0);
    if (u.flags4.z) {
        float invZ = 1.0 / p.z;
        out.position = float4(p.xy * invZ, invZ, invZ);
        out.color = in.color;
        out.offset = in.offset;
        out.uv = float3(in.uv, invZ);
    } else {
        out.position = float4(p.xy, 0.0, 1.0);
        out.color = in.color * p.z;
        out.offset = in.offset * p.z;
        out.uv = float3(in.uv * p.z, p.z);
    }
    out.flatColor = in.color;
    out.flatOffset = in.offset;
    return out;
}
vertex Varyings pvr_vertex(VertexIn in [[stage_in]], constant Uniforms& u [[buffer(1)]]) {
    return pvrMakeVertex(in, u);
}
struct FragmentOut { float4 color [[color(0)]]; float depth [[depth(any)]]; };
float4 paletteSample(texture2d<float> tex, sampler samp, texture2d<float> palette, float2 uv, uint base) {
    uint index = uint(round(tex.sample(samp, uv).r * 255.0)) + base;
    return palette.read(uint2(min(index, 1023u), 0));
}
FragmentOut pvrShade(Varyings in, constant Uniforms& u,
        texture2d<float> tex, sampler samp, texture2d<float> palette, texture2d<float> fog) {
    if (u.flags2.z && in.position.x >= u.clip.x && in.position.y >= u.clip.y &&
        in.position.x <= u.clip.z && in.position.y <= u.clip.w) discard_fragment();
    float z = max(in.uv.z, 1e-20);
    float actualZ = u.flags4.z ? 1.0 / z : z;
    float4 color = u.flags4.x ? (u.flags4.z ? in.color : in.color / z) : in.flatColor;
    float4 offset = u.flags4.x ? (u.flags4.z ? in.offset : in.offset / z) : in.flatOffset;
    if (!u.flags.y) color.a = 1.0;
    float fogZ = clamp(u.values.z * actualZ, 1.0, 255.9999);
    float e = floor(log2(fogZ));
    float m = fogZ * 16.0 / exp2(e) - 16.0;
    constexpr sampler fogSampler(coord::normalized, address::clamp_to_edge, filter::linear);
    float fogFactor = fog.sample(fogSampler, float2((floor(m) + e * 16.0 + 0.5) / 128.0, 0.75 - fract(m) / 2.0)).r;
    if (u.flags2.w == 3) color = float4(u.fogColor.rgb, fogFactor);
    if (u.flags.x) {
        float2 uv = u.flags4.z ? in.uv.xy : in.uv.xy / z;
        float4 t = tex.sample(samp, uv, bias(u.values.w));
        if (u.flags4.y != 0)
            t = metalSampleReconstruction(tex, samp, palette, u.flags4.y, u.flags.z != 0,
                    u.flags3.z, u.flags3.w, uv);
        else if (u.flags3.z) {
            if (u.flags3.z == 1) t = paletteSample(tex, samp, palette, uv, u.flags3.w);
            else {
                float2 size = float2(tex.get_width(), tex.get_height());
                float2 pixel = uv * size - 0.5;
                float2 base = (floor(pixel) + 0.5) / size;
                float2 f = fract(pixel);
                float4 a = paletteSample(tex, samp, palette, base, u.flags3.w);
                float4 b = paletteSample(tex, samp, palette, base + float2(1.0 / size.x, 0.0), u.flags3.w);
                float4 c = paletteSample(tex, samp, palette, base + float2(0.0, 1.0 / size.y), u.flags3.w);
                float4 d = paletteSample(tex, samp, palette, base + 1.0 / size, u.flags3.w);
                t = mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
            }
        }
        if (u.flags3.y) {
            float s = M_PI_F / 2.0 * (t.a * 240.0 + t.r * 15.0) / 255.0;
            float r = 2.0 * M_PI_F * (t.g * 240.0 + t.b * 15.0) / 255.0;
            t = float4(1.0, 1.0, 1.0, clamp(offset.a + offset.r * sin(s) + offset.g * cos(s) * cos(r - 2.0 * M_PI_F * offset.b), 0.0, 1.0));
        }
        if (u.flags.z && !u.flags3.y) t.a = 1.0;
        switch (u.flags.w) {
            case 0: color = t; break;
            case 1: color = float4(color.rgb * t.rgb, t.a); break;
            case 2: color.rgb = mix(color.rgb, t.rgb, t.a); break;
            case 3: color *= t; break;
        }
        if (u.flags2.x && !u.flags3.y) color.rgb += offset.rgb;
    }
    if ((u.flags4.w & 1u) != 0) color.rgb *= u.fogVert.w;
    if (u.flags3.x) color = clamp(color, u.fogClampMin, u.fogClampMax);
    if (u.flags2.w == 0) color.rgb = mix(color.rgb, u.fogColor.rgb, fogFactor);
    if (u.flags2.w == 1 && u.flags2.x && !u.flags3.y) color.rgb = mix(color.rgb, u.fogVert.rgb, offset.a);
    color *= u.values.y;
    if (u.flags2.y && round(color.a * 255.0) / 255.0 < u.values.x) discard_fragment();
    FragmentOut out;
    if (u.flags2.y) color.a = 1.0;
    out.color = color;
    out.depth = log2(1.0 + max(100000.0 * actualZ, -0.999999)) / 34.0;
    return out;
}
fragment FragmentOut pvr_fragment(Varyings in [[stage_in]], constant Uniforms& u [[buffer(1)]],
        texture2d<float> tex [[texture(0)]], sampler samp [[sampler(0)]],
        texture2d<float> palette [[texture(1)]], texture2d<float> fog [[texture(2)]]) {
    return pvrShade(in, u, tex, samp, palette, fog);
}
struct MirrorOut { float4 color [[color(0)]]; float depthMirror [[color(1)]]; float depth [[depth(any)]]; };
fragment MirrorOut pvr_mirror(Varyings in [[stage_in]], constant Uniforms& u [[buffer(1)]],
        texture2d<float> tex [[texture(0)]], sampler samp [[sampler(0)]],
        texture2d<float> palette [[texture(1)]], texture2d<float> fog [[texture(2)]]) {
    FragmentOut result = pvrShade(in, u, tex, samp, palette, fog);
    return {result.color, result.depth, result.depth};
}
)MSL";

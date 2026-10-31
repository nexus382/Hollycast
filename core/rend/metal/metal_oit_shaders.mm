// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "metal_oit_shaders.h"

// Fragment lists are consumed only by later encoders on the same queue. The
// append atomics allocate/publish indices; no fragment reads a partial node.
const char *metalOITShaderSource = R"MSL(
#include <metal_stdlib>
using namespace metal;
constant uint OIT_END = 0xffffffffu;
constant uint OIT_SHADOW_STENCIL = 1u;
constant uint OIT_SHADOW_ACC = 2u;
constant uint OIT_SHADOW_ENABLED = 4u;
constant uint OIT_TWO_VOLUMES = 8u;
struct OITNode {
    uint color0;
    uint color1;
    float depth;
    uint next;
    uint sequence;
    uint primitive;
    uint tsp0;
    uint tsp1;
    atomic_uint flags;
};
struct OITControl {
    atomic_uint count;
    atomic_uint overflow;
    atomic_uint layerOverflow;
    uint capacity;
    uint width;
    uint height;
    uint maxLayers;
    float shadowScale;
};
uint oitPack(float4 c) {
    uint4 q = uint4(round(clamp(c, 0.0, 1.0) * 255.0));
    return q.x | (q.y << 8) | (q.z << 16) | (q.w << 24);
}
float4 oitUnpack(uint x) { return float4(x & 255u, (x >> 8) & 255u, (x >> 16) & 255u, x >> 24) / 255.0; }
void oitAppend(device OITNode *nodes, device atomic_uint *heads, device OITControl *ctl,
               uint2 pos, float depth, float4 color0, float4 color1,
               uint sequence, uint primitive, uint tsp0, uint tsp1, uint flags) {
    uint idx = atomic_fetch_add_explicit(&ctl->count, 1u, memory_order_relaxed);
    if (idx >= ctl->capacity) {
        atomic_store_explicit(&ctl->overflow, 1u, memory_order_relaxed);
        return;
    }
    nodes[idx].color0 = oitPack(color0); nodes[idx].color1 = oitPack(color1);
    nodes[idx].depth = depth; nodes[idx].sequence = sequence; nodes[idx].primitive = primitive;
    nodes[idx].tsp0 = tsp0; nodes[idx].tsp1 = tsp1;
    atomic_store_explicit(&nodes[idx].flags, flags, memory_order_relaxed);
    uint old = atomic_exchange_explicit(&heads[pos.y * ctl->width + pos.x], idx, memory_order_relaxed);
    nodes[idx].next = old;
}
// The depth mirror is populated by opaque and punch-through draws.  Do not
// allocate or publish nodes behind it, even if fixed-function depth is late.
void oitAppendVisible(device OITNode *nodes, device atomic_uint *heads, device OITControl *ctl,
                      texture2d<float, access::read> opaqueDepth, uint2 pos, float depth,
                      float4 color0, float4 color1, uint sequence, uint primitive,
                      uint tsp0, uint tsp1, uint flags) {
    if (pos.x >= ctl->width || pos.y >= ctl->height || depth < opaqueDepth.read(pos).r) return;
    oitAppend(nodes, heads, ctl, pos, depth, color0, color1, sequence, primitive, tsp0, tsp1, flags);
}
kernel void oitClear(device atomic_uint *heads [[buffer(0)]], device OITControl *ctl [[buffer(1)]], uint i [[thread_position_in_grid]]) {
    if (i < ctl->width * ctl->height) atomic_store_explicit(&heads[i], OIT_END, memory_order_relaxed);
    if (i == 0) {
        atomic_store_explicit(&ctl->count, 0u, memory_order_relaxed);
        atomic_store_explicit(&ctl->overflow, 0u, memory_order_relaxed);
        atomic_store_explicit(&ctl->layerOverflow, 0u, memory_order_relaxed);
    }
}
bool oitLess(float depthA, uint seqA, uint primA, float depthB, uint seqB, uint primB) {
    return depthA < depthB || (depthA == depthB && (seqA < seqB || (seqA == seqB && primA < primB)));
}
float4 oitFactor(uint mode, float4 src, float4 dst, bool source) {
    switch (mode) {
        case 0: return float4(0);
        case 1: return float4(1);
        case 2: return source ? dst : src;
        case 3: return 1.0 - (source ? dst : src);
        case 4: return float4(src.a);
        case 5: return float4(1.0-src.a);
        case 6: return float4(dst.a);
        default: return float4(1.0-dst.a);
    }
}
kernel void oitResolve(texture2d<float, access::read> input [[texture(0)]],
                       texture2d<float, access::write> output [[texture(1)]],
                       texture2d<float, access::read> secondaryInput [[texture(2)]],
                       texture2d<float, access::write> secondaryOutput [[texture(3)]],
                       device const atomic_uint *heads [[buffer(0)]], device const OITNode *nodes [[buffer(1)]],
                       device OITControl *ctl [[buffer(2)]], uint2 pos [[thread_position_in_grid]]) {
    if (pos.x >= ctl->width || pos.y >= ctl->height) return;
    uint head = atomic_load_explicit(&heads[pos.y * ctl->width + pos.x], memory_order_relaxed);
    float4 primary = input.read(pos), secondary = secondaryInput.read(pos);
    uint count = 0;
    for (uint i = head; i != OIT_END && i < ctl->capacity; i = nodes[i].next) ++count;
    if (count > ctl->maxLayers) atomic_store_explicit(&ctl->layerOverflow, 1u, memory_order_relaxed);
    // Find the threshold for the nearest maxLayers fragments, then resolve them
    // in back-to-front order. Sequence and primitive ID break exact depth ties.
    uint keep = min(count, ctl->maxLayers), threshold = OIT_END;
    bool haveUpper = false; float upperDepth = 0; uint upperSeq = 0, upperPrim = 0;
    for (uint n = 0; n < keep; ++n) {
        uint best = OIT_END;
        for (uint i = head; i != OIT_END && i < ctl->capacity; i = nodes[i].next) {
            const device OITNode &x = nodes[i];
            if (haveUpper && !oitLess(x.depth, x.sequence, x.primitive, upperDepth, upperSeq, upperPrim)) continue;
            if (best == OIT_END || oitLess(nodes[best].depth, nodes[best].sequence, nodes[best].primitive, x.depth, x.sequence, x.primitive)) best = i;
        }
        if (best == OIT_END) break;
        threshold = best; haveUpper = true;
        upperDepth = nodes[best].depth; upperSeq = nodes[best].sequence; upperPrim = nodes[best].primitive;
    }
    bool havePrevious = false; float previousDepth = 0; uint previousSeq = 0, previousPrim = 0;
    for (uint n = 0; n < keep && threshold != OIT_END; ++n) {
        uint best = OIT_END;
        for (uint i = head; i != OIT_END && i < ctl->capacity; i = nodes[i].next) {
            const device OITNode &x = nodes[i];
            if (oitLess(x.depth, x.sequence, x.primitive, nodes[threshold].depth, nodes[threshold].sequence, nodes[threshold].primitive)) continue;
            if (havePrevious && !oitLess(previousDepth, previousSeq, previousPrim, x.depth, x.sequence, x.primitive)) continue;
            if (best == OIT_END || oitLess(x.depth, x.sequence, x.primitive, nodes[best].depth, nodes[best].sequence, nodes[best].primitive)) best = i;
        }
        if (best == OIT_END) break;
        const device OITNode &x = nodes[best];
        uint flags = atomic_load_explicit(&x.flags, memory_order_relaxed);
        bool shadowed = (flags & OIT_SHADOW_ACC) != 0;
        bool area1 = shadowed && (flags & OIT_TWO_VOLUMES) != 0;
        uint tsp = area1 ? x.tsp1 : x.tsp0;
        float4 src = (tsp & (1u << 25)) != 0 ? secondary : oitUnpack(area1 ? x.color1 : x.color0);
        if (shadowed && !area1 && (tsp & (1u << 25)) == 0) src.rgb *= ctl->shadowScale;
        float4 dst = (tsp & (1u << 24)) != 0 ? secondary : primary;
        float4 value = clamp(src * oitFactor((tsp >> 29) & 7u, src, dst, true) + dst * oitFactor((tsp >> 26) & 7u, src, dst, false), 0.0, 1.0);
        if ((tsp & (1u << 24)) != 0) secondary = value; else primary = value;
        havePrevious = true; previousDepth = x.depth; previousSeq = x.sequence; previousPrim = x.primitive;
    }
    output.write(primary, pos);
    secondaryOutput.write(secondary, pos);
}
)MSL";

const char *metalOITCollectShaderSource = R"MSL(
struct OITDraw { uint sequence, tsp0, tsp1, flags; };
struct OITVertexIn {
    float3 position [[attribute(0)]]; float4 color [[attribute(1)]]; float4 offset [[attribute(2)]]; float2 uv [[attribute(3)]];
    float4 color1 [[attribute(4)]]; float4 offset1 [[attribute(5)]]; float2 uv1 [[attribute(6)]];
};
struct OITVaryings {
    float4 position [[position]]; float4 color; float4 offset; float4 flatColor [[flat]]; float4 flatOffset [[flat]]; float3 uv;
    float4 color1; float4 offset1; float4 flatColor1 [[flat]]; float4 flatOffset1 [[flat]]; float2 uv1;
};
vertex OITVaryings pvr_oit_vertex(OITVertexIn input [[stage_in]], constant Uniforms& u [[buffer(1)]]) {
    VertexIn basic; basic.position=input.position; basic.color=input.color; basic.offset=input.offset; basic.uv=input.uv;
    Varyings v=pvrMakeVertex(basic,u); OITVaryings out;
    out.position=v.position; out.color=v.color; out.offset=v.offset; out.flatColor=v.flatColor; out.flatOffset=v.flatOffset; out.uv=v.uv;
    out.color1=u.flags4.z ? input.color1 : input.color1*v.uv.z;
    out.offset1=u.flags4.z ? input.offset1 : input.offset1*v.uv.z;
    out.flatColor1=input.color1; out.flatOffset1=input.offset1;
    out.uv1=u.flags4.z ? input.uv1 : input.uv1*v.uv.z;
    return out;
}
FragmentOut oitShadeColor(OITVaryings in, constant Uniforms& u, texture2d<float> tex,
        texture2d<float> palette, texture2d<float> fog, texture2d<float, access::read> secondary, sampler samp) {
    Varyings base; base.position=in.position; base.color=in.color; base.offset=in.offset; base.flatColor=in.flatColor; base.flatOffset=in.flatOffset; base.uv=in.uv;
    if ((u.flags4.w & 2u) != 0) {
        base.color=in.color1; base.offset=in.offset1; base.flatColor=in.flatColor1; base.flatOffset=in.flatOffset1; base.uv=float3(in.uv1,in.uv.z);
    }
    if ((u.flags4.w & 4u) != 0) {
        if (u.flags2.z && in.position.x >= u.clip.x && in.position.y >= u.clip.y &&
                in.position.x <= u.clip.z && in.position.y <= u.clip.w) discard_fragment();
        float z=max(in.uv.z,1e-20); float actualZ=u.flags4.z ? 1.0 / z : z;
        return {secondary.read(uint2(in.position.xy)), log2(1.0 + max(100000.0 * actualZ, -0.999999)) / 34.0};
    }
    return pvrShade(base,u,tex,samp,palette,fog);
}
fragment FragmentOut pvr_oit_color(OITVaryings in [[stage_in]], constant Uniforms& u [[buffer(1)]],
        texture2d<float> tex [[texture(0)]], texture2d<float> palette [[texture(1)]], texture2d<float> fog [[texture(2)]],
        texture2d<float, access::read> secondary [[texture(3)]], sampler samp [[sampler(0)]]) {
    return oitShadeColor(in,u,tex,palette,fog,secondary,samp);
}
fragment MirrorOut pvr_oit_color_mirror(OITVaryings in [[stage_in]], constant Uniforms& u [[buffer(1)]],
        texture2d<float> tex [[texture(0)]], texture2d<float> palette [[texture(1)]], texture2d<float> fog [[texture(2)]],
        texture2d<float, access::read> secondary [[texture(3)]], sampler samp [[sampler(0)]]) {
    FragmentOut result=oitShadeColor(in,u,tex,palette,fog,secondary,samp);
    return {result.color,result.depth,result.depth};
}
fragment void pvr_collect(OITVaryings in [[stage_in]], uint primitive [[primitive_id]],
        constant Uniforms& u [[buffer(1)]], constant OITDraw& draw [[buffer(2)]],
        device OITNode *nodes [[buffer(3)]], device atomic_uint *heads [[buffer(4)]],
        device OITControl *ctl [[buffer(5)]], constant Uniforms& u1 [[buffer(6)]],
        texture2d<float> tex [[texture(0)]], texture2d<float> palette [[texture(1)]], texture2d<float> fog [[texture(2)]],
        texture2d<float, access::read> opaqueDepth [[texture(3)]], texture2d<float> tex1 [[texture(4)]],
        sampler samp [[sampler(0)]], sampler samp1 [[sampler(1)]]) {
    Varyings base; base.position=in.position; base.color=in.color; base.offset=in.offset; base.flatColor=in.flatColor; base.flatOffset=in.flatOffset; base.uv=in.uv;
    FragmentOut color = pvrShade(base, u, tex, samp, palette, fog);
    float4 area1=color.color;
    if ((draw.flags & OIT_TWO_VOLUMES) != 0) {
        base.color=in.color1; base.offset=in.offset1; base.flatColor=in.flatColor1; base.flatOffset=in.flatOffset1;
        base.uv=float3(in.uv1,in.uv.z);
        area1=pvrShade(base,u1,tex1,samp1,palette,fog).color;
    }
    oitAppendVisible(nodes, heads, ctl, opaqueDepth, uint2(in.position.xy), color.depth,
            color.color, area1, draw.sequence, primitive, draw.tsp0, draw.tsp1, draw.flags);
}
)MSL";

const char *metalOITModifierShaderSource = R"MSL(
fragment void oitModifier(V in [[stage_in]], constant U& u [[buffer(1)]], constant uint& mode [[buffer(2)]],
        device OITNode *nodes [[buffer(3)]], device atomic_uint *heads [[buffer(4)]], device OITControl *ctl [[buffer(5)]]) {
    if (u.flags.x && all(in.position.xy >= u.clip.xy) && all(in.position.xy < u.clip.zw)) discard_fragment();
    uint2 pos = uint2(in.position.xy);
    if (pos.x >= ctl->width || pos.y >= ctl->height) return;
    float z = u.flags.y ? 1.0 / max(in.z, 1e-20) : in.z;
    float depth = log2(1.0 + max(100000.0 * z, -0.999999)) / 34.0;
    for (uint i = atomic_load_explicit(&heads[pos.y * ctl->width + pos.x], memory_order_relaxed);
         i != OIT_END && i < ctl->capacity; i = nodes[i].next) {
        device atomic_uint *flags = &nodes[i].flags;
        uint old = atomic_load_explicit(flags, memory_order_relaxed);
        if ((old & OIT_SHADOW_ENABLED) == 0) continue;
        if (mode == 0 && depth >= nodes[i].depth) atomic_fetch_xor_explicit(flags, OIT_SHADOW_STENCIL, memory_order_relaxed);
        else if (mode == 1 && depth >= nodes[i].depth) atomic_fetch_or_explicit(flags, OIT_SHADOW_STENCIL, memory_order_relaxed);
        else if (mode >= 2) {
            // Each summary operation is atomic even when geometry overlaps.
            uint desired;
            do {
                uint bits = old & (OIT_SHADOW_STENCIL | OIT_SHADOW_ACC);
                bool inside = mode == 2 ? bits != 0 : bits == OIT_SHADOW_ACC;
                desired = (old & ~(OIT_SHADOW_STENCIL | OIT_SHADOW_ACC)) | (inside ? OIT_SHADOW_ACC : 0u);
            } while (!atomic_compare_exchange_weak_explicit(flags, &old, desired, memory_order_relaxed, memory_order_relaxed));
        }
    }
}
)MSL";

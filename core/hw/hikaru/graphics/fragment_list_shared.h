// Portions Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stddef.h>
#include <stdint.h>

/* CPU parameter ABI and pool policy shared by Vulkan, DX11 and OpenGL. */
typedef struct HikaruFragmentListParameters {
    uint32_t width, height, capacity, orderBase;
    float colorLevels, alphaLevels;
    uint32_t blockCapacity;
} HikaruFragmentListParameters;

typedef struct HikaruFragmentPoolPolicy {
    uint32_t hint, peak;
    unsigned samples;
} HikaruFragmentPoolPolicy;

#if defined(__cplusplus)
static_assert(sizeof(HikaruFragmentListParameters) == 28, "Hikaru fragment-list parameter ABI changed");
static_assert(offsetof(HikaruFragmentListParameters, height) == 4 &&
              offsetof(HikaruFragmentListParameters, capacity) == 8 &&
              offsetof(HikaruFragmentListParameters, orderBase) == 12 &&
              offsetof(HikaruFragmentListParameters, colorLevels) == 16 &&
              offsetof(HikaruFragmentListParameters, alphaLevels) == 20 &&
              offsetof(HikaruFragmentListParameters, blockCapacity) == 24,
              "Hikaru fragment-list parameter offsets changed");
#else
typedef char HikaruFragmentListParameters_size_must_be_28[
    sizeof(HikaruFragmentListParameters) == 28 ? 1 : -1];
typedef char HikaruFragmentListParameters_offsets_must_match_std430[
    offsetof(HikaruFragmentListParameters, height) == 4 &&
    offsetof(HikaruFragmentListParameters, capacity) == 8 &&
    offsetof(HikaruFragmentListParameters, orderBase) == 12 &&
    offsetof(HikaruFragmentListParameters, colorLevels) == 16 &&
    offsetof(HikaruFragmentListParameters, alphaLevels) == 20 &&
    offsetof(HikaruFragmentListParameters, blockCapacity) == 24 ? 1 : -1];
#endif

static inline void hikaru_fragment_pool_reset_window(HikaruFragmentPoolPolicy *policy) {
    policy->peak = 0;
    policy->samples = 0;
}

static inline void hikaru_fragment_pool_grow(HikaruFragmentPoolPolicy *policy,
                                              uint32_t used, uint32_t capacity) {
    const uint64_t doubled = (uint64_t)capacity * 2;
    const uint64_t observed = (uint64_t)used + used / 2;
    uint64_t desired = doubled > observed ? doubled : observed;
    if (desired < 4096) desired = 4096;
    if (desired > UINT32_MAX) desired = UINT32_MAX;
    policy->hint = (uint32_t)desired;
    hikaru_fragment_pool_reset_window(policy);
}

static inline void hikaru_fragment_pool_observe(HikaruFragmentPoolPolicy *policy,
                                                 uint32_t used, uint32_t capacity) {
    /* Grow before exhaustion. Shrinking needs a full observation window and
     * at least 25% savings, with 50% usage headroom. */
    if ((uint64_t)used * 4 > (uint64_t)capacity * 3) {
        hikaru_fragment_pool_grow(policy, used, capacity);
        return;
    }
    if (used > policy->peak) policy->peak = used;
    if (++policy->samples < 120) return;
    uint64_t desired = (uint64_t)policy->peak + policy->peak / 2;
    if (desired < 4096) desired = 4096;
    if (desired * 4 <= (uint64_t)capacity * 3) policy->hint = (uint32_t)desired;
    hikaru_fragment_pool_reset_window(policy);
}

/* The GLSL node ABI is uvec4: packed UNORM color, D24 plus blend mode,
 * stable primitive order, and one-based next link (zero means empty).
 * RGBA8 keeps its original encoding. RGB10 targets retain eight-bit source
 * alpha: its low two bits share the RGB word and its upper six bits occupy
 * the unused metadata bits above the two-bit blend mode. This preserves
 * precision without growing nodes or reducing the memory-budget capacity.
 * API-specific storage qualifiers and helper guards precede these chunks. */
#define HIKARU_FRAGMENT_LIST_TYPES_SOURCE \
    "struct HikaruListParameters { uint width,height,capacity,orderBase; float colorLevels,alphaLevels; uint blockCapacity; };\n"

#define HIKARU_FRAGMENT_LIST_BUFFERS_SOURCE \
    "LIST_HEADS_LAYOUT buffer ListHeads { uint listHeads[]; };\n" \
    "LIST_NODES_LAYOUT buffer ListNodes { uvec4 nodes[]; } listPools[4];\n" \
    "LIST_STATUS_LAYOUT buffer ListStatus { uint listCount; uint listOverflow; };\n" \
    "uvec4 readListNode(uint index) { uint block=index/listParams.blockCapacity,offset=index%listParams.blockCapacity; " \
    "if(block==0u)return listPools[0].nodes[offset]; if(block==1u)return listPools[1].nodes[offset]; " \
    "if(block==2u)return listPools[2].nodes[offset]; return listPools[3].nodes[offset]; }\n" \
    "void writeListNode(uint index,uvec4 value) { uint block=index/listParams.blockCapacity,offset=index%listParams.blockCapacity; " \
    "if(block==0u)listPools[0].nodes[offset]=value; else if(block==1u)listPools[1].nodes[offset]=value; " \
    "else if(block==2u)listPools[2].nodes[offset]=value; else listPools[3].nodes[offset]=value; }\n" \
    "void writeListNext(uint index,uint next) { uint block=index/listParams.blockCapacity,offset=index%listParams.blockCapacity; " \
    "if(block==0u)listPools[0].nodes[offset].w=next; else if(block==1u)listPools[1].nodes[offset].w=next; " \
    "else if(block==2u)listPools[2].nodes[offset].w=next; else listPools[3].nodes[offset].w=next; }\n"

#define HIKARU_FRAGMENT_LIST_COLLECT_SOURCE \
    "void collectListFragment(vec4 color,float depthCode,uint blendMode) {\n" \
    "LIST_HELPER_GUARD\n" \
    "uvec2 pixel=uvec2(gl_FragCoord.xy); if(pixel.x>=listParams.width||pixel.y>=listParams.height)return;\n" \
    "uint index=atomicAdd(listCount,1u); if(index>=listParams.capacity){atomicOr(listOverflow,1u);return;}\n" \
    "uint previous=atomicExchange(listHeads[pixel.y*listParams.width+pixel.x],index+1u);\n" \
    "vec4 source=clamp(color,0.0,1.0); uint packedColor; " \
    "uint packedDepth=(uint(depthCode)&0xffffffu)|((blendMode&3u)<<24u);\n" \
    "if(listParams.colorLevels>255.0) { uvec3 rgb=uvec3(roundEven(source.rgb*1023.0)); " \
    "uint alpha=uint(roundEven(source.a*255.0)); " \
    "packedColor=rgb.r|(rgb.g<<10u)|(rgb.b<<20u)|((alpha&3u)<<30u); " \
    "packedDepth|=(alpha>>2u)<<26u; } else packedColor=packUnorm4x8(source);\n" \
    "writeListNode(index,uvec4(packedColor,packedDepth,\n" \
    "listParams.orderBase+LIST_PRIMITIVE_ID,previous));\n" \
    "}\n"

#define HIKARU_FRAGMENT_LIST_RESOLVE_HEAD_SOURCE \
    "bool fragmentBefore(uvec4 first,uvec4 second) { uint z1=first.y&0xffffffu,z2=second.y&0xffffffu; " \
    "return z1>z2||(z1==z2&&first.z<second.z); }\n" \
    "bool earlier(uint a,uint b) { return fragmentBefore(readListNode(a-1u),readListNode(b-1u)); }\n" \
    "vec4 blendListFragment(vec4 destination,uvec4 node) { vec4 source; " \
    "if(listParams.colorLevels>255.0) { uvec3 rgb=uvec3(node.x,node.x>>10u,node.x>>20u)&uvec3(1023u); " \
    "uint alpha=(node.x>>30u)|((node.y>>26u)<<2u); " \
    "source=vec4(vec3(rgb)/1023.0,float(alpha)/255.0); } else source=unpackUnorm4x8(node.x); " \
    "uint mode=(node.y>>24u)&3u; " \
    "precise vec4 result; if(mode==1u)result=vec4(source.rgb*destination.rgb,source.a); else { " \
    "vec3 contribution=(mode==3u?source.rgb:source.rgb*source.a); " \
    "result=vec4(contribution+destination.rgb*(mode==2u?1.0:1.0-source.a),source.a+destination.a*(1.0-source.a)); } " \
    "vec4 levels=vec4(listParams.colorLevels,listParams.colorLevels,listParams.colorLevels,listParams.alphaLevels); " \
    "return roundEven(clamp(result,0.0,1.0)*levels)/levels; }\n" \
    "void main() { uvec2 pixel=uvec2(gl_FragCoord.xy); uint head=listHeads[pixel.y*listParams.width+pixel.x]; " \
    "if(head==0u)discard;\n"

#define HIKARU_FRAGMENT_LIST_RESOLVE_SMALL_SOURCE \
    "uvec4 localFragments[8]; uint localCount=0u,link=head;\n" \
    "while(link!=0u&&localCount<8u) { uvec4 node=readListNode(link-1u); uint place=localCount; " \
    "while(place>0u&&fragmentBefore(node,localFragments[place-1u])){localFragments[place]=localFragments[place-1u];--place;} " \
    "localFragments[place]=node;++localCount;link=node.w; }\n" \
    "if(link==0u) { precise vec4 localColor=texelFetch(listBackground,ivec2(pixel),0); " \
    "for(uint i=0u;i<localCount;++i)localColor=blendListFragment(localColor,localFragments[i]); " \
    "outColor=localColor;return; }\n"

#define HIKARU_FRAGMENT_LIST_RESOLVE_TAIL_SOURCE \
    "uint run=1u,sorted=head; for(;;) { uint cursor=sorted,tail=0u,merges=0u; sorted=0u; " \
    "while(cursor!=0u) { ++merges; uint left=cursor,right=cursor,leftCount=0u,rightCount=run; " \
    "for(uint i=0u;i<run&&right!=0u;++i){right=readListNode(right-1u).w;++leftCount;} " \
    "while(leftCount!=0u||(rightCount!=0u&&right!=0u)) { uint chosen; " \
    "if(leftCount!=0u&&(rightCount==0u||right==0u||earlier(left,right))){chosen=left;left=readListNode(left-1u).w;--leftCount;} " \
    "else {chosen=right;right=readListNode(right-1u).w;--rightCount;} " \
    "if(tail==0u)sorted=chosen;else writeListNext(tail-1u,chosen);tail=chosen;} cursor=right;} " \
    "writeListNext(tail-1u,0u);if(merges<=1u)break;run*=2u;}\n" \
    "precise vec4 result=texelFetch(listBackground,ivec2(pixel),0); " \
    "while(sorted!=0u){uvec4 node=readListNode(sorted-1u);result=blendListFragment(result,node);sorted=node.w;} " \
    "outColor=result; }\n"

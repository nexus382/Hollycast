// Copyright 2026 The Hollycast Authors
// Portions Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "shaders.h"
namespace hikaru::graphics {
namespace {
const char* structures=R"(
struct Light { vec4 position,direction,diffuse,specular,extents,spot,flags; vec4 table[4]; };
struct Uniforms {
    mat4 projection,modelview,alternate,normal,alternateNormal;
    vec4 ambient,fog,fogColor,flags,material,peel,depth,family,blend;
    Light lights[4];
    vec4 factors,multipliers,direct,keyed,inverted,lodBias;
};
)";
const char* vertexBody=R"(
    int sel=int(v.extra.y+0.5);
    mat4 mv=(sel&1)!=0?u.alternate:u.modelview;
    mat4 nm=(sel&1)!=0?u.alternateNormal:u.normal;
    vec4 pos;
    if(sel==0||sel==1) pos=mv*vec4(v.position.xyz,1.0);
    else {
        vec3 xyz=(sel&64)==0?mat3of(mv)*v.position.xyz:v.position.xyz;
        if((sel&128)==0)xyz+=mv[3].xyz;
        pos=vec4(xyz,1.0);
    }
    o.viewPosition=pos;
    o.normal=(sel!=0&&sel!=1&&(sel&64)!=0)?v.normal.xyz:(mat3of(nm)*v.normal.xyz);
    o.diffuse=v.diffuse.xyz;o.ambient=v.ambient.xyz;o.specular=v.specular;
    o.unknown=v.unknown.xyz;o.uv=v.uv.xy;o.alpha=v.extra.x;
    o.position=u.projection*pos;
)";
const char* lightBody=R"(
    vec3 lightDirection;
    float distance;
    if(light.flags.y==0.0){lightDirection=normalize(light.direction.xyz);distance=0.001;}
    else {vec3 delta=light.position.xyz-o.viewPosition.xyz;distance=length(delta);lightDirection=normalize(delta);}
    if(light.flags.z==1.0)distance*=distance;
    else if(light.flags.z==2.0)distance=1.0/distance;
    else if(light.flags.z==3.0)distance=1.0/(distance*distance);
    float attenuation=clamp(light.extents.x*(light.extents.y+distance),0.0,1.0);
    vec3 normal=normalize(o.normal);
    float intensity=u.flags.z!=0.0?abs(dot(normal,lightDirection)):max(dot(normal,lightDirection),0.0);
    if(light.flags.y==2.0&&light.spot.z!=0.0){
        float unit=clamp((dot(normalize(light.direction.xyz),-lightDirection)-light.spot.x)*light.spot.y,0.0,1.0);
        float t=unit*16.0;int index=clamp(int(floor(t)),0,15);
        int lower=max(index-1,0);
        float lo=index==0?0.0:light.table[lower/4][lower%4];
        float hi=light.table[index/4][index%4];
        attenuation*=mix(lo,hi,t-float(index));
    }
    diffuse+=attenuation*(intensity+light.extents.z)*light.diffuse.xyz;
    if(light.flags.w!=0.0){
        vec3 viewDirection=normalize(-o.viewPosition.xyz);
        float angle=max(dot(viewDirection,reflect(-lightDirection,normal)),0.0);
        specular+=o.specular.xyz*light.specular.xyz*pow(angle,255.0*o.specular.w);
    }
)";
const char* meshFragmentBody=R"(
    // Mask coverage before either family depth or metadata can be written.
    uint2 patternPixel=uint2(o.position.xy);
    uint patternShift=(3u-(patternPixel.y&3u))*4u+(patternPixel.x&3u);
    if(((uint(u.family.w+0.5)>>patternShift)&1u)==0u)DISCARD;
    float eyeZ=max(u.depth.x,-o.viewPosition.z);
    float placementDepth=meshWindowDepth(max(u.depth.x,eyeZ*u.depth.y),u);
    float placementCode=ROUND_EVEN(placementDepth*16777215.0);
    float writtenDepth=placementDepth;
    if(u.material.w>0.0&&u.material.w!=u.depth.y)
        writtenDepth=meshWindowDepth(max(u.depth.x,eyeZ*u.material.w),u);
    float depthCode=ROUND_EVEN(writtenDepth*16777215.0);
    WRITE_DEPTH(writtenDepth);
    float rawCode=0.0;
    if(u.family.y!=0.0||u.family.x>1.0)
        rawCode=u.depth.y==1.0?placementCode:ROUND_EVEN(meshWindowDepth(eyeZ,u)*16777215.0);
    WRITE_METADATA(rawCode,u.family.z);
    if(u.family.x!=0.0){
        vec2 coordinate=o.position.xy/u.peel.zw;
        if(placementCode>ROUND_EVEN(DEPTH2(coordinate)*16777215.0))DISCARD;
        if(u.family.x>1.0){
            uint2 other=META3(o.position.xy);
            if(other.y!=0u&&uint(rawCode)>other.x)DISCARD;
        }
    }
    if(u.peel.x!=0.0){
        vec2 coordinate=o.position.xy/u.peel.zw;
        float opaqueCode=ROUND_EVEN(DEPTH2(coordinate)*16777215.0);
        if(u.family.x==0.0&&depthCode>opaqueCode)DISCARD;
        if(u.peel.y==0.0&&depthCode<=ROUND_EVEN(DEPTH1(coordinate)*16777215.0))DISCARD;
    }
    // Keep the material ordering identical to the repo's production GL
    // shader: 2C1/881 choose the base, 691 supplies ambient or emission.
    vec4 texel=u.flags.x!=0.0?SAMPLE0(o.uv):vec4(1.0);
    vec3 base;float alpha;
    if(u.flags.x!=0.0){
        if(u.material.x!=0.0){base=mix(o.diffuse,texel.xyz,o.alpha);alpha=texel.w;}
        else if(u.material.y==1.0){base=mix(o.diffuse,texel.xyz,texel.w);alpha=o.alpha;}
        else if(u.material.y==2.0){base=texel.xyz*o.ambient;alpha=texel.w*o.alpha;}
        else if(u.material.y==3.0){base=o.diffuse;alpha=texel.w*o.alpha;}
        else if(u.material.y==4.0){base=mix(o.diffuse,o.ambient,texel.xyz);alpha=texel.w*o.alpha;}
        else{base=texel.xyz;alpha=texel.w*o.alpha;}
    }else if(u.material.x!=0.0){base=mix(o.diffuse,o.ambient,o.alpha);alpha=1.0;}
    else{base=o.diffuse;alpha=o.alpha;}
    vec4 color;
    if(u.flags.y!=0.0){
        vec3 diffuse=vec3(0.0),specular=vec3(0.0);
        vec3 ambient=u.flags.y==1.0?u.ambient.xyz*o.unknown*257.0:vec3(0.0);
        vec3 emission=u.flags.y==2.0?o.unknown*257.0:vec3(0.0);
        for(int i=0;i<4;i++)if(u.lights[i].flags.x!=0.0)applyLight(diffuse,specular,u.lights[i],u,o);
        color=vec4((ambient+diffuse+emission)*base,alpha);
        if(u.flags.y!=3.0)color.xyz+=specular;
    }else color=vec4(base,alpha);
    if(u.flags.w!=0.0){float z=-o.viewPosition.z;float a=clamp(u.fog.x*(z-u.fog.y),0.0,1.0);color=mix(color,vec4(u.fogColor.xyz,color.a),a);}
    if(u.blend.x!=0.0)color.xyz*=color.a;
    if(color.a<=u.material.z)DISCARD;
    if(u.blend.y!=0.0)color.a=1.0;
    RETURN_COLOR(color);
)";
const char* bitmapBody=R"(
    vec2 inv=vec2(o.uv.x,1.0-o.uv.y);
    vec4 front=SAMPLE0(u.inverted.x==0.0?o.uv:inv)*u.factors.y;
    vec4 back=SAMPLE1(u.inverted.y==0.0?o.uv:inv)*u.factors.z;
    vec4 layer1=SAMPLE2(inv),layer2=SAMPLE3(inv);
    vec4 scene=mix(front,back,u.factors.x);
    // Bitmap planes sit either behind the 3D picture or in front of it.
    // A keyed plane covers only where its alpha key is set; an unkeyed
    // plane covers everywhere. Behind-planes show only where the 3D
    // picture drew nothing, so a drawn pixel covers them in full.
    float key1=u.keyed.x!=0.0?step(0.1,layer1.a):1.0;
    float key2=u.keyed.y!=0.0?step(0.1,layer2.a):1.0;
    bool hasBehind=(u.multipliers.x!=0.0&&u.direct.x==0.0)||(u.multipliers.y!=0.0&&u.direct.y==0.0);
    vec4 color=vec4(0.0);
    if(u.multipliers.y!=0.0&&u.direct.y==0.0)color=mix(color,vec4(layer2.xyz*u.multipliers.y,1.0),key2);
    if(u.multipliers.x!=0.0&&u.direct.x==0.0)color=mix(color,vec4(layer1.xyz*u.multipliers.x,1.0),key1);
    if(hasBehind)color=scene+color*(1.0-scene.a);
    else color=scene;
    if(u.multipliers.y!=0.0&&u.direct.y!=0.0)color=mix(color,vec4(layer2.xyz*u.multipliers.y,1.0),key2);
    if(u.multipliers.x!=0.0&&u.direct.x!=0.0)color=mix(color,vec4(layer1.xyz*u.multipliers.x,1.0),key1);
    RETURN_COLOR(color);
)";
const char* metalPrelude=R"(
#include <metal_stdlib>
using namespace metal;
typedef float2 vec2;typedef float3 vec3;typedef float4 vec4;
typedef float3x3 mat3;typedef float4x4 mat4;
mat3 mat3of(mat4 m){return mat3(m[0].xyz,m[1].xyz,m[2].xyz);}
struct Vertex { vec4 position,normal,diffuse,ambient,specular,unknown,uv,extra; };
struct Output {
    vec4 position [[position]]; vec4 viewPosition; vec3 normal,diffuse,ambient;vec4 specular;
    vec3 unknown;vec2 uv;float alpha;
};
#define SAMPLE0(uv) tex0.sample(samp0,uv,bias(u.lodBias.x))
#define SAMPLE1(uv) tex1.sample(samp1,uv,bias(u.lodBias.y))
#define SAMPLE2(uv) tex2.sample(samp2,uv,bias(u.lodBias.z))
#define SAMPLE3(uv) tex3.sample(samp3,uv,bias(u.lodBias.w))
#define DEPTH1(uv) tex1.sample(samp1,uv)
#define DEPTH2(uv) tex2.sample(samp2,uv)
#define DISCARD discard_fragment()
#define RETURN_COLOR(color) return color
#define ROUND_EVEN(value) rint(value)
#define WRITE_DEPTH(value) result.depth=value
#define WRITE_METADATA(code,order) result.metadata=uint2(uint(code),uint(order+0.5))
#define COLOR0(pixel) tex0.read(uint2(pixel))
#define COLOR1(pixel) tex1.read(uint2(pixel))
#define META2(pixel) tex2.read(uint2(pixel)).xy
#define META3(pixel) tex3.read(uint2(pixel)).xy
)";
const char* glslPrelude=R"(#version 450
#define SAMPLE0(uv) texture(tex0,uv,u.lodBias.x)
#define SAMPLE1(uv) texture(tex1,uv,u.lodBias.y)
#define SAMPLE2(uv) texture(tex2,uv,u.lodBias.z)
#define SAMPLE3(uv) texture(tex3,uv,u.lodBias.w)
#define DEPTH1(uv) texture(tex1,uv).r
#define DEPTH2(uv) texture(tex2,uv).r
#define DISCARD discard
#define RETURN_COLOR(color) outColor=color;return
#define ROUND_EVEN(value) roundEven(value)
#define WRITE_DEPTH(value) gl_FragDepth=value
#define WRITE_METADATA(code,order) outMetadata=uvec2(uint(code),uint(order+0.5))
#define uint2 uvec2
#define COLOR0(pixel) texelFetch(tex0,ivec2(pixel),0)
#define COLOR1(pixel) texelFetch(tex1,ivec2(pixel),0)
#define META2(pixel) texelFetch(tex2,ivec2(pixel),0).xy
#define META3(pixel) texelFetch(tex3,ivec2(pixel),0).xy
)";
const char* depthFunctionBody=R"(
    float encoded=clamp(log2(1.0+z)/log2(100001.0),0.0,1.0);
    return ROUND_EVEN(mix(u.depth.z,u.depth.w,encoded)*16777215.0)/16777215.0;
)";
const char* familyResolveBody=R"(
    uint2 first=META2(o.position.xy),second=META3(o.position.xy);
    if(first.y==0u&&second.y==0u)DISCARD;
    bool chooseFirst=second.y==0u||(first.y!=0u&&
        (first.x<second.x||(first.x==second.x&&first.y>second.y)));
    RETURN_COLOR(chooseFirst?COLOR0(o.position.xy):COLOR1(o.position.xy));
)";
const char* outputStruct=R"(
struct Output {vec4 position,viewPosition;vec3 normal,diffuse,ambient;vec4 specular;vec3 unknown;vec2 uv;float alpha;};
)";
const char* fragmentInput=R"(
layout(location=0) in vec4 viewPosition;
layout(location=1) in vec3 normal;
layout(location=2) in vec3 diffuse;
layout(location=3) in vec3 ambient;
layout(location=4) in vec4 specular;
layout(location=5) in vec3 unknown;
layout(location=6) in vec2 uv;
layout(location=7) in float alpha;
layout(location=0) out vec4 outColor;
layout(set=0,binding=1) uniform sampler2D tex0;
layout(set=0,binding=2) uniform sampler2D tex1;
layout(set=0,binding=3) uniform sampler2D tex2;
layout(set=0,binding=4) uniform sampler2D tex3;
)";
}
std::string metalShaderSource() {
    std::string s=metalPrelude;s+=structures;
    s+="vertex Output meshVertex(uint id [[vertex_id]],constant Vertex* vertices [[buffer(0)]],constant Uniforms& u [[buffer(1)]]) { Vertex v=vertices[id];Output o;";
    s+=vertexBody;
    s+="if(u.inverted.z!=0.0)o.position.y=-o.position.y;o.position.z=(o.position.z+o.position.w)*0.5;return o;}\n";
    s+="vertex Output bitmapVertex(uint id [[vertex_id]],constant Vertex* vertices [[buffer(0)]],constant Uniforms& u [[buffer(1)]]){Vertex v=vertices[id];Output o={};o.position=vec4(v.position.x*2.0-1.0,1.0-v.position.y*2.0,0.5,1.0);if(u.inverted.z!=0.0)o.position.y=-o.position.y;o.uv=v.uv.xy;return o;}\n";
    s+="void applyLight(thread vec3& diffuse,thread vec3& specular,Light light,constant Uniforms& u,Output o){";s+=lightBody;s+="}\n";
    const std::string args="(Output o [[stage_in]],constant Uniforms& u [[buffer(1)]],texture2d<float> tex0 [[texture(0)]],texture2d<float> tex1 [[texture(1)]],texture2d<float> tex2 [[texture(2)]],texture2d<float> tex3 [[texture(3)]],sampler samp0 [[sampler(0)]],sampler samp1 [[sampler(1)]],sampler samp2 [[sampler(2)]],sampler samp3 [[sampler(3)]]){";
    std::string meshArgs=args;
    for(auto index:{1,2}){
        const std::string from="texture2d<float> tex"+std::to_string(index);
        const auto at=meshArgs.find(from);meshArgs.replace(at,from.size(),"depth2d<float> tex"+std::to_string(index));
    }
    const std::string texture3="texture2d<float> tex3";
    meshArgs.replace(meshArgs.find(texture3),texture3.size(),"texture2d<uint> tex3");
    s+="struct MeshValue{vec4 color;float depth;uint2 metadata;};\n";
    s+="struct MeshFragment{vec4 color [[color(0)]];float depth [[depth(any)]];};\n";
    s+="struct FamilyFragment{vec4 color [[color(0)]];uint2 metadata [[color(1)]];float depth [[depth(any)]];};\n";
    s+="float meshWindowDepth(float z,constant Uniforms& u){";s+=depthFunctionBody;s+="}\n";
    s+="MeshValue shadeMesh(Output o,constant Uniforms& u,texture2d<float> tex0,depth2d<float> tex1,depth2d<float> tex2,texture2d<uint> tex3,sampler samp0,sampler samp1,sampler samp2,sampler samp3){MeshValue result={};\n";
    s+="#undef RETURN_COLOR\n#define RETURN_COLOR(value) result.color=value;return result\n";
    s+=meshFragmentBody;
    s+="}\n#undef RETURN_COLOR\n#define RETURN_COLOR(color) return color\n";
    const std::string invoke="auto value=shadeMesh(o,u,tex0,tex1,tex2,tex3,samp0,samp1,samp2,samp3);";
    s+="fragment MeshFragment meshFragment"+meshArgs+invoke+"return {value.color,value.depth};}\n";
    s+="fragment FamilyFragment familyMeshFragment"+meshArgs+invoke+"return {value.color,value.metadata,value.depth};}\n";
    s+="fragment vec4 bitmapFragment"+args+bitmapBody+"}\n";
    std::string resolveArgs=args;
    for(auto index:{2,3}){
        const std::string from="texture2d<float> tex"+std::to_string(index);
        resolveArgs.replace(resolveArgs.find(from),from.size(),"texture2d<uint> tex"+std::to_string(index));
    }
    s+="fragment vec4 familyResolveFragment"+resolveArgs+familyResolveBody+"}\n";
    return s;
}
std::string vulkanVertexSource(bool bitmap) {
    std::string s=glslPrelude;s+="mat3 mat3of(mat4 m){return mat3(m);}";s+=structures;s+="layout(set=0,binding=0,std140)uniform Constants{Uniforms u;};\n";s+=outputStruct;
    s+="struct Vertex{vec4 position,normal,diffuse,ambient,specular,unknown,uv,extra;};\n";
    const char* fields[]={"position","normal","diffuse","ambient","specular","unknown","uv","extra"};
    for(int i=0;i<8;i++)s+="layout(location="+std::to_string(i)+")in vec4 i_"+fields[i]+";\n";
    const char* outTypes[]={"vec4","vec3","vec3","vec3","vec4","vec3","vec2","float"};
    const char* outNames[]={"viewPosition","normal","diffuse","ambient","specular","unknown","uv","alpha"};
    for(int i=0;i<8;i++)s+="layout(location="+std::to_string(i)+")out "+outTypes[i]+" "+outNames[i]+";\n";
    s+="void main(){Vertex v=Vertex(i_position,i_normal,i_diffuse,i_ambient,i_specular,i_unknown,i_uv,i_extra);Output o;";
    if(bitmap)s+="o.position=vec4(v.position.x*2.0-1.0,1.0-v.position.y*2.0,0.0,1.0);o.uv=v.uv.xy;o.viewPosition=vec4(0);o.normal=o.diffuse=o.ambient=o.unknown=vec3(0);o.specular=vec4(0);o.alpha=0.0;";
    else s+=vertexBody;
    s+="if(u.inverted.z==0.0)o.position.y=-o.position.y;o.position.z=(o.position.z+o.position.w)*0.5;gl_Position=o.position;";
    for(auto name:outNames)s+=std::string(name)+"=o."+name+";";
    return s+"}";
}
std::string vulkanFragmentSource(bool bitmap, bool familyResolve) {
    std::string s=glslPrelude;s+="mat3 mat3of(mat4 m){return mat3(m);}";s+=structures;s+="layout(set=0,binding=0,std140)uniform Constants{Uniforms u;};\n";
    s+=outputStruct;
    std::string inputs=fragmentInput;
    for(int index=familyResolve?2:3;index<4;++index){
        if(bitmap&&!familyResolve)break;
        const std::string from="uniform sampler2D tex"+std::to_string(index);
        inputs.replace(inputs.find(from),from.size(),"uniform usampler2D tex"+std::to_string(index));
    }
    s+=inputs;
    if(!bitmap)s+="layout(location=1) out uvec2 outMetadata;\n";
    s+="void applyLight(inout vec3 diffuse,inout vec3 specular,Light light,Uniforms u,Output o){";s+=lightBody;s+="}\n";
    s+="float meshWindowDepth(float z,Uniforms u){";s+=depthFunctionBody;s+="}\n";
    s+="void main(){Output o=Output(gl_FragCoord,viewPosition,normal,diffuse,ambient,specular,unknown,uv,alpha);";
    s+=familyResolve?familyResolveBody:bitmap?bitmapBody:meshFragmentBody;
    return s+"}";
}
} // namespace hikaru::graphics

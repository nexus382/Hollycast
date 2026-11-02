// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "shaders.h"
namespace hikaru::graphics {
namespace {
const char* structures=R"(
struct Light { vec4 position,direction,diffuse,specular,extents,spot,flags; vec4 table[4]; };
struct Uniforms {
    mat4 projection,modelview,alternate,normal,alternateNormal;
    vec4 ambient,fog,fogColor,flags,material,peel;
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
        intensity*=mix(lo,hi,t-float(index));
    }
    diffuse+=attenuation*intensity*o.diffuse*light.diffuse.xyz;
    if(light.flags.w!=0.0){
        vec3 viewDirection=normalize(-o.viewPosition.xyz);
        float angle=max(dot(viewDirection,reflect(-lightDirection,normal)),0.0);
        specular+=o.specular.xyz*light.specular.xyz*pow(angle,255.0*o.specular.w);
    }
)";
const char* meshFragmentBody=R"(
    if(u.peel.x!=0.0){
        vec2 coordinate=o.position.xy/u.peel.zw;
        float opaqueDepth=DEPTH2(coordinate);
        if(o.position.z>opaqueDepth+0.000001)DISCARD;
        if(u.peel.y==0.0&&o.position.z<=DEPTH1(coordinate)+0.000001)DISCARD;
    }
    vec4 texel=u.flags.x!=0.0?SAMPLE0(o.uv):vec4(1.0);
    vec4 color;
    if(u.flags.y!=0.0){
        vec3 diffuse=vec3(0.0),specular=vec3(0.0);
        vec3 ambient=u.material.y!=0.0?o.unknown*257.0:u.ambient.xyz*o.ambient;
        for(int i=0;i<4;i++)if(u.lights[i].flags.x!=0.0)applyLight(diffuse,specular,u.lights[i],u,o);
        color=vec4(ambient+diffuse,o.alpha)*texel+vec4(specular,0.0);
    } else if(u.material.x!=0.0)color=vec4(o.diffuse,o.alpha)*texel;
    else color=vec4(o.ambient,o.alpha)*texel;
    if(u.flags.w!=0.0){float z=o.position.z/o.position.w;float a=clamp(u.fog.x*(z-u.fog.y),0.0,1.0);color=mix(color,vec4(u.fogColor.xyz,color.a),a);}
    if(color.a<=u.material.z)DISCARD;
    RETURN_COLOR(color);
)";
const char* bitmapBody=R"(
    vec2 inv=vec2(o.uv.x,1.0-o.uv.y);
    vec4 front=SAMPLE0(u.inverted.x==0.0?o.uv:inv)*u.factors.y;
    vec4 back=SAMPLE1(u.inverted.y==0.0?o.uv:inv)*u.factors.z;
    vec4 layer1=SAMPLE2(inv),layer2=SAMPLE3(inv);
    vec4 scene=mix(front,back,u.factors.x),color=vec4(0.0);
    float cover1=u.keyed.x!=0.0?step(0.1,layer1.a):1.0;
    float cover2=u.keyed.y!=0.0?step(0.1,layer2.a):1.0;
    bool combined=(u.multipliers.x!=0.0&&u.direct.x==0.0)||(u.multipliers.y!=0.0&&u.direct.y==0.0);
    if(u.multipliers.y!=0.0&&u.direct.y==0.0)color=mix(color,vec4(layer2.xyz*u.multipliers.y,1.0),cover2);
    if(u.multipliers.x!=0.0&&u.direct.x==0.0)color=mix(color,vec4(layer1.xyz*u.multipliers.x,1.0),cover1);
    color=combined?mix(color,vec4(scene.xyz,1.0),scene.a):scene;
    if(u.multipliers.y!=0.0&&u.direct.y!=0.0)color=mix(color,vec4(layer2.xyz*u.multipliers.y,1.0),cover2);
    if(u.multipliers.x!=0.0&&u.direct.x!=0.0)color=mix(color,vec4(layer1.xyz*u.multipliers.x,1.0),cover1);
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
    s+="fragment vec4 meshFragment"+meshArgs+meshFragmentBody+"}\n";
    s+="fragment vec4 bitmapFragment"+args+bitmapBody+"}\n";
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
std::string vulkanFragmentSource(bool bitmap) {
    std::string s=glslPrelude;s+="mat3 mat3of(mat4 m){return mat3(m);}";s+=structures;s+="layout(set=0,binding=0,std140)uniform Constants{Uniforms u;};\n";
    s+=outputStruct;s+=fragmentInput;
    s+="void applyLight(inout vec3 diffuse,inout vec3 specular,Light light,Uniforms u,Output o){";s+=lightBody;s+="}\n";
    s+="void main(){Output o=Output(gl_FragCoord,viewPosition,normal,diffuse,ambient,specular,unknown,uv,alpha);";
    s+=bitmap?bitmapBody:meshFragmentBody;
    return s+"}";
}
} // namespace hikaru::graphics

// Copyright 2026 The Hollycast Authors
// Portions Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "shaders.h"
#include "device.h"
#include <stdexcept>
#include <utility>
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
    WRITE_METADATA(rawCode,u.family.z+(u.peel.x!=0.0?u.blend.z*4.0:0.0));
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
    // Match the GL compositor: centre bitmap planes without stretching them,
    // including when a plane is selected as front/back or has its key disabled.
    vec2 inv=vec2((o.uv.x-0.5)/u.factors.w+0.5,1.0-o.uv.y);
    float visible=step(0.0,inv.x)*step(inv.x,1.0);
    vec4 front=SAMPLE0(u.inverted.x==0.0?o.uv:inv)*u.factors.y*(u.inverted.x==0.0?1.0:visible);
    vec4 back=SAMPLE1(u.inverted.y==0.0?o.uv:inv)*u.factors.z*(u.inverted.y==0.0?1.0:visible);
    vec4 layer1=SAMPLE2(inv),layer2=SAMPLE3(inv);
    vec4 scene=mix(front,back,u.factors.x);
    // Bitmap planes sit either behind the 3D picture or in front of it.
    // A keyed plane covers only where its alpha key is set; an unkeyed
    // plane covers everywhere. Behind-planes show only where the 3D
    // picture drew nothing, so a drawn pixel covers them in full.
    float key1=visible*(u.keyed.x!=0.0?step(0.1,layer1.a):1.0);
    float key2=visible*(u.keyed.y!=0.0?step(0.1,layer2.a):1.0);
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
const char* peelCompositeBody=R"(
    vec4 peelColor=COLOR0(o.position.xy);
    uint storedMode=META2(o.position.xy).y/4u;
    if(int(storedMode)!=int(u.peel.y+0.5))DISCARD;
    RETURN_COLOR(peelColor);
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
    std::string peelArgs=args;
    const std::string peelTexture="texture2d<float> tex2";
    peelArgs.replace(peelArgs.find(peelTexture),peelTexture.size(),"texture2d<uint> tex2");
    s+="fragment vec4 peelCompositeFragment"+peelArgs+peelCompositeBody+"}\n";
    std::string resolveArgs=args;
    for(auto index:{2,3}){
        const std::string from="texture2d<float> tex"+std::to_string(index);
        resolveArgs.replace(resolveArgs.find(from),from.size(),"texture2d<uint> tex"+std::to_string(index));
    }
    s+="fragment vec4 familyResolveFragment"+resolveArgs+familyResolveBody+"}\n";
    return s;
}
namespace {
void replaceAll(std::string& source, const std::string& from, const std::string& to) {
    for(std::size_t at=0;(at=source.find(from,at))!=std::string::npos;at+=to.size())
        source.replace(at,from.size(),to);
}
std::string vulkanMeshConstants() {
    const char* names[]={"Specialize","Texture","ShadingMode","TwoSided","Fog","AlphaMode","TextureMode","Pattern","TexturePalette"};
    std::string source;
    for(unsigned i=0;i<9;++i)
        source+="layout(constant_id="+std::to_string(i)+") const int k"+names[i]+"=0;\n";
    const char* fields[]={"Enabled","Type","Attenuation","Specular"};
    for(unsigned light=0;light<4;++light)for(unsigned field=0;field<4;++field)
        source+="layout(constant_id="+std::to_string(9+light*4+field)+") const int kLight"+
            std::to_string(light)+fields[field]+"=0;\n";
    return source;
}
}
std::string vulkanVertexSource(bool bitmap, bool palette) {
    std::string s=glslPrelude;s+="mat3 mat3of(mat4 m){return mat3(m);}";s+=structures;s+="layout(set=0,binding=0,std140)uniform Constants{Uniforms u;};\n";s+=outputStruct;
    s+="struct Vertex{vec4 position,normal,diffuse,ambient,specular,unknown,uv,extra;};\n";
    const char* fields[]={"position","normal","diffuse","ambient","specular","unknown","uv","extra"};
    for(int i=0;i<8;i++)s+="layout(location="+std::to_string(i)+")in vec4 i_"+fields[i]+";\n";
    const char* outTypes[]={"vec4","vec3","vec3","vec3","vec4","vec3","vec2","float"};
    const char* outNames[]={"viewPosition","normal","diffuse","ambient","specular","unknown","uv","alpha"};
    for(int i=0;i<8;i++)s+="layout(location="+std::to_string(i)+")out "+outTypes[i]+" "+outNames[i]+";\n";
    if(palette) {
        s+="struct DrawRecord{mat4 modelview,alternate,normal,alternateNormal;vec4 modes,sampling;};\n";
        s+="layout(set=0,binding=5,std430) readonly buffer Palette{DrawRecord records[];};\n";
        s+="layout(location=8) flat out vec4 drawModes;layout(location=9) flat out float textureBias;layout(location=10) flat out int drawIndex;\n";
    }
    s+="void main(){Vertex v=Vertex(i_position,i_normal,i_diffuse,i_ambient,i_specular,i_unknown,i_uv,i_extra);Output o;";
    if(bitmap)s+="o.position=vec4(v.position.x*2.0-1.0,1.0-v.position.y*2.0,0.0,1.0);o.uv=v.uv.xy;o.viewPosition=vec4(0);o.normal=o.diffuse=o.ambient=o.unknown=vec3(0);o.specular=vec4(0);o.alpha=0.0;";
    else {
        std::string body=vertexBody;
        if(palette) {
            s+="drawIndex=int(v.extra.z+0.5);DrawRecord record=records[drawIndex];drawModes=record.modes;textureBias=record.sampling.x;";
            replaceAll(body,"u.modelview","record.modelview");
            replaceAll(body,"u.alternateNormal","record.alternateNormal");
            replaceAll(body,"u.alternate","record.alternate");
            replaceAll(body,"u.normal","record.normal");
        }
        s+=body;
    }
    s+="if(u.inverted.z==0.0)o.position.y=-o.position.y;o.position.z=(o.position.z+o.position.w)*0.5;gl_Position=o.position;";
    for(auto name:outNames)s+=std::string(name)+"=o."+name+";";
    return s+"}";
}
std::string vulkanFragmentSource(bool bitmap, bool familyResolve, bool palette, unsigned textureSlots, bool peelComposite) {
    if(textureSlots!=1&&textureSlots!=16)throw std::invalid_argument("Invalid Hikaru Vulkan texture palette size");
    std::string s=glslPrelude;s+="mat3 mat3of(mat4 m){return mat3(m);}";s+=structures;s+="layout(set=0,binding=0,std140)uniform Constants{Uniforms u;};\n";
    s+=outputStruct;
    std::string inputs=fragmentInput;
    replaceAll(inputs,"uniform sampler2D tex0;","uniform sampler2D tex0["+std::to_string(textureSlots)+"];" );
    for(int index=(familyResolve||peelComposite)?2:3;index<4;++index){
        if(peelComposite&&index>2)break;
        if(bitmap&&!familyResolve&&!peelComposite)break;
        const std::string from="uniform sampler2D tex"+std::to_string(index);
        inputs.replace(inputs.find(from),from.size(),"uniform usampler2D tex"+std::to_string(index));
    }
    s+=inputs;
    s+="#undef SAMPLE0\n#define SAMPLE0(uv) texture(tex0[0],uv,u.lodBias.x)\n#undef COLOR0\n#define COLOR0(pixel) texelFetch(tex0[0],ivec2(pixel),0)\n";
    if(!bitmap) {
        s+="layout(location=1) out uvec2 outMetadata;\n";
        s+=vulkanMeshConstants();
        if(palette) {
            s+="layout(location=8) flat in vec4 drawModes;layout(location=9) flat in float textureBias;layout(location=10) flat in int drawIndex;\n";
            // Constant descriptor indices work without descriptor-indexing or
            // nonuniform sampler-array features on Android Vulkan devices.
            s+="vec4 samplePalette(int index,vec2 coordinate,float bias){\n";
            for(unsigned i=0;i+1<textureSlots;++i)
                s+="if(index=="+std::to_string(i)+")return texture(tex0["+std::to_string(i)+"],coordinate,bias);\n";
            s+="return texture(tex0["+std::to_string(textureSlots-1)+"],coordinate,bias);}\n";
            s+="#undef SAMPLE0\n#define SAMPLE0(uv) samplePalette(kTexturePalette!=0?drawIndex:0,uv,kTexturePalette!=0?textureBias:u.lodBias.x)\n";
        }
        // Match the GL shader's direct access to fragment inputs and each
        // constant-index UBO light. This avoids aggregate function arguments
        // and lets each light's flags specialize independently.
        const char* components[]={"x","y","z","w"};
        const char* fields[]={"Enabled","Type","Attenuation","Specular"};
        for(unsigned i=0;i<4;++i) {
            const std::string n=std::to_string(i),base="u.lights["+n+"]";
            std::string light=lightBody;
            replaceAll(light,"u.flags.z","(kSpecialize!=0?float(kTwoSided):u.flags.z)");
            for(unsigned field=1;field<4;++field)
                replaceAll(light,std::string("light.flags.")+components[field],
                    "(kSpecialize!=0?float(kLight"+n+fields[field]+"):"+base+".flags."+components[field]+")");
            replaceAll(light,"light.",base+".");
            replaceAll(light,"vec3 normal=","vec3 surfaceNormal=");
            replaceAll(light,"dot(normal,","dot(surfaceNormal,");
            replaceAll(light,",normal)",",surfaceNormal)");
            replaceAll(light,"o.viewPosition","viewPosition");
            replaceAll(light,"o.normal","normal");
            replaceAll(light,"o.specular","specular");
            replaceAll(light,"diffuse+=","accumDiffuse+=");
            replaceAll(light,"specular+=","accumSpecular+=");
            s+="void applyLight"+n+"(inout vec3 accumDiffuse,inout vec3 accumSpecular){"+light+"}\n";
        }
        s+="float meshWindowDepth(float z){";s+=depthFunctionBody;s+="}\n";
    }
    s+="void main(){Output o=Output(gl_FragCoord,viewPosition,normal,diffuse,ambient,specular,unknown,uv,alpha);";
    if(bitmap)s+=familyResolve?familyResolveBody:peelComposite?peelCompositeBody:bitmapBody;
    else {
        const std::string textureMode=palette?"drawModes.z":"u.material.y";
        const std::string shadingMode=palette?"drawModes.w":"u.flags.y";
        s+="float textureMode=kSpecialize!=0&&kTextureMode>=0?float(kTextureMode):"+textureMode+";\n";
        s+="float shadingMode=kSpecialize!=0&&kShadingMode>=0?float(kShadingMode):"+shadingMode+";\n";
        std::string body=meshFragmentBody;
        const std::string loop="        for(int i=0;i<4;i++)if(u.lights[i].flags.x!=0.0)applyLight(diffuse,specular,u.lights[i],u,o);";
        std::string lights;
        for(unsigned i=0;i<4;++i) {
            const std::string n=std::to_string(i);
            lights+="        if((kSpecialize!=0?kLight"+n+"Enabled:int(u.lights["+n+"].flags.x))!=0)applyLight"+n+"(diffuse,specular);\n";
        }
        replaceAll(body,loop,lights);
        replaceAll(body,",u)",")");
        replaceAll(body,"u.flags.x","(kSpecialize!=0?float(kTexture):u.flags.x)");
        replaceAll(body,"u.flags.y","shadingMode");
        replaceAll(body,"u.flags.w","(kSpecialize!=0?float(kFog):u.flags.w)");
        replaceAll(body,"u.material.x","(kSpecialize!=0?float(kAlphaMode):u.material.x)");
        replaceAll(body,"u.material.y","textureMode");
        if(palette){replaceAll(body,"u.depth.y","drawModes.x");replaceAll(body,"u.material.w","drawModes.y");}
        const auto patternBegin=body.find("    uint2 patternPixel"),patternEnd=body.find("    float eyeZ");
        body.insert(patternEnd,"    }\n");
        body.insert(patternBegin,"    if(kSpecialize==0||kPattern!=0){\n");
        s+=body;
    }
    return s+"}";
}

MeshShaderSpecialization meshShaderSpecialization(const Draw& d, bool specialize) {
    MeshShaderSpecialization result{};
    if(d.bitmap)return result;
    result[0]=specialize;result[1]=int32_t(d.uniforms.flags[0]);
    result[2]=d.shadingPalette?-1:int32_t(d.uniforms.flags[1]);result[3]=int32_t(d.uniforms.flags[2]);
    result[4]=int32_t(d.uniforms.flags[3]);result[5]=int32_t(d.uniforms.material[0]);
    result[6]=d.texturePalette?-1:int32_t(d.uniforms.material[1]);result[7]=d.uniforms.family[3]!=65535;
    result[8]=d.texturePalette;
    for(unsigned light=0;light<4;++light)for(unsigned field=0;field<4;++field)
        result[9+light*4+field]=int32_t(d.uniforms.lights[light].flags[field]);
    return result;
}

std::string dx11ShaderSource(bool bitmap, bool familyResolve, bool palette, const MeshShaderSpecialization& specialization, bool packed, unsigned passFlags, bool peelComposite) {
    // Uniforms and DrawRecord contain only column-major matrices and 16-byte
    // vectors, matching both the native CPU layout and HLSL constant packing.
    std::string s=R"(
#pragma pack_matrix(column_major)
#define vec2 float2
#define vec3 float3
#define vec4 float4
#define mat4 float4x4
#define mix lerp
#define ROUND_EVEN round
#define DISCARD discard
)";
    s+=structures;
    s+="cbuffer Constants : register(b0){Uniforms u;};\n";
    s+=R"(
struct DrawRecord {mat4 modelview,alternate,normal,alternateNormal;vec4 modes,sampling;};
cbuffer Palette : register(b1){DrawRecord records[16];};
struct Vertex {
    vec4 position:POSITION;vec4 normal:NORMAL;vec4 diffuse:COLOR0;vec4 ambient:COLOR1;
    vec4 specular:COLOR2;vec4 unknown:COLOR3;vec4 uv:TEXCOORD0;vec4 extra:TEXCOORD1;
};
struct Output {
    vec4 position:SV_Position;vec4 viewPosition:TEXCOORD0;vec3 normal:TEXCOORD1;
    vec3 diffuse:TEXCOORD2;vec3 ambient:TEXCOORD3;vec4 specular:TEXCOORD4;
    vec3 unknown:TEXCOORD5;vec2 uv:TEXCOORD6;float alpha:TEXCOORD7;
    nointerpolation vec4 drawModes:TEXCOORD8;
    nointerpolation float textureBias:TEXCOORD9;
    nointerpolation uint drawIndex:TEXCOORD10;
};
struct VertexInput {
    vec4 position:POSITION;vec4 normal:NORMAL;vec4 diffuse:COLOR0;vec4 ambient:COLOR1;
    vec4 specular:COLOR2;vec4 unknown:COLOR3;vec4 uv:TEXCOORD0;
    float alpha:TEXCOORD1;float matrixSelect:TEXCOORD2;float drawIndex:TEXCOORD3;
};
Output vertexMain(VertexInput input){Output o=(Output)0;Vertex v=(Vertex)0;
v.position=input.position;v.normal=input.normal;v.diffuse=input.diffuse;v.ambient=input.ambient;
v.specular=input.specular;v.unknown=input.unknown;v.uv=input.uv;
)";
    // UNORM8 supplies alpha directly. Recover the exact byte selector before
    // its existing round-to-integer/matrix-bank logic; colors stay normalized.
    s+="v.extra=vec4(input.alpha,input.matrixSelect"+std::string(packed?"*255.0":"")+",input.drawIndex,0);\n";
    if(bitmap)s+="o.position=vec4(v.position.x*2.0-1.0,1.0-v.position.y*2.0,0.0,1.0);o.uv=v.uv.xy;";
    else {
        std::string body=vertexBody;
        replaceAll(body,"mv*vec4(v.position.xyz,1.0)","mul(mv,vec4(v.position.xyz,1.0))");
        replaceAll(body,"mat3of(mv)*v.position.xyz","mul((float3x3)mv,v.position.xyz)");
        replaceAll(body,"mv[3].xyz","vec3(mv[0][3],mv[1][3],mv[2][3])");
        replaceAll(body,"mat3of(nm)*v.normal.xyz","mul((float3x3)nm,v.normal.xyz)");
        replaceAll(body,"u.projection*pos","mul(u.projection,pos)");
        if(palette) {
            s+="o.drawIndex=uint(v.extra.z+0.5);DrawRecord record=records[o.drawIndex];o.drawModes=record.modes;o.textureBias=record.sampling.x;";
            replaceAll(body,"u.modelview","record.modelview");
            replaceAll(body,"u.alternateNormal","record.alternateNormal");
            replaceAll(body,"u.alternate","record.alternate");
            replaceAll(body,"u.normal","record.normal");
        }
        s+=body;
    }
    // D3D's viewport reverses clip Y. Offscreen rows retain GL texture row
    // ordering; the final output is top-left and ReadPixels flips only it.
    s+="if(u.inverted.z!=0.0)o.position.y=-o.position.y;o.position.z=(o.position.z+o.position.w)*0.5;return o;}\n";
    s+="Texture2D<float4> tex0[16]:register(t0);SamplerState samplers[16]:register(s0);\n";
    if(bitmap&&peelComposite) {
        s+="Texture2D<float4> tex1:register(t16);Texture2D<uint2> tex2:register(t17);Texture2D<float4> tex3:register(t18);\n";
    } else if(bitmap&&!familyResolve) {
        s+="Texture2D<float4> tex1:register(t16);Texture2D<float4> tex2:register(t17);Texture2D<float4> tex3:register(t18);\n";
        s+="#define SAMPLE0(coord) tex0[0].SampleBias(samplers[0],coord,u.lodBias.x)\n";
        s+="#define SAMPLE1(coord) tex1.SampleBias(samplers[1],coord,u.lodBias.y)\n";
        s+="#define SAMPLE2(coord) tex2.SampleBias(samplers[2],coord,u.lodBias.z)\n";
        s+="#define SAMPLE3(coord) tex3.SampleBias(samplers[3],coord,u.lodBias.w)\n";
    } else if(familyResolve) {
        s+="Texture2D<float4> tex1:register(t16);Texture2D<uint2> tex2:register(t17);Texture2D<uint2> tex3:register(t18);\n";
    } else {
        s+="Texture2D<float> tex1:register(t16);Texture2D<float> tex2:register(t17);Texture2D<uint2> tex3:register(t18);\n";
        // Depth inputs use nearest/clamp at exact fragment coordinates. Loads
        // leave all 16 sampler slots available to the ordered texture palette.
        s+=R"(
float loadDepth(Texture2D<float> source,vec2 coordinate){
    uint width,height;source.GetDimensions(width,height);
    int2 pixel=clamp(int2(coordinate*vec2(width,height)),int2(0,0),int2(width-1,height-1));
    return source.Load(int3(pixel,0));
}
#define DEPTH1(coord) loadDepth(tex1,coord)
#define DEPTH2(coord) loadDepth(tex2,coord)
)";
    }
    s+="#define COLOR0(pixel) tex0[0].Load(int3(int2(pixel),0))\n#define COLOR1(pixel) tex1.Load(int3(int2(pixel),0))\n";
    s+="#define META2(pixel) tex2.Load(int3(int2(pixel),0)).xy\n#define META3(pixel) tex3.Load(int3(int2(pixel),0)).xy\n";
    if(bitmap) {
        s+="#define RETURN_COLOR(value) return value\nfloat4 pixelMain(Output o):SV_Target0{\n";
        std::string body=familyResolve?familyResolveBody:peelComposite?peelCompositeBody:bitmapBody;
        replaceAll(body,"vec4(0.0)","vec4(0.0,0.0,0.0,0.0)");
        s+=body;return s+"}\n";
    }
    const char* names[]={"Specialize","Texture","ShadingMode","TwoSided","Fog","AlphaMode","TextureMode","Pattern","TexturePalette"};
    for(unsigned i=0;i<9;++i)s+="static const int k"+std::string(names[i])+"="+std::to_string(specialization[i])+";\n";
    const char* fields[]={"Enabled","Type","Attenuation","Specular"};
    for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j)
        s+="static const int kLight"+std::to_string(i)+fields[j]+"="+std::to_string(specialization[9+i*4+j])+";\n";
    s+="static const uint kPassFlags="+std::to_string(passFlags)+"u;\n";
    s+="vec4 sampleBase(uint index,vec2 coord,vec2 dx,vec2 dy,float bias){float scale=exp2(bias);\n";
    const unsigned slots=palette&&specialization[8]?16:1;
    for(unsigned i=0;i+1<slots;++i)s+="if(index=="+std::to_string(i)+")return tex0["+std::to_string(i)+"].SampleGrad(samplers["+std::to_string(i)+"],coord,dx*scale,dy*scale);\n";
    const std::string last=std::to_string(slots-1);
    s+="return tex0["+last+"].SampleGrad(samplers["+last+"],coord,dx*scale,dy*scale);}\n";
    s+="#define SAMPLE0(coord) sampleBase(kTexturePalette!=0?o.drawIndex:0,coord,uvDx,uvDy,kTexturePalette!=0?o.textureBias:u.lodBias.x)\n";
    s+="float meshWindowDepth(float z){";
    s+=depthFunctionBody;s+="}\n";
    s+=R"(
struct PixelResult{vec4 color:SV_Target0;uint2 metadata:SV_Target1;float depth:SV_Depth;};
#define WRITE_DEPTH(value) result.depth=value
#define WRITE_METADATA(code,order) result.metadata=uint2(uint(code),uint(order+0.5))
#define RETURN_COLOR(value) result.color=value;return result
PixelResult pixelMain(Output o){PixelResult result=(PixelResult)0;
// Evaluate derivatives before any pattern/family discard or sampler branch.
vec2 uvDx=ddx(o.uv),uvDy=ddy(o.uv);
)";
    s+="float textureMode=kSpecialize!=0&&kTextureMode>=0?float(kTextureMode):"+std::string(palette?"o.drawModes.z":"u.material.y")+";\n";
    s+="float shadingMode=kSpecialize!=0&&kShadingMode>=0?float(kShadingMode):"+std::string(palette?"o.drawModes.w":"u.flags.y")+";\n";
    std::string body=meshFragmentBody;
    std::string lights;
    for(unsigned i=0;i<4;++i) {
        const std::string n=std::to_string(i),base="u.lights["+n+"]";
        std::string light=lightBody;
        replaceAll(light,"u.flags.z","(kSpecialize!=0?float(kTwoSided):u.flags.z)");
        const char* components[]={"x","y","z","w"};
        for(unsigned field=1;field<4;++field)
            replaceAll(light,std::string("light.flags.")+components[field],"(kSpecialize!=0?float(kLight"+n+fields[field]+"):"+base+".flags."+components[field]+")");
        replaceAll(light,"light.",base+".");
        lights+="if((kSpecialize!=0?kLight"+n+"Enabled:int("+base+".flags.x))!=0){"+light+"}\n";
    }
    replaceAll(body,"for(int i=0;i<4;i++)if(u.lights[i].flags.x!=0.0)applyLight(diffuse,specular,u.lights[i],u,o);",lights);
    replaceAll(body,",u)",")");
    replaceAll(body,"u.flags.x","(kSpecialize!=0?float(kTexture):u.flags.x)");
    replaceAll(body,"u.flags.y","shadingMode");
    replaceAll(body,"u.flags.w","(kSpecialize!=0?float(kFog):u.flags.w)");
    replaceAll(body,"u.material.x","(kSpecialize!=0?float(kAlphaMode):u.material.x)");
    replaceAll(body,"u.material.y","textureMode");
    if(palette){replaceAll(body,"u.depth.y","o.drawModes.x");replaceAll(body,"u.material.w","o.drawModes.y");}
    replaceAll(body,"vec3(0.0)","vec3(0.0,0.0,0.0)");
    replaceAll(body,"vec4(1.0)","vec4(1.0,1.0,1.0,1.0)");
    const auto patternBegin=body.find("    uint2 patternPixel"),patternEnd=body.find("    float eyeZ");
    body.insert(patternEnd,"    }\n");body.insert(patternBegin,"    if(kSpecialize==0||kPattern!=0){\n");
    // Compile out unused pass work without changing depth rounding, arithmetic
    // order, or the generic reference path. In particular an ordinary mesh
    // need not compute raw/placement depth for absent family/peel passes.
    const std::pair<const char*,std::pair<unsigned,const char*>> controls[]={
        {"familyEnabled",{Dx11FamilyEnabled,"u.family.x!=0.0"}},
        {"familyOther",{Dx11FamilyOther,"u.family.x>1.0"}},
        {"rawMetadata",{Dx11RawMetadata,"u.family.y!=0.0||u.family.x>1.0"}},
        {"peelEnabled",{Dx11PeelEnabled,"u.peel.x!=0.0"}},
        {"peelAfterFirst",{Dx11PeelAfterFirst,"u.peel.y==0.0"}},
        {"weighByAlpha",{Dx11WeighByAlpha,"u.blend.x!=0.0"}},
        {"forceCoverage",{Dx11ForceCoverage,"u.blend.y!=0.0"}}
    };
    // Replace composite predicates before their component expressions.
    replaceAll(body,"u.family.y!=0.0||u.family.x>1.0","rawMetadata");
    replaceAll(body,"u.family.x==0.0","!familyEnabled");
    for(const auto& control:controls) {
        const auto& [flag,expression]=control.second;
        s+="bool "+std::string(control.first)+"=kSpecialize!=0?(kPassFlags&"+std::to_string(flag)+"u)!=0:("+expression+");\n";
        replaceAll(body,expression,control.first);
    }
    // Palette runs may mix placement and sort scales. Elide an unused
    // encoding only when every record selects the same existing branch.
    s+="bool commonDepth=kSpecialize!=0&&(kPassFlags&"+std::to_string(Dx11CommonDepth)+"u)!=0;\n";
    replaceAll(body,"if(o.drawModes.y>0.0&&o.drawModes.y!=o.drawModes.x)","if(!commonDepth&&o.drawModes.y>0.0&&o.drawModes.y!=o.drawModes.x)");
    replaceAll(body,"if(u.material.w>0.0&&u.material.w!=u.depth.y)","if(!commonDepth&&u.material.w>0.0&&u.material.w!=u.depth.y)");
    s+="bool separateDepth=kSpecialize!=0&&(kPassFlags&"+std::to_string(Dx11SeparateDepth)+"u)!=0;\n";
    const std::string placement=palette?"o.drawModes.x":"u.depth.y",sorting=palette?"o.drawModes.y":"u.material.w";
    replaceAll(body,"float writtenDepth=placementDepth;","float writtenDepth=separateDepth?meshWindowDepth(max(u.depth.x,eyeZ*"+sorting+")):placementDepth;");
    // separateDepth proves the legacy condition true for the entire run.
    // The generic/mixed path still evaluates the original per-record test.
    replaceAll(body,"if(!commonDepth&&"+sorting+">0.0&&"+sorting+"!="+placement+")","if(!commonDepth&&!separateDepth&&"+sorting+">0.0&&"+sorting+"!="+placement+")");
    return s+body+"}\n";
}
} // namespace hikaru::graphics

// Portions Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include <SDL.h>
#ifdef __APPLE__
#include <OpenGL/gl3.h>
#else
#include <glad/gl.h>
#endif
#include <stdio.h>
#include <string.h>
#include "material_pixels.h"

/* Expected colors follow the material equations documented in Odin aa89944.
 * The alpha expectations use Hollycast's coverage buffer, not Odin's inverse
 * transmittance buffer. Values are deliberately different in all inputs. */
static const hikaru_material_pixel_case_t cases[] = {
 {"plain texture ignores black material colors",0x80881,0,{0,0,0},{0,0,0},{180,100,40,128},{0,0,0},1,0,255,255,false,{1,0,1,1},{180,100,40,128},true},
 {"texture mode 2 retains colored HUD",0x80881,2u<<29,{255,0,0},{0,224,64},{255,255,255,255},{0,0,0},1,0,255,255,false,{1,0,1,1},{0,224,64,255},true},
 {"texture mode 1 zero texel alpha preserves polygon opacity",0x80881,1u<<29,{10,20,30},{255,0,0},{200,100,50,0},{0,0,0},6,0,64,255,false,{1,0,1,1},{10,20,30,64},true},
 {"texture mode 1 full texel alpha preserves same opacity",0x80881,1u<<29,{10,20,30},{255,0,0},{200,100,50,255},{0,0,0},6,0,64,255,false,{1,0,1,1},{200,100,50,64},true},
 {"texture mode 3 uses primary color with texel opacity",0x80881,4u<<29,{20,40,60},{200,160,120},{255,0,128,128},{0,0,0},6,0,128,255,false,{1,0,1,1},{20,40,60,64},true},
 {"texture mode 4 mixes the two material colors",0x80881,5u<<29,{20,40,60},{200,160,120},{255,0,128,128},{0,0,0},6,0,128,255,false,{1,0,1,1},{200,40,90,64},true},
 {"material alpha selects color before texel opacity",0x180881,0,{0,0,0},{255,0,0},{200,100,50,64},{0,0,0},6,0,128,255,false,{1,0,1,1},{100,50,25,64},true},
 {"untextured alpha selects color with opaque output",0x100881,0,{255,0,0},{0,0,255},{0,0,0,0},{0,0,0},6,0,128,255,false,{1,0,1,1},{127,0,128,255},true},
 {"captured face ambient uses 691",0xd0881,0,{0,0,0},{0,0,0},{200,150,100,255},{255,234,234},1,0,255,255,false,{1,0,1,1},{200,138,92,255},true},
 {"mode 2 emission does not require a light",0xa0881,0,{255,255,255},{0,0,0},{180,100,40,128},{127,127,127},1,0,255,255,false,{1,0,1,1},{90,50,20,128},true},
 {"691 can exceed one without turning black",0xd0881,0,{0,0,0},{0,0,0},{50,50,50,255},{512,512,512},1,0,255,255,false,{1,0,1,1},{100,100,100,255},true},
 {"opaque geometry covers despite partial texture alpha",0x80881,0,{255,255,255},{255,255,255},{180,100,40,1},{0,0,0},1,0,255,255,false,{0,0,0,0},{180,100,40,255}},
 {"half-opacity puff contributes coverage on empty scene",0x80881,0,{255,255,255},{255,255,255},{200,100,50,255},{0,0,0},6,0,128,255,true,{0,0,0,0},{100,50,25,128}},
 {"half-opacity puff keeps opaque background covered",0x80881,0,{255,255,255},{255,255,255},{200,100,50,255},{0,0,0},6,0,128,255,true,{20.f/255,40.f/255,60.f/255,1},{110,70,55,255}},
 {"opaque puff cannot erase scene coverage",0x80881,0,{255,255,255},{255,255,255},{200,100,50,255},{0,0,0},6,0,255,255,true,{20.f/255,40.f/255,60.f/255,1},{200,100,50,255}},
 {"additive plume keeps opaque scene coverage",0x80881,0,{255,255,255},{255,255,255},{200,100,50,255},{0,0,0},6,2,128,255,true,{20.f/255,40.f/255,60.f/255,1},{120,90,85,255}},
 {"product changes color without erasing coverage",0x80881,0,{255,255,255},{255,255,255},{128,128,128,255},{0,0,0},6,1,255,255,true,{20.f/255,40.f/255,60.f/255,1},{10,20,30,255}},
 {"product blocks behind-planes even on an empty scene",0x80881,0,{255,255,255},{255,255,255},{128,128,128,255},{0,0,0},6,1,255,255,true,{0,0,0,0},{0,0,0,255}},
 {"premultiplied over preserves coverage",0x80881,0,{255,255,255},{255,255,255},{100,50,25,128},{0,0,0},6,3,255,255,true,{20.f/255,40.f/255,60.f/255,1},{110,70,55,255}}
};
int hikaru_vendor_test_pattern_commands(void);
int main(int argc,char **argv)
{
 const int gles=argc==2&&!strcmp(argv[1],"--gles");
 SDL_SetMainReady();if(SDL_Init(SDL_INIT_VIDEO)){fprintf(stderr,"SDL: %s\n",SDL_GetError());return 2;}
 SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,gles?SDL_GL_CONTEXT_PROFILE_ES:SDL_GL_CONTEXT_PROFILE_CORE);
 SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,3);SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,gles?0:2);
 SDL_Window *w=SDL_CreateWindow("Hikaru material coverage",0,0,16,16,SDL_WINDOW_OPENGL|SDL_WINDOW_HIDDEN);
 if(!w){fprintf(stderr,"SDL window: %s\n",SDL_GetError());SDL_Quit();return 3;}
 SDL_GLContext context=SDL_GL_CreateContext(w);if(!context){SDL_DestroyWindow(w);SDL_Quit();return 4;}
#ifndef __APPLE__
 if(!(gles?gladLoadGLES2((GLADloadfunc)SDL_GL_GetProcAddress):gladLoadGL((GLADloadfunc)SDL_GL_GetProcAddress)))return 5;
#endif
 unsigned failed=0;
 if(!hikaru_vendor_test_pattern_commands()){fprintf(stderr,"pattern command scope failed\n");++failed;}
 static const uint16_t masks[32]={
  0xffff,0xfbfe,0xfafa,0x7ada,0x5a5a,0x5258,0x5050,0x1040,
  0x0000,0x0401,0x0505,0x8525,0xa5a5,0xada7,0xafaf,0xefbf,
  0xffff,0xfdf7,0xf5f5,0xe5b5,0xa5a5,0xa4a1,0xa0a0,0x8020,
  0x0000,0x0208,0x0a0a,0x1a4a,0x5a5a,0x5b5e,0x5f5f,0x7fdf
 };
 for(unsigned pattern=0;pattern<32;++pattern){
  uint8_t pixels[64]={0},sample[4]={0};
  hikaru_material_pixel_case_t test={0};test.polytype=1;test.polygon_alpha=test.vertex_alpha=255;
  test.diffuse[0]=test.diffuse[1]=test.diffuse[2]=255;test.background[3]=1;
  test.pattern=pattern;test.coverage_pixels=pixels;
  int ok=hikaru_vendor_test_material_case_pixels(&test,sample);
  for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x){
   unsigned expected=(masks[pattern]>>((3-y)*4+x))&1u;
   for(unsigned c=0;c<3;++c)if(pixels[(y*4+x)*4+c]!=(expected?255:0))ok=0;
  }
  if(!ok){fprintf(stderr,"pattern %u coverage failed\n",pattern);++failed;}
 }
 printf("32 screen-door masks and nested pattern scope checked\n");
 for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);++i){
  uint8_t actual[4]={0};int ok=hikaru_vendor_test_material_case_pixels(&cases[i],actual);
  for(unsigned c=0;c<4;++c){int delta=(int)actual[c]-cases[i].expected[c];if(delta<-2||delta>2)ok=0;}
  printf("%s: %s RGBA %u,%u,%u,%u expected %u,%u,%u,%u\n",ok?"PASS":"FAIL",cases[i].name,actual[0],actual[1],actual[2],actual[3],cases[i].expected[0],cases[i].expected[1],cases[i].expected[2],cases[i].expected[3]);
  if(!ok)++failed;
 }
 SDL_GL_DeleteContext(context);SDL_DestroyWindow(w);SDL_Quit();printf("material coverage: %u failed\n",failed);return failed?1:0;
}

#define GL_SILENCE_DEPRECATION
#include <SDL.h>
#include <OpenGL/gl3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "engine.h"
#include "engine_internal.h"
#include "mach/hikaru/hikaru.h"
#include "mach/hikaru/hikaru-renderer-private.h"
#include "cpu/sh/sh4.h"
int hikaru_vendor_test_idma_completion(vk_machine_t *);
int hikaru_vendor_test_frame_state_growth(vk_renderer_t *);
void hikaru_vendor_test_fail_renderer_init(void);
void hikaru_vendor_test_with_gl_guard(void (*draw)(void));
void hikaru_vendor_test_setup_scene_scissor(void);
int hikaru_vendor_test_shader_uniform_cache(void);
int hikaru_vendor_test_vertex_chunks(void);
int hikaru_vendor_test_mesh_vao_pixels(void);
int hikaru_vendor_test_unlit_material_pixels(uint32_t, const uint8_t[3], const uint8_t[3], const uint8_t[4], uint8_t[4]);
int hikaru_vendor_test_layer_pixels(vk_renderer_t *, int, const uint32_t[4], const uint8_t[4], int, float, float, uint8_t[4][4]);
int hikaru_vendor_test_material_depth_pixels(uint32_t, const uint8_t[3], const uint8_t[3], const uint8_t[4], uint8_t[4], float *);
int hikaru_vendor_test_material_fog_depth_pixels(uint32_t, const uint8_t[3], const uint8_t[3], const uint8_t[4], uint8_t[4], float *, unsigned);
int hikaru_vendor_test_routed_layers(vk_renderer_t *, uint8_t[10][3][4]);
static vk_renderer_t *layer_renderer;
static int frame_state_growth_passed;
static void frame_state_growth_probe(void) { frame_state_growth_passed = hikaru_vendor_test_frame_state_growth(layer_renderer); }
static int layer_pixels_passed;
static void layer_pixels_probe(void) {
 const uint8_t scene[4]={23,47,89,255};
 const uint32_t rgb1555[4]={0x801f,0x83e0,0xfc00,0xffff};
 const uint8_t expected1555[4][4]={{255,0,0,255},{0,255,0,255},{0,0,255,255},{255,255,255,255}};
 const uint32_t rgb10[4]={0xc00003ff,0xc00ffc00,0xfff00000,0xa0080200};
 const uint8_t expected10[4][4]={{255,0,0,255},{0,255,0,255},{0,0,255,255},{128,128,128,170}};
 uint8_t actual[4][4]={{0}};
 layer_pixels_passed=1;
 for(int format=0;format<2;format++) for(int back=0;back<2;back++) {
  const uint32_t *input=format?rgb10:rgb1555;
  const uint8_t (*expected)[4]=format?expected10:expected1555;
  if(!hikaru_vendor_test_layer_pixels(layer_renderer,format,input,scene,back,1.0f,back?1.0f:0.0f,actual)) layer_pixels_passed=0;
  for(unsigned i=0;i<4;i++) for(unsigned c=0;c<4;c++) {
   int delta=(int)actual[i][c]-(int)expected[i][c];
   if(delta < -1 || delta > 1) { fprintf(stderr,"layer format %d back %d pixel %u channel %u: got %u expected %u\n",format,back,i,c,actual[i][c],expected[i][c]); layer_pixels_passed=0; }
  }
 }
 /* Selected layers also retain the existing front/back factor interpolation. */
 const uint8_t mixed1555[4][4]={{139,24,45,255},{12,151,45,255},{12,24,172,255},{139,151,172,255}};
 for(int back=0;back<2;back++) {
  if(!hikaru_vendor_test_layer_pixels(layer_renderer,0,rgb1555,scene,back,1.0f,0.5f,actual)) layer_pixels_passed=0;
  for(unsigned i=0;i<4;i++) for(unsigned c=0;c<4;c++) {
   int delta=(int)actual[i][c]-(int)mixed1555[i][c];
   if(delta < -1 || delta > 1) { fprintf(stderr,"selected layer back %d pixel %u channel %u: got %u expected %u\n",back,i,c,actual[i][c],mixed1555[i][c]); layer_pixels_passed=0; }
  }
 }
 /* Air Trix player select: opaque 1555 layers cover the scene; zero alpha
  * leaves it visible. Distinct first and last row pixels exercise both. */
 const uint8_t backdrop[4]={64,65,64,255};
 const uint32_t gray=0x8000 | 28 | (28 << 5) | (28 << 10);
 const uint32_t overlay[4]={gray,0,0x8000,gray};
 const uint8_t direct_expected[4][4]={{230,230,230,255},{64,65,64,255},{0,0,0,255},{230,230,230,255}};
 /* Layer 2 currently draws over layer 1; overlapping hardware priority has
  * not yet been established by a capture. */
 for(int slot=2;slot<=4;slot++) {
  if(!hikaru_vendor_test_layer_pixels(layer_renderer,0,overlay,backdrop,slot,1.0f,0.0f,actual)) layer_pixels_passed=0;
  for(unsigned i=0;i<4;i++) for(unsigned c=0;c<4;c++) {
   unsigned expected=slot==4?backdrop[c]:direct_expected[i][c];
   int delta=(int)actual[i][c]-(int)expected;
   if(delta < -1 || delta > 1) { fprintf(stderr,"direct slot %d pixel %u channel %u: got %u expected %u\n",slot,i,c,actual[i][c],expected); layer_pixels_passed=0; }
  }
 }
 const uint32_t transparent_color[4]={0x001f,0x03e0,0x7c00,0x7fff};
 const uint32_t half10=128 | (128 << 10) | (128 << 20);
 const uint32_t coverage10[4]={half10,half10 | 0x40000000u,half10 | 0x80000000u,half10 | 0xc0000000u};
 const uint8_t additive_backdrop[4]={64,65,64,0};
 const uint8_t expected_additive10[4][4]={{192,193,192,0},{192,193,192,255},{192,193,192,255},{192,193,192,255}};
 for(int slot=2;slot<=3;slot++) for(int format=0;format<2;format++) {
  if(!hikaru_vendor_test_layer_pixels(layer_renderer,format,format?coverage10:transparent_color,format?additive_backdrop:backdrop,slot,format?4.0f:1.0f,0.0f,actual)) layer_pixels_passed=0;
  for(unsigned i=0;i<4;i++) for(unsigned c=0;c<4;c++) {
   unsigned expected=format?expected_additive10[i][c]:backdrop[c];
   int delta=(int)actual[i][c]-(int)expected;
   if(delta < -1 || delta > 1) { fprintf(stderr,"layer blending format %d slot %d pixel %u channel %u: got %u expected %u\n",format,slot,i,c,actual[i][c],expected); layer_pixels_passed=0; }
  }
 }
}
static int routed_layers_passed;
static void routed_layers_probe(void) {
 /* Concrete visible output at each band; green is the 3D scene. */
 static const uint8_t expected[10][3][3]={
  {{255,0,0},{0,0,255},{255,0,0}}, /* both combined, transparent scene */
  {{0,255,0},{0,255,0},{0,255,0}}, /* opaque 3D covers combined */
  {{0,0,255},{0,0,255},{255,0,0}}, /* both direct: layer 1 on top */
  {{255,0,0},{0,0,255},{255,0,0}}, /* layer 0 direct */
  {{0,0,255},{0,0,255},{255,0,0}}, /* layer 1 direct */
  {{0,0,255},{0,0,255},{0,0,0}},   /* only layer 1 enabled */
  {{255,0,0},{0,0,0},{255,0,0}},   /* only layer 0 enabled */
  {{0,255,0},{0,255,0},{0,255,0}}, /* no layers */
  {{0,0,255},{0,0,255},{0,255,0}}, /* only direct layer 1 */
  {{127,128,0},{0,128,0},{127,128,0}}, /* half-alpha scene over combined */
 };
 uint8_t actual[10][3][4]={0};
 routed_layers_passed=hikaru_vendor_test_routed_layers(layer_renderer,actual);
 for(unsigned step=0;step<10;step++) for(unsigned x=0;x<3;x++) for(unsigned c=0;c<3;c++) {
  int d=(int)actual[step][x][c]-(int)expected[step][x][c];
  if(d < -1 || d > 1) {
   fprintf(stderr,"routed layer step %u band %u channel %u: got %u expected %u\n",step,x,c,actual[step][x][c],expected[step][x][c]);
   routed_layers_passed=0;
  }
 }
}
static int mesh_vao_passed;
static void mesh_vao_probe(void) { mesh_vao_passed = hikaru_vendor_test_mesh_vao_pixels(); }
static int unlit_material_passed;
static void unlit_material_probe(void) {
 /* Captured flat regions distinguish geometry colors from the colored
  * ambient modulation retained by textured Air Trix HUD elements. */
 static const struct {
  const char *name;
  uint32_t flags;
  uint8_t diffuse[3], ambient[3], texel[4], expected[3];
 } cases[]={
  {"white letter",0,{255,255,255},{128,128,128},{255,255,255,255},{255,255,255}},
  {"blue letter backing",0,{0,64,255},{128,128,128},{255,255,255,255},{0,64,255}},
  {"black shadow with pink ambient",0,{0,0,0},{255,96,192},{255,255,255,255},{0,0,0}},
  {"timer gray extrusion",0,{112,112,112},{128,128,128},{255,255,255,255},{112,112,112}},
  {"textured cream timer face",1u<<19,{96,96,96},{255,255,255},{255,244,204,255},{255,244,204}},
  {"textured green HUD",1u<<19,{255,255,255},{0,224,64},{255,255,255,255},{0,224,64}},
  {"textured orange HUD",1u<<19,{255,255,255},{255,128,0},{255,255,255,255},{255,128,0}},
  {"white letter bit18",1u<<18,{255,255,255},{128,128,128},{255,255,255,255},{255,255,255}},
  {"blue backing bit18",1u<<18,{0,64,255},{128,128,128},{255,255,255,255},{0,64,255}},
  {"green HUD bit18",(1u<<19)|(1u<<18),{255,255,255},{0,224,64},{255,255,255,255},{0,224,64}},
  {"alternate untextured color",1u<<20,{0,64,255},{128,128,128},{255,255,255,255},{128,128,128}},
  {"alternate color bit18",(1u<<20)|(1u<<18),{0,64,255},{128,128,128},{255,255,255,255},{128,128,128}},
  {"textured alternate green HUD",(1u<<19)|(1u<<20),{255,255,255},{0,224,64},{255,255,255,255},{0,224,64}},
  {"shade1 no-lights fallback",1u<<16,{0,64,255},{128,128,128},{255,255,255,255},{128,128,128}},
 };
 unlit_material_passed=1;
 for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
  uint8_t actual[4]={0};
  if(!hikaru_vendor_test_unlit_material_pixels(cases[i].flags,cases[i].diffuse,cases[i].ambient,cases[i].texel,actual)) {
   fprintf(stderr,"unlit %s: render failed\n",cases[i].name);unlit_material_passed=0;continue;
  }
  for(unsigned c=0;c<3;++c) {
   int delta=(int)actual[c]-(int)cases[i].expected[c];
   if(delta < -1 || delta > 1) {
    fprintf(stderr,"unlit %s channel %u: got %u expected %u\n",cases[i].name,c,actual[c],cases[i].expected[c]);
    unlit_material_passed=0;
   }
  }
 }
}
static int texture_cutout_passed;
static void texture_cutout_probe(void) {
 /* Gameplay mesh231: opaque, 881=000d0881, 2C1=404a02c1,
  * bank1 storage(68,864)=7fff: white RGB with alpha zero. C81=00200c81.
  * Test only zero rejection; the C81 threshold comparison remains unknown. */
 static const struct {
  const char *name; uint32_t flags;
  uint8_t diffuse[3], ambient[3], texel[4], expected[4]; float depth;
 } cases[]={
  {"captured opaque cutout",0x000d0881,{255,218,76},{178,178,178},{255,255,255,0},{17,34,51,255},0.75f},
  {"opaque texture positive alpha",0x000d0881,{255,218,76},{178,178,178},{255,255,255,255},{178,178,178,255},0.5f},
  {"positive alpha one is retained",0x000d0881,{255,218,76},{178,178,178},{255,255,255,1},{178,178,178,1},0.5f},
  {"captured green HUD",0x000c0881,{255,255,255},{90,240,6},{255,255,255,255},{90,240,6,255},0.5f},
  {"captured orange HUD",0x000c0881,{178,178,178},{255,178,0},{255,255,255,255},{255,178,0,255},0.5f},
  {"untextured primary ignores texture alpha",0,{0,64,255},{178,178,178},{255,255,255,0},{0,64,255,255},0.5f},
  {"untextured alternate ignores texture alpha",1u<<20,{0,64,255},{255,96,192},{255,255,255,0},{255,96,192,255},0.5f},
 };
 texture_cutout_passed=1;
 for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
  uint8_t actual[4]={0}; float depth=-1;
  if(!hikaru_vendor_test_material_depth_pixels(cases[i].flags,cases[i].diffuse,cases[i].ambient,cases[i].texel,actual,&depth)) {
   fprintf(stderr,"cutout %s: render failed\n",cases[i].name);texture_cutout_passed=0;continue;
  }
  for(unsigned c=0;c<4;++c) {
   int delta=(int)actual[c]-(int)cases[i].expected[c];
   if(delta < -1 || delta > 1) {
    fprintf(stderr,"cutout %s channel %u: got %u expected %u\n",cases[i].name,c,actual[c],cases[i].expected[c]);texture_cutout_passed=0;
   }
  }
  if(depth<cases[i].depth-0.0001f || depth>cases[i].depth+0.0001f) {
   fprintf(stderr,"cutout %s depth: got %.6f expected %.6f\n",cases[i].name,depth,cases[i].depth);texture_cutout_passed=0;
  }
 }
}
static int fog_cutout_passed;
static void fog_cutout_probe(void) {
 /* Fog must change RGB without changing texture coverage. The final zero
  * discard must also hold for the lighting/specular shader variant. */
 static const struct {
  const char *name; uint32_t flags; unsigned options;
  uint8_t texel[4], expected[4]; float depth;
 } cases[]={
  {"zero alpha",1u<<19,1,{255,255,255,0},{52,105,157,255},0.75f},
  {"partial alpha",1u<<19,1,{255,255,255,128},{121,153,185,128},0.5f},
  {"opaque control",1u<<19,1,{255,255,255,255},{121,153,185,255},0.5f},
  {"lit specular zero alpha",(1u<<19)|(1u<<16),3,{255,255,255,0},{52,105,157,255},0.75f},
 };
 const uint8_t diffuse[3]={255,218,76},ambient[3]={178,178,178};
 fog_cutout_passed=1;
 for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
  uint8_t actual[4]={0}; float depth=-1;
  if(!hikaru_vendor_test_material_fog_depth_pixels(cases[i].flags,diffuse,ambient,cases[i].texel,actual,&depth,cases[i].options)) {
   fprintf(stderr,"fog %s: render failed\n",cases[i].name);fog_cutout_passed=0;continue;
  }
  fprintf(stderr,"fog %s: RGBA(%u,%u,%u,%u), depth %.6f\n",cases[i].name,actual[0],actual[1],actual[2],actual[3],depth);
  for(unsigned c=0;c<4;++c) {
   int delta=(int)actual[c]-(int)cases[i].expected[c];
   if(delta < -1 || delta > 1) {
    fprintf(stderr,"fog %s channel %u: got %u expected %u\n",cases[i].name,c,actual[c],cases[i].expected[c]);fog_cutout_passed=0;
   }
  }
  if(depth<cases[i].depth-0.0001f || depth>cases[i].depth+0.0001f) {
   fprintf(stderr,"fog %s depth: got %.6f expected %.6f\n",cases[i].name,depth,cases[i].depth);fog_cutout_passed=0;
  }
 }
}
static int depth_probe_passed;
static void depth_probe(void) {
 const char *vs="#version 150\nvoid main(){vec2 p[3]=vec2[3](vec2(-1,-1),vec2(1,-1),vec2(0,1));gl_Position=vec4(p[gl_VertexID],0,1);}";
 const char *fs="#version 150\nout vec4 color;void main(){color=vec4(1,0,0,1);}";
 GLuint v=glCreateShader(GL_VERTEX_SHADER), f=glCreateShader(GL_FRAGMENT_SHADER), p=glCreateProgram(), vao;
 glShaderSource(v,1,&vs,NULL);glCompileShader(v);glShaderSource(f,1,&fs,NULL);glCompileShader(f);
 glAttachShader(p,v);glAttachShader(p,f);glLinkProgram(p);
 GLint linked=0;glGetProgramiv(p,GL_LINK_STATUS,&linked);
 glGenVertexArrays(1,&vao);glBindVertexArray(vao);glUseProgram(p);
 glViewport(0,0,640,480);hikaru_vendor_test_setup_scene_scissor();glDisable(GL_CULL_FACE);glDisable(GL_BLEND);
 glEnable(GL_DEPTH_TEST);glDepthFunc(GL_LEQUAL);glDepthMask(GL_TRUE);glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
 glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
 glDrawArrays(GL_TRIANGLES,0,3);
 unsigned char pixel[4]={0};glReadPixels(320,240,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
 GLdouble clear,range[2];glGetDoublev(GL_DEPTH_CLEAR_VALUE,&clear);glGetDoublev(GL_DEPTH_RANGE,range);
 depth_probe_passed=linked && pixel[0]>200 && pixel[1]<10 && pixel[2]<10 && clear==1.0 && range[0]==0.0 && range[1]==1.0;
 glDeleteVertexArrays(1,&vao);glDeleteProgram(p);glDeleteShader(v);glDeleteShader(f);
}
static void reset(void *p){(void)p;}
static int rd(void *p,unsigned size,uint32_t addr,uint64_t *v){(void)p;(void)size;(void)addr;(void)v;return -1;}
static uint8_t sound_pin=0xff;
static int sound_mask=1, sound_pending;
static int mie_rd(void *p,unsigned size,uint32_t addr,uint64_t *v){(void)p;if(size!=1||addr!=0x00800006)return -1;*v=sound_pin;return 0;}
static int mie_wr(void *p,unsigned size,uint32_t addr,uint64_t v){(void)p;if(size!=1||addr!=0x00800006)return -1;sound_pin=(uint8_t)v;return 0;}
static int mie_irq(void *p,int pending){(void)p;sound_pending=pending;return pending&&sound_mask;}
static int target_size(HikaruEngine *e, GLint w, GLint h) {
 hikaru_renderer_t *r=(hikaru_renderer_t *)e->machine->renderer; GLint old,tw,th;
 int ok=1;glGetIntegerv(GL_TEXTURE_BINDING_2D,&old);
 for(unsigned i=0;i<2;i++){glBindTexture(GL_TEXTURE_2D,r->framebuffer.cb[i]);
  glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_WIDTH,&tw);glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_HEIGHT,&th);ok &= tw==w&&th==h;}
 glBindTexture(GL_TEXTURE_2D,(GLuint)old);return ok;
}
static int back_target_clear(HikaruEngine *e) {
 hikaru_renderer_t *r=(hikaru_renderer_t *)e->machine->renderer;GLint old;unsigned char p[4]={1,1,1,1};
 glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&old);glBindFramebuffer(GL_READ_FRAMEBUFFER,r->framebuffer.fb[1]);
 glReadPixels(0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,p);glBindFramebuffer(GL_READ_FRAMEBUFFER,(GLuint)old);
 return p[0]==0&&p[1]==0&&p[2]==0&&p[3]==0;
}
static int wr(void *p,unsigned size,uint32_t addr,uint64_t v){(void)p;(void)size;(void)addr;(void)v;return -1;}
static int run(void *p,uint32_t cycles){(void)p;(void)cycles;return 0;}
static void input(void *p,const HikaruEngineInputs *i){(void)p;(void)i;}
typedef struct { uint8_t *ram[2]; uint64_t regs[2]; uint64_t cycles; int fail_board, detached, resets, pin, irq[2], detached_byte; } SoundProbe;
static int snd_attach(void *p,unsigned b,uint8_t *ram,size_t size){SoundProbe*s=p;if(b>1||size!=8u*1024*1024||!ram)return -1;if((int)b==s->fail_board)return -1;s->ram[b]=ram;return 0;}
static void snd_detach(void *p){SoundProbe*s=p;s->detached++;if(s->ram[0])s->detached_byte=s->ram[0][0x5c];s->ram[0]=s->ram[1]=NULL;}
static int snd_reset(void *p){((SoundProbe*)p)->resets++;return 0;}
static int snd_pin(void *p,int asserted){((SoundProbe*)p)->pin=asserted;return 0;}
static int snd_read(void *p,unsigned b,uint32_t off,unsigned size,uint64_t *value){SoundProbe*s=p;if(b>1||off!=4||size!=4)return -1;*value=s->regs[b];return 0;}
static int snd_write(void *p,unsigned b,uint32_t off,unsigned size,uint64_t value){SoundProbe*s=p;if(b>1||off!=4||size!=4)return -1;s->regs[b]=value;return 0;}
static int snd_advance(void *p,uint32_t cycles){((SoundProbe*)p)->cycles+=cycles;return 0;}
static int snd_irq(void *p,unsigned b){return ((SoundProbe*)p)->irq[b];}
int main(void){
 if(SDL_Init(SDL_INIT_VIDEO))return 10;
 SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_CORE); SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,3); SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,2);
 SDL_Window *window=SDL_CreateWindow("Hikaru smoke",0,0,640,480,SDL_WINDOW_OPENGL|SDL_WINDOW_HIDDEN); if(!window)return 11;
 SDL_GLContext context=SDL_GL_CreateContext(window); if(!context)return 12;
 if(!hikaru_vendor_test_shader_uniform_cache())return 50;
 if(!hikaru_vendor_test_vertex_chunks())return 51;
 GLuint host_vao; GLint restored_vao;
 glGenVertexArrays(1,&host_vao);glBindVertexArray(host_vao);
 hikaru_vendor_test_with_gl_guard(mesh_vao_probe);
 glGetIntegerv(GL_VERTEX_ARRAY_BINDING,&restored_vao);
 glBindVertexArray(0);glDeleteVertexArrays(1,&host_vao);
 if(!mesh_vao_passed || (GLuint)restored_vao!=host_vao)return 52;
 hikaru_vendor_test_with_gl_guard(unlit_material_probe);
 if(!unlit_material_passed)return 54;
 hikaru_vendor_test_with_gl_guard(texture_cutout_probe);
 if(!texture_cutout_passed)return 56;
 hikaru_vendor_test_with_gl_guard(fog_cutout_probe);
 if(!fog_cutout_passed)return 57;
 uint8_t *boot=calloc(1,0x200000),*program=calloc(1,0x800000),*data=calloc(1,0x8000000); if(!boot||!program||!data)return 13;
 boot[0]=0xfe;boot[1]=0xaf;boot[2]=0x09;boot[3]=0x00;memcpy(boot+0xd4,"SAMURAI BootROM Version 0.92",29);
 HikaruEngineRoms rom={boot,0x200000,program,0x800000,data,0x8000000}; HikaruEngineMie mie={NULL,reset,mie_rd,mie_wr,run,input,mie_irq}; HikaruEngineProtection protection={NULL,reset,rd,wr}; char error[256];
 GLuint fbo,tex,depth;glGenFramebuffers(1,&fbo);glBindFramebuffer(GL_FRAMEBUFFER,fbo);glGenTextures(1,&tex);glBindTexture(GL_TEXTURE_2D,tex);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,640,480,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,tex,0);
 glGenRenderbuffers(1,&depth);glBindRenderbuffer(GL_RENDERBUFFER,depth);glRenderbufferStorage(GL_RENDERBUFFER,GL_DEPTH_COMPONENT24,640,480);glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_RENDERBUFFER,depth);
 if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)return 14;
 glViewport(0,0,640,480);glClearColor(1,0,1,1);glClear(GL_COLOR_BUFFER_BIT);
 GLuint sentinel, unpack; glGenTextures(1,&sentinel); glActiveTexture(GL_TEXTURE7); glBindTexture(GL_TEXTURE_2D,sentinel);
 glGenBuffers(1,&unpack);glBindBuffer(GL_PIXEL_UNPACK_BUFFER,unpack);glBufferData(GL_PIXEL_UNPACK_BUFFER,4,NULL,GL_STATIC_DRAW);
 glPixelStorei(GL_UNPACK_ROW_LENGTH,17);glPixelStorei(GL_UNPACK_SKIP_ROWS,5);glPixelStorei(GL_UNPACK_SKIP_PIXELS,3);
 glColorMask(GL_FALSE,GL_TRUE,GL_FALSE,GL_TRUE);glFrontFace(GL_CW);glBlendEquation(GL_FUNC_REVERSE_SUBTRACT);
 glClearDepth(0.0);glDepthRange(0.2,0.4);
 glViewport(3,5,320,240);glScissor(1000,1000,1,1);glEnable(GL_SCISSOR_TEST);glEnable(GL_RASTERIZER_DISCARD);glEnable(GL_STENCIL_TEST);
 hikaru_vendor_test_fail_renderer_init();
 HikaruEngine *e=hikaru_engine_create(&rom,&mie,&protection,fbo,640,480,0,error,sizeof(error));
 if(e || !strstr(error,"injected renderer initialization failure"))return 23;
 e=hikaru_engine_create(&rom,&mie,&protection,fbo,640,480,0,error,sizeof(error)); if(!e){fprintf(stderr,"create failed: %s\n",error);return 15;}
 if(!hikaru_vendor_test_idma_completion(e->machine))return 59;
 layer_renderer=e->machine->renderer;
 hikaru_vendor_test_with_gl_guard(frame_state_growth_probe);
 if(!frame_state_growth_passed)return 60;
 hikaru_vendor_test_with_gl_guard(layer_pixels_probe);
 if(!layer_pixels_passed)return 53;
 hikaru_vendor_test_with_gl_guard(routed_layers_probe);
 if(!routed_layers_passed)return 58;
 uint8_t *saved=malloc(HIKARU_ENGINE_BRAM_SIZE), *check=malloc(HIKARU_ENGINE_BRAM_SIZE);
 if(!saved || !check)return 28;
 for(size_t i=0;i<HIKARU_ENGINE_BRAM_SIZE;++i)saved[i]=(uint8_t)(i*37u+i/256u);
 if(hikaru_engine_get_bram(NULL,check,HIKARU_ENGINE_BRAM_SIZE)!=-1 ||
    hikaru_engine_set_bram(e,saved,HIKARU_ENGINE_BRAM_SIZE-1)!=-1 ||
    hikaru_engine_set_bram(e,saved,HIKARU_ENGINE_BRAM_SIZE) ||
    hikaru_engine_get_bram(e,check,HIKARU_ENGINE_BRAM_SIZE) ||
    memcmp(saved,check,HIKARU_ENGINE_BRAM_SIZE))return 29;
 GLint viewport[4],scissor[4],active,binding,skip,pixelskip,pbo,face,blend; GLboolean mask[4];
 glGetIntegerv(GL_VIEWPORT,viewport);glGetIntegerv(GL_ACTIVE_TEXTURE,&active);glGetIntegerv(GL_TEXTURE_BINDING_2D,&binding);glGetIntegerv(GL_UNPACK_SKIP_ROWS,&skip);glGetBooleanv(GL_COLOR_WRITEMASK,mask);
 glGetIntegerv(GL_UNPACK_SKIP_PIXELS,&pixelskip);glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING,&pbo);glGetIntegerv(GL_FRONT_FACE,&face);glGetIntegerv(GL_BLEND_EQUATION_RGB,&blend);
 if(viewport[0]!=3||viewport[1]!=5||active!=GL_TEXTURE7||binding!=(GLint)sentinel||skip!=5||pixelskip!=3||pbo!=(GLint)unpack||face!=GL_CW||blend!=GL_FUNC_REVERSE_SUBTRACT||mask[0]||!mask[1]||!glIsEnabled(GL_SCISSOR_TEST)||!glIsEnabled(GL_RASTERIZER_DISCARD)||!glIsEnabled(GL_STENCIL_TEST))return 16;
 HikaruEngineInputs in={{0},{0},0,0,{0}};
 hikaru_t *board=(hikaru_t *)e->machine;
 sound_pin=0; board->porta_m|=0x80;
 if(hikaru_engine_run_frame(e,&in) || !(board->porta_m&0x80))return 33;
 sound_pin=0x20; board->porta_m|=0x80;
 int result=hikaru_engine_run_frame(e,&in);if(result){fprintf(stderr,"frame failed: %s\n",hikaru_engine_last_error(e));return 17;}
 if(board->porta_m&0x80)return 34;
 if(!target_size(e,640,480) || hikaru_engine_set_internal_resolution(e,0)==0 || hikaru_engine_set_internal_resolution(e,5000)==0)return 35;
 if(hikaru_engine_set_internal_resolution(e,960) || hikaru_engine_run_frame(e,&in) || !target_size(e,1280,960) || !back_target_clear(e))return 36;
 if(hikaru_engine_set_internal_resolution(e,480) || hikaru_engine_run_frame(e,&in) || !target_size(e,640,480))return 37;
 unsigned char pixel[4];glReadPixels(320,240,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
 if(pixel[0]==255&&pixel[1]==0&&pixel[2]==255)return 21;
 glGetIntegerv(GL_VIEWPORT,viewport);glGetIntegerv(GL_ACTIVE_TEXTURE,&active);glGetIntegerv(GL_TEXTURE_BINDING_2D,&binding);glGetIntegerv(GL_UNPACK_SKIP_ROWS,&skip);glGetBooleanv(GL_COLOR_WRITEMASK,mask);
 glGetIntegerv(GL_UNPACK_SKIP_PIXELS,&pixelskip);glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING,&pbo);glGetIntegerv(GL_FRONT_FACE,&face);glGetIntegerv(GL_BLEND_EQUATION_RGB,&blend);
 if(viewport[0]!=3||viewport[1]!=5||active!=GL_TEXTURE7||binding!=(GLint)sentinel||skip!=5||pixelskip!=3||pbo!=(GLint)unpack||face!=GL_CW||blend!=GL_FUNC_REVERSE_SUBTRACT||mask[0]||!mask[1]||!glIsEnabled(GL_SCISSOR_TEST)||!glIsEnabled(GL_RASTERIZER_DISCARD)||!glIsEnabled(GL_STENCIL_TEST))return 18;
 hikaru_vendor_test_with_gl_guard(depth_probe);
 GLdouble clear,range[2];glGetDoublev(GL_DEPTH_CLEAR_VALUE,&clear);glGetDoublev(GL_DEPTH_RANGE,range);
 glGetIntegerv(GL_SCISSOR_BOX,scissor);
 if(!depth_probe_passed || clear!=0.0 || range[0]<0.199 || range[0]>0.201 || range[1]<0.399 || range[1]>0.401 || scissor[0]!=1000 || scissor[1]!=1000 || scissor[2]!=1 || scissor[3]!=1)return 27;
 /* 640x360 must pillarbox a 4:3 image, clearing both sides of the target. */
 hikaru_engine_resize(e,fbo,640,360);
 if(hikaru_engine_run_frame(e,&in))return 24;
 glReadPixels(10,180,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
 if(pixel[0]||pixel[1]||pixel[2])return 25;
 glReadPixels(320,180,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
 if(pixel[0]==255&&pixel[1]==0&&pixel[2]==255)return 26;
 hikaru_engine_destroy(e);
 glGetIntegerv(GL_ACTIVE_TEXTURE,&active);glGetIntegerv(GL_TEXTURE_BINDING_2D,&binding);glGetIntegerv(GL_UNPACK_SKIP_ROWS,&skip);
 if(active!=GL_TEXTURE7||binding!=(GLint)sentinel||skip!=5)return 22;
 /* A synthetic backend proves both real-board bus paths and lifecycle while
  * leaving audio synthesis to the separately implemented backend. */
 e=hikaru_engine_create(&rom,&mie,&protection,fbo,640,480,0,error,sizeof(error)); if(!e)return 38;
 SoundProbe probe={0};probe.fail_board=1;
 HikaruEngineSound sound={&probe,snd_attach,snd_detach,snd_reset,snd_pin,snd_read,snd_write,snd_advance,snd_irq};
 if(hikaru_engine_set_sound(e,&sound)==0 || probe.detached!=1 || probe.ram[0] || probe.ram[1])return 39;
 probe.fail_board=-1;
 if(hikaru_engine_set_sound(e,&sound) || !probe.ram[0] || !probe.ram[1] || probe.ram[0]==probe.ram[1] || probe.resets!=1)return 40;
 board=(hikaru_t *)e->machine; uint32_t reg=0;
 if(vk_device_put(board->aica_m,4,0x0c700004,0x11223344) || vk_device_put(board->aica_s,4,0x0d700004,0x55667788) ||
    vk_device_get(board->aica_m,4,0x0c700004,&reg) || reg!=0x11223344 ||
    vk_device_get(board->aica_s,4,0x0d700004,&reg) || reg!=0x55667788)return 41;
 if(vk_device_get(board->aica_m,2,0x0c707fff,&reg)==0 ||
    vk_device_put(board->aica_s,4,0x0d707ffe,0)==0)return 49;
 if(vk_device_put(board->aica_m,1,0x0c80005c,0x5a) || probe.ram[0][0x5c]!=0x5a ||
    vk_device_put(board->aica_s,1,0x0d80005c,0xa5) || probe.ram[1][0x5c]!=0xa5)return 42;
 uint8_t status=0;if(vk_device_get(board->aica_m,1,0x0c80005c,&status) || status!=0x5a)return 43;
 if(vk_device_put(board->mie,1,0x00800006,0) || probe.pin!=1 ||
    vk_device_put(board->mie,1,0x00800006,0x20) || probe.pin!=0)return 48;
 sound_pin=0;probe.irq[1]=1;sound_mask=0;board->porta_m|=0xc0;
 if(hikaru_engine_run_frame(e,&in) || probe.pin!=1 || probe.cycles!=544u*(200000000u/(60u*544u)) ||
    (board->porta_m&0x80) || !sound_pending || ((sh4_t*)board->sh_m)->intc.irqs[SH4_IESOURCE_IRL2].state!=VK_IRQ_STATE_CLEAR)return 44;
 sound_mask=1;
 if(hikaru_engine_run_frame(e,&in) || ((sh4_t*)board->sh_m)->intc.irqs[SH4_IESOURCE_IRL2].state!=VK_IRQ_STATE_RAISED)return 50;
 sound_pin=0x20;probe.irq[1]=0;
 if(hikaru_engine_run_frame(e,&in) || probe.pin!=0 || !(board->porta_m&0x80) || sound_pending)return 45;
 if(hikaru_engine_set_sound(e,&sound)==0)return 46;
 hikaru_engine_destroy(e);if(probe.detached!=2 || probe.detached_byte!=0x5a || probe.ram[0] || probe.ram[1])return 47;
 boot[0]=0;boot[1]=0;
 e=hikaru_engine_create(&rom,&mie,&protection,fbo,640,480,0,error,sizeof(error)); if(!e){fprintf(stderr,"second create failed: %s\n",error);return 19;}
 if(hikaru_engine_get_bram(e,check,HIKARU_ENGINE_BRAM_SIZE))return 30;
 for(size_t i=0;i<HIKARU_ENGINE_BRAM_SIZE;++i)if(check[i])return 31;
 if(hikaru_engine_set_bram(e,saved,HIKARU_ENGINE_BRAM_SIZE) ||
    hikaru_engine_get_bram(e,check,HIKARU_ENGINE_BRAM_SIZE) ||
    memcmp(saved,check,HIKARU_ENGINE_BRAM_SIZE))return 32;
 free(saved);free(check);
 if(hikaru_engine_run_frame(e,&in)==0 || !hikaru_engine_last_error(e)[0])return 20;
 hikaru_engine_destroy(e);glBindBuffer(GL_PIXEL_UNPACK_BUFFER,0);glDeleteBuffers(1,&unpack);glDeleteTextures(1,&sentinel);glDeleteRenderbuffers(1,&depth);glDeleteTextures(1,&tex);glDeleteFramebuffers(1,&fbo);free(boot);free(program);free(data);SDL_GL_DeleteContext(context);SDL_DestroyWindow(window);SDL_Quit();puts("synthetic GL lifecycle passed");return 0;
}

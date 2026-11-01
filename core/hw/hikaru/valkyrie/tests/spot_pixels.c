// SPDX-License-Identifier: GPL-3.0-or-later
/* Exact saved Air Trix samples and synthetic boundaries exercise the actual
 * command decoder and production GLSL. Q1.15 cutoff packing is established
 * from the game builder. Compact-float reconstruction, LUT endpoints and
 * bank orientation remain the documented experimental renderer contract. */
#define GL_SILENCE_DEPRECATION
#include <SDL.h>
#include <OpenGL/gl3.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <stddef.h>
#include "mach/hikaru/hikaru-gpu-private.h"
int hikaru_vendor_test_spot_pixels(const hikaru_light_t *,const float[3],const float[3],int,unsigned,unsigned,const uint16_t[16],uint8_t[4]);
int hikaru_vendor_test_shader_uniform_cache(void);
int hikaru_vendor_test_decode_451(uint32_t,uint32_t,hikaru_light_t *);
void hikaru_vendor_test_spot_upload_counts(unsigned *, unsigned *);
void hikaru_vendor_test_spot_upload_counts_reset(void);
void hikaru_vendor_test_spot_force_replacement(void);
void hikaru_vendor_test_uniform_cache_bypass(bool);
void hikaru_vendor_test_with_gl_guard(void (*draw)(void));
static int failed;
static unsigned slot,curve;
static const uint16_t captured_table[16]={51,102,204,307,409,512,563,614,665,716,768,819,870,921,972,1024};
static const uint16_t nonlinear_table[16]={0,0,0,0,0,0,0,256,768,768,768,768,768,768,768,1024};
static const uint16_t *active_table=captured_table;
static void decode(hikaru_light_t *light,uint32_t payload,int disabled){
 if(!hikaru_vendor_test_decode_451(disabled?0x01000451:0x451,payload,light)){failed++;return;}
 int ok=light->has_451 && light->has_specular==!disabled;
 for(unsigned i=0;i<4;++i)if(((const uint8_t *)light)[offsetof(hikaru_light_t, specular)+i] != ((payload>>(8*i))&255))ok=0;
 if(!ok){fprintf(stderr,"451 decoder payload%08x or enabled flag FAIL\n",payload);failed++;}
}

static void check(const char *name,const hikaru_light_t *light,const float position[3],const float normal[3],int unlit,const uint8_t expected[3]) {
 uint8_t actual[4]={0};
 if(!hikaru_vendor_test_spot_pixels(light,position,normal,unlit,slot,curve,active_table,actual)){fprintf(stderr,"%s draw failed\n",name);failed++;return;}
 int ok=actual[3]==255;
 for(int c=0;c<3;c++){int d=(int)actual[c]-expected[c];if(d < -2 || d > 2)ok=0;}
 printf("%s: RGB %u,%u,%u expected %u,%u,%u %s\n",name,actual[0],actual[1],actual[2],expected[0],expected[1],expected[2],ok?"PASS":"FAIL");
 if(!ok)failed++;
}
static void expect_uploads(const char *name,unsigned expected_params,unsigned expected_tables) {
 unsigned params,tables; hikaru_vendor_test_spot_upload_counts(&params,&tables);
 if(params!=expected_params || tables!=expected_tables) {
  fprintf(stderr,"%s: spot uploads params=%u tables=%u expected %u,%u\n",name,params,tables,expected_params,expected_tables);failed++;
 }
}
static void cache_probe(void) {
 hikaru_light_t light={0};const float pos[3]={0,0,-10},normal[3]={0,0,1};
 const uint8_t full[3]={200,100,50},white[3]={255,255,255};
 uint16_t table[16];memcpy(table,captured_table,sizeof table);active_table=table;slot=curve=0;
 light.has_position=light.has_direction=1;light.direction[2]=-1;
 light.attenuation[0]=-1;light.attenuation[1]=-100;
 light.diffuse[0]=200;light.diffuse[1]=100;light.diffuse[2]=50;
 decode(&light,0,0);hikaru_vendor_test_spot_upload_counts_reset();
 check("cache initial",&light,pos,normal,0,full);expect_uploads("initial",1,1);
 check("cache repeated values",&light,pos,normal,0,full);expect_uploads("repeat",1,1);
 check("cache program switch control",&light,pos,normal,1,white);
 check("cache switch back",&light,pos,normal,0,full);expect_uploads("switch back",1,1);
 decode(&light,0x00100000,0);
 check("cache parameters change",&light,pos,normal,0,full);expect_uploads("params",2,1);
 table[0]++;
 check("cache table change",&light,pos,normal,0,full);expect_uploads("table",2,2);
 light.has_451=0;
 check("cache absent 451",&light,pos,normal,0,full);expect_uploads("absent",3,2);
 light.has_451=1;
 check("cache enable same table",&light,pos,normal,0,full);expect_uploads("enable same",4,2);
 light.has_451=0;
 check("cache absent again",&light,pos,normal,0,full);expect_uploads("absent again",5,2);
 table[1]++;
 check("cache absent table change",&light,pos,normal,0,full);expect_uploads("absent table",5,2);
 light.has_451=1;
 check("cache reenable changed table",&light,pos,normal,0,full);expect_uploads("enable changed",6,3);
 /* The disabled bit also changes the legacy specular shader variant. */
 decode(&light,0x00100000,1);
 check("cache disabled variant",&light,pos,normal,0,full);expect_uploads("disabled variant",7,3);
 decode(&light,0x00100000,0);
 check("cache enabled variant restored",&light,pos,normal,0,full);expect_uploads("variant restored",7,3);
 /* A second spotlight variant must retain its own program slot on cache hits. */
 slot=1;
 check("cache second spotlight program",&light,pos,normal,0,full);expect_uploads("second program",8,4);
 check("cache second spotlight hit",&light,pos,normal,0,full);expect_uploads("second hit",8,4);
 hikaru_vendor_test_uniform_cache_bypass(true);
 check("cache location bypass hit",&light,pos,normal,0,full);expect_uploads("bypass hit",8,4);
 hikaru_vendor_test_uniform_cache_bypass(false);
 slot=0;
 check("cache first spotlight restored",&light,pos,normal,0,full);expect_uploads("first restored",8,4);
 slot=1;
 check("cache second spotlight restored",&light,pos,normal,0,full);expect_uploads("second restored",8,4);
 slot=0;active_table=captured_table;
}
static void replacement_probe(void) {
 hikaru_light_t light={0};const float pos[3]={0,0,-10},normal[3]={0,0,1};const uint8_t full[3]={200,100,50};
 light.has_position=light.has_direction=1;light.direction[2]=-1;
 light.attenuation[0]=-1;light.attenuation[1]=-100;
 light.diffuse[0]=200;light.diffuse[1]=100;light.diffuse[2]=50;
 slot=curve=0;active_table=captured_table;decode(&light,0,0);
 check("cache replacement prime",&light,pos,normal,0,full);
 hikaru_vendor_test_spot_upload_counts_reset();hikaru_vendor_test_spot_force_replacement();
 check("cache linked-program replacement",&light,pos,normal,0,full);expect_uploads("replacement",1,1);
}
static void run(void) {
 int cache_ok=hikaru_vendor_test_shader_uniform_cache();
 printf("production uniform cache: %s\n",cache_ok?"PASS":"FAIL");
 if(!cache_ok)failed++;
 cache_probe();
 {hikaru_light_t light={0};
 const float pos[3]={0.356456961f,-1.16138943f,-45.3150313f}, normal[3]={0.19999989f,0.289478837f,0.929371739f};
 const float lp[3]={-148.103729f,215.048843f,77.0369034f},ld[3]={48.1948547f,-230.997681f,-209.461975f},atten[2]={-0.0250000004f,-400.0};
 const uint16_t rgb[3]={156,144,121}; const uint8_t expected[3]={0,0,0};
 memcpy(light.position,lp,sizeof lp);memcpy(light.direction,ld,sizeof ld);memcpy(light.attenuation,atten,sizeof atten);memcpy(light.diffuse,rgb,sizeof rgb);
 light.has_position=1;light.has_direction=1;light.att_type=0;light.has_specular=0;
 decode(&light,0x003c7642,0);
 check("captured22.5-degree cone rejects25.05-degree sample",&light,pos,normal,0,expected);
 }
 {hikaru_light_t light={0};
 const float pos[3]={-1.32913608f,-0.676665852f,-47.9179479f}, normal[3]={-0.894063474f,0.228729249f,-0.348578463f};
 const float lp[3]={-31.8713303f,199.078506f,-188.381592f},ld[3]={-28.1948605f,-204.214127f,235.666367f},atten[2]={-0.0250000004f,-400.0};
 const uint16_t rgb[3]={156,144,121}; const uint8_t expected[3]={34,31,26};
 memcpy(light.position,lp,sizeof lp);memcpy(light.direction,ld,sizeof ld);memcpy(light.attenuation,atten,sizeof atten);memcpy(light.diffuse,rgb,sizeof rgb);
 light.has_position=1;light.has_direction=1;light.att_type=0;light.has_specular=0;
 decode(&light,0x003c7642,0);
 check("captured22.5-degree cone illuminates18.59-degree penumbra",&light,pos,normal,0,expected);
 }
 {hikaru_light_t light={0};
 const float pos[3]={-1.77031479f,-1.47222201f,-32.791154f}, normal[3]={-0.938997115f,0.296541512f,0.136056429f};
 const float lp[3]={-10.1986761f,19.8942986f,-4.23469925f},ld[3]={10.2688437f,-22.4858189f,-30.7749462f},atten[2]={-1.0,-10000.0};
 const uint16_t rgb[3]={255,214,173}; const uint8_t expected[3]={127,107,86};
 memcpy(light.position,lp,sizeof lp);memcpy(light.direction,ld,sizeof ld);memcpy(light.attenuation,atten,sizeof atten);memcpy(light.diffuse,rgb,sizeof rgb);
 light.has_position=1;light.has_direction=1;light.att_type=0;light.has_specular=0;
 decode(&light,0x005f7c33,0);
 check("captured14-degree center unchanged",&light,pos,normal,0,expected);
 }
 {hikaru_light_t light={0};
 const float pos[3]={0,0,-10},normal[3]={0,0,1};
 const uint8_t full[3]={200,100,50},black[3]={0,0,0},quarter[3]={50,25,13},half[3]={100,50,25};
 light.has_position=light.has_direction=1;
 light.attenuation[0]=-1;light.attenuation[1]=-100;
 light.diffuse[0]=200;light.diffuse[1]=100;light.diffuse[2]=50;
 light.direction[0]=sinf(17.0f*3.14159265358979323846f/180.0f);light.direction[2]=-cosf(17.0f*3.14159265358979323846f/180.0f);
 decode(&light,0x005f7c33,0);
 check("authored14-degree cone rejects17degrees",&light,pos,normal,0,black);
 light.direction[0]=0;light.direction[2]=-1;
 decode(&light,0x003c8000,0);
 check("Q1.15 cutoff exactly1 has zero angular response",&light,pos,normal,0,black);
 decode(&light,0x00000000,0);
 check("cutoff0 scale1 LUT endpoint1",&light,pos,normal,0,full);
 light.direction[0]=1;light.direction[2]=0;
 check("cutoff0 LUT endpoint0",&light,pos,normal,0,black);
 active_table=nonlinear_table;
 decode(&light,0x00104000,0); /* cutoff1/2, scale2 */
 light.direction[0]=sqrtf(1.0f-0.75f*0.75f);light.direction[2]=-0.75f;
 check("nonlinear LUT exact interior sample",&light,pos,normal,0,quarter);
 light.direction[0]=sqrtf(1.0f-0.765625f*0.765625f);light.direction[2]=-0.765625f;
 check("nonlinear LUT interpolates adjacent samples",&light,pos,normal,0,half);
 slot=3;curve=3;decode(&light,0x06104000,0);
 check("explicit bank3 curve3 upload and full451 payload",&light,pos,normal,0,half);
 slot=0;curve=0;
 decode(&light,0x003c8000,1);
 check("disabled451 leaves positional Lambert unaffected",&light,pos,normal,0,full);
 light.has_451=0;light.has_specular=0;
 check("absent451 leaves positional Lambert unaffected",&light,pos,normal,0,full);
 active_table=captured_table;
 }
 {hikaru_light_t light={0}; const float pos[3]={0,0,-10},normal[3]={0,0,1};const uint8_t expected[3]={64,128,192},white[3]={255,255,255};
 light.has_position=1;light.diffuse[0]=64;light.diffuse[1]=128;light.diffuse[2]=192;light.attenuation[0]=-1;light.attenuation[1]=-100;
 check("point-light control",&light,pos,normal,0,expected);
 light.has_position=0;light.has_direction=1;light.direction[2]=1;light.attenuation[0]=1;light.attenuation[1]=1;
 check("directional-light control",&light,pos,normal,0,expected);
 check("unlit control",&light,pos,normal,1,white);
 }
 replacement_probe();
}
int main(void){
 if(SDL_Init(SDL_INIT_VIDEO))return 10;
 SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_CORE);SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,3);SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,2);
 SDL_Window *w=SDL_CreateWindow("Hikaru lighting pixels",0,0,9,9,SDL_WINDOW_OPENGL|SDL_WINDOW_HIDDEN);if(!w)return 11;
 SDL_GLContext context=SDL_GL_CreateContext(w);if(!context)return 12;
 hikaru_vendor_test_with_gl_guard(run);
 SDL_GL_DeleteContext(context);SDL_DestroyWindow(w);SDL_Quit();
 printf("lighting pixel cases: %s (%d failed)\n",failed?"FAIL":"PASS",failed);
 return failed?1:0;
}

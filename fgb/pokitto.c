
#include "defs.h"
#include "regs.h"

#include "input.h"
#include "rc.h"
#include "pcm.h"
#include "mem.h"
#include "hw.h"
#include "rtc.h"
#include "sound.h"

//#include "save.h"

char*romName, romSize ;

extern void	(*error)(const char * txt) ;

//-------------------  change default option here !!
// char mReset=0,mGamma=(200-(int)(GAMMA*100))/2;
// char mFSkip = 2 ; //,mGBPal=0,mDebug=0;
// char mLoadState=0,mSaveState=0,mLoadMovie=0,mSaveMovie=0,mSnapShot=0;
// char mStretch=0,mVSync=0,mBuffering=0,mType;
// char frequency=0, lastfreq=0, menuabout=0 ;
 // ------------------------------------

struct pcm pcm;

#define BLACK_COLOR 255

//int frames_rendered = 0, frames_displayed = 0 ;

//struct fb fb;


extern int norender;
extern uint32_t pt_count; // tick

//int frameTimeDiff = 0;
//u_int32_t frameTime = 0;

void vid_begin(void){
 //y_refresh_gp32=0;
 //frameTime = pt_count;
}

/*
void print(char *msg);
void printxy(char *msg,uint8_t x, uint8_t y);
char bf[96];
*/

void vid_end(void){
//  static u_int32_t frameTime = pt_count - 17 ;
/*  frameTime = frameTime ? pt_count - frameTime : 17 ;

  int diff = frameTime - 17 ; // 1000/60 -> 16.66
  frameTimeDiff += diff;
//  sprintf(bf,"%lu %lu %i %i",pt_count,frameTime,diff,frameTimeDiff);
//  print(bf);

  sprintf(bf,"%lu",pt_count);
  print(bf);
  sprintf(bf,"%lu",frameTime);
  printxy(bf,10,10);

//  sprintf(bf,"%i",diff);
//  print(bf);

//  sprintf(bf,"%i",frameTimeDiff);
//  print(bf);

  norender = frameTimeDiff > 0 ;
  frameTime = pt_count;*/
//  norender = (((frames_rendered++)%(mFSkip+1))!=0);
//  if(!norender) frames_displayed++;
}

uint8_t readKeys( void );

void ev_poll(void){
  static uint8_t last = 0;
  hw.pad = readKeys();

  if( hw.pad == last ) return;
  last = hw.pad;
  pad_refresh();
}

void *sys_timer() { return 0; }//pt_count; }
int sram_load(void){return 0;}

void vid_setpal(int i, int r, int g, int b){

}

void vid_init(void){
/*
       fb.w = 166;
       fb.h = 144;
       fb.pelsize = 1;
       fb.pitch = 166;
       fb.ptr = NULL;//(unsigned char *)&vidram[0]; //0x0C7B4000;
       fb.enabled = 1;
       fb.dirty = 0; ///1????
       fb.yuv = 0;

       fb.indexed = 1;
       fb.cc[0].l = 0; fb.cc[0].r = 0;
       fb.cc[1].l = 0; fb.cc[1].r = 0;
       fb.cc[2].l = 0; fb.cc[2].r = 0;
*/
}

uint8_t soundBf[ 11025/60 ];

void pcm_init( void ){
  pcm.hz = 11025;// 22050;
  pcm.stereo = 0;
  pcm.len = 11025/60;//22050/60;
  pcm.buf = 0;//soundBf;//gm_malloc(pcm.len);
  pcm.pos = 0;
  //memset(pcm.buf, 0, pcm.len);
  //Pokitto::soundInit()
}

void pcm_close( void ){

}

int pcm_submit(){
	if (!pcm.buf) return 0;
	if (pcm.pos < pcm.len) return 1;

	pcm.pos = 0;
	return 1;
}

void fgb( void *_rom ){
  //Gamma=GAMMA;
  //vid_init();pcm_init();init_video();
  pcm_init();
  loader_init((char*)_rom);
//  emu_running=1;
  emu_reset();
  //frames_rendered = frames_displayed = 0;
	emu_run();
  pcm_close();
  loader_unload();
}

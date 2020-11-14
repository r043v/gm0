


#include "defs.h"
#include "regs.h"
#include "hw.h"
#include "cpu.h"
#include "mem.h"
#include "lcd.h"
#include "rc.h"
#include "sound.h"



static int framelen = 16743;
static int framecount;

rcvar_t emu_exports[] =
{
	RCV_INT("framelen", &framelen),
	RCV_INT("framecount", &framecount),
	RCV_END
};

void emu_init(){

}


/*
 * emu_reset is called to initialize the state of the emulated
 * system. It should set cpu registers, hardware registers, etc. to
 * their appropriate values at powerup time.
 */

void emu_reset()
{
	hw_reset();
	lcd_reset();
	cpu_reset();
	mbc_reset();
	sound_reset();
}

void emu_step()
{
	cpu_emulate(cpu.lcdc);
}

/* This mess needs to be moved to another module; it's just here to
 * make things work in the mean time. */

void *sys_timer(void);

void print(char *msg);
void printxy(char *msg,uint8_t x, uint8_t y);
char bf[96];

extern int norender;
extern uint32_t pt_count; // tick

int frameTimeDiff = 0;
uint32_t frameTime = 0, frameStart;


//frameTime = frameTime ? pt_count - frameTime : 17 ;

//int diff = frameTime - 17 ; // 1000/60 -> 16.66
//frameTimeDiff += diff;
//  sprintf(bf,"%lu %lu %i %i",pt_count,frameTime,diff,frameTimeDiff);
//  print(bf);

/*sprintf(bf,"%lu",pt_count);
print(bf);
sprintf(bf,"%lu",frameTime);
printxy(bf,10,10);
*/
//  sprintf(bf,"%i",diff);
//  print(bf);

//  sprintf(bf,"%i",frameTimeDiff);
//  print(bf);

//norender = frameTimeDiff > 0 ;
//frameTime = pt_count;
/*
#define SYST_CVR (*(uint32_t *)0xE000E018)
#define SYST_RVR (*(uint32_t *)0xE000E014)
#define SYST_CSR (*(uint32_t *)0xE000E010)
*/
void emu_run()
{	void *timer = sys_timer();
	int delay;
	vid_begin();
	lcd_begin();
//	SYST_CSR = 1; // 36mhz counter, no interupt
	norender=0;
	int maxfs = 3;
	u_int32_t ticks = 0, frame = 0;
	int n=0;
	u_int32_t zmin=50, zmax=0;
//	int time;
	u_int32_t fs=0, mfs=0;
	while( 1 ){
		//SYST_CSR = 1;
//		SYST_CVR = 0; // reset counter value
		//frameTime = pt_count;
//		frameStart = *SYST_CVR & 0x00FFFFFF ;
		cpu_emulate(2280);
		while (R_LY > 0 && R_LY < 144)
			cpu_emulate(cpu.lcdc);
		vid_end();
		pk_rtc_tick();
		//doevents();
		ev_poll();
		vid_begin();
//		frame++;
/*			time = frameStart - ( *SYST_CVR & 0x00FFFFFF ) ;
			if( time < 0 ) time += 0x00ffffff ;
			frameTime = ( (u_int32_t)time ) >> 15 ;
*/
//			if( zmin > frameTime ) zmin=frameTime;
//			if( zmax < frameTime ) zmax=frameTime;
//			ticks += frameTime;
//			int diff = frameTime - 17 ;//17;// - 12;//18;// 18 ; // 1024/60 -> 17, (35840*17)>>15
//			frameTimeDiff += diff;
//			norender = frameTimeDiff > 0;

			if( fs ){
				fs--;
				norender = 1;
			} else {
				//mfs ^= 1;
				//fs = mfs;// + 1 ;
				fs=2;
				norender = 0;
			}

/*			if( norender ){
				if( maxfs-- == 0 ){
					norender=0;
					maxfs = 3;
				}
			} else maxfs = 3;
*/
//norender = 1;
//if( norender ){
//sprintf(bf,"%5lu %3lu %5i %i", ticks, frameTime, diff, frameTimeDiff );
//sprintf(bf,"%4lu %4lu %1u %4lu", zmin, zmax, norender, frameTime );
//sprintf(bf,"%i", frameTimeDiff );
//print(bf);
//}
//for( n = 0; n<100 ; n++ ){
//	SYST_CVR = 0;
//	while( ( (SYST_CVR) & 0x00ffffff ) < 20<<15 );
//}
		//} else norender = 1;

		//frameTime = SYST_CVR >> 15;//pt_count - frameTime ;

		/*if( frameTime )
			sprintf(bf,"%3lu %lu",frameTime, pt_count );
		else
			sprintf(bf,"zero");
		*/
		//sprintf(bf,"%lu",frameTime);
		//print(bf);
	};
}

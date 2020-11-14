//#define GP32
//#define GP32_ADS

#ifndef __DEFS_H__
#define __DEFS_H__


//#ifdef GP32

#define IS_LITTLE_ENDIAN

#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "ctype.h"

#define INLINE static __inline

//#endif

#undef byte
#undef word

#ifdef IS_LITTLE_ENDIAN
#define LO 0
#define HI 1
#else
#define LO 1
#define HI 0
#endif

//ifndef GP32
typedef unsigned char byte;
//endif

typedef unsigned char un8;
typedef unsigned short un16;
//typedef short u16;
typedef unsigned int un32;
typedef unsigned char u8;
typedef signed char n8;
typedef signed short n16;
typedef signed int n32;

//ifndef GP32
typedef un16 word;
//endif
typedef word addr;

void init_video(void);
void vid_preinit(void);
void vid_init(void);
void vid_begin(void);
void vid_end(void);
void vid_close(void);
void vid_settitle(void);
void vid_setpal(int i, int r, int g, int b);
void ev_poll(void);
void GetInfo(byte *header);
static void audio_callback(void *blah, byte *stream, int len);
void pcm_init(void);
int pcm_submit(void);
void pcm_close(void);
void *sys_timer(void);
int   sys_elapsed(void);
void  sys_sleep(int us);
void  sys_checkdir(char *path, int wr);
void  sys_initpath(char *exe);
void  sys_sanitize(char *s);
//char *strdup(char *s1);
//int strcasecmp(char *s1, char *s2);
byte *loadfile(FILE *fp, int *len);
int sram_load(void);
int sram_save(void);
void state_save(int n);
void state_load(int n);
//void *gm_realloc(void *x, int s);


static void initmem(void *mem, int size);
byte *loadfile(FILE *f, int *len);
static void inflate_callback(byte b);
static byte *decompress(byte *data, int *len);
int rom_load(char*);
void pk_rtc_save(void);
void pk_rtc_load(void);
void loader_unload(void);
static char *base(char *s);
static char *ldup(char *s);
static void cleanup(void);
void loader_init(char *s);
void die(char *fmt, ...);
void vid_end(void);
void doevents(void) ;
static byte bestmatch(int c);
static void makecourse(int c, byte n);
static byte findcourse(int c);
void pal_lock(byte n);
void FUX0R_PAL(void);
byte pal_getcolor(int c, int r, int g, int b);
void pal_release(byte n);
void pal_expire(void);
void pal_set332(void);

void pk_rtc_latch(byte b);
void pk_rtc_write(byte b);
void pk_rtc_tick(void);
void pk_rtc_save_internal(FILE *f);
void pk_rtc_load_internal(FILE *f);
void exit (int);
void ev_poll(void);
void vid_close(void);
void rc_dokey(int key, int st);

#include "rc.h"
int rc_command(char *line);
void rc_exportvars(rcvar_t *vars);
static int cmd_set(int argc, char **argv);
static int cmd_bind(int argc, char **argv);
static int cmd_unbind(int argc, char **argv);
static int cmd_unbindall(int argc, char **argv); //gp32
static int cmd_source(int argc, char **argv);
static int cmd_quit(int argc, char **argv); //gp32
static int cmd_reset(int argc, char **argv); //gp32
static int cmd_savestate(int argc, char **argv);
static int cmd_loadstate(int argc, char **argv);
int rc_setvar_n(int i, int c, char **v);
int rc_setvar(char *name, int c, char **v);
void rc_exportvars(rcvar_t *vars);
int rc_bindkey(char *keyname, char *cmd);
int rc_unbindkey(char *keyname);
void rc_unbindall(void);
void rc_dokey(int key, int st);
void rc_export(rcvar_t *v);
void rc_exportvars(rcvar_t *vars);
int rc_findvar(char *name);
int my_atoi(const char *s);
int rc_setvar_n(int i, int c, char **v);
int rc_setvar(char *name, int c, char **v);
void *rc_getmem_n(int i);
void *rc_getmem(char *name);
int rc_getint_n(int i);
int *rc_getvec_n(int i);
char *rc_getstr_n(int i);
int rc_getint(char *name);
int *rc_getvec(char *name);
char *rc_getstr(char *name);

void refresh_1(byte *dest, byte *src, byte *pal, int cnt);
void refresh_2(un16 *dest, byte *src, un16 *pal, int cnt);
void refresh_3(byte *dest, byte *src, un32 *pal, int cnt);
void refresh_4(un32 *dest, byte *src, un32 *pal, int cnt);
void refresh_1_2x(byte *dest, byte *src, byte *pal, int cnt);
void refresh_2_2x(un16 *dest, byte *src, un16 *pal, int cnt);
void refresh_3_2x(byte *dest, byte *src, un32 *pal, int cnt);
void refresh_4_2x(un32 *dest, byte *src, un32 *pal, int cnt);
void refresh_2_3x(un16 *dest, byte *src, un16 *pal, int cnt);
void refresh_3_3x(byte *dest, byte *src, un32 *pal, int cnt);
void refresh_4_3x(un32 *dest, byte *src, un32 *pal, int cnt);
void refresh_3_4x(byte *dest, byte *src, un32 *pal, int cnt);
void refresh_4_4x(un32 *dest, byte *src, un32 *pal, int cnt);

void updatepatpix(void);
void tilebuf(void);
void bg_scan(void);
void wnd_scan(void);
static void blendcpy(byte *dest, byte *src, byte b, int cnt);
static int priused(void *attr);
void bg_scan_pri(void);
void wnd_scan_pri(void);
void bg_scan_color(void);
void wnd_scan_color(void);
static void recolor(byte *buf, byte fill, int cnt);
void spr_count(void);
void spr_enum(void);
void spr_scan(void);
void lcd_begin(void);
void lcd_refreshline(void);
static void updatepalette(int i);
void pal_write(int i, byte b);
void pal_write_dmg(int i, int mapnum, byte d);
void vram_write(int a, byte b);
void vram_dirty(void);
void pal_dirty(void);
void lcd_reset(void);
/*
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
void strncpy(void * dst, void * src, int size);
char *path_search(char *name, char *mode, char *path);
char *strdup(char *s1) ;
*/

void hw_interrupt(byte i, byte mask);
void hw_dma(byte b);
void hw_hdma_cmd(byte c);
void hw_hdma(void);
void pad_refresh(void);
void pad_press(byte k);
void pad_release(byte k);
void pad_set(byte k, int st);
void hw_reset(void);

void cpu_reset(void);
void div_advance(int cnt);
void timer_advance(int cnt);
void lcdc_advance(int cnt);
void sound_advance(int cnt);
void cpu_timers(int cnt);
int cpu_idle(int max);
int cpu_emulate(int cycles);
int cpu_step(int max);

void stat_trigger(void);
void stat_write(byte b);
static void stat_change(int stat);
void lcdc_change(byte b);
void lcdc_trans(void);

void debug_disassemble(addr a, int c);
void emu_init(void);
void emu_reset(void);
void emu_step(void);
void emu_run(void);

void init_exports(void);
void show_exports(void);

int rc_sourcefile(char *filename);
int splitline(char **argv, int max, char *line);

int unzip (const unsigned char *data, long *p, void (* callback) (unsigned char d));

static void s1_freq_d(int d);
static void s1_freq(void);
static void s2_freq(void);
static void s3_freq(void);
static void s4_freq(void);
void sound_dirty(void);
void sound_off(void);
void sound_reset(void);
void sound_mix(void);
byte sound_read(byte r);
void s1_init(void);
void s2_init(void);
void s3_init(void);
void s4_init(void);
void sound_write(byte r, byte b);
void alternateMain(char * tripowa);

#endif

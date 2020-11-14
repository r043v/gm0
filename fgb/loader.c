
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
//#include "time.h"

#include "defs.h"
#include "regs.h"
#include "mem.h"
#include "hw.h"
//#include "rtc.h"
#include "rc.h"

#define u8 unsigned char

const static u8 mbc_table[256] =
{
	0, 1, 1, 1, 0, 2, 2, 0, 0, 0, 0, 0, 0, 0, 0, 3,
	3, 3, 3, 3, 0, 0, 0, 0, 0, 5, 5, 5, MBC_RUMBLE, MBC_RUMBLE, MBC_RUMBLE, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,

	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,

	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,

	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, MBC_HUC3, MBC_HUC1
};

/*static int rtc_table[256] =
{
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1,
	1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0
};*/

const static u8 batt_table[256] =
{
	0, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 0, 1, 0, 0,
	1, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 0,
	0
};

const static unsigned short romsize_table[256] =
{
	2, 4, 8, 16, 32, 64, 128, 256, 512,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 128, 128, 128
	/* 0, 0, 72, 80, 96  -- actual values but bad to use these! */
};

const static u8 ramsize_table[256] =
{
	1, 1, 1, 4, 16,
	4 /* FIXME - what value should this be?! */
};

static char *romfile;
static char *sramfile;
//static char *rtcfile;
static char *saveprefix;

static char *savename;
static char *savedir;

static int saveslot;

static int forcebatt, nobatt;
static int forcedmg=0, gbamode;

static int memfill = 0, memrand = -1; //-1,-1

static void initmem(void *mem, int size)
{	memset(mem, memfill, size);
}

/*
#ifdef GP32
       byte *loadfile(FILE *f, int *len);
#else
static byte *loadfile(FILE *f, int *len)
{
	int c, l = 0, p = 0;
	byte *d = 0, buf[512];

	for(;;)
	{
		c = fread(buf, 1, sizeof buf, f);
		if (c <= 0) break;
		l += c;
		d = realloc(d, l);
		if (!d) return 0;
		memcpy(d+p, buf, c);
		p += c;
	}
	*len = l;
	return d;
}
#endif
*/
/*
static byte *inf_buf;
static int inf_pos, inf_len;

static void inflate_callback(byte b)
{
	if (inf_pos >= inf_len)
	{
		inf_len += 512;
		inf_buf = realloc(inf_buf, inf_len);
		if (!inf_buf) die("out of memory inflating file @ %d bytes\n", inf_pos);
	}
	inf_buf[inf_pos++] = b;
}

static byte *decompress(byte *data, int *len)
{
	unsigned long pos = 0;
	if (data[0] != 0x1f || data[1] != 0x8b)
		return data;
	inf_buf = 0;
	inf_pos = inf_len = 0;
	if (unzip(data, (long*)&pos, inflate_callback) < 0)
		return data;
	*len = inf_pos;
	return inf_buf;
}
*/

int rom_load(char*myrom)
{	byte c, *data, *header;
	//int rlen;
  header = data = (byte*)myrom ;
	memcpy(rom.name, header+0x0134, 16);
	if (rom.name[14] & 0x80) rom.name[14] = 0;
	if (rom.name[15] & 0x80) rom.name[15] = 0;
	rom.name[16] = 0;
	c = header[0x0147];
	mbc.type = mbc_table[c];
	mbc.batt = (batt_table[c] && !nobatt) || forcebatt;
	//rtc.batt = ( c == 15 || c == 16 ) ? 1 : 0; //rtc_table[c];
	mbc.romsize = romsize_table[header[0x0148]];
	if( mbc.romsize > 152 ) mbc.romsize = 152 ; // pokitto rom limit
	mbc.ramsize = 1; // force 8k //ramsize_table[header[0x0149]];
	//rlen = 16384 * mbc.romsize;
	rom.bank = (void *)data;
	//ram.sbank = (void*)gbram;// malloc(8192 * mbc.ramsize);
	initmem(ram.sbank, 8192 * mbc.ramsize);
	initmem(ram.ibank, 4096 * MAX_RAM_BANK); // force dmg, 2x4k ram bank // *8
	mbc.rombank = 1;
	mbc.rambank = 0;
	c = header[ 0x0143 ];
//	hw.cgb = 0;//((c == 0x80) || (c == 0xc0)) && !forcedmg;
//	hw.gba = 0;// (hw.cgb && gbamode);
	return 0;
}

/*
void rtc_save()
{
	FILE *f;
	if (!rtc.batt) return;
	if (!(f = fopen(rtcfile, "wb"))) return;
	rtc_save_internal(f);
	fclose(f);
}

void rtc_load()
{
	FILE *f;
	if (!rtc.batt) return;
	if (!(f = fopen(rtcfile, "r"))) return;
	rtc_load_internal(f);
	fclose(f);
}
*/

void loader_unload()
{
/*
#ifndef GP32
	sram_save();
	if (romfile) free(romfile);
	if (sramfile) free(sramfile);
	if (saveprefix) free(saveprefix);
#endif
	if (rom.bank) free(rom.bank);
	if (ram.sbank) free(ram.sbank);
*/
	romfile = sramfile = saveprefix = 0;
	rom.bank = 0;
	//ram.sbank = 0;
	mbc.type = mbc.romsize = mbc.ramsize = mbc.batt = 0;
}
/*
static char *base(char *s)
{
	char *p;
	p = strrchr(s, '/');
	if (p) return p+1;
	return s;
}

static char *ldup(char *s)
{
	int i;
	char *n, *p;
	p = n = malloc(strlen(s));
	for (i = 0; s[i]; i++) if (isalnum(s[i])) *(p++) = tolower(s[i]);
	*p = 0;
	return n;
}
*/
static void cleanup()
{	sram_save();
	//rtc_save();
	/* IDEA - if error, write emergency savestate..? */
}

void loader_init(char *s)
{	char *name, *p;
	romfile = s;
	rom_load(s);
  sram_load();
}
/*
rcvar_t loader_exports[] =
{
	RCV_STRING("savedir", &savedir),
	RCV_STRING("savename", &savename),
	RCV_INT("saveslot", &saveslot),
	RCV_BOOL("forcebatt", &forcebatt),
	RCV_BOOL("nobatt", &nobatt),
	RCV_BOOL("forcedmg", &forcedmg),
	RCV_BOOL("gbamode", &gbamode),
	RCV_INT("memfill", &memfill),
	RCV_INT("memrand", &memrand),
	RCV_END
};*/

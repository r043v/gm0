

#ifndef __LOADER_H__
#define __LOADER_H__


typedef struct loader_s
{
	char *rom;
	char *base;
	char *sram;
	char *state;
	int ramloaded;
} loader_t;


extern loader_t loader;


int rom_load(char*,int);
//int rom_load(const unsigned char*myrom);
int sram_load();
int sram_save();



#endif

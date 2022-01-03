//char dbg[512];

#ifndef _gdlText_
#define _gdlText_

#include "gdl.h"
#include <stdio.h>
#include <stdarg.h>


#define CHAR_PADDING 0 /* padding between chars */
#define CHAR_SPACE 4 /* space char width */
#define textBfSize 64

void drawText_(const char *txt,int x, int y, int align, clDeep **font, u8 start);
void drawText(const char *txt,int x, int y);
void setGdlfont(clDeep ** font);
//void setGdlfont(clDeep ** font, u32 frmNb);
clDeep ** getGdlfont(void);
void prints(int x,int y,const char * format, ...);
void drawInt(int n, int x, int y, const char *method, int align, clDeep **font, int start);

#endif

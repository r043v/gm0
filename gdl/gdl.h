
#ifndef _gdl_
#define _gdl_

#include "pokitto.h"

//#include <Arduino.h>
//#undef min
//#undef max

//	return ((c.r & 0xF8) << 8) | ((c.g & 0xFC) << 3) | (c.b >> 3);
#define _rgb565(r,g,b) (((r & 0b11111000) << 8) | ((g & 0b11111100) << 3) | (b >> 3))
//#define rgb565(r,g,b) ( (r << 8) | (g << 3) | (b >> 3) )
//#define RGB_COLOR16(r,g,b)   ( ((r<<8) & 0xF800) | ((g << 3)&0x07E0) | (b>>3) )
#define rgb565(r,g,b) ((uint16_t)((r << 11) | (g << 5) | b))

#define u32 uint32_t
#define u16 uint16_t
#define u8 uint8_t


#define clDeep u16
#define clDeepDec  1
#define clDeepSze  2
#define deepByte    16

/*
#define clDeep u8
#define clDeepDec 0
#define clDeepSze 1
#define deepByte  8
*/

extern u32 tick;

#define WIDTH 160
#define HEIGHT 144

extern u32 bufWidth, bufHeight;


#include "gfm.h"
#include "gfa.h"
#include "text.h"
//#include "map.h"
//#include "rate.h"
//#include "misc.h"
//#include "btn.h"

#endif

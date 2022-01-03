
#ifndef _gdlGfm2_
#define _gdlGfm2_

#include "gdl.h"
#include <string.h> // memcpy

//#define clDeep u8

typedef union gfmHeader {
  u16 raw;
  struct {
    u8 raw1;
    u8 raw2;
  };
  struct {
    u16 largeSize : 1 ; // 1B or 2B for size
    u16 largeInfo : 1 ; // jump&data length, short 0 (max 15), long 1 (max 255)
    u16 deep : 3 ; // 1/4/8/32/16b color deep 0:1 1:4 2:8 3:32 4:16
    u16 palShipped : 1 ;
    u16 lineOffsetTable : 1 ; // present or not
    u16 largeOffset : 1 ; // 1B or 2B offset
    u16 : 8 ; // reserved
  };
} gfmHeader ;

void blitGfm( u8*gfm, clDeep*screen ) ;

extern clDeep *framebuffer ;

#endif

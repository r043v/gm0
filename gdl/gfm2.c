
#include "gdl.h"

u8 * gfmPal = 0;
clDeep gfmPal16[16] = { 1,2,3,0 };

u8 framebuffer8[ SCREEN_WIDTH * 8 ];
clDeep *framebuffer = (clDeep*)framebuffer8;

void blitGfm( u8*gfm, clDeep*screen ){
  // header
  union gfmHeader * h = (union gfmHeader*)gfm ;
  gfm += 2;

//  screen += *gfm++ ; // real pos x
  gfm++;
  screen += (*gfm++) * SCREEN_WIDTH ; // real pos y
  u8 sx = *gfm++; // real sx
  u8 sy = *gfm++; // real sy
  if( !sy ) return;

  if( h->lineOffsetTable ){
    gfm += sy; // skip
  }

  if( h->palShipped ){
    gfmPal = gfm + 1 ;
    gfm += ( (*gfm) + 1 ); // skip
  }

  clDeep *s = screen ;
  u32 y = sy;
  while( 1 ){ // y
    u8 x = *gfm++;
    while( x-- ){ // x
      register u8 p = *gfm++;
      screen += p >> 4 ; // jump
      u8 l = p & 0xf ; // px length
      while( l > 1 ){
        p = *gfm++;
        *screen++ = gfmPal16[ p >> 4  ] ; // px 1
        *screen++ = gfmPal16[ p & 0xf ] ; // px 2
        l -= 2;
      };

      if( l ) *screen++ = gfmPal16[ *gfm++ ] ;
    };

    if( !--y ) break;
    s += SCREEN_WIDTH ;
    screen = s ;
  }

}

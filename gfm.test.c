
#include "./font.gfm2.h"
#include "stdio.h"

#define SCREEN_WIDTH 160

#define u32 unsigned int
#define u16 unsigned short
#define u8 unsigned char
#define clDeep u8

clDeep gfmPal16[16] = { 1,2,3,0 };

union gfmHeader {
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
  };
};

void logGfm( u8*gfm, clDeep*screen ){
  // header
  union gfmHeader * h = (union gfmHeader*)gfm ;
  gfm += 2;

  u8 px = *gfm++ ; // real pos x
  u8 py = *gfm++ ; // real pos y
  u8 sx = *gfm++; // real sx
  u8 sy = *gfm++; // real sy

  printf("\n%ux%u - position %u %u\n",sx,sy,px,py);

  if( h->lineOffsetTable ){
    printf("line offset here\n");
    gfm += sy;
  }

  if( h->palShipped ){
    printf("pal here\n");
    //gfmPal = gfm+1;
    gfm += *gfm;
    gfm++;
  }

  printf("\n");

  clDeep *s = screen ;
  u32 y = sy;
  while( 1 ){ // y
    u8 x = *gfm++;
    printf(" -- %u colons -- 0x%02X\n",x,*gfm);
    while( x-- ){ // x
      register u8 p = *gfm++;
      u8 j = p >> 4 ;
//      while(j--) printf("-");
      u8 l = p & 0xf ;
      printf("jump %u, length %u ",j,l);
      while( l > 1 ){
        p = *gfm++;
        u8 p1 = p >> 4  ;
        u8 p2 = p & 0xf ;
        printf("%u%u",p1,p2);
        l -= 2;
      };

      if( l ){
        u8 p1 = *gfm++ ;
        printf("%u",p1);
      }

      printf("\n");
      //if( ! --x ) break;
    };
    printf("\n");
    if( !--y ) break;
    s += SCREEN_WIDTH ;
    screen = s ;
  }

}

void blitGfm( u8*gfm, clDeep*screen ){
  // header
  union gfmHeader * h = (union gfmHeader*)gfm ;
  gfm += 2;

  u8 px = *gfm++ ; // real pos x
  u8 py = *gfm++ ; // real pos y
  u8 sx = *gfm++; // real sx
  u8 sy = *gfm++; // real sy

  if( !sy ) return;

  if( h->lineOffsetTable ){
    gfm += sy;
  }

  if( h->palShipped ){
    //gfmPal = gfm+1;
    gfm += *gfm;
    gfm++;
  }

  printf("\n");

  clDeep *s = screen ;
  u32 y = sy ;

  for(u8 n=0;n<py;n++) printf("-\n");

  while( 1 ){ // y
    u8 x = *gfm++;
    for(u8 n=0;n<px;n++) printf("-");
//    printf(" -- %u colons -- 0x%02X\n",x,*gfm);
    while( x-- ){ // x
      register u8 p = *gfm++;
      u8 j = p >> 4 ;
      while(j--) printf(" ");
      u8 l = p & 0xf ;
      //printf("jump %u, length %u ",j,l);
      while( l > 1 ){
        p = *gfm++;
        u8 p1 = p >> 4  ;
        u8 p2 = p & 0xf ;
        printf("%x%x",p1,p2);
        l -= 2;
      };

      if( l ){
        u8 p1 = *gfm++ ;
        printf("%x",p1);
      }

//      printf("\n");
      //if( ! --x ) break;
    };
    printf("\n");
    if( !--y ) break;
    s += SCREEN_WIDTH ;
    screen = s ;
  }

}


u8 s[512];

int main(int argc, char const *argv[]) {
  //u8 * g = font86;
  //logGfm(g, s);
  for( u8 n=0;n<91;n++ ){
    printf("*** %u ***\n",n);
    blitGfm((u8*)font[n], s);
  }
  //blitGfm((u8*)font[3], s);
  return 0;
}

/*
unsigned char font64[18] = {
  0x4, 0xB3, // header
  0x3, 0x5, 0x3, 0x3, // pos & size
  0x1, // 1 colon
  0x3, // jump 0 3px
  0x0, 0x0, // px
  0x1, // 1 colon
  0x3, // jump 0 3px
  0x1, 0x0, // px
  0x1, // 1 colon
  0x3, // jump 0 3px
  0x0, 0x0 // px
} ;
*/

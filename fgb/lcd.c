
#include "defs.h"
#include "regs.h"
#include "hw.h"
#include "mem.h"
#include "lcd.h"
#include "rc.h"
//#include "fb.h"
#include "lcd.h"
//#include "pokitto.h"

struct lcd lcd;
struct scan scan;

#define L (scan.l) /* line */
#define X (scan.x) /* screen position */
#define Y (scan.y)
#define S (scan.s) /* tilemap position */
#define T (scan.t)
#define U (scan.u) /* position within tile */
#define V (scan.v)
#define WX (scan.wx)
#define WY (scan.wy)
#define WT (scan.wt)
#define WV (scan.wv)

//#define DEF_PAL { 0x98d0e0, 0x68a0b0, 0x60707C, 0x2C3C3C }
//static int dmg_pal[4][4] = { DEF_PAL, DEF_PAL, DEF_PAL, DEF_PAL };

/* */

const u_int32_t palettes[] = {
  // grayscale
  0x73ae0,0x4e7a0,0x25460,0x8518,  // bg
  0x73ae0,0x4e7a0,0x25460,0x8518,  // sprite pal 0
  0x73ae0,0x4e7a0,0x25460,0x8518, // sprite pal 1
  0x73ae0,0x4e7a0,0x25460,0x8518, // wd

  0x73ae0,0x4e7a0,0x25460,0x8518,  // bg
  0x3fff0,0x330b0,0x15650,0x4718,  // sprite pal 0
  0x3bfa8,0x26b78,0x15438,0x7fff8, // sprite pal 1
  0x73ae0,0x4e7a0,0x25460,0x8518, // wd

  0xce08,0x1d620,0x53428,0x7bc68,0xce08,0x1d620,0x53428,0x7bc68,0xce08,0x1d620,0x53428,0x7bc68,0xce08,0x1d620,0x53428,0x7bc68,
  0x0,0x3cc00,0x7ea60,0x7fff8,0x0,0x3cc00,0x7b0a0,0x7fff8,0x0,0x3cc00,0x7ea60,0x7fff8,0x0,0x3cc00,0x7ea60,0x7fff8,
  0x0,0x44e38,0x7e080,0x7fff8,0x0,0x6c000,0x7e080,0x7fff8,0x0,0x2000,0x3bf28,0x7fff8,0x0,0xf0,0x32890,0x7fff8,
  0x2cc08,0x41a20,0x66680,0x7f9b8,0x0,0x35428,0x52048,0x7fff8,0x0,0x3cc00,0x7ea60,0x7fff8,0x0,0x3cc00,0x7ea60,0x7fff8,
  0x0,0x52080,0x29560,0x7fd90,0x0,0x6c000,0x7e080,0x7ff00,0x0,0x2000,0x6e068,0x7fff8,0x0,0x14a40,0x2e2a8,0x7fff8,
  0x7fff8,0x7e930,0x12500,0x21830,0x0,0x6c000,0x7e080,0x7fff8,0x7ff00,0x3cc10,0x3bf28,0x7fff8,0x0,0x75b08,0x7fff8,0x7fff8,
  0x625c8,0x21440,0x4c980,0x4310,0x625c8,0x18c88,0x4c980,0x4310,0x0,0x10820,0xa668,0x7ebd0,0x0,0x1c6b0,0x74000,0x7fff8,
  0x0,0x2800,0x7c000,0x7f610,0x7df00,0x47680,0x10920,0x0,0x54ff8,0x0,0x7ff78,0x7df00,0x2bf08,0x75b08,0x7fff8,0x7fff8,
  0x1408,0x7cc00,0x7ff70,0x1edf0,0x0,0x7ff00,0x5d800,0x7fff8,0x5c000,0x7c000,0x7b7d0,0x7fff8,0x0,0x75b08,0x7fff8,0x7fff8,
  0x20e18,0x31628,0x56760,0x736a0,0x0,0x6c000,0x7e080,0x7fff8,0x0,0x2000,0x56760,0x7fff8,0x0,0x78700,0x7fc00,0x7fff8,
  0x10038,0x49c00,0x7e540,0x7f8b0,0x0,0x1cd0,0xe6f8,0x7fdc0,0x10038,0x2c0b0,0x514f8,0x7fff8,0x1480,0x11fb8,0x370f8,0x7fff8,
  0x0,0x5a248,0x73270,0x5bae8,0x0,0x6c000,0x7e080,0x7fff8,0x70718,0x110b8,0x77690,0x7fff8,0x1da00,0x63918,0x7bf98,0x7fff8
};

u_int32_t * palette = (u_int32_t*)palettes;
extern u_int8_t * framebuffer;
//uint8_t framebuffer[ /* 220 * 8 * 2 */ 160*8 /* *4 */ ];
u_int8_t gfxLineBuffer[8];
u_int8_t bgDrawLimitX ; // if bg is partially draw (covered by window)
u_int8_t LastLYdraw;

u_int8_t bgColorTable[4] = { 0,0,0,0 }, wdColorTable[4] = { 0,0,0,0 }, bgCurrentPal = 0;
u_int8_t spriteColorTable[2][4] = { { 0,0,0,0 }, { 0,0,0,0 } }, spriteCurrentPal[2] = { 0, 0 };

uint8_t gfxLineBuffer[8];

//void write_command_16( uint16_t );
//void write_data_16( uint16_t );
void drawBackground( void );
void drawWindow( void );
void drawSprites( void );
void flushScanline( void );
void cqsort( int*f, int*l );

#define bgDisabledColor 0

void clearFramebuffer( void ){
   u_int32_t *p = (u_int32_t*)framebuffer;
  do {
    *p++ = bgDisabledColor; *p++ = bgDisabledColor;
    *p++ = bgDisabledColor; *p++ = bgDisabledColor;
    *p++ = bgDisabledColor; *p++ = bgDisabledColor;
    *p++ = bgDisabledColor; *p++ = bgDisabledColor;
  } while( p != (u_int32_t*)&framebuffer[ 40 /* 160 */ ] );
}

void SetScanline( void ){
  volatile uint32_t *SET = (uint32_t *) 0xA0002200;

  write_command_16(0x20);

  write_data_16(L+((L+3)>>2)-4);
  write_command_16(0x21);
  write_data_16(10);

  write_command_16(0x22);

  // CLR_CS_SET_CD_RD_WR;
  SET[0] = 1 << 2;
  SET[1] = 1 << 24;
  SET[1] = 1 << 12;
}

/* */

void lcd_begin()
{
/*	if (fb.indexed)
	{
		if (rgb332) pal_set332();
		else pal_expire();
	}
	while (scale * 160 > fb.w || scale * 144 > fb.h) scale--;
	vdest = fb.ptr + ((fb.w*fb.pelsize)>>1)
		- (80*fb.pelsize) * scale
		+ ((fb.h>>1) - 72*scale) * fb.pitch;
	WY = R_WY;
*/
	WY = R_WY;
}

int norender = 0;

void lcd_refreshline(){
	if( norender || !LCD_ENABLE ) return ; // skip this frame or lcd off

    // recompute bg pal if need
    if( bgCurrentPal != R_BGP ){
      bgCurrentPal = R_BGP;
      bgColorTable[0] = bgCurrentPal & 3;
      wdColorTable[0] = bgColorTable[0] + 12;
      bgColorTable[1] = (bgCurrentPal >> 4) & 3;
      wdColorTable[1] = bgColorTable[1] + 12;
      bgColorTable[2] = (bgCurrentPal >> 2) & 3;
      wdColorTable[2] = bgColorTable[2] + 12;
      bgColorTable[3] = (bgCurrentPal >> 6) & 3;
      wdColorTable[3] = bgColorTable[3] + 12;
    }

	L = R_LY;

	if( L < 2 || L > 154 ) return;

	X = R_SCX;
	Y = (R_SCY + L) & 0xff;

	S = X >> 3;
	T = Y >> 3;
	U = X & 7;
	V = Y & 7;

	WX = R_WX - 7;

	if( WINDOW_ENABLE
		&& L >= WY
		&& WX < 160
		&& WX >= 0
	){

		//if (WY>L || WY<0 || WY>143 || WX<-7 || WX>159 || !(R_LCDC&0x20))
		//	WX = 160;
		WT = (L - WY) >> 3;
		WV = (L - WY) & 7;

		bgDrawLimitX = WX ; // bg hide when window start
		if( bgDrawLimitX != 0 ){ // background is visible
			BG_ENABLE ? drawBackground() : clearFramebuffer() ;
		}

		drawWindow();
	} else {
		bgDrawLimitX = 160; // full line
		BG_ENABLE ? drawBackground() : clearFramebuffer() ;
	}

	if( SPRITE_ENABLE ) drawSprites();

	if( LastLYdraw != L - 1 ) SetScanline();

  //if( L & 1 ) memset( framebuffer, 0, 160*4 );

	flushScanline();
	if( !( L & 3 ) ) flushScanline();

  LastLYdraw = L;
}

static void updatepalette(int i)
{
/*
	int c, r, g, b, y, u, v, rr, gg;

	c = (lcd.pal[i<<1] | ((int)lcd.pal[(i<<1)|1] << 8)) & 0x7FFF;
	r = (c & 0x001F) << 3;
	g = (c & 0x03E0) >> 2;
	b = (c & 0x7C00) >> 7;
	r |= (r >> 5);
	g |= (g >> 5);
	b |= (b >> 5);

	if (usefilter && (filterdmg || hw.cgb))
	{
		rr = ((r * filter[0][0] + g * filter[0][1] + b * filter[0][2]) >> 8) + filter[0][3];
		gg = ((r * filter[1][0] + g * filter[1][1] + b * filter[1][2]) >> 8) + filter[1][3];
		b = ((r * filter[2][0] + g * filter[2][1] + b * filter[2][2]) >> 8) + filter[2][3];
		r = rr;
		g = gg;
	}

	if (fb.yuv)
	{
		y = (((r *  263) + (g * 516) + (b * 100)) >> 10) + 16;
		u = (((r *  450) - (g * 377) - (b *  73)) >> 10) + 128;
		v = (((r * -152) - (g * 298) + (b * 450)) >> 10) + 128;
		if (y < 0) y = 0; if (y > 255) y = 255;
		if (u < 0) u = 0; if (u > 255) u = 255;
		if (v < 0) v = 0; if (v > 255) v = 255;
		PAL4[i] = (y<<fb.cc[0].l) | (y<<fb.cc[3].l)
			| (u<<fb.cc[1].l) | (v<<fb.cc[2].l);
		return;
	}

	if (fb.indexed)
	{
		pal_release(PAL1[i]);
		c = pal_getcolor(c, r, g, b);
		PAL1[i] = c;
		PAL2[i] = (c<<8) | c;
		PAL4[i] = (c<<24) | (c<<16) | (c<<8) | c;
		return;
	}

	r = (r >> fb.cc[0].r) << fb.cc[0].l;
	g = (g >> fb.cc[1].r) << fb.cc[1].l;
	b = (b >> fb.cc[2].r) << fb.cc[2].l;
	c = r|g|b;

	switch (fb.pelsize)
	{
	case 1:
		PAL1[i] = c;
		PAL2[i] = (c<<8) | c;
		PAL4[i] = (c<<24) | (c<<16) | (c<<8) | c;
		break;
	case 2:
		PAL2[i] = c;
		PAL4[i] = (c<<16) | c;
		break;
	case 3:
	case 4:
		PAL4[i] = c;
		break;
	}
*/
}

void pal_write(int i, byte b)
{
/*	if (lcd.pal[i] == b) return;
	lcd.pal[i] = b;
	updatepalette(i>>1);
*/
}

void pal_write_dmg(int i, int mapnum, byte d){}

void vram_write(int a, byte b)
{
	lcd.vbank[a] = b;
//	if (a >= 0x1800) return;
//	patdirty[((R_VBK&1)<<9)+(a>>4)] = 1;
//	anydirty = 1;
}

void vram_dirty()
{
//	anydirty = 1;
//	memset(patdirty, 1, sizeof patdirty);
}

void pal_dirty()
{
/*	int i;
	if (!hw.cgb)
	{
		pal_write_dmg(0, 0, R_BGP);
		pal_write_dmg(8, 1, R_BGP);
		pal_write_dmg(64, 2, R_OBP0);
		pal_write_dmg(72, 3, R_OBP1);
	}
	for (i = 0; i < 64; i++)
		updatepalette(i);
*/
}

void lcd_reset()
{
	memset(&lcd, 0, sizeof lcd);
	lcd_begin();
	vram_dirty();
	pal_dirty();
}

u_int32_t LastDisplayedTile, TilesDataAddr, TilesMapAddr;

void drawBackground( void ){
  LastDisplayedTile = 0xffffffff;
  TilesDataAddr = lcdControlRegister & 16 ? 0x0000 : 0x0800;
  TilesMapAddr = ( lcdControlRegister & 8 ? 0x1C00 : 0x1800 ) + ( T << 5 ) ;

  register u_int8_t * frameBfPtr = framebuffer - U ;

  u_int8_t *drawLimitX = framebuffer + bgDrawLimitX, *maxFullLine = drawLimitX - 8;

  u_int8_t TileNum = R_SCX >> 3;
  u_int8_t *tilePtr = &lcd.vbank[ TilesMapAddr + TileNum ], *lastTile = &tilePtr[ 32 - TileNum ];
  u_int8_t *tileDataPtr = &lcd.vbank[ TilesDataAddr + ( V << 1 ) ]; // add line offset

  while( 1 ){
    u_int8_t TileToDisplay = TilesDataAddr == 0x0000 ? *tilePtr : UbyteToByte( *tilePtr ) + 128 ;

    if( ++tilePtr == lastTile ) tilePtr -= 32;

    if( TileToDisplay != LastDisplayedTile ){
      LastDisplayedTile = TileToDisplay;

      u_int8_t * tileDataTempPtr = tileDataPtr + ( TileToDisplay << 4 );

      register u_int32_t
        b1 = (*tileDataTempPtr++) << 1,
        b2 = *tileDataTempPtr ;

      gfxLineBuffer[0] = bgColorTable[
      ( ( b1 & bx100000000 )
      | ( b2 &  bx10000000 ) ) >> 7
      ];

      gfxLineBuffer[1] = bgColorTable[
      ( ( b1 & bx10000000 )
      | ( b2 & bx01000000 ) ) >> 6
      ];

      gfxLineBuffer[2] = bgColorTable[
      ( ( b1 & bx01000000 )
      | ( b2 & bx00100000 ) ) >> 5
      ];

      gfxLineBuffer[3] = bgColorTable[
      ( ( b1 & bx00100000 )
      | ( b2 & bx00010000 ) ) >> 4
      ];

      gfxLineBuffer[4] = bgColorTable[
      ( ( b1 & bx00010000 )
      | ( b2 & bx00001000 ) ) >> 3
      ];

      gfxLineBuffer[5] = bgColorTable[
      ( ( b1 & bx00001000 )
      | ( b2 & bx00000100 ) ) >> 2
      ];

      gfxLineBuffer[6] = bgColorTable[
      ( ( b1 & bx00000100 )
      | ( b2 & bx00000010 ) ) >> 1
      ];

      gfxLineBuffer[7] = bgColorTable[
        ( b1 & bx00000010 )
      | ( b2 & bx00000001 )
      ];

      //tileBfPtr -= 7;
    }

    // crop right
    if( frameBfPtr > maxFullLine ){
      u_int8_t * p = gfxLineBuffer;
      while( frameBfPtr != drawLimitX ) *frameBfPtr++ = *p++;
      return;
    }

    // crop left
    if( frameBfPtr < framebuffer ){
      u_int8_t * frameBfPtrEnd = &frameBfPtr[ 8 ];
      u_int8_t n = framebuffer - frameBfPtr;
      frameBfPtr += n;
      u_int8_t * p = gfxLineBuffer + n;
      while( frameBfPtr != frameBfPtrEnd ) *frameBfPtr++ = *p++;
    } else {
      *frameBfPtr++ = gfxLineBuffer[0];
      *frameBfPtr++ = gfxLineBuffer[1];
      *frameBfPtr++ = gfxLineBuffer[2];
      *frameBfPtr++ = gfxLineBuffer[3];
      *frameBfPtr++ = gfxLineBuffer[4];
      *frameBfPtr++ = gfxLineBuffer[5];
      *frameBfPtr++ = gfxLineBuffer[6];
      *frameBfPtr++ = gfxLineBuffer[7];
    }
  }
}

void drawWindow(void){
  LastDisplayedTile = 0xffffffff;
  TilesDataAddr = lcdControlRegister & 16 ? 0x0000 : 0x0800 ;
  TilesMapAddr = ( lcdControlRegister & 64 ? 0x1C00 : 0x1800 ) ;

  register u_int8_t * frameBfPtr = &framebuffer[ R_WX - 7 ];
  u_int8_t *tilePtr = &lcd.vbank[ TilesMapAddr + ( WT << 5 ) ] ;
  u_int8_t *tileDataPtr = &lcd.vbank[ TilesDataAddr + ( WV << 1 ) ];

  while(1){
    u_int8_t TileToDisplay = TilesDataAddr == 0x0000 ? *tilePtr : UbyteToByte( *tilePtr ) + 128 ;
    tilePtr += 1;

    if( TileToDisplay != LastDisplayedTile ){
      LastDisplayedTile = TileToDisplay;

      u_int8_t * tileDataTempPtr = tileDataPtr + ( TileToDisplay << 4 );

      register u_int32_t b1 = ( *tileDataTempPtr++ ) << 1 ;
      register u_int32_t b2 = *tileDataTempPtr ;

      gfxLineBuffer[0] = wdColorTable[
      ( ( b1 & bx100000000 )
      | ( b2 &  bx10000000 ) ) >> 7
      ];

      gfxLineBuffer[1] = wdColorTable[
      ( ( b1 & bx10000000 )
      | ( b2 & bx01000000 ) ) >> 6
      ];

      gfxLineBuffer[2] = wdColorTable[
      ( ( b1 & bx01000000 )
      | ( b2 & bx00100000 ) ) >> 5
      ];

      gfxLineBuffer[3] = wdColorTable[
      ( ( b1 & bx00100000 )
      | ( b2 & bx00010000 ) ) >> 4
      ];

      gfxLineBuffer[4] = wdColorTable[
      ( ( b1 & bx00010000 )
      | ( b2 & bx00001000 ) ) >> 3
      ];

      gfxLineBuffer[5] = wdColorTable[
      ( ( b1 & bx00001000 )
      | ( b2 & bx00000100 ) ) >> 2
      ];

      gfxLineBuffer[6] = wdColorTable[
      ( ( b1 & bx00000100 )
      | ( b2 & bx00000010 ) ) >> 1
      ];

      gfxLineBuffer[7] = wdColorTable[
        ( b1 & bx00000010 )
      | ( b2 & bx00000001 )
      ];

      //tileBfPtr -= 7;
    }

    // crop right
    if( frameBfPtr > &framebuffer[ 151 ] ){
      u_int8_t * p = gfxLineBuffer;
      while( frameBfPtr != &framebuffer[160] ) *frameBfPtr++ = *p++;
      return;
    }

    *frameBfPtr++ = gfxLineBuffer[0];
    *frameBfPtr++ = gfxLineBuffer[1];
    *frameBfPtr++ = gfxLineBuffer[2];
    *frameBfPtr++ = gfxLineBuffer[3];
    *frameBfPtr++ = gfxLineBuffer[4];
    *frameBfPtr++ = gfxLineBuffer[5];
    *frameBfPtr++ = gfxLineBuffer[6];
    *frameBfPtr++ = gfxLineBuffer[7];
  }
}

union sprite * sprites = (union sprite *)lcd.oam.mem;//&SpriteOAM[0xFE00];
union sprite * spritesEnd = (union sprite *)(lcd.oam.mem + 40*4);// &SpriteOAM[0xFE00 + 40*4];//sprites + 40;

void drawSprites( void ){
  int SpritePosX, SpritePosY;
  u_int8_t NumberOfSpritesToDisplay;
  int ListOfSpritesToDisplay[ 10 ] = { 0 };

  u_int8_t SpriteMaxY = lcdControlRegister & bx00000100 ? 15 : 7;

  union sprite *sprite = sprites;
  u_int32_t * display = (u_int32_t*)ListOfSpritesToDisplay, *maxDisplay = &display[ 10 ];
  u_int32_t scanline = L + 16;

  while( sprite != spritesEnd ){
    if( !sprite->x || sprite->x >= 168 // on screen x
     || !sprite->y //|| sprite->y >= 160 // on screen y
     || sprite->y > scanline // on scanline
     || sprite->y + SpriteMaxY < scanline
    ){ sprite++; continue; } // no!

    // visible at this scanline
    *display++ = ( sprite->x << 8 ) | ( sprite - sprites ) ;
    if( display == maxDisplay ) break;
    sprite++;
  };

  NumberOfSpritesToDisplay = display - (u_int32_t*)ListOfSpritesToDisplay;

//    NumberOfSpritesToDisplay = display - (u_int32_t*)ListOfSpritesToDisplay;
  if( !NumberOfSpritesToDisplay ) return;

  // Sort the list of sprites to display
  if( NumberOfSpritesToDisplay > 1 )
    cqsort( ListOfSpritesToDisplay, &ListOfSpritesToDisplay[ NumberOfSpritesToDisplay - 1 ] );

  //u_int32_t * lastDisplay = display ;
  //display = (u_int32_t*)ListOfSpritesToDisplay;
  u_int32_t * lastDisplay = (u_int32_t*)ListOfSpritesToDisplay - 1;
  display--;
  do {
    //sprite = &sprites[ ListOfSpritesToDisplay & bx00111111 ];
    //u_int8_t * p = (u_int8_t*)&sprite;
    u_int8_t * p = &lcd.oam.mem[ ( (*display--/*--*//* ++ */) & 0xff ) << 2 ];
    SpritePosY = (*p++) - 16 ;
    SpritePosX = (*p++) - 8 ;

    u_int8_t * flag = p + 1 ;
    u_int8_t pal = (*flag) & bx00010000 ? 1 : 0 ;

    // recompute palette color table if change
    u_int8_t *spritePal = spriteColorTable[ pal ],
    *spriteCurrentPalette = &spriteCurrentPal[ pal ],
    *spritePalIndex = pal ? &R_OBP1 : &R_OBP0 ;//&IoRegisters[ pal + pal_OBP0 ] ;

    if( *spriteCurrentPalette != *spritePalIndex ){
      u_int8_t more = pal ? 8 : 4 ;
      *spriteCurrentPalette = *spritePalIndex;
      spritePal[0] = ( (*spriteCurrentPalette) & 3 ) + more;
      spritePal[1] = ( ((*spriteCurrentPalette) >> 4) & 3 ) + more;
      spritePal[2] = ( ((*spriteCurrentPalette) >> 2) & 3 ) + more;
      spritePal[3] = ( ((*spriteCurrentPalette) >> 6) & 3 ) + more;
    }

    if( SpriteMaxY == 7 ){
      if( (*flag) & bx01000000 ){ // yflip
        p = lcd.vbank + ( *p << 4 )
          + ( ( 7 - ( L - SpritePosY ) ) << 1 );
      } else {
        p = lcd.vbank + ( *p << 4 )
          + ( ( L - SpritePosY ) << 1 );
      }
    } else {
      if( (*flag) & bx01000000 ){ // yflip
        p = lcd.vbank + ( ( *p & bx11111110 ) << 4 )
          + ( ( 15 - ( L - SpritePosY ) ) << 1 );
      } else {
        p = lcd.vbank + ( ( *p & bx11111110 ) << 4 )
          + ( ( L - SpritePosY ) << 1 );
      }
    }

    register u_int32_t b1 = ( *p++ ) << 1 ;
    register u_int32_t b2 = *p ;

    if( (*flag) & bx00100000 ){ // xflip
      gfxLineBuffer[0] =
        ( b1 & bx00000010 )
      | ( b2 & bx00000001 )
      ;

      gfxLineBuffer[1] =
      ( ( b1 & bx00000100 )
      | ( b2 & bx00000010 ) ) >> 1
      ;

      gfxLineBuffer[2] =
      ( ( b1 & bx00001000 )
      | ( b2 & bx00000100 ) ) >> 2
      ;

      gfxLineBuffer[3] =
      ( ( b1 & bx00010000 )
      | ( b2 & bx00001000 ) ) >> 3
      ;

      gfxLineBuffer[4] =
      ( ( b1 & bx00100000 )
      | ( b2 & bx00010000 ) ) >> 4
      ;

      gfxLineBuffer[5] =
      ( ( b1 & bx01000000 )
      | ( b2 & bx00100000 ) ) >> 5
      ;

      gfxLineBuffer[6] =
      ( ( b1 & bx10000000 )
      | ( b2 & bx01000000 ) ) >> 6
      ;

      gfxLineBuffer[7] =
      ( ( b1 & bx100000000 )
      | ( b2 &  bx10000000 ) ) >> 7
      ;
    } else {
      gfxLineBuffer[0] =
      ( ( b1 & bx100000000 )
      | ( b2 &  bx10000000 ) ) >> 7
      ;

      gfxLineBuffer[1] =
      ( ( b1 & bx10000000 )
      | ( b2 & bx01000000 ) ) >> 6
      ;

      gfxLineBuffer[2] =
      ( ( b1 & bx01000000 )
      | ( b2 & bx00100000 ) ) >> 5
      ;

      gfxLineBuffer[3] =
      ( ( b1 & bx00100000 )
      | ( b2 & bx00010000 ) ) >> 4
      ;

      gfxLineBuffer[4] =
      ( ( b1 & bx00010000 )
      | ( b2 & bx00001000 ) ) >> 3
      ;

      gfxLineBuffer[5] =
      ( ( b1 & bx00001000 )
      | ( b2 & bx00000100 ) ) >> 2
      ;

      gfxLineBuffer[6] =
      ( ( b1 & bx00000100 )
      | ( b2 & bx00000010 ) ) >> 1
      ;

      gfxLineBuffer[7] =
        ( b1 & bx00000010 )
      | ( b2 & bx00000001 )
      ;
    }

    register u_int8_t *screen, *s ;
    u_int8_t *end;

    if( SpritePosX < 0 ){ //crop left
      screen = framebuffer ;
      s = gfxLineBuffer - SpritePosX;
      //u_int8_t sx = 8 + SpritePosX ;
      end = &framebuffer[ /*sx*/ 8 + SpritePosX ] ;
    } else {
      s = gfxLineBuffer;
      screen = &framebuffer[ SpritePosX ] ;
      if( screen > &framebuffer[ 151 ] ) // crop right
        end = &framebuffer[160] ;
      else // no crop
        end = 0;//&screen[ 8 ] ;
    }

    if( end ){
      if( (*flag) & bx10000000 ){ // sprite can be hidden
        while( screen != end ){
          if( *s && *screen != bgColorTable[0] )
            *screen++ = spritePal[ *s++ ];
          else {
            screen++;
            s++;
          }
        };
      } else {
        while( screen != end ){
          if( *s )
            *screen++ = spritePal[ *s++ ];
          else {
            screen++;
            s++;
          }
        };
      }
    } else { // no crop
      if( (*flag) & bx10000000 ){ // sprite can be hidden
        uint8_t bg = bgColorTable[0];
        if( *s && *screen == bg ) *screen = spritePal[ *s ];
        screen++;
        if( *++s && *screen == bg ) *screen = spritePal[ *s ];
        screen++;
        if( *++s && *screen == bg ) *screen = spritePal[ *s ];
        screen++;
        if( *++s && *screen == bg ) *screen = spritePal[ *s ];
        screen++;
        if( *++s && *screen == bg ) *screen = spritePal[ *s ];
        screen++;
        if( *++s && *screen == bg ) *screen = spritePal[ *s ];
        screen++;
        if( *++s && *screen == bg ) *screen = spritePal[ *s ];
        screen++;
        if( *++s && *screen == bg ) *screen = spritePal[ *s ];
      } else { // no hide
        if( *s ) *screen = spritePal[ *s ];
        screen++;
        if( *++s ) *screen = spritePal[ *s ];
        screen++;
        if( *++s ) *screen = spritePal[ *s ];
        screen++;
        if( *++s ) *screen = spritePal[ *s ];
        screen++;
        if( *++s ) *screen = spritePal[ *s ];
        screen++;
        if( *++s ) *screen = spritePal[ *s ];
        screen++;
        if( *++s ) *screen = spritePal[ *s ];
        screen++;
        if( *++s ) *screen = spritePal[ *s ];
      }
    }

  } while( display != lastDisplay ) ;
}
#undef LCD
void flushScanline(){
  uint32_t *LCD = (uint32_t *) 0xA0002188;
  volatile uint32_t *SET = (uint32_t *) 0xA0002284;
  volatile uint32_t *CLR = (uint32_t *) 0xA0002204;

  uint8_t *d = framebuffer;
  uint32_t x = 160, pixel, quad, WR = 1<<12, *pal=palette;
  /* */

  asm volatile(
      ".syntax unified                \n"

      "nextPixelLoop%=:                       \n"
      FLUSH_QUAD_SCALE
      FLUSH_QUAD_SCALE
      "bne nextPixelLoop%=                    \n"

      : // outputs
	[pixelptr]"+l"(d),
	[x]"+l"(x),
	[pixel]"=l"(pixel),
	[qd]"=l"(quad),
	[palette]"+l"(pal),
	[WR]"+l"(WR),
	[LCD]"+l"(LCD)

      : // inputs

      : // clobbers
	"cc"
      );
}

void dflushScanline(u_int8_t * f){
  uint32_t *LCD = (uint32_t *) 0xA0002188;
  volatile uint32_t *SET = (uint32_t *) 0xA0002284;
  volatile uint32_t *CLR = (uint32_t *) 0xA0002204;

  uint8_t *d = f;
  uint32_t x = 160, pixel, quad, WR = 1<<12, *pal=palette;
  /* */

  asm volatile(
      ".syntax unified                \n"

      "nextPixelLoop%=:                       \n"
      FLUSH_QUAD_SCALE
      FLUSH_QUAD_SCALE
      "bne nextPixelLoop%=                    \n"

      : // outputs
	[pixelptr]"+l"(d),
	[x]"+l"(x),
	[pixel]"=l"(pixel),
	[qd]"=l"(quad),
	[palette]"+l"(pal),
	[WR]"+l"(WR),
	[LCD]"+l"(LCD)

      : // inputs

      : // clobbers
	"cc"
      );
}

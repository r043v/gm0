
#include "pokitto.h"

u8 ** gdlFont = 0;

u8 cpuMhz = 72 ;
//u8 spi1Mhz = 10 ;

u8 spi1Prescaler, spi1Divider ;

void setCpuMhz( u8 mhz ){
  if( mhz == cpuMhz ) return ;
  cpuMhz = mhz ;
  switch( mhz ){
    case 48:
      *LPC_SYSPLLCTRL = 0x25 ;
    break;
    case 72:
      *LPC_SYSPLLCTRL = 0x23 ;
    break;
    default:
      cpuMhz = 48 ;
      *LPC_SYSPLLCTRL = 0x25 ;
    break;
  };
}

void spi1ComputePrescalerDivider( void ){
    if( cpuMhz == 72 ){ // 72mhz cpu
      // 9mhz spi
      spi1Prescaler = 4 ;
      spi1Divider = 1 ;

      // 720khz spi
/*      spi1Prescaler = 100 ;
      spi1Divider = 0 ;
*/    } else { // 48mhz cpu
      // 8mhz spi
      spi1Prescaler = 2 ;
      spi1Divider = 2 ;
    }
}

void spi1SetPrescaleDivider( void ){
  	*SPI1_CR0 = (
        ( SPI_LENGTH_8 << 0 ) // 8-bit
      | ( 0 << 6 ) // polarity > 0
      | ( 0 << 7 ) // phase > 0
      | ( spi1Divider << 8 ) // divider-1
    ) ;

    *SPI1_CPSR = spi1Prescaler ;
}

void updateSpiDivider( void ){
  SPI1_DISABLE ;
  spi1ComputePrescalerDivider() ;
  spi1SetPrescaleDivider() ;
  SPI1_ENABLE ;
}

void spi1Init( void ){
    //SPI1_CONF_IOCON ; // pin 20-23 i/o definition
    *IOCON_PIO1_20 = IOCON_F2;
    *IOCON_PIO1_21 = IOCON_F2;
    *IOCON_PIO1_22 = IOCON_F1;
    *IOCON_PIO1_23 = IOCON_F2;

    SET_PORT1_PIN_DIRECTION( 20, GPIO_OUTPUT ); // sclk
    SET_PORT1_PIN_DIRECTION( 21, GPIO_INPUT  ); // miso
    SET_PORT1_PIN_DIRECTION( 22, GPIO_OUTPUT ); // mosi
    SET_PORT1_PIN_DIRECTION( 23, GPIO_OUTPUT ); // ssel

    SPI1_ENABLE_CLK ;
    *SPI1_CLK_DIV = 1 ;

    spi1ComputePrescalerDivider();

    *SPI1_CR1 = (
        ( 0 << 0 ) // loop back disabled
      | ( 0 << 1 ) // disabled spi 1
      | ( 0 << 2 ) // master mode
      | ( 0 << 3 ) // slave output
    ) ;

    spi1SetPrescaleDivider();

    // enable
    SPI1_DISABLE_RESET;
    SPI1_ENABLE;
}

u8 MCP23S17_WRITE( u8 chip, u8 address, u8 data ){
  register u8 out ;
  SPI1_SELECT_MCP23S17 ;
  SPI1_WRITE( chip );
  SPI1_WAIT_READ;
  out = *SPI1_DATA;
  SPI1_WRITE( address );
  SPI1_WAIT_READ;
  out = *SPI1_DATA;
  SPI1_WRITE( data );
  SPI1_WAIT_READ;
  out = *SPI1_DATA;
  SPI1_UNSELECT_MCP23S17 ;
  return out ;
}

void MCP23S17_initDirection( void ){
  // 0xff = input / 0x00 output
  MCP23S17_1_SET_DIRECTION_A( 0x00 );
  MCP23S17_1_SET_DIRECTION_B( 0x00 );
  MCP23S17_2_SET_DIRECTION_A( 0xFF );
  MCP23S17_2_SET_DIRECTION_B( 0x00 );
}

/*
u8 PortGB;
void wrPin_high () { PortGB |= (1<<1); }
void wrPin_low  () { PortGB &= ~(1<<1); }
void clkPin_high () { PortGB |= (1<<0); }
void clkPin_low  () { PortGB &= ~(1<<0); }
void resetPin_high () { PortGB |= (1<<4); }
void resetPin_low  () { PortGB &= ~(1<<4); }

void mreqPin_high () { PortGB |= (1<<3); }
void mreqPin_low  () { PortGB &= ~(1<<3); }
void rdPin_high () { PortGB |= (1<<2); }
void rdPin_low  () { PortGB &= ~(1<<2); }
*/
/*
void updatePortGB( void ){
  //chip2.write(PORT_B, PortGB);
  MCP23S17_2_WRITE_DATA_B( PortGB );
}

char gameTitle[17];
*/
/*
void rd_wr_mreq_reset () {
  rdPin_high(); // RD off
  wrPin_high(); // WR off
  mreqPin_high(); // MREQ off
  updatePortGB();
}
*/
/*
u8 zread_byte(u16 address) {
  //shiftout_address(address); // Shift out address
    MCP23S17_1_WRITE_DATA_B( (address >> 8) );
    MCP23S17_1_WRITE_DATA_A( (address & 0xFF) );
//    chip1.write(PORT_B, address >> 8);
//    chip1.write(PORT_A, address & 0xFF);
//  mreqPin_low();
  rdPin_low();
  updatePortGB();

 //wait_us(10);
  //uint8_t bval = chip2.read(PORT_A); // Read data
  u8 bval = MCP23S17_2_READ_DATA_A();
  rdPin_high();
  updatePortGB();
 // mreqPin_high();
  // wait_us(10);

  return bval;
}
*/

/*
u8 * gfmPal = 0;
clDeep gfmPal16[16] = { 1,2,3,0 };

u8 framebuffer8[ SCREEN_WIDTH * 8 ];
u8 *framebuffer = framebuffer8;

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
*/
//extern const u8 ** font;

void zdrawText( u8 x, u8 y, const char *txt ){
  const char *p = txt ;
  clDeep * screen = framebuffer + y*SCREEN_WIDTH + x ;
	while(*p){
    u8 c = ( *p++ ) - ' ';
    if( c > 91 ) c = 0 ;
    if( !c ){ x += CHAR_SPACE; continue; }

    const u8 * frm = gdlFont[ c ];
    blitGfm( frm, screen ) ;
    screen += ( frm[4] + CHAR_PADDING ) ;
  };
}

void setGdlFont( const u8 ** font ){
  gdlFont = (u8**)font ;
}

void drawText( u8 x, u8 y, const char *txt ){
  u8 *f = framebuffer + y*SCREEN_WIDTH + x ;
    const char *h = txt ;
    while(*h){
      u8 ch = *h++ - ' ' ;
      if( !ch ){ f += CHAR_SPACE ; continue ; }
      if( ch > 91 ) continue ; // ch = '*' - ' ';
      u8 * g = gdlFont[ch] ;
      blitGfm( g, f ) ;
      f += ( g[4] + CHAR_PADDING ) ;
    };
}

char textBf[ textBfSize ];
#include <stdio.h>
#include <stdarg.h>
void prints( u8 x, u8 y, const char * format, ...){
	va_list va;
  va_start(va,format);
  vsprintf(textBf,format,va);
	drawText(x,y,textBf);
}

/* timer */

void initSysTick( void ){
    *SYST_RVR = TICKS_MAX ; // count down from maximum value, 0x00ffffff

    *SYST_CSR = (
      ( 1 << 0 ) // enabled
    | ( 0 << 1 ) // TICKINT, no interrupt
    | ( 0 << 2 ) // CLKSOURCE, 0:cpuHz/2 1:cpuHz
    ) ;

    resetSysTick ;
}

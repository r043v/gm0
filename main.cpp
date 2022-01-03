
extern "C" {
  #include "pokitto.h"
  #include "fgb/hw.h"
  #include "lcd.h"

  void fgb( void *_rom );
  void cqsort( int*f, int*l );
  void generateGfxBank( void );
  void initPal(void);
}

#include "./pokitto/MCP23S17/MCP23S17.h"
#include "./font.gfm2.h"

/*
SPI spi(P1_22, P1_21, P1_20, P1_23);
MCP23S17 chip1 = MCP23S17(spi, P1_23, 0x40);
MCP23S17 chip2 = MCP23S17(spi, P1_23, 0x42);

u8 gbPort = 0 ;

#define BIT_HIGH(V,B) ( V |= (1<<B) )
#define  BIT_LOW(V,B) ( V &= ~(1<<B) )
#define GB_BIT_HIGH(B) BIT_HIGH(gbPort,B)
#define  GB_BIT_LOW(B)  BIT_LOW(gbPort,B)

#define GB_CLK_H GB_BIT_HIGH(0)
#define GB_CLK_L GB_BIT_LOW(0)
#define GB_WR_H GB_BIT_HIGH(1)
#define GB_WR_L GB_BIT_LOW(1)
#define GB_RD_H GB_BIT_HIGH(2)
#define GB_RD_L GB_BIT_LOW(2)
#define GB_CS_H GB_BIT_HIGH(3)
#define GB_CS_L GB_BIT_LOW(3)
#define GB_RST_H GB_BIT_HIGH(4)
#define GB_RST_L GB_BIT_LOW(4)

#define GB_RD_WR_CS_H ( gbPort |= ( (1<<1) | (1<<2) | (1<<3) ) )

#define gbPortUpdate chip2.write( PORT_B, gbPort )
#define gbData chip2.read(PORT_A)
*/
/*
uint8_t read_byte(uint16_t address) {
  //shiftout_address(address); // Shift out address
  chip1.write(PORT_B, address >> 8);
  chip1.write(PORT_A, address & 0xFF);
  waitUs( 10 ) ;
  GB_RD_L ;
  GB_CS_L ;
  //GB_WR_H ;
  gbPortUpdate ;
  waitUs( 10 ) ;
  uint8_t bval = gbData ; // Read data
  GB_RD_H ;
  GB_CS_H ;
//  GB_WR_L ;
  gbPortUpdate ;

  return bval;
}

char title[32] ;
void rd_wr_mreq_reset () {
  //rdPin_high(); // RD off
  //wrPin_high(); // WR off
  //mreqPin_high(); // MREQ off
  GB_RD_WR_CS_H;
  gbPortUpdate ;
}
void gbGetTitle( void ){

  u32 i = 0x0134;
  char * p = title;
  while( i <= 0x143 ){
    char c = (char)read_byte( i );
    *p = c;
    p++;
    i++;
  }
  *p = 0;
}
*/
extern u32 SystemCoreClock;

void write_command_16(uint16_t data){
     CLR_CS; // select lcd
     CLR_CD; // clear CD = command
     SET_RD; // RD high, do not read
    *LCD = ((uint32_t)data) << 3;
     CLR_WR_SLOW;  // WR low
     SET_WR;  // WR low, then high = write strobe
     SET_CS; // de-select lcd
  }

void write_data_16(uint16_t data){
    CLR_CS;
    SET_CD;
    SET_RD;
    *LCD = ((uint32_t)data) << 3;
    CLR_WR;
    SET_WR;
    SET_CS;
  }

void dflushScanline(u_int8_t * f);

void SetScanlineN( u_int8_t n ){
  volatile uint32_t *SET = (uint32_t *) 0xA0002200;

  write_command_16(0x20);

  write_data_16(n+((n+3)>>2)-4);
  write_command_16(0x21);
  write_data_16(10);

  write_command_16(0x22);

  // CLR_CS_SET_CD_RD_WR;
  SET[0] = 1 << 2;
  SET[1] = 1 << 24;
  SET[1] = 1 << 12;
}

extern u_int32_t * palette ;
//extern char title[32] ;

#define clearFramebufferColor 3
#define clearFramebufferColor32 ( ( clearFramebufferColor << 24 ) | ( clearFramebufferColor << 16 ) | ( clearFramebufferColor << 8 ) | clearFramebufferColor )

void clearFramebuffer( void ){
  u32 *p = (u32*)framebuffer;
  while( p != (u32*)&framebuffer[SCREEN_WIDTH*8]){
    *p++ = clearFramebufferColor32;
    *p++ = clearFramebufferColor32;
    *p++ = clearFramebufferColor32;
    *p++ = clearFramebufferColor32;
    *p++ = clearFramebufferColor32;
    *p++ = clearFramebufferColor32;
    *p++ = clearFramebufferColor32;
    *p++ = clearFramebufferColor32;
  };
  //memset(f,c,SCREEN_WIDTH*8);
}

void sendFramebuffer( void ){
  u8 *p = framebuffer;
  while( p != &framebuffer[SCREEN_WIDTH*8])
    write_data_16( palette[ *p++ ] );
}

void showTicks( void ){
  u_int8_t bReleased = 0;
  u32 time = 0, total = 0 ;
  resetSysTick;
  while(1){
    time = getTick ;
    resetSysTick;

    if( *bBtn ){
       if( bReleased ) break;
    } else bReleased = 1;

    u8 line = 0;

    clearFramebuffer();
    u32 ms = TICKS_TO_MS(time);
    total += ms ;
    prints( 8, 0, "%ums", total );
    sendFramebuffer(); line++;

    clearFramebuffer();
    prints( 8, 0, "ms %u, us %u", ms, TICKS_TO_US(time) );
    sendFramebuffer(); line++;

    // fill screen with empty lines
    clearFramebuffer();
    while( line++ != 22 ) sendFramebuffer();
  };
}
/*
void writeAdress( u8 v ){
  u8 line=0;
  chip1.write(PORT_B, v);
  chip1.write(PORT_A, v);
  clearFramebuffer();
  prints( 8, 0, "adr 0x%x",v );
  sendFramebuffer(); line++;
  clearFramebuffer();
  while( line++ != 22 ) sendFramebuffer();
}
*/

union conf {
  u_int8_t raw ;
  struct {
    u8 overclock:1 ;
    u8 frameskip:2 ;
    u8 palette:4 ;
  };
} conf ;

struct menuEntry {
  char ** text ;
  u8 current ;
  u8 nb ;
} menuEntry ;

void showChoiceMenuLine( struct menuEntry * e ){
  clearFramebuffer();
  //tmp = read_byte(0x0104);
  char * p = e->text[0] ;
  prints( 8, 0, p ); // title
  u8 offset = 2; while(*p++) offset++ ;
  u8 n = 0 ;
  while( n != e->nb ){
    ++n ;
    p = e->text[ n ] ;
    prints( 8*offset, 0, n == e->current ? "<%s>" : " %s ", p ) ;
    while( *p++ ) offset++ ;
    offset+=2;
  };
  sendFramebuffer();
}

struct menuEntry menu1 ;


void showMenu( void ){
  u_int8_t cReleased = 0;
  static u8 selectedLine = 0 ;
  u32 time = 0, total = 0 ;
  resetSysTick;
  while(1){
    time = getTick ;
    resetSysTick;
    
    if( *bBtn ){
      showTicks();
      while( *bBtn );
    }

    if( *leftBtn ){
      while( *leftBtn );
      if( menu1.current > 1 ) menu1.current-- ;
      //while( !*leftBtn );
      //while( *leftBtn );
    }

    if( *rightBtn ){
      while( *rightBtn );
      if( menu1.current != menu1.nb ) menu1.current++ ;
      //while( !*rightBtn );
      //while( *rightBtn );
    }

/*
    if( *upBtn ){
        while( *upBtn );
        writeAdress( 0xFF );
        while( !*upBtn );
        while( *upBtn );
    }

    if( *downBtn ){
        while( *downBtn );
        writeAdress( 0x00 );
        while( !*downBtn );
        while( *downBtn );
    }
*/
    if( *cBtn ){
       if( cReleased ) break;
    } else cReleased = 1;

  u8 line = 0;

  //rd_wr_mreq_reset();
  //waitUs(10);

  clearFramebuffer();
  prints( 8, 0, "%uMHz", SystemCoreClock/MHZ );
  sendFramebuffer(); line++;

  clearFramebuffer();
  //gbGetTitle();
  //prints(  8, 0, "title -%s-", (const char *)title );
  sendFramebuffer(); line++;

  u32 tmp ;

  showChoiceMenuLine(&menu1); line++;

  //clearFramebuffer();
  //tmp = read_byte(0x0104);
  //prints( 8, 0, "nintendo logo : 0x%02X %s", tmp, tmp == 0xCE ? "OK" : "KO" );
  //sendFramebuffer(); line++;

  //clearFramebuffer();
  //tmp = read_byte(0x0147);
  //prints( 8, 0, "cart type : 0x%02X", tmp );
  //sendFramebuffer(); line++;

  //clearFramebuffer();
  //tmp = read_byte(0x0148);
  //prints( 8, 0, "rom size : 0x%02X", tmp );
  //sendFramebuffer(); line++;

  clearFramebuffer();
  //tmp = read_byte(0x0149);
  //prints( 8, 0, "ram size : 0x%02X", tmp );
  sendFramebuffer(); line++;

clearFramebuffer();
    u32 ms = TICKS_TO_MS(time);
    total += ms ;
    prints( 8, 0, "%ums", total );
    sendFramebuffer(); line++;

    clearFramebuffer();
    prints( 8, 0, "ms %u, us %u", ms, TICKS_TO_US(time) );
    sendFramebuffer(); line++;


  clearFramebuffer();
  //tmp = read_byte(0x014A);
  //prints( 8, 0, "destination : 0x%02X", tmp );
  sendFramebuffer(); line++;

  clearFramebuffer();
  sendFramebuffer(); line++;

  prints( 8, 0, " - SPI1 -" );
  sendFramebuffer(); line++;

  clearFramebuffer();
  tmp = *SPI1_CR0 ;
  u32 divider = ( (tmp >> 8) & 0xff ) + 1 ;
  prints( 8, 0, "CR0 0x%X", tmp );
  sendFramebuffer(); line++;

  clearFramebuffer();
  prints( 8, 0, "%ubit, divider %u",
    (tmp & 0x3f ) + 1,
    divider
  );
  sendFramebuffer(); line++;

  clearFramebuffer();
  prints( 8, 0, "polarity o%s, phase o%s",
    (tmp>>6)&1 ? "n" : "ff",
    (tmp>>7)&1 ? "n" : "ff"
  );
  sendFramebuffer(); line++;

  clearFramebuffer();
  tmp = *SPI1_CR1 ;
  prints( 8, 0, "CR1 0x%X", tmp );
  sendFramebuffer(); line++;
  clearFramebuffer();
  prints( 8, 0, "%s %s %s",
    (tmp >> 1) & 1 ? "on" : "off", // on off
    (tmp >> 2) & 1 ? "slave" : "master", // master or slave
    tmp & 1 ? "lp" : "!lp" // loopback
  ); // 0x2
  sendFramebuffer(); line++;

  clearFramebuffer();
  u32 prescaler = *SPI1_CPSR ;
  prints( 8, 0, "CPSR, prescaler, 0x%X", prescaler ); // 0x2
  sendFramebuffer(); line++;

  clearFramebuffer();
  u32 spiSpeed = ( SystemCoreClock / ( prescaler * divider ) ) ;
  if( spiSpeed >= MHZ ){
    spiSpeed /= MHZ;
    prints( 8, 0, "speed %u MHz", spiSpeed );
  } else prints( 8, 0, "speed %u Hz", spiSpeed );
  sendFramebuffer(); line++;

  // fill screen with empty lines
  clearFramebuffer();
  while( line++ != 22 ) sendFramebuffer();

/*
    u_int8_t n = 3;
    SetScanlineN(n);
    for( y=0;y<18;y++ ){
      n=3;
      f = framebuffer;
      while( n != 11 ){
        dflushScanline(f);
      	if( !( n & 3 ) ) dflushScanline(f);
        f += 160;
        n++;
      }
    }
*/
  }

  while(*cBtn);
}

  u8 keys, lastKeys;
extern "C" {
  uint8_t readKeys( void ){
    u8 b = 0;
    static u8 last = 0, cTime = 0 ;

    if( *upBtn )
      b |= PAD_UP;
    if( *downBtn )
      b |= PAD_DOWN;
    if( *leftBtn )
      b |= PAD_LEFT;
    if( *rightBtn )
      b |= PAD_RIGHT;
    if( *aBtn )
      b |= PAD_A;
    if( *bBtn )
      b |= PAD_B;
    if( *cBtn ){
      b |= PAD_START;
      if(++cTime == 128){
        showMenu();
        cTime = 0;
      }
    } else cTime = 0 ;

    last = b;

    lastKeys = last;
    keys = b;

    return b;
  }
}

void screenInit( void ){
  setDirection(0x1038);
  setVerticalRange( 0, 175 );
  setHorizontalRange( 0,9 );
  beginStream();
  u32 n = 176*10;
  while( --n ) writeData(0);
  setHorizontalRange( 210,219 );
  beginStream();
  n = 176*10;
  while( --n ) writeData(0);
  setHorizontalRange( 10,209 );
  beginStream();
}

#ifndef ROM
#define ROM 2048
#endif

#define XSTR(x) #x
#define STR(x) XSTR(x)

#define romDir roms/header

#define EMBEDROM STR(romDir/ROM.h)

#include EMBEDROM

/*
  CHIP 1
    port A
      output, A0-A7
    port B
      output, A8-A15

  CHIP 2
    port A
      input, D0-d7
    port B
      b0 - CLK
      b1 - WR
      b2 - RD
      b3 - CS
      b4 - RESET
*/

char * menu1text[4] = { "title", "mdr","wtf","lol" } ;

int main( void ) {
  *(uint32_t *)0x40048080 |= 3 << 26; // enable high ram
  screenInit();

menu1.text = menu1text ;
menu1.current = 2;
menu1.nb = 3;

/*
  chip1.direction(PORT_A, 0x00); //OUTPUT
  chip1.direction(PORT_B, 0x00);
  chip2.direction(PORT_A, 0xFF); //INPUT
  chip2.direction(PORT_B, 0x00);

  GB_RST_H ; // power on cart
  GB_CLK_L ; // clock is not used

  GB_WR_H ;
  gbPortUpdate ;
*/
  setGdlFont( font );
  initSysTick();
  generateGfxBank();
  initPal();
  
  fgb( (char*)embedrom );

  return 0;
}

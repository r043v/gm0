
#ifndef _pokitto_
#define _pokitto_

#include "stdint.h"

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;


/*  */


#define LPC_SYSAHBCLKCTRL ((volatile u32*)0x40048080)
#define LPC_PRESETCTRL    ((volatile u32*)0x40048004) // page 33
#define LPC_SYSPLLCTRL    ((volatile u32*)0x40048008) // page 34


/* spi */


#define SPI0_CR0    ((volatile u32*)0x40040000)
#define SPI0_CR1    ((volatile u32*)0x40040004)
#define SPI0_DATA   ((volatile u32*)0x40040008)
#define SPI0_STATUS ((volatile u32*)0x4004000C)
#define SPI0_CPSR   ((volatile u32*)0x40040010)


/*
PIO1_20 PIO1_20 U0_DSR SSP1_SCK CT16B0_MAT0 - Table 84 // f2
PIO1_21 PIO1_21 U0_DCD SSP1_MISO CT16B0_CAP1 - Table 84 // f2
PIO1_22 PIO1_22 SSP1_MOSI CT32B1_CAP1 ADC_4 R_29 Table 87 // f1
PIO1_23 PIO1_23 CT16B1_MAT1 SSP1_SSEL U2_TXD - Table 84 //f2
*/

#define SPI_LENGTH_8 0x7
#define SPI_LENGTH_16 0xF

#define IOCON_F1 ( 1<<7 | 1<<0 )
#define IOCON_F2 ( 1<<7 | 1<<1 )

#define IOCON_PIO1_20 ((volatile u32*)0x400440B0)
#define IOCON_PIO1_21 ((volatile u32*)0x400440B4)
#define IOCON_PIO1_22 ((volatile u32*)0x400440B8)
#define IOCON_PIO1_23 ((volatile u32*)0x400440BC)

#define SPI1_CLK_DIV ((volatile u32*)0x4004809C)

#define SPI1_CR0    ((volatile u32*)0x40058000)
#define SPI1_CR1    ((volatile u32*)0x40058004)
#define SPI1_DATA   ((volatile u32*)0x40058008)
#define SPI1_STATUS ((volatile u32*)0x4005800C)
#define SPI1_CPSR   ((volatile u32*)0x40058010)

#define SPI1_CONF_IOCON *IOCON_PIO1_20=IOCON_F2;*IOCON_PIO1_21=IOCON_F2;*IOCON_PIO1_22=IOCON_F1;*IOCON_PIO1_23=IOCON_F2
#define SPI1_ENABLE_RESET  ( *LPC_PRESETCTRL &= ~( 1 << 2 ) ) // page 33
#define SPI1_DISABLE_RESET ( *LPC_PRESETCTRL |= ( 1 << 2 ) ) // page 33
#define SPI1_ENABLE_CLK    ( *LPC_SYSAHBCLKCTRL |= ( 1 << 18 ) ) // page 42

#define SPI1_DISABLE       ( *SPI1_CR1 &=  ~(1 << 1) )
#define SPI1_ENABLE        ( *SPI1_CR1 |=   (1 << 1) )

#define SPI1_CAN_READ      ( *SPI1_STATUS & (1 << 2) )
#define SPI1_CAN_WRITE     ( *SPI1_STATUS & (1 << 1) )

#define SPI1_WAIT_WRITE    while(!SPI1_CAN_WRITE)
#define SPI1_WAIT_READ     while(!SPI1_CAN_READ)

#define SPI1_WRITE(v)      SPI1_WAIT_WRITE;*SPI1_DATA=v
#define SPI1_READ(v)       SPI1_WAIT_READ;*v=*SPI1_DATA
#define SPI1_WRITE_READ(w,r) SPI1_WRITE(w);SPI1_READ(r)

void spi1Init( void );

/* gpio */

#define GPIO_ENABLE_CLK    ( *LPC_SYSAHBCLKCTRL |= ( 1 << 6 ) ) // page 41

#define GPIO_INPUT 0
#define GPIO_OUTPUT 1

#define GPIO_PORT_REGISTERS 0xA0000000
#define GPIO_PORT1_BYTE_PIN (GPIO_PORT_REGISTERS + 0x0020)
#define GPIO_PORT1_WORD_PIN (GPIO_PORT_REGISTERS + 0x1080)
#define GPIO_PORT1_DIRECTION ((volatile u32 *)(GPIO_PORT_REGISTERS + 0x2004))
#define P1_23_WORD ((volatile u32 *)(GPIO_PORT1_WORD_PIN+(23*4)))
#define P1_23_PIN ((volatile u8 *)(GPIO_PORT1_BYTE_PIN+23))
#define P1_23_AS_OUTPUT *GPIO_PORT1_DIRECTION |= ( 1 << 23 )

#define SET_PORT1_PIN_DIRECTION( pin, direction ) *GPIO_PORT1_DIRECTION |= ( direction << pin )

/* MCP23S17 */

// all register addresses assume IOCON.BANK = 0 (POR default)

#define IODIRA   0x00
#define GPINTENA 0x04
#define DEFVALA  0x06
#define INTCONA  0x08
#define IOCONA   0x0A
#define GPPUA    0x0C
#define GPIOA    0x12
#define OLATA    0x14

#define IODIRB   0x01
#define GPINTENB 0x05
#define DEFVALB  0x07
#define INTCONB  0x09
#define IOCONB   0x0B
#define GPPUB    0x0D
#define GPIOB    0x13
#define OLATB    0x15


#define MCP23S17_CS P1_23_PIN
#define SPI1_SELECT_MCP23S17 *MCP23S17_CS=0
#define SPI1_UNSELECT_MCP23S17 *MCP23S17_CS=1

#define MCP23S17_1_WRITE_OP 0x40
#define MCP23S17_2_WRITE_OP 0x42
#define MCP23S17_1_READ_OP (MCP23S17_1_WRITE_OP|1)
#define MCP23S17_2_READ_OP (MCP23S17_2_WRITE_OP|1)


u8 MCP23S17_WRITE( u8 chip, u8 address, u8 data );
#define MCP23S17_1_WRITE( a, d ) MCP23S17_WRITE( MCP23S17_1_WRITE_OP, a, d )
#define MCP23S17_2_WRITE( a, d ) MCP23S17_WRITE( MCP23S17_2_WRITE_OP, a, d )

#define MCP23S17_READ( c, a ) MCP23S17_WRITE(c,a,0)
#define MCP23S17_1_READ( a ) MCP23S17_READ( MCP23S17_1_READ_OP, a )
#define MCP23S17_2_READ( a ) MCP23S17_READ( MCP23S17_2_READ_OP, a )

// port A
#define MCP23S17_1_WRITE_DATA_A( d ) MCP23S17_1_WRITE( OLATA, d )
#define MCP23S17_2_WRITE_DATA_A( d ) MCP23S17_2_WRITE( OLATA, d )

#define MCP23S17_1_READ_DATA_A() MCP23S17_1_READ( GPIOA )
#define MCP23S17_2_READ_DATA_A() MCP23S17_2_READ( GPIOA )

#define MCP23S17_1_WRITE_DATA_A( d ) MCP23S17_1_WRITE( OLATA, d )
#define MCP23S17_2_WRITE_DATA_A( d ) MCP23S17_2_WRITE( OLATA, d )

#define MCP23S17_1_SET_DIRECTION_A( d ) MCP23S17_1_WRITE( IODIRA, d )
#define MCP23S17_2_SET_DIRECTION_A( d ) MCP23S17_2_WRITE( IODIRA, d )

// port B
#define MCP23S17_1_WRITE_DATA_B( d ) MCP23S17_1_WRITE( OLATB, d )
#define MCP23S17_2_WRITE_DATA_B( d ) MCP23S17_2_WRITE( OLATB, d )

#define MCP23S17_1_READ_DATA_B() MCP23S17_1_READ( GPIOB )
#define MCP23S17_2_READ_DATA_B() MCP23S17_2_READ( GPIOB )

#define MCP23S17_1_WRITE_DATA_B( d ) MCP23S17_1_WRITE( OLATB, d )
#define MCP23S17_2_WRITE_DATA_B( d ) MCP23S17_2_WRITE( OLATB, d )

#define MCP23S17_1_SET_DIRECTION_B( d ) MCP23S17_1_WRITE( IODIRB, d )
#define MCP23S17_2_SET_DIRECTION_B( d ) MCP23S17_2_WRITE( IODIRB, d )

void MCP23S17_initDirection( void ) ;

/* lcd */

#define DATABUS 0x62 // GPIO_PORT_MPIN2 (0xA0002188)
#define DCX_SET 0x80 // GPIO_PORT_SET0 (0xA0002200)
#define DCX_CLR 0xA0 // GPIO_PORT_SET1 (0xA0002204)
#define WR_SET  0x81 // GPIO_PORT_CLR0 (0xA0002280)
#define WR_CLR  0xA1 // GPIO_PORT_CLR1 (0xA0002284)

#define DCX_PIN (1 << 2)
#define WR_PIN  (1 << 12)

#define LCD_BASE ((volatile u32 *)0xA0002000)
#define LCD ((volatile u32 *)0xA0002188)//(LCD_BASE[DATABUS])

//extern volatile u32 * LCD;

static inline void writeCommand(u32 number) {
  LCD_BASE[DCX_CLR] = DCX_PIN;     // Clear DCX (Enter command mode)
  LCD_BASE[WR_CLR] = WR_PIN;       // Clear WR
  LCD_BASE[DATABUS] = number << 3; // Write Data Bus
  LCD_BASE[WR_SET] = WR_PIN;       // Set WR
  LCD_BASE[DCX_SET] = DCX_PIN;     // Set DCX (Resume data mode)
}

static inline void writeData(u32 data) {
  LCD_BASE[DATABUS] = data << 3 ; // Write to data bus
  LCD_BASE[WR_CLR] = WR_PIN;     // Clear WR
  LCD_BASE[WR_SET] = WR_PIN;     // Set WR
}

static inline void setDirection(u32 dir) {
  writeCommand(0x03);
  writeData(dir);
}

static inline void setX(u32 x) {
  writeCommand(0x21);
  writeData(x);
}

static inline void setY(u32 y) {
  writeCommand(0x20);
  writeData(y) ;
}

static inline void beginStream() {
  writeCommand(0x22) ;
}

static inline void setHorizontalRange(u32 start, u32 end) {
  writeCommand(0x38);
  writeData(end);
  writeCommand(0x39);
  writeData(start);
}

static inline void setVerticalRange(u32 start, u32 end) {
  writeCommand(0x36);
  writeData(end);
  writeCommand(0x37);
  writeData(start);
}

static inline void oscillatorControl(u32 val) {
  writeCommand(0x0f);
  writeData(val);
}

void write_command_16(uint16_t data);
void write_data_16(uint16_t data);

/* buttons */

#define BTN_DOWN 0xa0000023
#define BTN_B 0xa0000024
#define BTN_RIGHT 0xa0000027
#define BTN_A 0xa0000029
#define BTN_C 0xa000002A
#define BTN_UP 0xa000002D
#define BTN_LEFT 0xa0000039

#define aBtn ((volatile u8*)BTN_A)
#define bBtn ((volatile u8*)BTN_B)
#define cBtn ((volatile u8*)BTN_C)
#define upBtn ((volatile u8*)BTN_UP)
#define downBtn ((volatile u8*)BTN_DOWN)
#define leftBtn ((volatile u8*)BTN_LEFT)
#define rightBtn ((volatile u8*)BTN_RIGHT)

/* cpu */

extern u8 cpuMhz ; // 72 at start
void setCpuMhz( u8 mhz ) ;

#define OVERCLOCK setCpuMhz(72)
#define DOWNCLOCK setCpuMhz(48)

/* other */

void __wrap_exit( int num );

void gbGetTitle( void );
void rd_wr_mreq_reset( void );

#include "gdl.h"

/* gfm */

/*

#define SCREEN_WIDTH 200
#define SCREEN_HEIGHT 176
#define clDeep u8

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
    u16 : 8 ;
  };
};

void blitGfm( u8*gfm, clDeep*screen );

extern u8 *framebuffer ;
*/

#define CHAR_PADDING 1 /* padding between chars */
#define CHAR_SPACE 4 /* space char width */
#define textBfSize 64

void drawText( u8 x, u8 y, const char *txt );
void prints( u8 x, u8 y, const char * format, ...);
void setGdlFont( const u8 ** font );

/* timer */

/* max timer
  timer clock = 72000000/2
  (72000000/2)/1000 = 36000 by ms
  0x00FFFFFF / 36000 = 466ms
  SYST_CSR reload bit enabled ? 466ms + current value, max 930ms

  */


#define MHZ 1000000
#define CPU_MHZ 72

#define CPU_HZ (CPU_MHZ*MHZ)
#define TIMER_CLK_HZ (CPU_HZ/2)

#define TICKS_BY_MS (TIMER_CLK_HZ/1000) // 36000
#define TICKS_BY_US (TIMER_CLK_HZ/MHZ) // 36
#define TICKS_MAX 0x00ffffff

// max timer count is 0x00ffffff, max multiplier with 32b is 256
#define TICK_US_MULTIPLIER_SHIFT_13 ((1<<13)/TICKS_BY_US) // 8192/36 = 227
#define TICK_MS_MULTIPLIER_SHIFT_23 ((1<<23)/TICKS_BY_MS) // (1<<23)/36000 = 233

#define TICKS_TO_US(t) (((t)*TICK_US_MULTIPLIER_SHIFT_13)>>13)
#define TICKS_TO_MS(t) (((t)*TICK_MS_MULTIPLIER_SHIFT_23)>>23)

#define MS_TO_TICKS(ms) ((ms)*TICKS_BY_MS)
#define US_TO_TICKS(us) ((us)*TICKS_BY_US)

#define SYST_CVR ((volatile u32 *)0xE000E018)
#define SYST_RVR ((volatile u32 *)0xE000E014)
#define SYST_CSR ((volatile u32 *)0xE000E010)

void initSysTick( void );

#define resetSysTick *SYST_CVR=0
#define getSysTick ( (*SYST_CVR) & TICKS_MAX )
#define getTick ( TICKS_MAX - getSysTick )
#define getMs TICKS_TO_MS( getTick )
#define getUs TICKS_TO_US( getTick )

#define waitTicks( ticks ){\
  resetSysTick;\
  register u32 t = TICKS_MAX - (ticks) ;\
  while( getSysTick > t );\
}

#define waitMs( m ) waitTicks( MS_TO_TICKS( m ) )
#define waitUs( u ) waitTicks( US_TO_TICKS( u ) )

#endif

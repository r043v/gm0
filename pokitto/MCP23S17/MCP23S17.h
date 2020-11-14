/* MCP23S17 - drive the Microchip MCP23S17 16-bit Port Extender using SPI
* Copyright (c) 2010 Romilly Cocking
* Released under the MIT License: http://mbed.org/license/mit
*
* version 0.4
*/
#include "mbed.h"
 
#ifndef  MCP23S17_H
#define  MCP23S17_H
 
#define INTERRUPT_POLARITY_BIT 0x02
#define INTERRUPT_MIRROR_BIT   0x40
 
// all register addresses assume IOCON.BANK = 0 (POR default)
 
#define IODIRA   0x00
#define GPINTENA 0x04
#define DEFVALA  0x06
#define INTCONA  0x08
#define IOCON    0x0A
#define GPPUA    0x0C
#define GPIOA    0x12
#define OLATA    0x14
 
// Control settings
 
#define IOCON_BANK  0x80 // Banked registers
#define IOCON_BYTE_MODE 0x20 // Disables sequential operation. If bank = 0, operations toggle between A and B registers
#define IOCON_HAEN  0x08 // Hardware address enable
 
enum Polarity { ACTIVE_LOW , ACTIVE_HIGH };
enum Port { PORT_A, PORT_B };
 
class MCP23S17 {
public:
    MCP23S17(SPI& spi, PinName ncs, char writeOpcode);
    void direction(Port port, char direction);
    void configurePullUps(Port port, char offOrOn);
    void interruptEnable(Port port, char interruptsEnabledMask);
    void interruptPolarity(Polarity polarity);
    void mirrorInterrupts(bool mirror);
    void defaultValue(Port port, char valuesToCompare);
    void interruptControl(Port port, char interruptContolBits);
    char read(Port port);
    void write(Port port, char byte);
protected:
    SPI& _spi;
    DigitalOut _ncs;
    void _init();
    void _write(Port port, char address, char data);
    void _write(char address, char data);
    char _read(Port port, char address);
    char _read(char address);
    char _readOpcode;
    char _writeOpcode;
};
 
#endif

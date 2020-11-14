/* MCP23S17 - drive the Microchip MCP23S17 16-bit Port Extender using SPI
* Copyright (c) 2010 Romilly Cocking
* Released under the MIT License: http://mbed.org/license/mit
*
* version 0.4
*/
 
#include "mbed.h"
#include "MCP23S17.h"
 
MCP23S17::MCP23S17(SPI& spi, PinName ncs, char writeOpcode) : _spi(spi), _ncs(ncs) {
    _writeOpcode = writeOpcode;
    _readOpcode = _writeOpcode | 1; // low order bit = 1 for read
    _init();
}
 
char MCP23S17::_read(char address) {
    _ncs = 0;
    _spi.write(_readOpcode);
    _spi.write(address);
    char result = _spi.write(0);
    _ncs = 1;
    return result;
}
 
char MCP23S17::_read(Port port, char address) {
    return _read(address + (char) port);
}
 
void MCP23S17::_write(char address, char data) {
    _ncs = 0;
    _spi.write(_writeOpcode);
    _spi.write(address);
    _spi.write(data);
    _ncs = 1;
}
 
void  MCP23S17::_write(Port port, char address, char data) {
    _write(address + (char) port, data);
}
 
void MCP23S17::_init() {
    _write(IOCON, (IOCON_BYTE_MODE | IOCON_HAEN )); // Hardware addressing on, operations toggle between A and B registers
}
 
void MCP23S17::direction(Port port, char direction) {
    _write(port, IODIRA, direction);
}
 
void MCP23S17::configurePullUps(Port port, char offOrOn) {
    _write(port, GPPUA, offOrOn);
}
 
void MCP23S17::interruptEnable(Port port, char interruptsEnabledMask) {
    _write(port, GPINTENA, interruptsEnabledMask);
}
 
void MCP23S17::mirrorInterrupts(bool mirror) {
 char iocon = _read(IOCON);
    if (mirror) {
        iocon = iocon | INTERRUPT_MIRROR_BIT;
    } else {
        iocon = iocon & ~INTERRUPT_MIRROR_BIT;
    }
    _write(IOCON, iocon);
 
}
 
void  MCP23S17::interruptPolarity(Polarity polarity) {
    char iocon = _read(IOCON);
    if (polarity == ACTIVE_LOW) {
        iocon = iocon & ~INTERRUPT_POLARITY_BIT;
    } else {
        iocon = iocon | INTERRUPT_POLARITY_BIT;
    }
    _write(IOCON, iocon);
}
 
void MCP23S17::defaultValue(Port port, char valuesToCompare) {
    _write(port, DEFVALA, valuesToCompare);
}
 
void MCP23S17::interruptControl(Port port, char interruptContolBits) {
    _write(port, INTCONA, interruptContolBits);
}
 
void MCP23S17::write(Port port, char byte) {
    _write(port, OLATA, byte);
}
 
char MCP23S17::read(Port port) {
    return _read(port, GPIOA);
}
 
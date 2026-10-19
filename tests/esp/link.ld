MEMORY { iram : ORIGIN = 0x40100000, LENGTH = 0xE000  dram : ORIGIN = 0x3FFE8000, LENGTH = 0x14000 }
SECTIONS {
  .text : { *(.literal .text .literal.* .text.*) } > iram
  .rodata : { *(.rodata .rodata.*) } > dram
  .data : { *(.data .data.*) } > dram
  .bss : { *(.bss .bss.* COMMON) } > dram
}
ENTRY(_start)

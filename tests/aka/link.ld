ENTRY(m0boy_console_main)
SECTIONS {
 . = 0x42000020;
 .text : { *(.literal*) *(.text*) }
 .rodata : { *(.rodata*) }
 . = 0x3fc88000;
 .data : { *(.data*) }
 .bss : { *(.bss*) *(COMMON) }
}

 .text
 .literal_position
 .global _start
 .align 4
_start:
 movi a1, 0x3fffc000
 call0 compute
 break 1, 1

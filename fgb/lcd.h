

#ifndef __LCD_H__
#define __LCD_H__

#include "defs.h"
#include "pokitto.h"
#include <string.h>


/*
struct vissprite
{
	byte *buf;
	int x;
	byte pal, pri, pad[6];
};

struct scan
{
	int bg[64];
	int wnd[64];
	byte buf[256];
	byte pal1[128];
	un16 pal2[64];
	un32 pal4[64];
	byte pri[256];
	struct vissprite vs[16];
	int ns, l, x, y, s, t, u, v, wx, wy, wt, wv;
};

struct obj
{
	byte y;
	byte x;
	byte pat;
	byte flags;
};

struct lcd
{
	byte vbank[2][8192];
	union
	{
		byte mem[256];
		struct obj obj[40];
	} oam;
	byte pal[128];
};
*/

struct scan
{
	int ns, l, x, y, s, t, u, v, wx, wy, wt, wv;
};

struct obj
{
	byte y;
	byte x;
	byte pat;
	byte flags;
};

struct lcd
{
	byte vbank[8192]; // video ram
	union
	{
		byte mem[160];
		struct obj obj[40];
	} oam;
	byte pal[128];
};

extern struct lcd lcd;
extern struct scan scan;

#define LCD_ENABLE		(R_LCDC & 0x80)
#define SPRITE_ENABLE	(R_LCDC & 0x02)
#define WINDOW_ENABLE	(R_LCDC & 0x20)
#define BG_ENABLE			(R_LCDC & 1)

#define lcdControlRegister R_LCDC

//extern u_int32_t * palette ;

#define FLUSH_QUAD_SCALE					\
  " ldm %[pixelptr]!, {%[qd]}             \n"		\
  " uxtb %[pixel], %[qd]                  \n"		\
  " lsls %[pixel], %[pixel], 2            \n"		\
  " ldr %[pixel], [%[palette], %[pixel]]  \n"	       \
  " str %[pixel], [%[LCD], 0]             \n"	       \
  " movs %[pixel], 252                    \n"	       \
  " str %[WR], [%[LCD], %[pixel]]         \n"	       \
  " lsrs %[qd], %[qd], 8                  \n"	       \
  " str %[WR], [%[LCD], 124]              \n"	       \
						       \
  " uxtb %[pixel], %[qd]                  \n"	       \
  " lsls %[pixel], %[pixel], 2            \n"	       \
  " ldr %[pixel], [%[palette], %[pixel]]  \n"	       \
  " str %[pixel], [%[LCD], 0]             \n"	       \
  " movs %[pixel], 252                    \n"	       \
  " str %[WR], [%[LCD], %[pixel]]         \n"	       \
  " lsrs %[qd], %[qd], 8                  \n"	       \
  " str %[WR], [%[LCD], 124]              \n"	       \
						       \
  " movs %[pixel], 252                    \n"	       \
  " str %[WR], [%[LCD], %[pixel]]         \n"	       \
  " movs %[pixel], 252                    \n"	       \
  " str %[WR], [%[LCD], 124]              \n"	       \
						       \
  " uxtb %[pixel], %[qd]                  \n"	 \
  " lsls %[pixel], %[pixel], 2            \n"	 \
  " ldr %[pixel], [%[palette], %[pixel]]  \n"	 \
  " str %[pixel], [%[LCD], 0]             \n"	 \
  " movs %[pixel], 252                    \n"	 \
  " str %[WR], [%[LCD], %[pixel]]         \n"	 \
  " lsrs %[qd], %[qd], 8                  \n"	 \
  " str %[WR], [%[LCD], 124]              \n"	 \
						 \
  " uxtb %[pixel], %[qd]                  \n"	 \
  " lsls %[pixel], %[pixel], 2            \n"	 \
  " ldr %[pixel], [%[palette], %[pixel]]  \n"	 \
  " str %[pixel], [%[LCD], 0]             \n"	 \
  " movs %[pixel], 252                    \n"	 \
  " str %[WR], [%[LCD], %[pixel]]         \n"	 \
  "subs %[x], 4                           \n"	 \
  " str %[WR], [%[LCD], 124]              \n"


//#define u32 u_int32_t
//#define u8 u_int8_t

#define UbyteToByte(ub) (signed char)(ub)


#pragma pack(push, 1)

union sprite {
  u32 raw;
  struct {
    u8 y;
    u8 x;
    u8 data;
    union {
      u8 flag;
      struct {
        u8 z:1;
        u8 yflip:1;
        u8 xflip:1;
        u8 palette:1;
        u8:4;
      };
    };
  };
};

#pragma pack(pop)

  #define bx00000000 0
  #define bx00000001 1
  #define bx00000010 2
  #define bx00000011 3
  #define bx00000100 4
  #define bx00000101 5
  #define bx00000110 6
  #define bx00000111 7
  #define bx00001000 8
  #define bx00001001 9
  #define bx00001010 10
  #define bx00001011 11
  #define bx00001100 12
  #define bx00001101 13
  #define bx00001110 14
  #define bx00001111 15
  #define bx00010000 16
  #define bx00010001 17
  #define bx00010010 18
  #define bx00010011 19
  #define bx00010100 20
  #define bx00010101 21
  #define bx00010110 22
  #define bx00010111 23
  #define bx00011000 24
  #define bx00011001 25
  #define bx00011010 26
  #define bx00011011 27
  #define bx00011100 28
  #define bx00011101 29
  #define bx00011110 30
  #define bx00011111 31
  #define bx00100000 32
  #define bx00100001 33
  #define bx00100010 34
  #define bx00100011 35
  #define bx00100100 36
  #define bx00100101 37
  #define bx00100110 38
  #define bx00100111 39
  #define bx00101000 40
  #define bx00101001 41
  #define bx00101010 42
  #define bx00101011 43
  #define bx00101100 44
  #define bx00101101 45
  #define bx00101110 46
  #define bx00101111 47
  #define bx00110000 48
  #define bx00110001 49
  #define bx00110010 50
  #define bx00110011 51
  #define bx00110100 52
  #define bx00110101 53
  #define bx00110110 54
  #define bx00110111 55
  #define bx00111000 56
  #define bx00111001 57
  #define bx00111010 58
  #define bx00111011 59
  #define bx00111100 60
  #define bx00111101 61
  #define bx00111110 62
  #define bx00111111 63
  #define bx01000000 64
  #define bx01000001 65
  #define bx01000010 66
  #define bx01000011 67
  #define bx01000100 68
  #define bx01000101 69
  #define bx01000110 70
  #define bx01000111 71
  #define bx01001000 72
  #define bx01001001 73
  #define bx01001010 74
  #define bx01001011 75
  #define bx01001100 76
  #define bx01001101 77
  #define bx01001110 78
  #define bx01001111 79
  #define bx01010000 80
  #define bx01010001 81
  #define bx01010010 82
  #define bx01010011 83
  #define bx01010100 84
  #define bx01010101 85
  #define bx01010110 86
  #define bx01010111 87
  #define bx01011000 88
  #define bx01011001 89
  #define bx01011010 90
  #define bx01011011 91
  #define bx01011100 92
  #define bx01011101 93
  #define bx01011110 94
  #define bx01011111 95
  #define bx01100000 96
  #define bx01100001 97
  #define bx01100010 98
  #define bx01100011 99
  #define bx01100100 100
  #define bx01100101 101
  #define bx01100110 102
  #define bx01100111 103
  #define bx01101000 104
  #define bx01101001 105
  #define bx01101010 106
  #define bx01101011 107
  #define bx01101100 108
  #define bx01101101 109
  #define bx01101110 110
  #define bx01101111 111
  #define bx01110000 112
  #define bx01110001 113
  #define bx01110010 114
  #define bx01110011 115
  #define bx01110100 116
  #define bx01110101 117
  #define bx01110110 118
  #define bx01110111 119
  #define bx01111000 120
  #define bx01111001 121
  #define bx01111010 122
  #define bx01111011 123
  #define bx01111100 124
  #define bx01111101 125
  #define bx01111110 126
  #define bx01111111 127
  #define bx10000000 128
  #define bx10000001 129
  #define bx10000010 130
  #define bx10000011 131
  #define bx10000100 132
  #define bx10000101 133
  #define bx10000110 134
  #define bx10000111 135
  #define bx10001000 136
  #define bx10001001 137
  #define bx10001010 138
  #define bx10001011 139
  #define bx10001100 140
  #define bx10001101 141
  #define bx10001110 142
  #define bx10001111 143
  #define bx10010000 144
  #define bx10010001 145
  #define bx10010010 146
  #define bx10010011 147
  #define bx10010100 148
  #define bx10010101 149
  #define bx10010110 150
  #define bx10010111 151
  #define bx10011000 152
  #define bx10011001 153
  #define bx10011010 154
  #define bx10011011 155
  #define bx10011100 156
  #define bx10011101 157
  #define bx10011110 158
  #define bx10011111 159
  #define bx10100000 160
  #define bx10100001 161
  #define bx10100010 162
  #define bx10100011 163
  #define bx10100100 164
  #define bx10100101 165
  #define bx10100110 166
  #define bx10100111 167
  #define bx10101000 168
  #define bx10101001 169
  #define bx10101010 170
  #define bx10101011 171
  #define bx10101100 172
  #define bx10101101 173
  #define bx10101110 174
  #define bx10101111 175
  #define bx10110000 176
  #define bx10110001 177
  #define bx10110010 178
  #define bx10110011 179
  #define bx10110100 180
  #define bx10110101 181
  #define bx10110110 182
  #define bx10110111 183
  #define bx10111000 184
  #define bx10111001 185
  #define bx10111010 186
  #define bx10111011 187
  #define bx10111100 188
  #define bx10111101 189
  #define bx10111110 190
  #define bx10111111 191
  #define bx11000000 192
  #define bx11000001 193
  #define bx11000010 194
  #define bx11000011 195
  #define bx11000100 196
  #define bx11000101 197
  #define bx11000110 198
  #define bx11000111 199
  #define bx11001000 200
  #define bx11001001 201
  #define bx11001010 202
  #define bx11001011 203
  #define bx11001100 204
  #define bx11001101 205
  #define bx11001110 206
  #define bx11001111 207
  #define bx11010000 208
  #define bx11010001 209
  #define bx11010010 210
  #define bx11010011 211
  #define bx11010100 212
  #define bx11010101 213
  #define bx11010110 214
  #define bx11010111 215
  #define bx11011000 216
  #define bx11011001 217
  #define bx11011010 218
  #define bx11011011 219
  #define bx11011100 220
  #define bx11011101 221
  #define bx11011110 222
  #define bx11011111 223
  #define bx11100000 224
  #define bx11100001 225
  #define bx11100010 226
  #define bx11100011 227
  #define bx11100100 228
  #define bx11100101 229
  #define bx11100110 230
  #define bx11100111 231
  #define bx11101000 232
  #define bx11101001 233
  #define bx11101010 234
  #define bx11101011 235
  #define bx11101100 236
  #define bx11101101 237
  #define bx11101110 238
  #define bx11101111 239
  #define bx11110000 240
  #define bx11110001 241
  #define bx11110010 242
  #define bx11110011 243
  #define bx11110100 244
  #define bx11110101 245
  #define bx11110110 246
  #define bx11110111 247
  #define bx11111000 248
  #define bx11111001 249
  #define bx11111010 250
  #define bx11111011 251
  #define bx11111100 252
  #define bx11111101 253
  #define bx11111110 254
  #define bx11111111 255
  #define bx100000000 256

#endif

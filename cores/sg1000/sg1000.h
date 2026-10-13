#ifndef M0_SG1000_H
#define M0_SG1000_H
#include "z80.h"
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
#define SG_WIDTH 256
#define SG_HEIGHT 192
#define SG_LINES 262
#define SG_LINE_CYCLES 228
/* SG-1000 baseline: unbanked cartridge, 1 KiB work RAM, TMS9918A.
 * No SC-3000 keyboard, expansion RAM or cartridge mapper. */
typedef unsigned (*sg_compiled_fn)(z80_t *);
typedef struct {
    z80_t cpu;
    const uint8_t *rom;
    size_t rom_size;
    uint8_t ram[1024],vram[16384],vdp[8];
    uint16_t vdp_address,line;
    uint32_t line_cycles;
    uint8_t vdp_latch,vdp_pending,vdp_buffer,vdp_status,buttons[2],pause;
    uint16_t psg_tone[3];
    uint8_t psg_volume[4],psg_noise,psg_latch;
    /* Sample queue follows emulated time, including writes within a frame. */
    uint32_t audio_rate,audio_phase,audio_pending,audio_tick_phase;
    uint32_t audio_lfsr;
    uint16_t audio_count[4],audio_head,audio_tail;
    uint8_t audio_output[4],audio_buffer[512];
    uint32_t frames,compiled_steps,interpreted_steps;
    sg_compiled_fn compiled;
    void *platform;
    int (*draw_line)(void *,unsigned);
    void (*row)(void *,unsigned,const uint8_t *);
    void (*frame)(void *);
} sg1000_t;
void sg1000_init(sg1000_t *,const uint8_t *rom,size_t size);
void sg1000_reset(sg1000_t *);
unsigned sg1000_step(sg1000_t *);
void sg1000_buttons(sg1000_t *,unsigned player,uint8_t pressed);
void sg1000_pause(sg1000_t *,int pressed);
uint8_t sg1000_read(void *,uint16_t);
void sg1000_write(void *,uint16_t,uint8_t);
uint8_t sg1000_in(void *,uint16_t);
void sg1000_out(void *,uint16_t,uint8_t);
void sg1000_render_line(sg1000_t *,unsigned,uint8_t *);
void sg1000_audio_rate(sg1000_t *,unsigned);
void sg1000_audio_advance(sg1000_t *);
void sg1000_audio_render(sg1000_t *,uint8_t *,unsigned);
extern const uint32_t sg1000_palette[16];
#ifdef __cplusplus
}
#endif
#endif

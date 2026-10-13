#ifndef M0_Z80_H
#define M0_Z80_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum { Z80_C=1, Z80_N=2, Z80_PV=4, Z80_X=8, Z80_H=16, Z80_Y=32, Z80_Z=64, Z80_S=128 };
typedef uint8_t (*z80_read_fn)(void *, uint16_t);
typedef void (*z80_write_fn)(void *, uint16_t, uint8_t);
typedef struct {
    union { struct { uint8_t c,b; }; uint16_t bc; };
    union { struct { uint8_t e,d; }; uint16_t de; };
    union { struct { uint8_t l,h; }; uint16_t hl; };
    uint8_t a,f;
    uint16_t bc2,de2,hl2,af2,ix,iy,sp,pc;
    uint8_t i,r,im,iff1,iff2,halted,ei_delay,irq,nmi,irq_vector,fault;
    uint64_t cycles;
    void *user;
    z80_read_fn read,in;
    z80_write_fn write,out;
} z80_t;

/* Predecoded constants can be compiled through z80_execute in z80_ops.h.
 * Index: 0 none, 1 IX, 2 IY. Group: 0 base, 1 CB, 2 ED. */
typedef struct {
    uint16_t immediate,length;
    uint8_t opcode,index,group,displacement,refresh;
    uint32_t extra_cycles;
} z80_decoded_t;
void z80_reset(z80_t *cpu);
int z80_decode(const z80_t *cpu, z80_decoded_t *instruction);
/* Nonzero when an interrupt or a HALT cycle was serviced. */
unsigned z80_service(z80_t *cpu);
unsigned z80_step(z80_t *cpu);
void z80_finish(z80_t *cpu, unsigned cycles);
#ifdef __cplusplus
}
#endif
#endif

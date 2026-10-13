#include "z80_ops.h"
#include <stddef.h>
#include <string.h>
void z80_reset(z80_t *c) {
    memset(c,0,offsetof(z80_t,user));c->sp=0xffff;c->irq_vector=0xff;
}
int z80_decode(const z80_t *c,z80_decoded_t *d) {
    memset(d,0,sizeof(*d));
    uint32_t pos=0,prefixes=0;
#define BYTE() c->read(c->user,(uint16_t)(c->pc+pos++))
    unsigned op=BYTE();
    while(op==0xdd||op==0xfd) {
        if(++prefixes>=65536)return 0;
        d->index=op==0xdd?1:2;op=BYTE();
    }
    d->refresh=(uint8_t)(prefixes+1);
    if(op==0xcb){d->group=1;
        if(d->index){d->displacement=BYTE();d->extra_cycles=4*(prefixes-1);}
        else {d->refresh++;}
        op=BYTE();
    } else if(op==0xed){d->group=2;d->index=0;d->extra_cycles=4*prefixes;d->refresh++;op=BYTE();
    } else d->extra_cycles=4*prefixes;
    d->opcode=(uint8_t)op;
    unsigned x=op>>6,y=(op>>3)&7,z=op&7,p=y>>1,q=y&1,n=0;
    if(d->group==2) {if(x==1&&z==3)n=2;}
    else if(d->group==0) {
        unsigned memory=(x==0&&(z==4||z==5||z==6)&&y==6)||
            (x==1&&op!=0x76&&(y==6||z==6))||(x==2&&z==6);
        if(d->index&&memory)d->displacement=BYTE();
        if(x==0){if(z==0&&y>=2)n=1;else if(z==1&&!q)n=2;
            else if(z==2&&p>=2)n=2;else if(z==6)n=1;}
        else if(x==3){if(z==2||z==4||(z==3&&y==0)||(z==5&&q&&p==0))n=2;
            else if(z==6||(z==3&&(y==2||y==3)))n=1;}
    }
    if(n){d->immediate=BYTE();if(n==2){uint8_t hi=BYTE();d->immediate|=(uint16_t)hi<<8;}}
    if(pos>=65536)return 0;
    d->length=(uint16_t)pos;
#undef BYTE
    return 1;
}
void z80_finish(z80_t *c,unsigned cycles) {c->cycles+=cycles;if(c->ei_delay)c->ei_delay--;}
unsigned z80_service(z80_t *c) {
    unsigned cycles=0;
    if(c->nmi){c->nmi=0;c->halted=0;c->iff2=c->iff1;c->iff1=0;z80_push(c,c->pc);c->pc=0x66;cycles=11;}
    else if(c->irq&&c->iff1&&!c->ei_delay){c->halted=0;c->iff1=c->iff2=0;
        if(c->im==0&&(c->irq_vector&0xc7)!=0xc7){c->fault=1;return 0;}
        z80_push(c,c->pc);
        c->pc=c->im==2?z80_word(c,(uint16_t)((c->i<<8)|c->irq_vector)):c->im==0?(c->irq_vector&0x38):0x38;
        cycles=c->im==2?19:13;
    } else if(c->halted)cycles=4;
    if(cycles){c->r=(c->r&128)|((c->r+1)&127);z80_finish(c,cycles);}
    return cycles;
}
unsigned z80_step(z80_t *c) {
    unsigned cycles=z80_service(c);
    if(cycles||c->fault)return cycles;
    z80_decoded_t d;if(!z80_decode(c,&d)){c->fault=1;return 0;}
    cycles=z80_execute(c,&d);z80_finish(c,cycles);return cycles;
}

#ifndef M0_Z80_OPS_H
#define M0_Z80_OPS_H
#include "z80.h"
/* Shared by the reference interpreter and constant-specialized native code.
 * Every flag below has Z80 semantics, independent of the Game Boy runtime. */
static inline uint8_t z80_sz(uint8_t v) {
    return (v & (Z80_S|Z80_Y|Z80_X)) | (v ? 0 : Z80_Z);
}
static inline uint8_t z80_parity(uint8_t v) {
    v ^= v >> 4; v ^= v >> 2; v ^= v >> 1;
    return (v & 1) ? 0 : Z80_PV;
}
static inline uint8_t z80_szp(uint8_t v) { return z80_sz(v) | z80_parity(v); }
static inline uint8_t z80_rd(z80_t *c, uint16_t a) {
#ifdef Z80_NATIVE_READ
    return Z80_NATIVE_READ(c,a);
#else
    return c->read(c->user,a);
#endif
}
static inline void z80_wr(z80_t *c, uint16_t a, uint8_t v) {
#ifdef Z80_NATIVE_WRITE
    Z80_NATIVE_WRITE(c,a,v);
#else
    c->write(c->user,a,v);
#endif
}
static inline uint16_t z80_word(z80_t *c, uint16_t a) {
    uint8_t lo=z80_rd(c,a), hi=z80_rd(c,(uint16_t)(a+1));
    return (uint16_t)(lo | (hi<<8));
}
static inline void z80_store_word(z80_t *c, uint16_t a, uint16_t v) {
    z80_wr(c,a,(uint8_t)v); z80_wr(c,(uint16_t)(a+1),(uint8_t)(v>>8));
}
static inline void z80_push(z80_t *c,uint16_t v) {
    z80_wr(c,--c->sp,(uint8_t)(v>>8)); z80_wr(c,--c->sp,(uint8_t)v);
}
static inline uint16_t z80_pop(z80_t *c) {
    uint8_t lo=z80_rd(c,c->sp++), hi=z80_rd(c,c->sp++);
    return (uint16_t)(lo | (hi<<8));
}
static inline uint16_t *z80_hl(z80_t *c,unsigned idx) { return idx==1?&c->ix:idx==2?&c->iy:&c->hl; }
static inline uint16_t *z80_pair(z80_t *c,unsigned p,unsigned idx) {
    return p==0?&c->bc:p==1?&c->de:p==2?z80_hl(c,idx):&c->sp;
}
static inline uint16_t z80_address(z80_t *c,const z80_decoded_t *d) {
    return (uint16_t)(*z80_hl(c,d->index)+(d->index?(int8_t)d->displacement:0));
}
static inline uint8_t z80_get(z80_t *c,unsigned r,const z80_decoded_t *d,unsigned plain) {
    switch(r) {
    case 0:return c->b; case 1:return c->c; case 2:return c->d; case 3:return c->e;
    case 4:return d->index&&!plain?(uint8_t)(*z80_hl(c,d->index)>>8):c->h;
    case 5:return d->index&&!plain?(uint8_t)*z80_hl(c,d->index):c->l;
    case 6:return z80_rd(c,z80_address(c,d)); default:return c->a;
    }
}
static inline void z80_set(z80_t *c,unsigned r,const z80_decoded_t *d,unsigned plain,uint8_t v) {
    switch(r) {
    case 0:c->b=v;break; case 1:c->c=v;break; case 2:c->d=v;break; case 3:c->e=v;break;
    case 4:if(d->index&&!plain) *z80_hl(c,d->index)=(*z80_hl(c,d->index)&255)|(v<<8); else c->h=v;break;
    case 5:if(d->index&&!plain) *z80_hl(c,d->index)=(*z80_hl(c,d->index)&0xff00)|v; else c->l=v;break;
    case 6:z80_wr(c,z80_address(c,d),v);break; default:c->a=v;break;
    }
}
static inline int z80_condition(z80_t *c,unsigned cc) {
    switch(cc) {
    case 0:return !(c->f&Z80_Z); case 1:return !!(c->f&Z80_Z);
    case 2:return !(c->f&Z80_C); case 3:return !!(c->f&Z80_C);
    case 4:return !(c->f&Z80_PV);case 5:return !!(c->f&Z80_PV);
    case 6:return !(c->f&Z80_S); default:return !!(c->f&Z80_S);
    }
}
static inline uint8_t z80_add8(z80_t *c,uint8_t a,uint8_t b,unsigned carry,unsigned sub) {
    unsigned value=sub?(unsigned)a-b-carry:(unsigned)a+b+carry;
    uint8_t r=(uint8_t)value;
    c->f=z80_sz(r) | ((a^b^r)&Z80_H) | ((value>>8)&1) |
        (((sub?(a^b):(uint8_t)~(a^b))&(a^r)&128)?Z80_PV:0) | (sub?Z80_N:0);
    return r;
}
static inline void z80_alu(z80_t *c,unsigned op,uint8_t value) {
    switch(op) {
    case 0:c->a=z80_add8(c,c->a,value,0,0);break;
    case 1:c->a=z80_add8(c,c->a,value,c->f&1,0);break;
    case 2:c->a=z80_add8(c,c->a,value,0,1);break;
    case 3:c->a=z80_add8(c,c->a,value,c->f&1,1);break;
    case 4:c->a&=value;c->f=z80_szp(c->a)|Z80_H;break;
    case 5:c->a^=value;c->f=z80_szp(c->a);break;
    case 6:c->a|=value;c->f=z80_szp(c->a);break;
    default:(void)z80_add8(c,c->a,value,0,1);c->f=(c->f&~(Z80_Y|Z80_X))|(value&(Z80_Y|Z80_X));break;
    }
}
static inline uint8_t z80_incdec(z80_t *c,uint8_t value,unsigned dec) {
    uint8_t r=(uint8_t)(value+(dec?-1:1));
    c->f=(c->f&Z80_C)|z80_sz(r)|((value^r)&Z80_H)|(dec?Z80_N:0)|
        ((value==(dec?0x80:0x7f))?Z80_PV:0);
    return r;
}
static inline uint8_t z80_rotate(z80_t *c,uint8_t v,unsigned op) {
    unsigned carry=0;uint8_t r=v;
    switch(op) {
    case 0:carry=v>>7;r=(uint8_t)((v<<1)|carry);break;
    case 1:carry=v&1;r=(uint8_t)((v>>1)|(carry<<7));break;
    case 2:carry=v>>7;r=(uint8_t)((v<<1)|(c->f&1));break;
    case 3:carry=v&1;r=(uint8_t)((v>>1)|((c->f&1)<<7));break;
    case 4:carry=v>>7;r=(uint8_t)(v<<1);break;
    case 5:carry=v&1;r=(uint8_t)((v>>1)|(v&128));break;
    case 6:carry=v>>7;r=(uint8_t)((v<<1)|1);break;
    case 7:carry=v&1;r=(uint8_t)(v>>1);break;
    }
    c->f=z80_szp(r)|carry;return r;
}
static inline void z80_daa(z80_t *c) {
    uint8_t old=c->a, adjust=0, carry=c->f&Z80_C;
    if ((c->f&Z80_H) || (old&15)>9) adjust|=6;
    if (carry || old>0x99) {adjust|=0x60;carry=Z80_C;}
    c->a=(uint8_t)(old+((c->f&Z80_N)?-adjust:adjust));
    c->f=(c->f&Z80_N)|carry|((old^c->a)&Z80_H)|z80_szp(c->a);
}
#ifndef Z80_NATIVE_INLINE
#define Z80_NATIVE_INLINE
#endif
static inline Z80_NATIVE_INLINE unsigned z80_execute(z80_t *c,const z80_decoded_t *d) {
    unsigned op=d->opcode,x=op>>6,y=(op>>3)&7,z=op&7,p=y>>1,q=y&1;
    unsigned cycles=4;uint16_t nn=d->immediate;
    c->pc=(uint16_t)(c->pc+d->length);
    c->r=(c->r&128)|((c->r+d->refresh)&127);
    if(d->group==1) {
        unsigned r=d->index?6:z;
        uint8_t value=z80_get(c,r,d,1),out=value;
        if(x==0) out=z80_rotate(c,value,y);
        else if(x==1) {
            uint8_t xy=d->index?(uint8_t)(z80_address(c,d)>>8):value;
            c->f=(c->f&Z80_C)|Z80_H|(xy&(Z80_Y|Z80_X))|
                ((value&(1u<<y))?((y==7)?Z80_S:0):(Z80_Z|Z80_PV));
        } else if(x==2) out=value&~(1u<<y);
        else out=value|(1u<<y);
        if(x!=1) {
            z80_set(c,r,d,1,out);
            if(d->index&&z!=6) z80_set(c,z,d,1,out);
        }
        cycles=d->index?(x==1?20:23):z==6?(x==1?12:15):8;
    } else if(d->group==2) {
        cycles=8;
        if(x==1) switch(z) {
        case 0:{uint8_t v=c->in(c->user,c->bc);if(y!=6) z80_set(c,y,d,1,v);c->f=(c->f&1)|z80_szp(v);cycles=12;break;}
        case 1:c->out(c->user,c->bc,y==6?0:z80_get(c,y,d,1));cycles=12;break;
        case 2:{
            uint16_t a=c->hl,b=*z80_pair(c,p,0);unsigned carry=c->f&1;
            uint32_t value=q?(uint32_t)a+b+carry:(uint32_t)a-b-carry;
            uint16_t r=(uint16_t)value;
            c->f=((r>>8)&(Z80_S|Z80_Y|Z80_X))|(r?0:Z80_Z)|
                (((a^b^r)&0x1000)?Z80_H:0)|((value>>16)&1)|(q?0:Z80_N)|
                ((((q?~(a^b):(a^b))&(a^r))&0x8000)?Z80_PV:0);
            c->hl=r;cycles=15;break;}
        case 3:if(q)*z80_pair(c,p,0)=z80_word(c,nn);else z80_store_word(c,nn,*z80_pair(c,p,0));cycles=20;break;
        case 4:c->a=z80_add8(c,0,c->a,0,1);break;
        case 5:c->pc=z80_pop(c);c->iff1=c->iff2;cycles=14;break;
        case 6:{static const uint8_t modes[8]={0,0,1,2,0,0,1,2};c->im=modes[y];break;}
        case 7:
            if(y==0)c->i=c->a;else if(y==1)c->r=c->a;
            else if(y<4){c->a=y==2?c->i:c->r;c->f=(c->f&1)|z80_sz(c->a)|(c->iff2?Z80_PV:0);}
            else if(y<6){uint8_t v=z80_rd(c,c->hl),a=c->a;
                if(y==4){z80_wr(c,c->hl,(uint8_t)((a<<4)|(v>>4)));c->a=(a&0xf0)|(v&15);}
                else{z80_wr(c,c->hl,(uint8_t)((v<<4)|(a&15)));c->a=(a&0xf0)|(v>>4);}
                c->f=(c->f&1)|z80_szp(c->a);}
            cycles=y<4?9:y<6?18:8;break;
        } else if(x==2&&y>=4&&z<=3) {
            int delta=(y&1)?-1:1;uint8_t value;
            unsigned repeat=0;
            if(z==0){value=z80_rd(c,c->hl);z80_wr(c,c->de,value);c->hl+=delta;c->de+=delta;c->bc--;
                uint8_t sum=(uint8_t)(c->a+value);
                c->f=(c->f&(Z80_S|Z80_Z|Z80_C))|(c->bc?Z80_PV:0)|(sum&Z80_X)|((sum&2)<<4);repeat=c->bc!=0;
            } else if(z==1){value=z80_rd(c,c->hl);uint8_t old_c=c->f&1,r=z80_add8(c,c->a,value,0,1);
                c->hl+=delta;c->bc--;uint8_t adjust=(uint8_t)(r-((c->f&Z80_H)?1:0));
                c->f=(c->f&(Z80_S|Z80_Z|Z80_H|Z80_N))|old_c|(c->bc?Z80_PV:0)|
                    (adjust&Z80_X)|((adjust&2)<<4);repeat=c->bc&&r;
            } else {
                unsigned sum;
                if(z==2){value=c->in(c->user,c->bc);z80_wr(c,c->hl,value);c->b--;c->hl+=delta;
                    sum=value+(uint8_t)(c->c+delta);
                } else{value=z80_rd(c,c->hl);c->b--;c->hl+=delta;c->out(c->user,c->bc,value);sum=value+c->l;}
                c->f=z80_sz(c->b)|(value&128?Z80_N:0)|(sum>255?(Z80_H|Z80_C):0)|z80_parity((sum&7)^c->b);
                repeat=c->b!=0;
            }
            cycles=16;if(y>=6&&repeat){c->pc-=2;cycles=21;}
        }
    } else if(x==0) switch(z) {
    case 0:
        if(y==1){uint16_t af=(uint16_t)((c->a<<8)|c->f);c->a=c->af2>>8;c->f=c->af2;c->af2=af;}
        else if(y==2){cycles=8;if(--c->b){c->pc+=(int8_t)nn;cycles=13;}}
        else if(y>=3){cycles=y==3?12:7;if(y==3||z80_condition(c,y-4)){c->pc+=(int8_t)nn;cycles=12;}}
        break;
    case 1:if(!q){*z80_pair(c,p,d->index)=nn;cycles=10;}else{
        uint16_t a=*z80_hl(c,d->index),b=*z80_pair(c,p,d->index);uint32_t v=(uint32_t)a+b;
        *z80_hl(c,d->index)=(uint16_t)v;
        c->f=(c->f&(Z80_S|Z80_Z|Z80_PV))|((v>>8)&(Z80_Y|Z80_X))|
            (((a^b^v)&0x1000)?Z80_H:0)|(v>>16);cycles=11;}break;
    case 2:{uint16_t a=p==0?c->bc:p==1?c->de:nn;
        if(p==2){if(q)*z80_hl(c,d->index)=z80_word(c,a);else z80_store_word(c,a,*z80_hl(c,d->index));cycles=16;}
        else{if(q)c->a=z80_rd(c,a);else z80_wr(c,a,c->a);cycles=p==3?13:7;}break;}
    case 3:*z80_pair(c,p,d->index)+=q?-1:1;cycles=6;break;
    case 4:case 5:{uint8_t v=z80_get(c,y,d,0);z80_set(c,y,d,0,z80_incdec(c,v,z==5));cycles=y==6?(d->index?19:11):4;break;}
    case 6:z80_set(c,y,d,0,(uint8_t)nn);cycles=y==6?(d->index?15:10):7;break;
    case 7:
        if(y<4){uint8_t saved=c->f; c->a=z80_rotate(c,c->a,y);c->f=(saved&(Z80_S|Z80_Z|Z80_PV))|(c->a&(Z80_Y|Z80_X))|(c->f&1);}
        else if(y==4)z80_daa(c);
        else if(y==5){c->a=~c->a;c->f=(c->f&(Z80_S|Z80_Z|Z80_PV|Z80_C))|Z80_H|Z80_N|(c->a&(Z80_Y|Z80_X));}
        else if(y==6)c->f=(c->f&(Z80_S|Z80_Z|Z80_PV))|Z80_C|(c->a&(Z80_Y|Z80_X));
        else{unsigned carry=c->f&1;c->f=(c->f&(Z80_S|Z80_Z|Z80_PV))|(carry?Z80_H:Z80_C)|(c->a&(Z80_Y|Z80_X));}
        break;
    } else if(x==1) {
        if(op==0x76)c->halted=1;
        else{unsigned plain=y==6||z==6;z80_set(c,y,d,plain,z80_get(c,z,d,plain));cycles=plain?(d->index?15:7):4;}
    } else if(x==2){z80_alu(c,y,z80_get(c,z,d,0));cycles=z==6?(d->index?15:7):4;}
    else switch(z) {
    case 0:cycles=5;if(z80_condition(c,y)){c->pc=z80_pop(c);cycles=11;}break;
    case 1:
        if(!q){uint16_t v=z80_pop(c);if(p==3){c->a=v>>8;c->f=v;}else *z80_pair(c,p,d->index)=v;cycles=10;}
        else if(p==0){c->pc=z80_pop(c);cycles=10;}
        else if(p==1){uint16_t v=c->bc;c->bc=c->bc2;c->bc2=v;v=c->de;c->de=c->de2;c->de2=v;v=c->hl;c->hl=c->hl2;c->hl2=v;}
        else if(p==2)c->pc=*z80_hl(c,d->index);
        else{c->sp=*z80_hl(c,d->index);cycles=6;}break;
    case 2:if(z80_condition(c,y))c->pc=nn;cycles=10;break;
    case 3:
        if(y==0){c->pc=nn;cycles=10;}
        else if(y==2){c->out(c->user,(uint16_t)((c->a<<8)|(uint8_t)nn),c->a);cycles=11;}
        else if(y==3){c->a=c->in(c->user,(uint16_t)((c->a<<8)|(uint8_t)nn));cycles=11;}
        else if(y==4){uint16_t v=z80_word(c,c->sp);z80_store_word(c,c->sp,*z80_hl(c,d->index));*z80_hl(c,d->index)=v;cycles=19;}
        else if(y==5){uint16_t v=c->de;c->de=c->hl;c->hl=v;}
        else if(y==6){c->iff1=c->iff2=0;c->ei_delay=0;}
        else if(y==7){c->iff1=c->iff2=1;c->ei_delay=2;}
        break;
    case 4:cycles=10;if(z80_condition(c,y)){z80_push(c,c->pc);c->pc=nn;cycles=17;}break;
    case 5:if(!q){z80_push(c,p==3?(uint16_t)((c->a<<8)|c->f):*z80_pair(c,p,d->index));cycles=11;}
        else{z80_push(c,c->pc);c->pc=nn;cycles=17;}break;
    case 6:z80_alu(c,y,(uint8_t)nn);cycles=7;break;
    case 7:z80_push(c,c->pc);c->pc=(uint16_t)(y*8);cycles=11;break;
    }
    return cycles+d->extra_cycles;
}
#endif

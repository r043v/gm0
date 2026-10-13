#include "sg1000.h"
#include "z80_ops.h"
#include <string.h>
const uint32_t sg1000_palette[16]={
    0x000000,0x000000,0x21c842,0x5edc78,0x5455ed,0x7d76fc,0xd4524d,0x42ebf5,
    0xfc5554,0xff7978,0xd4c154,0xe6ce80,0x21b03b,0xc95bba,0xcccccc,0xffffff
};
static void irq(sg1000_t *s){s->cpu.irq=!!((s->vdp_status&128)&&(s->vdp[1]&32));}
uint8_t sg1000_read(void *ctx,uint16_t address){
    sg1000_t *s=ctx;
    if(address>=0xc000)return s->ram[address&1023];
    /* Baseline power-of-two boards mirror ROM through the cartridge window.
     * Irregular 40/48 KiB dumps map directly, with unmapped reads at 0xff. */
    if(s->rom_size && !(s->rom_size&(s->rom_size-1)))address&=(uint16_t)(s->rom_size-1);
    return address<s->rom_size?s->rom[address]:0xff;
}
void sg1000_write(void *ctx,uint16_t address,uint8_t value){
    sg1000_t *s=ctx;if(address>=0xc000)s->ram[address&1023]=value;
}
uint8_t sg1000_in(void *ctx,uint16_t port){
    sg1000_t *s=ctx;port&=255;
    if(port>=0x80&&port<0xc0){
        s->vdp_pending=0;
        if(port&1){uint8_t v=s->vdp_status;s->vdp_status&=31;irq(s);return v;}
        uint8_t v=s->vdp_buffer;s->vdp_buffer=s->vram[s->vdp_address];
        s->vdp_address=(s->vdp_address+1)&0x3fff;return v;
    }
    if(port>=0xc0){
        if(!(port&1))return (uint8_t)~(s->buttons[0]|((s->buttons[1]&3)<<6));
        return (uint8_t)(0xf0|(~(s->buttons[1]>>2)&15));
    }
    return 0xff;
}
void sg1000_out(void *ctx,uint16_t port,uint8_t value){
    sg1000_t *s=ctx;port&=255;
    if(port>=0x40&&port<0x80){
        sg1000_audio_advance(s);
        if(value&128)s->psg_latch=(value>>4)&7;
        unsigned r=s->psg_latch,ch=r>>1;
        if(r&1)s->psg_volume[ch]=value&15;
        else if(ch==3){s->psg_noise=value&7;s->audio_lfsr=0x10000;s->audio_count[3]=0;s->audio_output[3]=0;}
        else if(value&128)s->psg_tone[ch]=(s->psg_tone[ch]&0x3f0)|(value&15);
        else s->psg_tone[ch]=(s->psg_tone[ch]&15)|((value&63)<<4);
    } else if(port>=0x80&&port<0xc0){
        if(!(port&1)){s->vdp_pending=0;s->vram[s->vdp_address]=value;s->vdp_buffer=value;s->vdp_address=(s->vdp_address+1)&0x3fff;}
        else if(!s->vdp_pending){s->vdp_latch=value;s->vdp_pending=1;}
        else{s->vdp_pending=0;
            if(value&128){s->vdp[value&7]=s->vdp_latch;irq(s);}
            else{s->vdp_address=(uint16_t)(((value&63)<<8)|s->vdp_latch);
                if(!(value&64)){s->vdp_buffer=s->vram[s->vdp_address];s->vdp_address=(s->vdp_address+1)&0x3fff;}}
        }
    }
}
void sg1000_init(sg1000_t *s,const uint8_t *rom,size_t size){
    memset(s,0,sizeof(*s));s->rom=rom;s->rom_size=size;
    s->cpu.user=s;s->cpu.read=sg1000_read;s->cpu.write=sg1000_write;
    s->cpu.in=sg1000_in;s->cpu.out=sg1000_out;sg1000_reset(s);
}
void sg1000_reset(sg1000_t *s){
    z80_reset(&s->cpu);memset(s->ram,0,sizeof(s->ram));memset(s->vram,0,sizeof(s->vram));memset(s->vdp,0,sizeof(s->vdp));
    s->vdp_address=s->line=s->line_cycles=0;s->vdp_pending=s->vdp_latch=s->vdp_buffer=0;
    s->vdp_status=31;s->frames=s->compiled_steps=s->interpreted_steps=0;s->pause=0;
    memset(s->buttons,0,sizeof(s->buttons));memset(s->psg_tone,0,sizeof(s->psg_tone));
    memset(s->psg_volume,15,sizeof(s->psg_volume));s->psg_noise=s->psg_latch=0;
    s->audio_phase=s->audio_pending=s->audio_tick_phase=0;
    s->audio_head=s->audio_tail=0;s->audio_lfsr=0x10000;
    memset(s->audio_count,0,sizeof(s->audio_count));memset(s->audio_output,0,sizeof(s->audio_output));
}
void sg1000_buttons(sg1000_t *s,unsigned player,uint8_t pressed){if(player<2)s->buttons[player]=pressed&63;}
void sg1000_pause(sg1000_t *s,int pressed){if(pressed&&!s->pause)s->cpu.nmi=1;s->pause=!!pressed;}
unsigned sg1000_step(sg1000_t *s){
    /* Fast-forward HALT only to the next raster event. Refresh and EI delay
     * still count every four T-states; IRQ/NMI are tested before batching. */
    unsigned cycles;
    if(s->cpu.halted&&!s->cpu.nmi&&!(s->cpu.irq&&s->cpu.iff1&&!s->cpu.ei_delay)){
        unsigned ticks=(SG_LINE_CYCLES-s->line_cycles+3)/4;
        cycles=ticks*4;s->cpu.r=(s->cpu.r&128)|((s->cpu.r+ticks)&127);
        s->cpu.cycles+=cycles;
        s->cpu.ei_delay=s->cpu.ei_delay>ticks?s->cpu.ei_delay-ticks:0;
    }else if(s->cpu.nmi||s->cpu.halted||(s->cpu.irq&&s->cpu.iff1&&!s->cpu.ei_delay))
        cycles=z80_service(&s->cpu);
    else cycles=0;
    if(!cycles&&!s->cpu.fault){
        if(s->compiled&&s->cpu.pc<s->rom_size)cycles=s->compiled(&s->cpu);
        if(cycles){s->cpu.cycles+=cycles;if(s->cpu.ei_delay)s->cpu.ei_delay--;s->compiled_steps++;}
        else{cycles=z80_step(&s->cpu);s->interpreted_steps++;}
    }
    s->line_cycles+=cycles;
    if(s->audio_rate)s->audio_pending+=cycles;
    while(s->line_cycles>=SG_LINE_CYCLES){s->line_cycles-=SG_LINE_CYCLES;
        sg1000_audio_advance(s);
        if(s->line<SG_HEIGHT){uint8_t pixels[SG_WIDTH];
            int draw=!s->draw_line||s->draw_line(s->platform,s->line);
            sg1000_render_line(s,s->line,draw?pixels:0);
            if(draw&&s->row)s->row(s->platform,s->line,pixels);}
        if(++s->line==SG_HEIGHT){s->vdp_status|=128;irq(s);}
        if(s->line==SG_LINES){s->line=0;s->frames++;if(s->frame)s->frame(s->platform);}
    }
    return cycles;
}

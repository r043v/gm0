#include "sg1000.h"
#include <string.h>
#define SG_CLOCK 3579545u
/* SN76489AN: three square waves, 2 dB attenuation, TI noise LFSR (15 stages plus output delay).
 * The output queue is filled on raster events and before PSG writes so
 * short notes and volume changes retain their position within a frame. */
static const uint8_t level[16]={30,24,19,15,12,10,8,6,5,4,3,2,2,1,1,0};
void sg1000_audio_rate(sg1000_t *s,unsigned rate){
    s->audio_rate=rate>48000?48000:rate;
    s->audio_phase=s->audio_pending=s->audio_tick_phase=0;
    s->audio_head=s->audio_tail=0;
}
static uint8_t sample(sg1000_t *s){
    /* PSG clock divider 16. Bresenham preserves fractional ticks/sample. */
    s->audio_tick_phase+=SG_CLOCK;
    unsigned divisor=s->audio_rate*16,ticks=s->audio_tick_phase/divisor;
    s->audio_tick_phase%=divisor;
    int mix=0;
    for(unsigned ch=0;ch<4;ch++){
        unsigned remaining=ticks;
        unsigned period=ch<3?s->psg_tone[ch]:
            (s->psg_noise&3)==3?s->psg_tone[2]*2:32u<<(s->psg_noise&3);
        /* Zero tone divider acts as 0x400 on the TI part. */
        if(!period)period=ch<3?1024:2048;
        while(remaining>=s->audio_count[ch]){
            remaining-=s->audio_count[ch];s->audio_count[ch]=(uint16_t)period;
            if(ch<3)s->audio_output[ch]^=1;
            else{
                unsigned feedback=(s->psg_noise&4)?(((s->audio_lfsr>>2)^(s->audio_lfsr>>3))&1):((s->audio_lfsr>>2)&1);
                s->audio_lfsr=(s->audio_lfsr>>1)|(feedback<<16);
                s->audio_output[ch]=s->audio_lfsr&1;
            }
        }
        s->audio_count[ch]-=(uint16_t)remaining;
        mix+=s->audio_output[ch]?level[s->psg_volume[ch]]:-level[s->psg_volume[ch]];
    }
    return (uint8_t)(128+mix);
}
void sg1000_audio_advance(sg1000_t *s){
    if(!s->audio_rate)return;
    s->audio_phase+=s->audio_pending*s->audio_rate;s->audio_pending=0;
    while(s->audio_phase>=SG_CLOCK){
        s->audio_phase-=SG_CLOCK;
        uint8_t v=sample(s);unsigned next=(s->audio_head+1)&511;
        if(next!=s->audio_tail){s->audio_buffer[s->audio_head]=v;s->audio_head=(uint16_t)next;}
    }
}
void sg1000_audio_render(sg1000_t *s,uint8_t *buffer,unsigned n){
    while(n--){
        if(s->audio_tail==s->audio_head)*buffer++=128;
        else{*buffer++=s->audio_buffer[s->audio_tail];s->audio_tail=(s->audio_tail+1)&511;}
    }
}

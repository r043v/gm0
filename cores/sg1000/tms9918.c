#include "sg1000.h"
#include <string.h>
static uint8_t vr(sg1000_t *s,unsigned a){return s->vram[a&0x3fff];}
/* The bare-metal nano libc fills byte-by-byte. Use aligned word stores for
 * raster buffers while retaining the public API's unaligned-buffer support. */
typedef uint32_t pixel_word __attribute__((may_alias));
static void backdrop_fill(uint8_t *pixels,unsigned color){
    if(!pixels)return;
    if((uintptr_t)pixels&3){memset(pixels,(int)color,SG_WIDTH);return;}
    pixel_word *p=(pixel_word *)pixels;uint32_t value=color*0x01010101u;
    for(unsigned i=0;i<SG_WIDTH/4;i++)p[i]=value;
}
void sg1000_render_line(sg1000_t *s,unsigned y,uint8_t *pixels){
    unsigned backdrop=s->vdp[7]&15;
    if(!(s->vdp[1]&64)||y>=SG_HEIGHT){backdrop_fill(pixels,backdrop);return;}
    unsigned text=!!(s->vdp[1]&16),multi=!!(s->vdp[1]&8),graphics2=!!(s->vdp[0]&2);
    unsigned name=(s->vdp[2]&15)<<10,pattern=(s->vdp[4]&7)<<11,color=s->vdp[3]<<6;
    if(pixels){
    if(text){memset(pixels,(int)backdrop,8);memset(pixels+248,(int)backdrop,8);}
    unsigned columns=text?40:32,width=text?6:8;
    unsigned rowname=name+(y/8)*columns;
    uint8_t *dest=pixels+(text?8:0);
    for(unsigned tx=0;tx<columns;tx++){
        unsigned tile=vr(s,rowname+tx),bits=0,colors;
        if(text){bits=vr(s,pattern+tile*8+(y&7));colors=s->vdp[7];}
        else if(multi){colors=vr(s,pattern+tile*8+((y&31)>>2));
            unsigned hi=colors>>4,lo=colors&15;
            memset(dest,hi?hi:backdrop,4);memset(dest+4,lo?lo:backdrop,4);dest+=8;continue;}
        else{
            unsigned offset=tile*8+(y&7);
            if(graphics2){offset|=(y&0xc0)<<5;
                unsigned pmask=((s->vdp[4]&3)<<11)|0x7ff;
                unsigned cmask=((s->vdp[3]&127)<<6)|63;
                bits=vr(s,((s->vdp[4]&4)<<11)|(offset&pmask));
                colors=vr(s,((s->vdp[3]&128)<<6)|(offset&cmask));
            }else{bits=vr(s,pattern+offset);colors=vr(s,color+(tile>>3));}
        }
        unsigned hi=colors>>4,lo=text?backdrop:colors&15;
        if(!hi&&!text)hi=backdrop;
        if(!lo)lo=backdrop;
        for(unsigned xx=0;xx<width;xx++){*dest++=(bits&128)?hi:lo;bits<<=1;}
    }
    }
    if(text)return;
    uint8_t occupied[32]={0};unsigned count=0;
    unsigned table=(s->vdp[5]&127)<<7,base=(s->vdp[6]&7)<<11;
    unsigned size=(s->vdp[1]&2)?16:8,shift=s->vdp[1]&1,zoom=1u<<shift;
    for(unsigned i=0;i<32;i++){
        unsigned sy=vr(s,table+i*4);if(sy==208)break;
        int top=(int)sy+1;if(sy>208)top-=256;
        int row=(int)y-top;if(row<0||row>=(int)(size*zoom))continue;
        if(++count==5){if(!(s->vdp_status&64))s->vdp_status=(s->vdp_status&0xa0)|64|i;break;}
        unsigned attr=vr(s,table+i*4+3),c=attr&15;
        int left=(int)vr(s,table+i*4+1)-((attr&128)?32:0);
        unsigned tile=vr(s,table+i*4+2);if(size==16)tile&=~3u;
        unsigned patternrow=(unsigned)row>>shift;
        if(!c)continue;
        for(unsigned xx=0;xx<size*zoom;xx++){
            int x=left+(int)xx;if(x<0||x>=SG_WIDTH)continue;
            unsigned column=xx>>shift;
            unsigned address=base+tile*8+(patternrow&7)+(patternrow>=8?8:0)+(column>=8?16:0);
            if(!(vr(s,address)&(128>>(column&7))))continue;
            unsigned bit=1u<<(x&7);
            if(occupied[x>>3]&bit)s->vdp_status|=32;
            else{occupied[x>>3]|=bit;if(pixels)pixels[x]=(uint8_t)c;}
        }
    }
    if(!(s->vdp_status&64))s->vdp_status=(s->vdp_status&0xe0)|31;
}

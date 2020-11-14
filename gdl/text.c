
#include "gdl.h"

char textBf[ textBfSize ];

clDeep ** gdlfont = 0 ;

void drawText_(const char *txt, int x, int y, int align, clDeep **font, u8 start){//, u8 max){
  if(!txt)  return ; // empty string
  if(!font) font = gdlfont; // default font
  const u8 * t = (const u8*)txt;
	const u8 * p = t ;
	while( *p++ );//&& t - p < max );
  p-=2; // count string size, max is 32
	while(*p == 0x20) p--;	// remove space at end..
	p++; int sz = p - t;
  p = t ;
  while(sz--){
    u32 c = *p++;
    if( c < start || c > 123 ){ // bad char
      c = '*';
    }
    c -= start;
    if( !c ){ x += CHAR_SPACE; continue; } // space char

    clDeep * charFrm = font[ c ];
    u32 sx = *(u16*)(charFrm+8);//((u8*)charFrm)[0]; // char width

    //clDeep * screen = xy2scr(x,y);
    #ifdef FORCE_GFM_4
      udraw4bGfm(charFrm, xy2scr(x, y));
    #else
      drawGfm(charFrm,x,y) ;
    #endif

    x += sx + CHAR_PADDING ;

    if(x > (int)bufWidth) return ;
  };
}

void drawText(const char *txt,int x, int y){
	drawText_(txt,x,y,0,gdlfont,' ');
}

void setGdlfont(clDeep ** font){
	gdlfont = font ;
}

/*void setGdlfont(clDeep ** font, u32 frmNb){
	unCrunchGfm(font,frmNb);
	gdlfont = font ;
}*/

clDeep ** getGdlfont(void){
	return gdlfont ;
}

void prints(int x,int y,const char * format, ...) // a screen printf
{	if(!gdlfont) return ;
		va_list va; va_start(va,format); vsprintf(textBf,format,va);
	if(x<0 || y<0)
	{	const char *p = textBf ;
		while(*p++);
    p--;// count string size
		int size = p-textBf; if(!size) return ;
		int fsx,fsy; getGfmSize(&fsx,&fsy,*gdlfont);
		if(x<0) x = (bufWidth - (size*fsx))>>1;
		if(y<0) y = (bufHeight - fsy)>>1;
	}
		drawText_(textBf,x,y,0,gdlfont,' ');
}

void drawInt(int n, int x, int y, const char *method, int align, clDeep **font, int start){
  //char t[16];
  sprintf(textBf,method,n);
  drawText_(textBf,x,y,align,font,start);
}

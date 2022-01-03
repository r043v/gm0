
#include "gdl.h"

clDeep *scrStart,*scrEnd,*pixel ;
int frmWidth, frmWidthStart, frmWidthEnd;
u32 bufWidth, bufHeight;

clDeep *saved_scrStart=0, *saved_scrEnd=0, *saved_screen=0 ;
u32 saved_frmWidth=0, saved_frmWidthStart=0, saved_frmWidthEnd=0;

clDeep *isaved_scrStart=0, *isaved_scrEnd=0, *isaved_screen=0 ;
u32 isaved_frmWidth=0, isaved_frmWidthStart=0, isaved_frmWidthEnd=0;

void fullBlitLimit(void){
	scrStart  = pixel;
	scrEnd    = &pixel[bufWidth*bufHeight-1];
	frmWidth  = bufWidth;
	frmWidthStart = 0;
	frmWidthEnd   = bufWidth;
}

void setOutBuffer(clDeep*bf,int w, int h){
	pixel = bf;
	bufWidth = w;
	bufHeight = h;
	fullBlitLimit();
}

void saveBlitLimit(void)
{	saved_scrStart  = scrStart;
	saved_scrEnd    = scrEnd;
	saved_frmWidth  = frmWidth;
	saved_frmWidthStart = frmWidthStart;
	saved_frmWidthEnd   = frmWidthEnd;
}

void loadBlitLimit(void)
{	scrStart  = saved_scrStart;
	scrEnd    = saved_scrEnd;
	frmWidth  = saved_frmWidth;
	frmWidthStart = saved_frmWidthStart;
	frmWidthEnd   = saved_frmWidthEnd;
}

void internalSaveBlitLimit(void)
{	isaved_scrStart  = scrStart;
	isaved_scrEnd    = scrEnd;
	isaved_frmWidth  = frmWidth;
	isaved_frmWidthStart = frmWidthStart;
	isaved_frmWidthEnd   = frmWidthEnd;
}

void internalLoadBlitLimit(void)
{	scrStart  = isaved_scrStart;
	scrEnd    = isaved_scrEnd;
	frmWidth  = isaved_frmWidth;
	frmWidthStart = isaved_frmWidthStart;
	frmWidthEnd   = isaved_frmWidthEnd;
}

void setBlitLimitPointer(clDeep *start, clDeep *end, u32 widthStart, u32 widthEnd)
{	scrStart  = start;
	scrEnd    = end  ;
	frmWidth  = widthEnd-widthStart;
	frmWidthStart = widthStart;
	frmWidthEnd   = widthEnd;
}

void setBlitLimitX(u32 widthStart, u32 widthEnd)
{	u32 width = widthEnd - widthStart;
	scrStart  = &pixel[widthStart];
	scrEnd    = &pixel[bufWidth*(bufHeight-1)+widthEnd];
	frmWidth  = width;
	frmWidthStart = widthStart;
	frmWidthEnd   = widthEnd;
}

void setBlitLimit(u32 x, u32 y, u32 x1, u32 y1)
{	u32 width = x1 - x;
	scrStart  = &pixel[x+y*bufWidth];
	scrEnd    = &pixel[x1+y1*bufWidth];
	frmWidth  = width;
	frmWidthStart = x;
	frmWidthEnd   = x1;
}

// convert a 4b Gfm to a 16b Gfm
/*clDeep * data2Gfm(unsigned char *data)
{ u32 clNum = data[3] ;
	data += 8 ;
  u32 sx   = *(u16*)data ; data += 2 ;
  u32 sy   = *(u16*)data ; data += 2 ;

  u32 outSize = ( (*(u32*)data) / 2 ) ; data += 4 ;

  clDeep *Gfm = (clDeep*)malloc( outSize ); // + 2*clNum ) ;
  clDeep *pal = (clDeep*)malloc( 2 * clNum );

	for( u32 n=0 ; n < clNum ; n++ ){
		pal[n] = _rgb565( data[3], data[2], data[1] );
    data += 4;
	}

	clDeep *o = Gfm ;
  u32 cnt=0, c, jump, size, p1, p2 ;

  // *o++ = 0x6d6647 ;              // put signature "Gfm\0"
  // *o++ = outSize ;               // put Gfm object size
  // *o++ = sx<<8 | (sy & 0xff) ; // put frame size x and y
  *o++ = sx; *o++ = sy;
  while(cnt++ < sy){
    *o++ = c = *data++ ; //printf("\n* line %i, %i sublines",cnt,c) ;
    while(c--) {
      jump = *data++ ; size = *data++ ; *o++ = jump ; *o++ = size ;
      while(size > 1) { p1 = (*data)>>4 ; p2 = (*data)&15 ;
        size-=2 ; *o++ = pal[p1]>>8 ; *o++ = pal[p2]>>8 ;
        ++data ;
      };
      if( size ) *o++ = pal[(*data++)]>>8 ;
    };
  };

  //if( pal )
  free( pal );
  return Gfm ;
}

clDeep ** unCrunchGfm(clDeep ** gfm, u32 frmNb)
{ clDeep**Gfm = (clDeep**)malloc( frmNb * sizeof(clDeep*) );
  for(u32 c=0;c<frmNb;c++)
		Gfm[c] = data2Gfm((u8*)(gfm[c]));
  return Gfm;
}
*/

/*void * imemcpy( void * dst, const void * src, size_t sze ){
	sze >>= clDeepDec;

	clDeep * dest = (clDeep*)dst;
	clDeep * end = (clDeep*)src;
	clDeep * source = &end[ sze ];

	do { *dest++ = *--source; } while( source != end );
	return (void *)dest;
}

void * (*memcpyFn)(void*, const void*, size_t) = memcpy;
*/

void udrawGfm( register clDeep*Gfm, register clDeep*scr ){
	u8 *g = (u8*)Gfm++;
Gfm += 2; /* real size */

  register u32 lnb, sze ;

  if(scr > scrEnd) return ; // out of screen bottom

	clDeep * scrLast = scr + /*sy*/g[1]*bufWidth;
	if( scrLast < scrStart ) return ; // out of screen top
	if( scrLast > scrEnd ) scrLast = scrEnd;// down clipping

	while( scr < scrStart ){ // up clipping
		lnb = *Gfm++ ;
		while(lnb--){ g = (u8*)Gfm++ ; Gfm += g[1]; } //Gfm += *Gfm++ ; };
		scr += bufWidth ;
  };

	clDeep * screen = scr;

	while( screen < scrLast ){
		scr = screen;

		lnb = *Gfm++ ;
		while( lnb-- ){
			//scr += *Gfm++ ; sze = *Gfm++ ;
			g = (u8*)Gfm++ ; scr += *g; sze = g[1];
			//while( sze-- ){ *scr++ = *Gfm++; };
			/*memcpy( scr, Gfm, sze << clDeepDec );
			scr += sze;
			Gfm += sze;*/
			while( sze > 4 ){
				*scr++ = *Gfm++; *scr++ = *Gfm++; *scr++ = *Gfm++; *scr++ = *Gfm++;
				sze -= 4;
			};
			while( sze-- ){ *scr++ = *Gfm++; };
		};

		screen += bufWidth;
	};
}

void iudrawGfm( register clDeep*Gfm, register clDeep*scr ){
	u8 *g = (u8*)Gfm++;
	Gfm += 2; /* real size */

	u32 sx = g[0] - 1;
  register u32 lnb, sze ;

  if(scr > scrEnd) return ; // out of screen bottom

	clDeep * scrLast = scr + /*sy*/g[1]*bufWidth;
	if( scrLast < scrStart ) return ; // out of screen top
	if( scrLast > scrEnd ) scrLast = scrEnd;// down clipping

	while( scr < scrStart ){ // up clipping
		lnb = *Gfm++ ;
		while(lnb--){ g = (u8*)Gfm++; Gfm += g[1]; }// { Gfm++ ; Gfm += *Gfm++ ; };
		scr += bufWidth ;
  };

	clDeep * screen = &scr[ sx ];

	while( screen < scrLast ){
		scr = screen;

		lnb = *Gfm++ ;
		while( lnb-- ){
			//scr -= *Gfm++ ; sze = *Gfm++ ;
			g = (u8*)Gfm++ ; scr -= *g; sze = g[1];
			while( sze > 4 ){
				*scr-- = *Gfm++; *scr-- = *Gfm++; *scr-- = *Gfm++; *scr-- = *Gfm++;
				sze -= 4;
			};
			while( sze-- ){ *scr-- = *Gfm++; };
		};

		screen += bufWidth;
	};
}

void drawGfm(clDeep *Gfm, int x, int y){
	u8 *g = (u8*)Gfm;
  u32 sx = g[0], sy = g[1] ;

     if(x >= frmWidthStart && x + (int)sx < frmWidthEnd)
		{		int p = y*bufWidth+x;
				//printf("%ix%i >>> %i\n",x,y,p);
				udrawGfm(Gfm,&pixel[ p ]) ;
                return;
        }

     if(x < (int)(frmWidthStart-sx) || x > frmWidthEnd) return ;// out of screen on x

     if(x >= frmWidthStart) // clip right only
     {   u32 max = frmWidthEnd-x ;  u32 lnb, sze ;
			 	int p = y*bufWidth+x;
         clDeep *scr = &pixel[p] ;
         if(scr > scrEnd) return ; // out of screen at down

         u32 upClip = (scr + sy*bufWidth > scrEnd) ; // is clipped at down ?

//          Gfm += 2;
Gfm++;
Gfm += 2; /* real size */
         if(scr < scrStart) // is clipped at up ?
         {	if(scr + sy*bufWidth < scrStart) return ; // out of screen at up
            do{ lnb = *Gfm++ ;
                while(lnb--){ u8*g = (u8*)Gfm++ ; Gfm += g[1]; }// { Gfm++ ; Gfm += *Gfm++ ; };
                scr += bufWidth ; sy-- ;
            } while(scr < scrStart) ;
         }

         clDeep *screen = scr ; u32 c = 0 ; clDeep * lend ;
         while(c < sy)
         {  lnb = *Gfm++ ; lend = scr + max ;
            while(lnb--) {
							u8 *g = (u8*)Gfm++ ; scr += *g; sze = g[1];
							//scr += *Gfm++ ; sze = *Gfm++ ;
                           if(scr + sze < lend) memcpy(scr,Gfm,sze<<clDeepDec) ;
                           else if(scr < lend)  memcpy(scr,Gfm,(lend-scr)<<clDeepDec) ;
                           Gfm += sze ;    scr+=sze ;
            };  scr = screen + bufWidth*(++c) ;
            if(upClip) if(scr > scrEnd) return ;
         };
     } else if(x+(int)sx < frmWidthEnd) { // clip left only
			 int p = y*bufWidth + frmWidthStart;
         u32 lnb, sze ; clDeep *s = &pixel[p] ;

         if(s > scrEnd) return ; // out of screen at up
         u32 upClip = (s + sy*bufWidth > scrEnd) ; // is clipped at up ?

//          Gfm += 2;
Gfm++;
Gfm += 2; /* real size */

         if(s < scrStart) // is clipped at down ?
         {  if(s + sy*bufWidth < scrStart) return ; // out of screen at down
            do{ lnb = *Gfm++ ;
                while(lnb--){
									u8*g = (u8*)Gfm++ ;
									Gfm += g[1];
								};// { Gfm++ ; Gfm += *Gfm++ ; };
                s += bufWidth ; sy-- ;
            } while(s < scrStart) ;
         }

         clDeep*scr = s+x ;
         scr -= frmWidthStart ;
         clDeep *screen = scr ;
         u32 c=0 ; u32 size ;

         while(c < sy)
         {  lnb = *Gfm++ ;
            while(lnb--) { //scr += *Gfm++ ; sze = *Gfm++ ;
							u8*g = (u8*)Gfm++ ; scr += *g; sze = g[1];
                           if(scr >= s)   memcpy(scr,Gfm,sze<<clDeepDec) ;
                           else if(scr + sze > s) { size = (scr + sze)-s ;
                                                    memcpy(s,Gfm+(sze-size),size<<clDeepDec) ;
                                                  }
                           Gfm += sze ;    scr+=sze ;
            };  scr = screen + bufWidth*(++c) ; s = scr+frmWidthStart ; s -= x ;
            if(upClip) if(s > scrEnd) return ;
         };
     } else { // clip left and right
			 int p = y*bufWidth + frmWidthStart;
		 			u32 lnb, sze ; clDeep *s = &pixel[p] ;
		 			u32 max = frmWidthEnd-x ;
         if(s > scrEnd) return ; // out of screen at up
         u32 upClip = (s + sy*bufWidth > scrEnd) ; // is clipped at up ?

//          Gfm += 2;
Gfm++; /* Gfm size */
Gfm += 2; /* real size */

         if(s < scrStart) // is clipped at down ?
         {  if(s + sy*bufWidth < scrStart) return ; // out of screen at down
            do{ lnb = *Gfm++ ;
                while(lnb--){u8*g = (u8*)Gfm++ ; Gfm += g[1];}// { Gfm++ ; Gfm += *Gfm++ ; };
                s += bufWidth ; sy-- ;
            } while(s < scrStart) ;
         }

         clDeep*scr = s+x ;
         scr -= frmWidthStart ;
         clDeep *screen = scr ;
         u32 c=0 ; u32 size ; clDeep * lend ;

         while(c < sy)
         {  lnb = *Gfm++ ; lend = scr + max ;
            while(lnb--) {// scr += *Gfm++ ; sze = *Gfm++ ;
							g = (u8*)Gfm++ ; scr += *g; sze = g[1];
						if(scr >= s)
						{
							if(scr + sze < lend) memcpy(scr,Gfm,sze<<clDeepDec);
								else
							if(scr < lend)	memcpy(scr,Gfm,(lend-scr)<<clDeepDec);
						}
							else
						if(scr + sze > s) {
							size = (scr + sze)-s;
											if(scr + sze < lend)
												memcpy(s,Gfm+(sze-size),size<<clDeepDec);
												else
											if(scr < lend)
											memcpy(s,Gfm+((lend-scr)-size),size<<clDeepDec);
            }

						Gfm += sze ;    scr+=sze ;
            };

			scr = screen + bufWidth*(++c) ; s = scr+frmWidthStart ; s -= x ;
            if(upClip) if(s > scrEnd) return ;
         };

	}
}

void idrawGfm(clDeep *Gfm, int x, int y){
	u8 *g = (u8*)Gfm;
	u32 sx = g[0];//, sy = g[1] ;

	if( x >= frmWidthStart && ( ( x + (int)sx ) < frmWidthEnd ) ){
		iudrawGfm( Gfm, &pixel[ y*bufWidth+x ] ) ;
		return;
	}

}


void udraw4bGfm( u8*Gfm, u8*scr ){
	u8 clNum = Gfm[3] ;
	Gfm += 8 ;
  u16 sx   = *(u16*)Gfm ; Gfm+=2 ;
  u16 sy   = *(u16*)Gfm ; Gfm+=2 ;
  Gfm += 4 ; // out size
  //clDeep*pal = mypal ? mypal : (clDeep*)Gfm ;
  Gfm += ( 4*clNum ); // pal

  u8 cnt=0, c, size, p1, p2 ;
	u8 * s;

  while(cnt++ < sy){
		c = *Gfm++ ;
		s = scr;
		while(c--){
			scr += *Gfm++ ; // jump
			size = *Gfm++ ;
			while( size > 1 ){
				p1 = (*Gfm) >> 4 ;
				p2 = (*Gfm++) & 15 ;
				size -= 2 ;
				*scr++ = p1;//pal[p1] ;
				*scr++ = p2;//pal[p2] ;
			};
			if( size ) *scr++ = *Gfm++;//pal[*Gfm++] ;
		};
		scr = s + WIDTH;
  };
}

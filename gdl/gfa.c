
#include "gdl.h"

int animate( anim **b) // check for time and animate if need, not draw
{    anim *a = *b ;
	if(a->lastTime + a->frmTime < tick)
	{	if(++(a->curentFrm) >= a->frmNumber)
        {    switch(a->animType)
             { case 0 : a->curentFrm = 0 ; break ; /* loop anim */
               case 1 : a->curentFrm = a->frmNumber-1 ; break ; /* stop at last frame */
             };
             if(a->onfinish) (a->onfinish)(b) ;
        } else if(a->onflip) (a->onflip)(b) ;
		a->lastTime = tick ; return 1 ;
	} else if(a->onplay) a->onplay(b) ;
    return 0 ;
}

void playAnimScreen( anim **b,clDeep * screen){//, int way)
	 anim *a = *b ;
	animate(b) ;
	udrawGfm((a->Gfm)[a->curentFrm],screen) ;
}

extern void * (*memcpyFn)(void*, const void*, size_t);
extern void * imemcpy( void * dst, const void * src, size_t sze );

#define revertMemcpy memcpyFn=imemcpy;
#define normalMemcpy memcpyFn=memcpy;

void playAnim( anim **b, int x, int y, u32 way){
	 anim *a = *b ;
	animate(b) ;
	if( !way )
		drawGfm( a->Gfm[a->curentFrm], x, y ) ;
	else {
		//revertMemcpy;
		idrawGfm( a->Gfm[a->curentFrm], x, y ) ;
		//normalMemcpy;
	}
}

void drawAnim( anim **b, int x, int y, u32 way){
	 anim *a = *b ;
//	drawGfm( (way ? a->iGfm : a->Gfm)[a->curentFrm],x,y) ;
	if( !way )
		drawGfm( a->Gfm[a->curentFrm], x, y ) ;
	else {
		//revertMemcpy;
		idrawGfm( a->Gfm[a->curentFrm], x, y ) ;
		//normalMemcpy;
	}

}

void drawFrm( anim **b, int x, int y, int frm, u32 way){
	 anim *a = *b ;
	//drawGfm( (way ? a->iGfm : a->Gfm)[frm],x,y) ;
		if( !way )
			drawGfm( a->Gfm[frm], x, y ) ;
		else {
			//revertMemcpy;
			idrawGfm( a->Gfm[frm], x, y ) ;
			//normalMemcpy;
		}

}
/*
void ifreeAnim( anim *a)
{ if(!(a->iGfm)) return ;
  for(u32 c=0;c<a->frmNumber;c++) free((a->iGfm)[c]) ;
}

void mifreeAnim( anim **a, u32 nb)
{ for(u32 c=0;c<nb;c++) ifreeAnim(a[c]) ; }
*/

void setAnim(
	 anim * a,
	clDeep **Gfm, u32 nb,
	u32 frmTime, u32 animType,
	void (*onfinish)( anim**),
	void (*onflip)( anim**),
	void (*onplay)( anim**)
)
{	// anim *a = ( anim *)malloc(sizeof( anim)) ;
	a->Gfm = Gfm ;	a->frmNumber = nb ; //a->iGfm = flipGfm(Gfm,nb) ;
	a->curentFrm = a->lastTime = 0 ;
  a->onplay=onplay ; a->onflip=onflip ; a->onfinish=onfinish ;
	a->frmTime = frmTime ; a->animType = animType ;
	a->iGfm = Gfm;
	//a->iGfm = flipGfm(Gfm,nb);
//    return a ;
}

void setIGfm(  anim * a, clDeep ** iGfm ){
	a->iGfm = iGfm;
}

void resetAnim( anim **b){
	 anim *a = *b ;
	a->curentFrm=0 ; a->lastTime = tick ;
}

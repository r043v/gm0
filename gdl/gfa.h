
#ifndef _gdlAnim_
#define _gdlAnim_

#include "gdl.h"

typedef	struct anim
{	u32	lastTime ;
	u32	frmTime  ;
	u32	curentFrm;
	u32	frmNumber;
	u32	animType;
	void (*onfinish)( struct anim **);
	void (*onflip)( struct anim **);
	void (*onplay)( struct anim **);
	clDeep	**Gfm, **iGfm;
} anim ;

int animate( anim **b) ;
void playAnim( anim **b, int x, int y, u32 way);
void playAnimScreen( anim **b, clDeep *screen);
void drawAnim( anim **b, int x, int y, u32 way);
void drawFrm( anim **b, int x, int y, int frm, u32 way);
//void  ifreeAnim(struct anim *a);
//void mifreeAnim(struct anim *a, u32 nb);
clDeep** flipGfm(clDeep **Gfm, u32 nb);
void setAnim(
	anim * a, clDeep **Gfm, u32 nb,
	u32 frmTime, u32 animType,
	void (*onfinish)(anim**),
	void (*onflip)(anim**),
	void (*onplay)(anim**)
);
void setIGfm( anim * a, clDeep ** iGfm );
void resetAnim( anim **b );

#endif

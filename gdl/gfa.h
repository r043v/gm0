
#ifndef _gdlAnim_
#define _gdlAnim_

#include "gdl.h"

struct	anim
{	u32	lastTime ;
	u32	frmTime  ;
	u32	curentFrm;
	u32	frmNumber;
	u32	animType;
	void (*onfinish)(struct anim **);
	void (*onflip)(struct anim **);
	void (*onplay)(struct anim **);
	clDeep	**Gfm, **iGfm;
};

int Animate(struct anim **b) ;
void playAnim(struct anim **b, int x, int y, u32 way);
void playAnimScreen(struct anim **b, clDeep *screen);
void drawAnim(struct anim **b, int x, int y, u32 way);
void drawFrm(struct anim **b, int x, int y, int frm, u32 way);
//void  ifreeAnim(struct anim *a);
//void mifreeAnim(struct anim *a, u32 nb);
clDeep** flipGfm(clDeep **Gfm, u32 nb);
void setAnim(
	struct anim * a, clDeep **Gfm, u32 nb,
	u32 frmTime, u32 animType,
	void (*onfinish)(struct anim**),
	void (*onflip)(struct anim**),
	void (*onplay)(struct anim**)
);
void setIGfm( struct anim * a, clDeep ** iGfm );
void resetAnim(struct anim **b);

#endif

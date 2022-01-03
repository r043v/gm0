
#ifndef _GdlRate_
#define _GdlRate_

typedef struct rate {
  u32 last;
  u32 acc;
  u32 ticks;
  u16 px;
} rate ;

void rateInit( rate * r, u16 px ) ;
void rateReset( rate * r ) ;
void rateTicksByPx( rate * r, u32 ticks ) ;
void ratePxBySecond( rate * r, u16 px ) ;
u32 rateUpdate( rate * r ) ;

#endif


#include "./gdl.h"



void rateInit( rate * r, u16 px ){
  ratePxBySecond( r, px );
  rateReset( r );
}

void rateReset( rate * r ){
  r->last = tick ;
  r->acc = 0 ;
}

void rateTicksByPx( rate * r, u32 ticks ){
  r->ticks = ticks * 1024 ;
  r->px = 1024000 / r->ticks ;
}

void ratePxBySecond( rate * r, u16 px ){
  r->ticks = 1024000 / px ;
  r->px = px ;
}

u32 rateUpdate( rate * r ){
  if( tick == r->last ) return 0;

  r->acc += ( tick - r->last ) * 1024;
  r->last = tick;

  if( r->acc < r->ticks ) return 0;

	u32 px = 0;
  do {
    px++;
    r->acc -= r->ticks;
  }	while( r->acc >= r->ticks );

  return px;
}

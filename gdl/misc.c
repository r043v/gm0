
#include "gdl.h"

u32 computeDec(u32 value)
{	u32 v=1, dec=0;
	while(v<value) { v<<=1; dec++; }
	return dec ;
}

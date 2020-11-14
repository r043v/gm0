
//#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <stdarg.h>
#include <signal.h>

#include "defs.h"
#include "input.h"
#include "rc.h"

void doevents()
{	event_t ev;
	int st;
	ev_poll();
	while (ev_getevent(&ev))
	{
		if (ev.type != EV_PRESS && ev.type != EV_RELEASE)
			continue;
		st = (ev.type != EV_RELEASE);
		rc_dokey(ev.code, st);
	}
}

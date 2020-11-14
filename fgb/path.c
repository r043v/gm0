


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "defs.h"

#ifdef ALT_PATH_SEP
#define SEP ';'
#else
#define SEP ':'
#endif
/*
char *path_search(char *name, char *mode, char *path)
{
	FILE *f;
	static char *buf;
	char *p, *n;
	int l;

	if (buf) free(buf); buf = 0;
	if (!path || !*path || *name == '/')
		return (buf = strdup(name));

	buf = (char *)malloc(strlen(path) + strlen(name) + 2);

	for (p = path; *p; p += l)
	{
		if (*p == SEP) p++;
		n = strchr(p, SEP);
		if (n) l = n - p;
		else l = strlen(p);
		strncpy(buf, p, l);
		buf[l] = '/';
		strcpy(buf+l+1, name);
		f = fopen(buf, mode) ;
		if (f)
		{
			fclose(f);
			return buf;
		}
	}
	return name;
}

*/






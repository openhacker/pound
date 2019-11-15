/* example plugin for pound */

#include <stdio.h>
#include "pound.h"

void plugin_startup(void)
{
	fprintf(stderr, "%s startup\n", __FILE__);	
}


void plugin_shutdown(void)
{
	fprintf(stderr, "%s shutdown\n", __FILE__);
}

BACKEND *testLookUp(BACKEND *list, const char *request)
{
	fprintf(stderr, "%s called with %s\n", __func__, request);
	return NULL;
}

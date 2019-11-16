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
	static BACKEND *last_backend = NULL;
	BACKEND *this_backend;

	fprintf(stderr, "%s called with %s\n", __func__, request);
	if(!last_backend) {
		fprintf(stderr, "head of list\n");
		last_backend = list;
		return last_backend;
	}

	for(this_backend = list; this_backend; this_backend = this_backend->next) {
		fprintf(stderr, "this_backend = %p, last_backend = %p\n", this_backend, last_backend);
		if(this_backend == last_backend) {
			last_backend = this_backend->next;
			return last_backend;
		}
	}
	
	return NULL;
}

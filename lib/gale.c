#include <string.h>
#include "gale/util.h"

void *gale_realloc(const void *s,size_t len) {
	return gale_memdup(s,len); /* XXX bad bad */
}

void *gale_memdup(const void *s,int len) {
	void *r = gale_malloc(len);
	memcpy(r,s,len);
	return r;
}

char *gale_strndup(const char *s,int len) {
	char *r = gale_memdup(s,len + 1);
	r[len] = '\0';
	return r;
}

char *gale_strdup(const char *s) {
	return gale_strndup(s,strlen(s));
}

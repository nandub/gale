#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <pwd.h>

#include "gale/all.h"

struct gale_dir {
	int len,alloc;
	char *buf;
};

struct gale_dir *dot_gale,*home_dir;

static void read_conf(const char *s) {
	char ch,var[40],value[256];
	int num;
	FILE *fp = fopen(s,"r");
	if (fp == NULL) return;
	do {
		while (fscanf(fp," #%*[^\n]%c",&ch) == 1) ;
		num = fscanf(fp,"%39s %255[^\n]",var,value);
		if (num == 2) {
			char *both,*prev = getenv(var);
			if (prev && prev[0]) continue;
			both = gale_malloc(strlen(var) + strlen(value) + 2);
			sprintf(both,"%s=%s",var,value);
			putenv(both);
		}
	} while (num == 2);
}

void gale_init(const char *s) {
	char *dir;

	gale_error_prefix = s;
	read_conf(GALE_CONF);

	dir = getenv("HOME");
	if (!dir) {
		struct passwd *pwd;
		char *user;
		if ((user = getenv("LOGNAME")))
			pwd = getpwnam(user);
		else
			pwd = getpwuid(getuid());
		if (pwd) 
			dir = pwd->pw_dir;
		else {
			gale_alert(GALE_WARNING,"no home dir, using cwd",0);
			dir = ".";
		}
	}
	home_dir = make_dir(dir,0777);

	dir = getenv("GALE_DIR");
	if (dir) {
		dot_gale = make_dir(dir,0777);
		return;
	}

	dot_gale = dup_dir(home_dir);
	sub_dir(dot_gale,".gale",0777);

	read_conf(dir_file(dot_gale,"conf"));
}

const char *dir_file(struct gale_dir *d,const char *s) {
	int l = strlen(s);
	if (l + 1 + d->len >= d->alloc) {
		char *n = gale_malloc(d->alloc = l + 1 + d->len + 1);
		strncpy(n,d->buf,d->len + 1);
		gale_free(d->buf);
		d->buf = n;
	}
	d->buf[d->len] = '/';
	strcpy(d->buf + d->len + 1,s);
	return d->buf;
}

struct gale_dir *dup_dir(struct gale_dir *d) {
	struct gale_dir *r = gale_malloc(sizeof(struct gale_dir));
	r->len = d->len;
	r->buf = gale_malloc(r->alloc = d->len + 1);
	strncpy(r->buf,d->buf,r->alloc);
	return r;
}

struct gale_dir *make_dir(const char *s,int mode) {
	struct stat buf;
	struct gale_dir *r = gale_malloc(sizeof(struct gale_dir));
	r->len = strlen(s);
	strcpy(r->buf = gale_malloc(r->alloc = r->len + 1),s);
	if ((stat(r->buf,&buf) || !S_ISDIR(buf.st_mode)) && mkdir(r->buf,mode))
		gale_alert(GALE_WARNING,r->buf,errno);
	return r;
}

void free_dir(struct gale_dir *d) {
	gale_free(d->buf);
	gale_free(d);
}

void sub_dir(struct gale_dir *d,const char *s,int mode) {
	struct stat buf;
	d->len = strlen(dir_file(d,s));
	if ((stat(d->buf,&buf) || !S_ISDIR(buf.st_mode)) && mkdir(d->buf,mode))
		gale_alert(GALE_WARNING,d->buf,errno);
}

void up_dir(struct gale_dir *d) {
	char *c;
	d->buf[d->len] = '\0';
	c = strrchr(d->buf,'/');
	if (c) d->len = c - d->buf;
}

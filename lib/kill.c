#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <string.h>

#include "gale/all.h"

static char *dotfile = NULL;

static void remove_dotfile(void) {
	if (dotfile) unlink(dir_file(dot_gale,dotfile));
}

void gale_kill(const char *class,int do_kill) {
	int len,fd,pid = getpid();
	DIR *pdir;
	struct dirent *de;
	const char *host = getenv("HOST");

	len = strlen(host) + strlen(class) + strlen(gale_error_prefix) + 3;
	dotfile = gale_malloc(len + 15);
	sprintf(dotfile,"%s.%s.%s.%d",gale_error_prefix,host,class,pid);

	gale_cleanup(remove_dotfile);
	fd = creat(dir_file(dot_gale,dotfile),0666);
	if (fd >= 0) 
		close(fd);
	else
		gale_alert(GALE_WARNING,dotfile,errno);

	if (do_kill) {
		pdir = opendir(dir_file(dot_gale,"."));
		if (pdir == NULL) {
			gale_alert(GALE_WARNING,"opendir",errno);
			return;
		}

		while ((de = readdir(pdir)))
			if (!strncmp(de->d_name,dotfile,len)) {
				int kpid = atoi(de->d_name + len);
				if (kpid != pid) {
					kill(kpid,SIGHUP);
					unlink(dir_file(dot_gale,de->d_name));
				}
			}
		closedir(pdir);
	}
}

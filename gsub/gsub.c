/* gsub.c -- subscription client, outputs messages to the tty, optionally
   sending them through a gsubrc filter. 

   Beware of using this as an example; it's insufficiently Unicode-ized. */

#include "gale/all.h"
#include "gale/gsubrc.h"

#include <time.h>
#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/utsname.h>

#ifdef HAVE_DLFCN_H
#define HAVE_DLOPEN
#include <dlfcn.h>
#endif

#ifdef HAVE_CURSES_H
#define HAVE_CURSES
#include <curses.h>
#ifdef HAVE_TERM_H
#include <term.h>
#endif
#endif

extern char **environ;

const char *rcprog = "gsubrc";		/* Filter program name. */
gsubrc_t *dl_gsubrc = NULL;		/* Loaded gsubrc function. */
gsubrc2_t *dl_gsubrc2 = NULL;		/* Extended gsubrc function. */
struct gale_client *client;             /* Connection to server. */
char *tty,*agent;                       /* TTY device, user-agent string. */

int do_ping = 1;			/* Should we answer Receipt-To's? */
int do_beep = 1;			/* Should we beep? */
int do_termcap = 0;                     /* Should we highlight headers? */
int sequence = 0;

#define TIMEOUT 300			/* Interval to poll for tty death */

void *gale_malloc(size_t size) { return malloc(size); }
void gale_free(void *ptr) { free(ptr); }

/* Generate a trivial little message with the given category.  Used for
   return receipts, login/logout notifications, and such. */
struct gale_message *slip(struct gale_text cat,
                          struct gale_id *sign,struct auth_id *encrypt)
{
	struct gale_message *msg;
	int len = strlen(agent) + strlen(getenv("GALE_FROM"));

	/* Create a new message. */
	msg = new_message();
	msg->cat = gale_text_dup(cat);
	msg->data.p = gale_malloc(128 + len);

	/* A few obvious headers. */
	sprintf(msg->data.p,
		"From: %s\r\n"
		"Time: %lu\r\n"
		"Agent: %s\r\n"
		"Sequence: %d\r\n"
		"\r\n",
	        getenv("GALE_FROM"),time(NULL),agent,sequence++);

	msg->data.l = strlen(msg->data.p);

	/* Sign and encrypt the message, if appropriate. */
	if (sign) {
		struct gale_message *new = sign_message(sign,msg);
		if (new) {
			release_message(msg);
			msg = new;
		}
	}

	if (encrypt) {
		/* For safety's sake, don't leave the old message in place
		   if encryption fails. */
		struct gale_message *new = encrypt_message(1,&encrypt,msg);
		release_message(msg);
		msg = new;
	}

	return msg;
}

/* Reply to an AKD request: post our key. */

struct gale_message *send_key(void) {
	struct gale_data data;
	struct gale_message *key = new_message();
	export_auth_id(user_id,&data,0);
	key->cat = id_category(user_id,_G("auth/key"),_G(""));
	key->data.p = gale_malloc(256 
	            + auth_id_name(user_id).l
	            + strlen(getenv("GALE_FROM"))
	            + data.l);

	sprintf(key->data.p,
		"From: %s\r\n"
		"Time: %lu\r\n"
		"Agent: %s\r\n"
		"Sequence: %d\r\n"
		"Subject: success %s\r\n"
		"\r\n",
	        getenv("GALE_FROM"),time(NULL),agent,sequence++,
		gale_text_hack(auth_id_name(user_id)));
	key->data.l = strlen(key->data.p);
	memcpy(key->data.p + key->data.l,data.p,data.l);
	key->data.l += data.l;

	return key;
}

/* Output a terminal mode string. */
void tmode(char id[2]) {
#ifdef HAVE_CURSES
	char *cap;
	if (do_termcap && (cap = tgetstr(id,NULL))) 
		tputs(cap,1,(TPUTS_ARG_3_T) putchar);
#else
	(void) id;
#endif
}

/* Print a user ID, with a default string (like "everyone") for NULL. */
void print_id(const char *id,const char *dfl) {
	putchar(' ');
	putchar(id ? '<' : '*');
	tmode("md");
	fputs(id ? id : dfl,stdout);
	tmode("me");
	putchar(id ? '>' : '*');
}

/* The default gsubrc implementation. */
void default_gsubrc(void) {
	char *tmp,buf[80],*cat = getenv("GALE_CATEGORY");
	char *nl = tty ? "\r\n" : "\n";
	int count = 0;

	/* Ignore messages to category /ping */
	tmp = cat;
	while ((tmp = strstr(tmp,"/ping"))) {
		if ((tmp == cat || tmp[-1] == ':')
		&&  (tmp[5] == '\0' || tmp[5] == ':'))
			return;
		tmp += 5;
	}

	/* Format return receipts specially */
	tmp = cat;
	while ((tmp = strstr(tmp,"/receipt"))) {
		tmp += 8;
		if (!*tmp || *tmp == ':') {
			char *from_comment = getenv("HEADER_FROM");
			fputs("* Received by",stdout);
			print_id(getenv("GALE_SIGNED"),"unverified");
			if (from_comment) printf(" (%s)",from_comment);
			if ((tmp = getenv("HEADER_TIME"))) {
				time_t when = atoi(tmp);
				strftime(buf,sizeof(buf)," %m/%d %H:%M",
					 localtime(&when));
				fputs(buf,stdout);
			}
			fputs(nl,stdout);
			fflush(stdout);
			return;
		}
	}

	/* Print the header: category, time, et cetera */
	putchar('[');
	tmode("md");
	fputs(cat,stdout);
	tmode("me");
	putchar(']');
	if ((tmp = getenv("HEADER_TIME"))) {
		time_t when = atoi(tmp);
		strftime(buf,sizeof(buf)," %m/%d %H:%M",localtime(&when));
		fputs(buf,stdout);
	}
	if (getenv("HEADER_RECEIPT_TO")) 
		printf(" [rcpt]");
	fputs(nl,stdout);

	/* Print who the message is from and to. */
	{
		char *from_comment = getenv("HEADER_FROM");
		char *from_id = getenv("GALE_SIGNED");
		char *to_comment = getenv("HEADER_TO");
		char *to_id = getenv("GALE_ENCRYPTED");

		fputs("From",stdout);
		if (from_comment || from_id) {
			print_id(from_id,"unverified");
			if (from_comment) printf(" (%s)",from_comment);
		} else
			print_id(NULL,"anonymous");

		fputs(" to",stdout);
		print_id(to_id,"everyone");
		if (to_comment) printf(" (%s)",to_comment);

		putchar(':');
		fputs(nl,stdout);
		if (tty && to_id && do_beep) putchar('\a');
	}

	/* Print the message body.  Make sure to escape unprintables. */
	fputs(nl,stdout);
	while (fgets(buf,sizeof(buf),stdin)) {
		char *ptr,*end = buf + strlen(buf);
		for (ptr = buf; ptr < end; ++ptr) {
			if (isprint(*ptr & 0x7F) || *ptr == '\t')
				putchar(*ptr);
			else if (*ptr == '\n')
				fputs(nl,stdout);
			else {
				tmode("mr");
				if (*ptr < 32)
					printf("^%c",*ptr + 64);
				else
					printf("[0x%X]",*ptr);
				tmode("me");
			}
		}
		++count;
	}

	/* Add a final newline, if for some reason the message did not
           contain one. */
	if (count) fputs(nl,stdout);

	/* Out it goes! */
	if (fflush(stdout) < 0) exit(1);
}

/* Transmit a message body to a gsubrc process. */
void send_message(char *body,char *end,int fd) {
	char *tmp;

	while (body != end) {
		/* Write data up to a newline. */
		tmp = memchr(body,'\r',end - body);
		if (!tmp) tmp = end;
		while (body != tmp) {
			int r = write(fd,body,tmp - body);
			if (r <= 0) {
				gale_alert(GALE_WARNING,"write",errno);
				return;
			}
			body += r;
		}

		/* Translate CRLF to NL. */
		if (tmp != end) {
			if (write(fd,"\n",1) != 1) {
				gale_alert(GALE_WARNING,"write",errno);
				return;
			}
			++tmp;
			if (tmp != end && *tmp == '\n') ++tmp;
		}
		body = tmp;
	}
}

/* Take the message passed as an argument and show it to the user, running
   their gsubrc if present, using the default formatter otherwise. */
void present_message(struct gale_message *_msg) {
	int pfd[2];             /* Pipe file descriptors. */

	/* Lots of crap.  Discussed below, where they're used. */
	char *next,**envp = NULL,*key,*data,*end,*tmp;
	struct gale_id *id_encrypted = NULL,*id_sign = NULL;
	struct gale_message *rcpt = NULL,*akd = NULL,*msg = NULL;
	int envp_global,envp_alloc,envp_len,status;
	struct gale_text restart = gale_text_from_latin1("debug/restart",-1);
	pid_t pid;

	/* Count the number of global environment variables. */
	envp_global = 0;
	for (envp = environ; *envp; ++envp) ++envp_global;

	/* Allocate space for ten more for the child.  That should do. */
	envp_alloc = envp_global + 10;
	envp = gale_malloc(envp_alloc * sizeof(*envp));
	memcpy(envp,environ,envp_global * sizeof(*envp));
	envp_len = envp_global;

	/* GALE_CATEGORY: the message category */
	tmp = gale_text_to_local(_msg->cat);
	next = gale_malloc(strlen(tmp) + 15);
	sprintf(next,"GALE_CATEGORY=%s",tmp);
	gale_free(tmp);
	envp[envp_len++] = next;

	/* Decrypt, if necessary. */
	id_encrypted = decrypt_message(_msg,&msg);
	if (!msg) {
		char *tmp = gale_malloc(_msg->cat.l + 80);
		sprintf(tmp,"cannot decrypt message on category \"%s\"",
		        gale_text_hack(_msg->cat));
		gale_alert(GALE_WARNING,tmp,0);
		gale_free(tmp);
		goto error;
	}

	if (id_encrypted) {
		tmp = gale_malloc(auth_id_name(id_encrypted).l + 16);
		sprintf(tmp,"GALE_ENCRYPTED=%s",
			gale_text_hack(auth_id_name(id_encrypted)));
		envp[envp_len++] = tmp;
	}

	/* Verify a signature, if possible. */
	id_sign = verify_message(msg);
	if (id_sign) {
		tmp = gale_malloc(auth_id_name(id_sign).l + 13);
		sprintf(tmp,"GALE_SIGNED=%s",
			gale_text_hack(auth_id_name(id_sign)));
		envp[envp_len++] = tmp;
	}

	/* Go through the message headers. */
	next = msg->data.p;
	end = msg->data.p + msg->data.l;
	while (parse_header(&next,&key,&data,end)) {

		/* Process receipts, if we do. */
		if (do_ping && !strcasecmp(key,"Receipt-To")) {
			/* Generate a receipt. */
			struct gale_text cat;
			cat = gale_text_from_latin1(data,-1);
			if (rcpt) release_message(rcpt);
			rcpt = slip(cat,user_id,id_sign);
			free_gale_text(cat);
		}

		if (do_ping && !strcasecmp(key,"Request-Key")) {
			struct gale_text text = gale_text_from_latin1(data,-1);
			if (gale_text_compare(text,auth_id_name(user_id)))
				gale_alert(GALE_WARNING,
				           "invalid key request",0);
			else {
				if (akd) release_message(akd);
				akd = send_key();
			}
			free_gale_text(text);
		}

		/* Create a HEADER_... environment entry for this. */
		for (tmp = key; *tmp; ++tmp)
			*tmp = isalnum(*tmp) ? toupper(*tmp) : '_';
		tmp = gale_malloc(strlen(key) + strlen(data) + 9);
		sprintf(tmp,"HEADER_%s=%s",key,data);

		/* Allocate more space for the environment if necessary. */
		if (envp_len == envp_alloc - 1) {
			char **tmp = envp;
			envp_alloc *= 2;
			envp = gale_malloc(envp_alloc * sizeof(*envp));
			memcpy(envp,tmp,envp_len * sizeof(*envp));
			gale_free(tmp);
		}

		envp[envp_len++] = tmp;
	}

#ifndef NDEBUG
	/* In debug mode, restart if we get a properly authorized message. */
	if (!gale_text_compare(msg->cat,restart) && id_sign 
	&&  !gale_text_compare(auth_id_name(id_sign),_G("egnor@ofb.net"))) {
		gale_alert(GALE_NOTICE,"Restarting from debug/restart.",0);
		gale_restart();
	}
#endif

	/* Give them our key, if they wanted it. */
	if (akd) {
		link_put(client->link,akd);
		if (rcpt) release_message(rcpt);
		rcpt = NULL;
		status = 0;
		goto done;
	}

	/* Terminate the new environment. */
	envp[envp_len] = NULL;

	/* Use the extended loaded gsubrc, if present. */
	if (dl_gsubrc2) {
		status = dl_gsubrc2(envp,next,end - next);
		goto done;
	}

	/* Create a pipe to communicate with the gsubrc with. */
	if (pipe(pfd)) {
		gale_alert(GALE_WARNING,"pipe",errno);
		goto error;
	}

	/* Fork off a subprocess.  This should use gale_exec ... */
	pid = fork();
	if (!pid) {
		const char *rc;

		/* Set the environment. */
		environ = envp;

		/* Close off file descriptors. */
		close(client->socket);
		close(pfd[1]);

		/* Pipe goes to stdin. */
		dup2(pfd[0],0);
		if (pfd[0] != 0) close(pfd[0]);

		/* Use the loaded gsubrc, if we have one. */
		if (dl_gsubrc) exit(dl_gsubrc());

		/* Look for the file. */
		rc = dir_search(rcprog,1,dot_gale,sys_dir,NULL);
		if (rc) {
			execl(rc,rcprog,NULL);
			gale_alert(GALE_WARNING,rc,errno);
			exit(1);
		}

		/* If we can't find or can't run gsubrc, use default. */
		default_gsubrc();
		exit(0);
	}

	if (pid < 0) gale_alert(GALE_WARNING,"fork",errno);

	/* Send the message to the gsubrc. */
	close(pfd[0]);
	send_message(next,end,pfd[1]);
	close(pfd[1]);

	/* Wait for the gsubrc to terminate. */
	status = -1;
	if (pid > 0) {
		waitpid(pid,&status,0);
		if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
			status = 0;
		else
			status = -1;
	}

done:
	/* Put the receipt on the queue, if we have one. */
	if (rcpt && !status) link_put(client->link,rcpt);

error:
	/* Clean up after ourselves. */
	if (envp) {
		while (envp_global != envp_len) gale_free(envp[envp_global++]);
		gale_free(envp);
	}
	if (id_encrypted) free_id(id_encrypted);
	if (id_sign) free_id(id_sign);
	if (msg) release_message(msg);
	if (rcpt) release_message(rcpt);
	if (akd) release_message(akd);
}

/* Send a login notification, arrange for a logout notification. */
void notify(void) {
	struct gale_message *msg;
	struct gale_text tmp;

	/* Login: send it right away. */
	tmp = id_category(user_id,_G("notice"),_G("login"));
	msg = slip(tmp,user_id,NULL);
	free_gale_text(tmp);
	link_put(client->link,msg);
	release_message(msg);

	/* Logout: "will" it to happen when we disconnect. */
	tmp = id_category(user_id,_G("notice"),_G("logout"));
	msg = slip(tmp,user_id,NULL);
	free_gale_text(tmp);
	link_will(client->link,msg);
	release_message(msg);
}

/* Set the value to use for Agent: headers. */
void set_agent(void) {
	char *user = getenv("LOGNAME");
	const char *host = getenv("HOST");
	int len;

	/* Construct the string from our version, the user running us,
           the host we're on and so on. */
	len = strlen(VERSION) + strlen(user) + strlen(host);
	len += (tty ? strlen(tty) : 0) + 30;
	agent = gale_malloc(len);
	sprintf(agent,"gsub/%s %s@%s %s %d",
	        VERSION,user,host,tty ? tty : "none",(int) getpid());
}

void usage(void) {
	fprintf(stderr,
	"%s\n"
	"usage: gsub [-benkKpa] [-f rcprog] [-l rclib] cat\n"
	"flags: -b          Do not beep (normally personal messages beep)\n"
	"       -e          Do not include default subscriptions\n"
	"       -n          Do not fork (default if stdout redirected)\n"
	"       -k          Do not kill other gsub processes\n"
	"       -K          Kill other gsub processes and terminate\n"
	"       -f rcprog   Use rcprog (default gsubrc, if found)\n"
#ifdef HAVE_DLOPEN
	"       -l rclib    Use module (default gsubrc.so, if found)\n" 
#endif
	"       -p          Suppress return-receipt processing altogether\n"
	"       -a          Disable login/logout notification\n"
	,GALE_BANNER);
	exit(1);
}

/* Search for and load a shared library with a custom message presenter. */
void load_gsubrc(const char *name) {
#ifdef HAVE_DLOPEN
	const char *rc,*err;
	void *lib;

	rc = dir_search(name ? name : "gsubrc.so",1,dot_gale,sys_dir,NULL);
	if (!rc) {
		if (name) 
			gale_alert(GALE_WARNING,
			"Cannot find specified shared library.",0);
		return;
	}

	lib = dlopen((char *) rc,RTLD_LAZY); /* FreeBSD needs the cast. */
	if (!lib) {
		while ((err = dlerror())) gale_alert(GALE_WARNING,err,0);
		return;
	}

	dl_gsubrc2 = dlsym(lib,"gsubrc2");
	if (!dl_gsubrc2) {
		dl_gsubrc = dlsym(lib,"gsubrc");
		if (!dl_gsubrc) {
			while ((err = dlerror())) 
				gale_alert(GALE_WARNING,err,0);
			dlclose(lib);
			return;
		}
	}

#else
	if (name) gale_alert(GALE_WARNING,"Dynamic loading not supported.",0);
#endif
}

/* add subscriptions to a list */

void add_subs(struct gale_text *subs,struct gale_text add) {
	struct gale_text n;
	if (add.p == NULL) return;
	n = new_gale_text(subs->l + add.l + 1);
	gale_text_append(&n,*subs);
	gale_text_append(&n,_G(":"));
	gale_text_append(&n,add);
	free_gale_text(*subs);
	*subs = n;
}

/* main */

int main(int argc,char **argv) {
	/* Various flags. */
	int opt,do_notify = 1,do_fork = 0,do_kill = 0;
	const char *rclib = NULL;
	/* Subscription list. */
	struct gale_text serv = null_text;

	/* Initialize the gale libraries. */
	gale_init("gsub",argc,argv);

	/* If we're actually on a TTY, we do things a bit differently. */
	if ((tty = ttyname(1))) {
		/* Truncate the tty name for convenience. */
		char *tmp = strrchr(tty,'/');
#ifdef HAVE_CURSES
		char buf[1024];
		/* Find out the terminal type. */
		char *term = getenv("TERM");
		/* Do highlighting, if available. */
		if (term && 1 == tgetent(buf,term)) do_termcap = 1;
#endif
		if (tmp) tty = tmp + 1;
		/* Go into the background; kill other gsub processes. */
		do_fork = do_kill = 1;
	}

	/* Default subscriptions. */
	add_subs(&serv,gale_text_from_local(getenv("GALE_SUBS"),-1));
	add_subs(&serv,gale_text_from_local(getenv("GALE_GSUB"),-1));

	/* Parse command line arguments. */
	while (EOF != (opt = getopt(argc,argv,"benkKpaf:l:h"))) switch (opt) {
	case 'b': do_beep = 0; break;		/* Do not beep */
	case 'e': free_gale_text(serv);		/* Do not include defaults */
	          serv.l = 0; serv.p = NULL; break;
	case 'n': do_fork = 0; break;           /* Do not go into background */
	case 'k': do_kill = 0; break;           /* Do not kill other gsubs */
	case 'K': if (tty) gale_kill(tty,1);    /* *only* kill other gsubs */
	          return 0;
	case 'f': rcprog = optarg; break;       /* Use a wacky gsubrc */
	case 'l': rclib = optarg; break;	/* Use a wacky gsubrc.so */
	case 'p': do_ping = 0; break;           /* Do not honor Receipt-To: */
	case 'a': do_notify = 0; break;         /* Do not send login/logout */
	case 'h':                               /* Usage message */
	case '?': usage();
	}

	/* One argument, at most (subscriptions) */
	if (optind < argc - 1) usage();
	if (optind == argc - 1) 
		add_subs(&serv,gale_text_from_local(argv[optind],-1));

	/* We need to subscribe to *something* */
	if (0 == serv.l)
		gale_alert(GALE_ERROR,"No subscriptions specified.",0);

	/* Look for a gsubrc.so */
	load_gsubrc(rclib);

	/* Generate keys so people can send us messages. */
	gale_keys();

	/* Act as AKD proxy for this particular user. */
	if (do_ping)
		add_subs(&serv,id_category(user_id,_G("auth/query"),_G("")));

#ifndef NDEBUG
	/* If in debug mode, listen to debug/ for restart messages. */
	add_subs(&serv,_G("debug/"));
#endif

	/* Open a connection to the server. */
	client = gale_open(serv);

	/* Fork ourselves into the background, unless we shouldn't. */
	if (do_fork) gale_daemon(1);
	if (tty) gale_kill(tty,do_kill);

	/* Set our Agent: header value. */
	set_agent();

	/* Send a login message, as needed. */
	if (do_notify) notify();
	for (;;) {
		int r = 0;

		while (!gale_error(client) && r >= 0 && !gale_send(client)) {
			fd_set fds;
			struct timeval timeout;
			struct gale_message *msg;

			FD_ZERO(&fds);
			FD_SET(client->socket,&fds);
			timeout.tv_sec = TIMEOUT;
			timeout.tv_usec = 0;

			r = select(FD_SETSIZE,(SELECT_ARG_2_T) &fds,NULL,NULL,
			           &timeout);

			/* Make sure the tty still exists. */
			if (tty && !isatty(1)) exit(1);

			if (r < 0) 
				gale_alert(GALE_WARNING,"select",errno);
			else if (r > 0 && gale_next(client))
				r = -1;
			else while ((msg = link_get(client->link))) {
				present_message(msg);
				release_message(msg);
			}
		}

		/* Retry the server connection, unless we shouldn't. */
		gale_retry(client);
		if (do_notify) notify();
	}

	gale_alert(GALE_ERROR,"connection lost",0);
	return 0;
}

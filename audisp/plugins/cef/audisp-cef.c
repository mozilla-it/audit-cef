/* audisp-cef.c --
 * Copyright (c) 2013 Mozilla Corporation.
 * Portions Copyright 2008 Red Hat Inc., Durham, North Carolina.
 * All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Authors:
 *   Guillaume Destuynder <gdestuynder@mozilla.com>
 *   Steve Grubb <sgrubb@redhat.com>
 *
 */

#include "config.h"
#include <stdio.h>
#include <signal.h>
#include <syslog.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <sys/utsname.h>
#include <sys/stat.h>
#include <time.h>
#include "libaudit.h"
#include "auparse.h"
#include "private.h"
#include "cef-config.h"

#define CONFIG_FILE "/etc/audisp/audisp-cef.conf"
//This is the maximum arg len for commands before truncating. Syslog often will otherwise truncate the msg.
#define MAX_ARG_LEN 512
#define BUF_SIZE 32
//Bump when the message is modifie
#define CEF_AUDIT_MESSAGE_VERSION 2

static volatile int stop = 0;
static volatile int hup = 0;
static cef_conf_t config;
static char *hostname = NULL;
static auparse_state_t *au = NULL;
static int machine = -1;

typedef struct	ll {
	char val[1024];
	struct ll *next;
} attr_t;

struct cef_msg_type {
char	*hdr;
char	*type;
char	*app;
int		version;
char	*msgname;
char	*msgdesc;
int		severity;
struct	ll *attr;
time_t	au_time;
};

static void handle_event(auparse_state_t *au,
		auparse_cb_event_t cb_event_type, void *user_data);

static void term_handler( int sig )
{
	stop = 1;
}

static void hup_handler( int sig )
{
	hup = 1;
}

static void reload_config(void)
{
	hup = 0;
}

/* find string distance from *in until char c is reached */
unsigned int strstok(char *in, char c)
{
	unsigned int slen, len = 0;

	if (in == NULL)
		return len;

	slen = strlen(in);

	while (in[len] != c && len <= slen)
		len++;
	len++;
	return len;
}

int main(int argc, char *argv[])
{
	char tmp[MAX_AUDIT_MESSAGE_LENGTH];
	struct sigaction sa;
	struct utsname uts;

	sa.sa_flags = 0;
	sigemptyset(&sa.sa_mask);
	sa.sa_handler = term_handler;
	sigaction(SIGTERM, &sa, NULL);
	sa.sa_handler = hup_handler;

	uname(&uts);
	hostname = (char *)malloc(sizeof(uts.nodename));
	sprintf(hostname, "%s", uts.nodename);

	if (load_config(&config, CONFIG_FILE))
		return 1;

	openlog("audit-cef", LOG_CONS, config.facility);

	au = auparse_init(AUSOURCE_FEED, 0);
	if (au == NULL) {
		syslog(LOG_ERR, "could not initialize auparse");
		free_config(&config);
		return -1;
	}
   
   machine = audit_detect_machine();
   if (machine < 0)
       return -1;

	auparse_add_callback(au, handle_event, NULL, NULL);

	syslog(LOG_INFO, "audisp-cef loaded\n");
	do {
		if (hup)
			reload_config();

		while (fgets_unlocked(tmp, MAX_AUDIT_MESSAGE_LENGTH, stdin) &&
							hup==0 && stop==0)
			auparse_feed(au, tmp, strnlen(tmp, MAX_AUDIT_MESSAGE_LENGTH));

		if (feof(stdin))
			break;
	} while (stop == 0);

	auparse_flush_feed(au);
	auparse_destroy(au);
	free(hostname);
	syslog(LOG_INFO, "audisp-cef unloaded\n");
	closelog();

	return 0;
}

/*
 * This function seeks to the specified record returning its type on succees
 */
static int goto_record_type(auparse_state_t *au, int type)
{
	int cur_type;

	auparse_first_record(au);
	do {
		cur_type = auparse_get_type(au);
		if (cur_type == type) {
			auparse_first_field(au);
			return type;  // Normal exit
		}
	} while (auparse_next_record(au) > 0);

	return -1;
}

char *unescape(char *in)
{
	char *dst = in;
	char *s = dst;
	char *src = in;
	char c;

	while ((c = *src++) != '\0') {
    	if (c != '"')
        	*dst++ = c;
	}
	*dst = '\0';
	return s;
}

attr_t *cef_add_attr(attr_t *list, const char *st, char *val)
{
	attr_t *new;

	if (val == NULL)
			return list;
	if (strstr(val, "(null)") != NULL)
			return list;

	new = malloc(sizeof(attr_t));
	snprintf(new->val, 1023, "%s%s ", st, unescape(val));
	new->next = list;
	return new;
}

char *get_username(int uid)
{
	size_t bufsize;
	char *buf;
	char *name;
	struct passwd pwd;
	struct passwd *result;

	bufsize = sysconf(_SC_GETPW_R_SIZE_MAX);
	if (bufsize == -1)
			bufsize = 16384;
	buf = (char *)malloc(bufsize);

	if (uid == -1)
			return NULL;
	getpwuid_r(uid, &pwd, buf, bufsize, &result);
	name = strdup(pwd.pw_name);
	free(buf);
	return name;
}

char *get_proc_name(int pid)
{
	char p[1024];
	char proc[64];
	FILE *fp;
	snprintf(p, 512, "/proc/%d/status", pid);
	fp = fopen(p, "r");
	if (fp) {
		fscanf(fp, "Name: %63s", proc);
		fclose(fp);
	} else
		return NULL;
	return proc;
}

void syslog_cef_msg(struct cef_msg_type cef_msg)
{
	attr_t *head = cef_msg.attr;
	attr_t *prev;
	char msg[1500];

	snprintf(msg, 1500, "%s|%s|%s|%u|%s|%s|%u|end=%ld ", cef_msg.hdr, cef_msg.type, cef_msg.app,
		cef_msg.version, cef_msg.msgname, cef_msg.msgdesc, cef_msg.severity, cef_msg.au_time);
	while (head) {
			snprintf(msg+strlen(msg), 1500, "%s", head->val);
			prev = head;
			head = head->next;
			free(prev);
	}
	syslog(LOG_INFO, msg);
}

static void handle_event(auparse_state_t *au,
		auparse_cb_event_t cb_event_type, void *user_data)
{
	int type, rc, num=0;
	time_t au_time;

	struct cef_msg_type cef_msg = {
		.hdr		= "CEF:0",
		.type		= "Unix",
		.app		= "auditd",
		.version	= CEF_AUDIT_MESSAGE_VERSION,
		.severity	= 3,
	};

	const char *cwd = NULL, *argc = NULL, *cmd = NULL;
	char *sys;
	const char *syscall = NULL;
	char fullcmd[MAX_ARG_LEN+1] = "\0";
	char fullcmdt[5] = "No\0";
	char extra[512] = "\0";

	char f[8];
	int len, tmplen;
	int argcount, i;
	int havecef = 0;

	if (cb_event_type != AUPARSE_CB_EVENT_READY)
		return;

	while (auparse_goto_record_num(au, num) > 0) {
		type = auparse_get_type(au);
		rc = 0;
		auparse_first_field(au);
		cef_msg.au_time = auparse_get_time(au);
		switch (type) {
		   	case AUDIT_AVC:
				argc = auparse_find_field(au, "apparmor");
				if (!argc)
					return;

				havecef = 1;
				cef_msg.msgname = "AVC_APPARMOR";

				cef_msg.attr = cef_add_attr(cef_msg.attr, "cs1Label=Result cs1=", auparse_get_field_str(au));
				goto_record_type(au, type);

				cef_msg.msgdesc = unescape(auparse_find_field(au, "info"));
				goto_record_type(au, type);

				cef_msg.attr = cef_add_attr(cef_msg.attr, "cs2Label=Operation cs2=", auparse_find_field(au, "operation"));
				goto_record_type(au, type);

				cef_msg.attr = cef_add_attr(cef_msg.attr, "cs3Label=Profile cs3=", auparse_find_field(au, "profile"));
				goto_record_type(au, type);

				cef_msg.attr = cef_add_attr(cef_msg.attr, "cs4Label=Command cs4=", auparse_find_field(au, "comm"));
				goto_record_type(au, type);

				if (auparse_find_field(au, "parent"))
					cef_msg.attr = cef_add_attr(cef_msg.attr, "sproc=", get_proc_name(auparse_get_field_int(au)));
				goto_record_type(au, type);

				if (auparse_find_field(au, "pid"))
					cef_msg.attr = cef_add_attr(cef_msg.attr, "dproc=", get_proc_name(auparse_get_field_int(au)));
				goto_record_type(au, type);

				snprintf(extra+strlen(extra), 16, "error\\=%s ", auparse_find_field(au, "error"));
				goto_record_type(au, type);
				snprintf(extra+strlen(extra), 16, "name\\=%s ", auparse_find_field(au, "name"));
				goto_record_type(au, type);
				snprintf(extra+strlen(extra), 16, "srcname\\=%s ", auparse_find_field(au, "srcname"));
				goto_record_type(au, type);
				snprintf(extra+strlen(extra), 16, "flags\\=%s ", auparse_find_field(au, "flags"));
				goto_record_type(au, type);
				break;
			case AUDIT_EXECVE:
				argc = auparse_find_field(au, "argc");
				if (argc)
					argcount = auparse_get_field_int(au);
				else
					argcount = 0;
				fullcmd[0] = '\0';
				len = 0;
				for (i = 0; i != argcount; i++) {
					goto_record_type(au, type);
					tmplen = snprintf(f, 7, "a%d", i);
					f[tmplen] = '\0';
					cmd = auparse_find_field(au, f);
					cmd = auparse_interpret_field(au);
					if (!cmd)
						continue;
					if (MAX_ARG_LEN-strlen(fullcmd) > strlen(cmd)) {
						if (len == 0)
							len += sprintf(fullcmd+len, "%s", cmd);
						else
							len += sprintf(fullcmd+len, " %s", cmd);
					} else
							strncpy(fullcmdt, "Yes\0", 4);
				}
				cef_msg.attr = cef_add_attr(cef_msg.attr, "cs2Label=Truncated cs2=", fullcmdt);
				cef_msg.attr = cef_add_attr(cef_msg.attr, "cs1Label=Command cs1=", fullcmd);
				break;
			case AUDIT_CWD:
				cwd = auparse_find_field(au, "cwd");
				if (cwd) {
					auparse_interpret_field(au);
					snprintf(extra+strlen(extra), 128, "cwd\\=%s ", cwd);
				}
				break;
			case AUDIT_PATH:
				cef_msg.attr = cef_add_attr(cef_msg.attr, "fname=", auparse_find_field(au, "name"));
				goto_record_type(au, type);

				snprintf(extra+strlen(extra), 16, "inode\\=%s ", auparse_find_field(au, "inode"));
				goto_record_type(au, type);
				snprintf(extra+strlen(extra), 16, "dev\\=%s ", auparse_find_field(au, "dev"));
				goto_record_type(au, type);
				snprintf(extra+strlen(extra), 16, "mode\\=%s ", auparse_find_field(au, "mode"));
				goto_record_type(au, type);
				snprintf(extra+strlen(extra), 16, "ouid\\=%s ", auparse_find_field(au, "ouid"));
				goto_record_type(au, type);
				snprintf(extra+strlen(extra), 16, "ogid\\=%s ", auparse_find_field(au, "ogid"));
				goto_record_type(au, type);
				snprintf(extra+strlen(extra), 16, "rdev\\=%s ", auparse_find_field(au, "rdev"));
				goto_record_type(au, type);
				break;
			case AUDIT_SYSCALL:
				syscall = auparse_find_field(au, "syscall");
				if (!syscall)
					return;
				i = auparse_get_field_int(au);
				sys = audit_syscall_to_name(i, machine);
				if (!sys) {
					syslog(LOG_INFO, "Unknown system call %u", i);
					return;
				}

				if (!strncmp(sys, "write", 5) || !strncmp(sys, "open", 4) || !strncmp(sys, "unlink", 6)) {
					havecef = i;
					cef_msg.msgname = "WRITE";
					cef_msg.msgdesc = "Write or append to file";
				} else if (!strncmp(sys, "setxattr", 8)) {
					havecef = i;
					cef_msg.msgname = "ATTR";
					cef_msg.msgdesc = "Change file attributes";
				} else if (!strncmp(sys, "chmod", 5)) {
					havecef = i;
					cef_msg.msgname = "CHMOD";
					cef_msg.msgdesc = "Change file mode";
				} else if (!strncmp(sys, "chown", 5)) {
					havecef = i;
					cef_msg.msgname = "CHOWN";
					cef_msg.msgdesc = "Change file owner";
				} else if (!strncmp(sys, "ptrace",  6)) {
					havecef = i;
					cef_msg.msgname = "PTRACE";
					cef_msg.msgdesc = "Process tracing";
				} else if (!strncmp(sys, "execve", 6)) {
					havecef = i;
					cef_msg.msgname = "EXECVE";
					cef_msg.msgdesc = "Unix Exec";
				} else {
					syslog(LOG_INFO, "Unhandled system call %u %s", i, sys);
				}

				cef_msg.attr = cef_add_attr(cef_msg.attr, "cs3Label=AuditKey cs3=", auparse_find_field(au, "key"));
				goto_record_type(au, type);

				if (auparse_find_field(au, "ppid"))
					cef_msg.attr = cef_add_attr(cef_msg.attr, "cs5Label=ParentProcess cs5=", get_proc_name(auparse_get_field_int(au)));
				goto_record_type(au, type);

				i = 0;
				if (auparse_find_field(au, "auid")) {
					cef_msg.attr = cef_add_attr(cef_msg.attr, "suser=", get_username(auparse_get_field_int(au)));
					cef_msg.attr = cef_add_attr(cef_msg.attr, "cn1Label=auid cn1=",  auparse_get_field_str(au));
					i = 1;
				}
				goto_record_type(au, type);

				if (auparse_find_field(au, "uid")) {
					//if we have had no auid, we set suser to the real source user (else its set to the audited source user)
					if (i == 0)
						cef_msg.attr = cef_add_attr(cef_msg.attr, "suser=", get_username(auparse_get_field_int(au)));
					cef_msg.attr = cef_add_attr(cef_msg.attr, "suid=", auparse_get_field_str(au));
				}
				goto_record_type(au, type);

				cef_msg.attr = cef_add_attr(cef_msg.attr, "cs4Label=TTY cs4=", auparse_find_field(au, "tty"));
				goto_record_type(au, type);
				cef_msg.attr = cef_add_attr(cef_msg.attr, "dproc=", auparse_find_field(au, "exe"));
				goto_record_type(au, type);

				snprintf(extra+strlen(extra), 16, "pid\\=%s ", auparse_find_field(au, "pid"));
				goto_record_type(au, type);
				snprintf(extra+strlen(extra), 16, "gid\\=%s ", auparse_find_field(au, "gid"));
				goto_record_type(au, type);
				snprintf(extra+strlen(extra), 16, "euid\\=%s ", auparse_find_field(au, "euid"));
				goto_record_type(au, type);
				snprintf(extra+strlen(extra), 16, "suid\\=%s ", auparse_find_field(au, "suid"));
				goto_record_type(au, type);
				snprintf(extra+strlen(extra), 16, "fsuid\\=%s ", auparse_find_field(au, "fsuid"));
				goto_record_type(au, type);
				snprintf(extra+strlen(extra), 16, "egid\\=%s ", auparse_find_field(au, "egid"));
				goto_record_type(au, type);
				snprintf(extra+strlen(extra), 16, "sgid\\=%s ", auparse_find_field(au, "sgid"));
				goto_record_type(au, type);
				snprintf(extra+strlen(extra), 16, "fsgid\\=%s ", auparse_find_field(au, "fsgid"));
				goto_record_type(au, type);
				snprintf(extra+strlen(extra), 16, "ses\\=%s ", auparse_find_field(au, "ses"));
				goto_record_type(au, type);
				break;
			default:
				break;
		}
		num++;
	}

	if (!havecef)
		return;

	if (strlen(extra) > 512) {
		extra[512] = '\0';
		cef_msg.attr = cef_add_attr(cef_msg.attr, "cs6Label=MsgTruncated cs6=", "Yes");
	}
	cef_msg.attr = cef_add_attr(cef_msg.attr, "msg=", extra);
	cef_msg.attr = cef_add_attr(cef_msg.attr, "dhost=", hostname);
	syslog_cef_msg(cef_msg);
}

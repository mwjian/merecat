/* libhttpd.c - HTTP protocol library
**
** Copyright (C) 1995-2015  Jef Poskanzer <jef@mail.acme.com>
** Copyright (C) 2016-2018  Joachim Nilsson <troglobit@gmail.com>
** All rights reserved.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions
** are met:
** 1. Redistributions of source code must retain the above copyright
**    notice, this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**    notice, this list of conditions and the following disclaimer in the
**    documentation and/or other materials provided with the distribution.
**
** THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
** AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
** IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
** ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNERS OR CONTRIBUTORS BE
** LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
** CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
** SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
** INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
** CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
** ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
** THE POSSIBILITY OF SUCH DAMAGE.
*/


#include <config.h>

#include <sys/types.h>
#include <sys/param.h>
#include <sys/stat.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#ifdef HAVE_MEMORY_H
#include <memory.h>
#endif
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <stdarg.h>

#include <stdint.h>		/* int64_t */
#include <inttypes.h>		/* PRId64 */

#ifdef HAVE_OSRELDATE_H
#include <osreldate.h>
#endif

#ifdef HAVE_DIRENT_H
# include <dirent.h>
# define NAMLEN(dirent) strlen((dirent)->d_name)
#else
# define dirent direct
# define NAMLEN(dirent) (dirent)->d_namlen
# ifdef HAVE_SYS_NDIR_H
#  include <sys/ndir.h>
# endif
# ifdef HAVE_SYS_DIR_H
#  include <sys/dir.h>
# endif
# ifdef HAVE_NDIR_H
#  include <ndir.h>
# endif
#endif

extern char *crypt(const char *key, const char *setting);

#include "base64.h"
#include "file.h"
#include "libhttpd.h"
#include "match.h"
#include "md5.h"
#include "merecat.h"
#include "mmc.h"
#include "ssl.h"
#include "tdate_parse.h"
#include "timers.h"

#ifdef SHOW_SERVER_VERSION
#define EXPOSED_SERVER_SOFTWARE SERVER_SOFTWARE
#else
#define EXPOSED_SERVER_SOFTWARE PACKAGE_NAME
#endif

#ifndef STDIN_FILENO
#define STDIN_FILENO 0
#endif
#ifndef STDOUT_FILENO
#define STDOUT_FILENO 1
#endif
#ifndef STDERR_FILENO
#define STDERR_FILENO 2
#endif

#ifndef SHUT_WR
#define SHUT_WR 1
#endif

#ifdef __CYGWIN__
#define timezone  _timezone
#endif

#ifndef MAX
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#endif
#ifndef MIN
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#endif

#define SETSOCKOPT(sd, level, opt)					\
	do {								\
		int val = 1;						\
		if (setsockopt(sd, level, opt, &val, sizeof(val)) < 0)	\
			syslog(LOG_CRIT, "Failed enabling %s: %s",	\
			       #opt, strerror(errno));			\
	} while (0)

/* Forwards. */
static void check_options(void);
static void free_httpd_server(struct httpd_server *hs);
static int initialize_listen_socket(httpd_sockaddr *hsa);
static void add_response(struct httpd_conn *hc, const char *str);
static void send_mime(struct httpd_conn *hc, int status, char *title, char *encodings, const char *extraheads, const char *type, off_t length,
		      time_t mod);
static void send_response(struct httpd_conn *hc, int status, char *title, const char *extraheads, char *form, char *arg);
static void send_response_tail(struct httpd_conn *hc);
static void defang(char *str, char *dfstr, int dfsize);

#ifdef ERR_DIR
static int send_err_file(struct httpd_conn *hc, int status, char *title, const char *extraheads, char *filename);
#endif
#ifdef AUTH_FILE
static void send_authenticate(struct httpd_conn *hc, char *realm);
static int auth_check(struct httpd_conn *hc, char *dir);
static int auth_check2(struct httpd_conn *hc, char *dir);
#endif
#ifdef ACCESS_FILE
static int access_check(struct httpd_conn *hc, char *dir);
static int access_check2(struct httpd_conn *hc, char *dir);
#endif
static void send_dirredirect(struct httpd_conn *hc);
static int hexit(char c);
static void strdecode(char *to, char *from);

#ifdef GENERATE_INDEXES
static void strencode(char *to, int tosize, char *from);
#endif
#ifdef TILDE_MAP_1
static int tilde_map_1(struct httpd_conn *hc);
#endif
#ifdef TILDE_MAP_2
static int tilde_map_2(struct httpd_conn *hc);
#endif
static int vhost_map(struct httpd_conn *hc);
static char *expand_symlinks(char *path, char **trailer, int no_symlink_check, int tildemapped);
static char *bufgets(struct httpd_conn *hc);
static void de_dotdot(char *file);
static void init_mime(void);
static void figure_mime(struct httpd_conn *hc);

#ifdef CGI_TIMELIMIT
static void cgi_kill2(arg_t arg, struct timeval *now);
static void cgi_kill(arg_t arg, struct timeval *now);
#endif
#ifdef GENERATE_INDEXES
static int ls(struct httpd_conn *hc);
#endif
static char *build_env(char *fmt, char *arg);

#ifdef SERVER_NAME_LIST
static char *hostname_map(char *hostname);
#endif
static char **make_envp(struct httpd_conn *hc);
static char **make_argp(struct httpd_conn *hc);
static void cgi_interpose_input(struct httpd_conn *hc, int wfd);
static void post_post_garbage_hack(struct httpd_conn *hc);
static void cgi_interpose_output(struct httpd_conn *hc, int rfd);
static void cgi_child(struct httpd_conn *hc);
static int cgi(struct httpd_conn *hc);
static int really_start_request(struct httpd_conn *hc, struct timeval *now);
static void make_log_entry(struct httpd_conn *hc);
static int check_referer(struct httpd_conn *hc);
static int really_check_referer(struct httpd_conn *hc);
static int sockaddr_check(httpd_sockaddr *hsa);
static size_t sockaddr_len(httpd_sockaddr *hsa);

#ifndef HAVE_ATOLL
static long long atoll(const char *str);
#endif

/* This global keeps track of whether we are in the main process or a
** sub-process.  The reason is that httpd_send_response() can get called
** in either context; when it is called from the main process it must use
** non-blocking I/O to avoid stalling the server, but when it is called
** from a sub-process it wants to use blocking I/O so that the whole
** response definitely gets written.  So, it checks this variable.  A bit
** of a hack but it seems to do the right thing.
*/
static int sub_process = 0;


static void check_options(void)
{
#if defined(TILDE_MAP_1) && defined(TILDE_MAP_2)
	syslog(LOG_CRIT, "both TILDE_MAP_1 and TILDE_MAP_2 are defined");
	exit(1);
#endif
}


static void free_httpd_server(struct httpd_server *hs)
{
	if (hs->binding_hostname)
		free(hs->binding_hostname);
	if (hs->cwd)
		free(hs->cwd);
	if (hs->cgi_pattern)
		free(hs->cgi_pattern);
	if (hs->charset)
		free(hs->charset);
	if (hs->url_pattern)
		free(hs->url_pattern);
	if (hs->local_pattern)
		free(hs->local_pattern);
	free(hs);
}


struct httpd_server *httpd_init(char *hostname, httpd_sockaddr *hsav4, httpd_sockaddr *hsav6,
				unsigned short port, void *ssl_ctx, char *cgi_pattern, int cgi_limit,
				char *charset, int max_age, char *cwd, int no_log,
				int no_symlink_check, int vhost, int global_passwd, char *url_pattern,
				char *local_pattern, int no_empty_referers, int list_dotfiles)
{
	struct httpd_server *hs;
	static char ghnbuf[256];
	char *cp;

	check_options();

	hs = NEW(struct httpd_server, 1);
	if (!hs) {
		syslog(LOG_CRIT, "out of memory allocating struct httpd_server");
		return NULL;
	}

	if (hostname) {
		hs->binding_hostname = strdup(hostname);
		if (!hs->binding_hostname) {
			syslog(LOG_CRIT, "out of memory copying hostname");
			return NULL;
		}

		hs->server_hostname = hs->binding_hostname;
	} else {
		hs->binding_hostname = NULL;
		hs->server_hostname  = NULL;
		if (gethostname(ghnbuf, sizeof(ghnbuf)) < 0)
			ghnbuf[0] = '\0';

#ifdef SERVER_NAME_LIST
		if (ghnbuf[0] != '\0')
			hs->server_hostname = hostname_map(ghnbuf);
#endif

		if (!hs->server_hostname) {
#ifdef SERVER_NAME
			hs->server_hostname = SERVER_NAME;
#else
			if (ghnbuf[0] != '\0')
				hs->server_hostname = ghnbuf;
#endif
		}
	}

	hs->port = port;
	hs->ctx = ssl_ctx;

	if (!cgi_pattern) {
		hs->cgi_pattern = NULL;
	} else {
		/* Nuke any leading slashes. */
		if (cgi_pattern[0] == '/')
			++cgi_pattern;

		hs->cgi_pattern = strdup(cgi_pattern);
		if (!hs->cgi_pattern) {
			syslog(LOG_CRIT, "out of memory copying cgi_pattern");
			return NULL;
		}

		/* Nuke any leading slashes in the cgi pattern. */
		while ((cp = strstr(hs->cgi_pattern, "|/")))
			/* -2 for the offset, +1 for the '\0' */
			memmove(cp + 1, cp + 2, strlen(cp) - 1);
	}

	hs->cgi_tracker = calloc(cgi_limit, sizeof(pid_t));
	hs->cgi_limit = cgi_limit;
	hs->cgi_count = 0;
	hs->charset = strdup(charset);
	hs->max_age = max_age;

	hs->cwd = strdup(cwd);
	if (!hs->cwd) {
		syslog(LOG_CRIT, "out of memory copying cwd");
		return NULL;
	}

	if (!url_pattern) {
		hs->url_pattern = NULL;
	} else {
		hs->url_pattern = strdup(url_pattern);
		if (!hs->url_pattern) {
			syslog(LOG_CRIT, "out of memory copying url_pattern");
			return NULL;
		}
	}

	if (!local_pattern) {
		hs->local_pattern = NULL;
	} else {
		hs->local_pattern = strdup(local_pattern);
		if (!hs->local_pattern) {
			syslog(LOG_CRIT, "out of memory copying local_pattern");
			return NULL;
		}
	}

	hs->no_log = no_log;
	hs->no_symlink_check = no_symlink_check;
	hs->vhost = vhost;
	hs->global_passwd = global_passwd;
	hs->no_empty_referers = no_empty_referers;
	hs->list_dotfiles = list_dotfiles;

	/* Initialize listen sockets.  Try v6 first because of a Linux peculiarity;
	** like some other systems, it has magical v6 sockets that also listen for
	** v4, but in Linux if you bind a v4 socket first then the v6 bind fails.
	*/
	if (!hsav6)
		hs->listen6_fd = -1;
	else
		hs->listen6_fd = initialize_listen_socket(hsav6);
	if (!hsav4)
		hs->listen4_fd = -1;
	else
		hs->listen4_fd = initialize_listen_socket(hsav4);

	/* If we didn't get any valid sockets, fail. */
	if (hs->listen4_fd == -1 && hs->listen6_fd == -1) {
		free_httpd_server(hs);
		return NULL;
	}

	init_mime();

	/* Done initializing. */
	if (!hs->binding_hostname)
		syslog(LOG_NOTICE, "%s starting on port %d, vhost: %d", PACKAGE_STRING, hs->port, vhost);
	else
		syslog(LOG_NOTICE, "%s starting on %s, port %d, vhost: %d", PACKAGE_STRING,
		       httpd_ntoa(hs->listen4_fd != -1 ? hsav4 : hsav6), (int)hs->port, vhost);

	return hs;
}


static int initialize_listen_socket(httpd_sockaddr *hsa)
{
	int listen_fd;
	int flags;

	/* Check sockaddr. */
	if (!sockaddr_check(hsa)) {
		syslog(LOG_CRIT, "unknown sockaddr family on listen socket");
		return -1;
	}

	/* Create socket. */
	listen_fd = socket(hsa->sa.sa_family, SOCK_STREAM, 0);
	if (listen_fd < 0) {
		syslog(LOG_CRIT, "Failed opening socket for %s: %s", httpd_ntoa(hsa), strerror(errno));
		return -1;
	}
	fcntl(listen_fd, F_SETFD, 1);

	/* Allow reuse of local addresses. */
	SETSOCKOPT(listen_fd, SOL_SOCKET, SO_REUSEADDR);
#ifdef SO_REUSEPORT
	SETSOCKOPT(listen_fd, SOL_SOCKET, SO_REUSEPORT);
#endif
	/* Bind to it. */
	if (bind(listen_fd, &hsa->sa, sockaddr_len(hsa)) < 0) {
		syslog(LOG_CRIT, "Failed binding to %s port %d: %s", httpd_ntoa(hsa), httpd_port(hsa), strerror(errno));
		close(listen_fd);
		return -1;
	}

	/* Set the listen file descriptor to no-delay / non-blocking mode. */
	flags = fcntl(listen_fd, F_GETFL, 0);
	if (flags == -1) {
		syslog(LOG_CRIT, "fcntl F_GETFL: %s", strerror(errno));
		close(listen_fd);
		return -1;
	}
	if (fcntl(listen_fd, F_SETFL, flags | O_NDELAY) < 0) {
		syslog(LOG_CRIT, "fcntl O_NDELAY: %s", strerror(errno));
		close(listen_fd);
		return -1;
	}

	/* Start a listen going. */
	if (listen(listen_fd, LISTEN_BACKLOG) < 0) {
		syslog(LOG_CRIT, "listen: %s", strerror(errno));
		close(listen_fd);
		return -1;
	}

	/* Use accept filtering, if available. */
#ifdef SO_ACCEPTFILTER
	{
#if (__FreeBSD_version >= 411000)
#define ACCEPT_FILTER_NAME "httpready"
#else
#define ACCEPT_FILTER_NAME "dataready"
#endif
		struct accept_filter_arg af;

		memset(&af, 0, sizeof(af));
		strcpy(af.af_name, ACCEPT_FILTER_NAME);
		setsockopt(listen_fd, SOL_SOCKET, SO_ACCEPTFILTER, &af, sizeof(af));
	}
#endif /* SO_ACCEPTFILTER */

	return listen_fd;
}


void httpd_exit(struct httpd_server *hs)
{
	httpd_ssl_exit(hs);
	httpd_unlisten(hs);
	free_httpd_server(hs);
}


void httpd_unlisten(struct httpd_server *hs)
{
	if (hs->listen4_fd != -1) {
		close(hs->listen4_fd);
		hs->listen4_fd = -1;
	}
	if (hs->listen6_fd != -1) {
		close(hs->listen6_fd);
		hs->listen6_fd = -1;
	}
}


/* Conditional macro to allow two alternate forms for use in the built-in
** error pages.  If EXPLICIT_ERROR_PAGES is defined, the second and more
** explicit error form is used; otherwise, the first and more generic
** form is used.
*/
#ifdef EXPLICIT_ERROR_PAGES
#define ERROR_FORM(a,b) b
#else
#define ERROR_FORM(a,b) a
#endif


static char *ok200title = "OK";
static char *ok206title = "Partial Content";

static char *err302title = "Found";
static char *err302form = "The actual URL is '%s'.\n";

static char *err304title = "Not Modified";

char *httpd_err400title = "Bad Request";
char *httpd_err400form = "Your request has bad syntax(%s) or is inherently impossible to satisfy.\n";

#ifdef AUTH_FILE
static char *err401title = "Unauthorized";
static char *err401form = "Authorization required for the URL '%s'.\n";
#endif

static char *err403title = "Forbidden";

#ifndef EXPLICIT_ERROR_PAGES
static char *err403form = "You do not have permission to get URL '%s' from this server.\n";
#endif

static char *err404title = "Not Found";
static char *err404form = "The requested URL '%s' was not found on this server.\n";

char *httpd_err408title = "Request Timeout";
char *httpd_err408form = "No request appeared within a reasonable time period.\n";

static char *err500title = "Internal Error";
static char *err500form = "There was an unusual problem serving the requested URL '%s'.\n";

static char *err501title = "Not Implemented";
static char *err501form = "The requested method '%s' is not implemented by this server.\n";

char *httpd_err503title = "Service Temporarily Overloaded";
char *httpd_err503form = "The requested URL '%s' is temporarily overloaded.  Please try again later.\n";


/* Append a string to the buffer waiting to be sent as response. */
static void add_response(struct httpd_conn *hc, const char *str)
{
	size_t len;

	len = strlen(str);
	httpd_realloc_str(&hc->response, &hc->maxresponse, hc->responselen + len);
	memmove(&(hc->response[hc->responselen]), str, len + 1);
	hc->responselen += len;
}

/* Merecat default style */
const char *httpd_css_default(void)
{
	const char *style = "  <style type=\"text/css\">\n"
		"    body { background-color:#f2f1f0; font-family: sans-serif;}\n"
		"    h2 { border-bottom: 1px solid #f2f1f0; font-weight: normal;}"
		"    address { border-top: 1px solid #f2f1f0; margin-top: 1em; padding-top: 5px; color:#c8c5c2; }"
		"    table { table-layout: fixed; border-collapse: collapse;}\n"
		"    table tr:hover { background-color:#f2f1f0;}\n"
		"    table tr td { text-align: left; padding: 0 5px 0 0px; }\n"
		"    table tr th { text-align: left; padding: 0 5px 0 0px; }\n"
		"    table tr td.icon  { text-align: center; }\n"
		"    table tr th.icon  { text-align: center; }\n"
		"    table tr td.right { text-align: right; }\n"
		"    table tr th.right { text-align: right; }\n"
		"    .right { padding-right: 20px; }\n"
		"    #wrapper {\n"
		"     background-color:white; width:1024px;\n"
		"     padding:1.5em; margin:4em auto; position:absolute;\n"
		"     top:0; left:0; right:0;\n"
		"     border-radius: 10px; border: 1px solid #c8c5c2;\n"
		"    }\n"
		"    #table {\n"
		"     padding: 0em; margin: 0em auto; overflow: auto;\n"
		"    }\n"
		"  </style>\n";

	return style;
}

/* Send the buffered response. */
void httpd_send_response(struct httpd_conn *hc)
{
	/* If we are in a sub-process, turn off no-delay mode. */
	if (sub_process)
		httpd_clear_ndelay(hc->conn_fd);

	/* Send the response, if necessary. */
	if (hc->responselen > 0) {
		make_log_entry(hc);
		httpd_write(hc, hc->response, hc->responselen);
		hc->responselen = 0;
	}
}


/* Set no-delay / non-blocking mode on a socket. */
void httpd_set_ndelay(int fd)
{
	int flags, newflags;

	flags = fcntl(fd, F_GETFL, 0);
	if (flags != -1) {
		newflags = flags | (int)O_NDELAY;
		if (newflags != flags)
			fcntl(fd, F_SETFL, newflags);
	}
}


/* Clear no-delay / non-blocking mode on a socket. */
void httpd_clear_ndelay(int fd)
{
	int flags, newflags;

	flags = fcntl(fd, F_GETFL, 0);
	if (flags != -1) {
		newflags = flags & ~(int)O_NDELAY;
		if (newflags != flags)
			fcntl(fd, F_SETFL, newflags);
	}
}

static int content_encoding(struct httpd_conn *hc, char *encodings, char *buf, size_t len)
{
	int gz, ret = 0, addgz = 0, hasenc = 0;

	gz = hc->compression_type == COMPRESSION_GZIP;
	if (encodings && encodings[0]) {
		hasenc = 1;
		addgz  = gz && !strstr(encodings, "gzip");
	}

	if (hasenc)
		ret = snprintf(buf, len, "Content-Encoding: %s%s\r\n", encodings, addgz ? ", gzip" : "");
	else if (gz)
		ret = snprintf(buf, len, "Content-Encoding: gzip\r\n");

	return ret;
}

static void
send_mime(struct httpd_conn *hc, int status, char *title, char *encodings, const char *extraheads, const char *type, off_t length, time_t mod)
{
	time_t now;
	const char *rfc1123fmt = "%a, %d %b %Y %H:%M:%S GMT";
	char fixed_type[500];
	char buf[1000];
	int partial_content;
	int s100;

	if (status != 200)
		hc->compression_type = COMPRESSION_NONE;

	hc->status = status;
	hc->bytes_to_send = length;
	if (hc->mime_flag) {
		char nowbuf[100];
		char modbuf[100];
		char etagbuf[45] = { 0 };

		if (status == 200 && hc->got_range &&
		    (hc->last_byte_index >= hc->first_byte_index) &&
		    ((hc->last_byte_index != length - 1) ||
		     (hc->first_byte_index != 0)) && (hc->range_if == (time_t)-1 || hc->range_if == hc->sb.st_mtime)) {
			partial_content = 1;
			hc->status = status = 206;
			title = ok206title;
			hc->compression_type = COMPRESSION_NONE;  /* probably some way to get around this... */
		} else {
			partial_content = 0;
			hc->got_range = 0;
		}

		now = time(NULL);
		if (!mod)
			mod = now;
		strftime(nowbuf, sizeof(nowbuf), rfc1123fmt, gmtime(&now));
		strftime(modbuf, sizeof(modbuf), rfc1123fmt, gmtime(&mod));
		snprintf(fixed_type, sizeof(fixed_type), type, hc->hs->charset);

		/* Match Apache as close as possible, but follow RFC 2616, section 4.2 */
		snprintf(buf, sizeof(buf),
			 "%.20s %d %s\r\n"
			 "Date: %s\r\n"
			 "Server: %s\r\n"
			 "Last-Modified: %s\r\n"
			 "Accept-Ranges: bytes\r\n",
			 hc->protocol, status, title, nowbuf, EXPOSED_SERVER_SOFTWARE, modbuf);
		add_response(hc, buf);

		if (partial_content) {
			snprintf(buf, sizeof(buf),
				 "Content-Range: bytes %" PRId64 "-%" PRId64 "/%" PRId64 "\r\n"
				 "Content-Length: %" PRId64 "\r\n",
				 (int64_t)hc->first_byte_index, (int64_t)hc->last_byte_index,
				 (int64_t)length, (int64_t)(hc->last_byte_index - hc->first_byte_index + 1));
			add_response(hc, buf);
		} else if (length >= 0) {
			/*
			 * Avoid sending Content-Length on content we
			 * deflate or have .gz files of already.  In the
			 * former case we don't know the length yet.
			 */
			if (hc->compression_type == COMPRESSION_NONE) {
				snprintf(buf, sizeof(buf), "Content-Length: %" PRId64 "\r\n", (int64_t)length);
				add_response(hc, buf);
			}
		} else {
// Experimental: Allow keep-alive also for dir listings etc.
//			hc->do_keep_alive = 0;
		}

		snprintf(buf, sizeof(buf), "Content-Type: %s\r\n", fixed_type);
		add_response(hc, buf);

		if (content_encoding(hc, encodings, buf, sizeof(buf)))
			add_response(hc, buf);

		s100 = status / 100;
		if (s100 != 2 && s100 != 3) {
			snprintf(buf, sizeof(buf), "Cache-Control: no-cache,no-store\r\n");
			add_response(hc, buf);
		}

		/* EntityTag -- https://en.wikipedia.org/wiki/HTTP_ETag */
		if (hc->file_address) {
			uint8_t dig[MD5_DIGEST_LENGTH];
			MD5_CTX ctx;

			MD5Init(&ctx);
			MD5Update(&ctx, (const u_int8_t *)hc->file_address, length);
			MD5Final(dig, &ctx);
			snprintf(etagbuf, sizeof(etagbuf),
				 "ETag: \"%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x\"\r\n",
				 dig[0], dig[1], dig[2], dig[3], dig[4], dig[5], dig[6], dig[7],
				 dig[8], dig[9], dig[10], dig[11], dig[12], dig[13], dig[14], dig[15]);
		}

		if (hc->hs->max_age >= 0) {
			snprintf(buf, sizeof(buf), "Cache-Control: max-age=%d\r\n%s", hc->hs->max_age, etagbuf);
			add_response(hc, buf);

#ifdef USE_SUPERSEDED_EXPIRES
			char expbuf[100];
			time_t expires;

			expires = now + hc->hs->max_age;
			strftime(expbuf, sizeof(expbuf), rfc1123fmt, gmtime(&expires));
			snprintf(buf, sizeof(buf), "Expires: %s\r\n", expbuf);
			add_response(hc, buf);
#endif
		}

		if (hc->do_keep_alive)
			snprintf(buf, sizeof(buf), "Connection: keep-alive\r\n");
		else
			snprintf(buf, sizeof(buf), "Connection: close\r\n");
		add_response(hc, buf);

		if (extraheads[0] != '\0')
			add_response(hc, extraheads);
		add_response(hc, "\r\n");
	}
}


static int str_alloc_count = 0;
static size_t str_alloc_size = 0;

void httpd_realloc_str(char **str, size_t *curr_len, size_t new_len)
{
	if (*curr_len == 0) {
		*curr_len = MAX(200, new_len + 100);
		*str = NEW(char, *curr_len + 1);

		++str_alloc_count;
		str_alloc_size += *curr_len;
	} else if (new_len > *curr_len) {
		str_alloc_size -= *curr_len;
		*curr_len = MAX(*curr_len * 2, new_len * 5 / 4);
		*str = RENEW(*str, char, *curr_len + 1);

		str_alloc_size += *curr_len;
	} else {
		return;
	}

	if (!*str) {
		syslog(LOG_ERR, "out of memory reallocating a string to %zu bytes", *curr_len);
		exit(1);
	}
}


static void send_response(struct httpd_conn *hc, int status, char *title, const char *extraheads, char *form, char *arg)
{
	char defanged_arg[1000], buf[2000];

	send_mime(hc, status, title, "", extraheads, "text/html; charset=%s", (off_t) - 1, (time_t)0);
	snprintf(buf, sizeof(buf), "<!DOCTYPE html>\n"
		 "<html>\n"
		 " <head>\n"
		 "  <title>%d %s</title>\n"
		 "  <link rel=\"icon\" type=\"image/x-icon\" href=\"/icons/favicon.ico\">\n"
		 "%s"
		 " </head>\n"
		 " <body>\n"
		 "<div id=\"wrapper\" tabindex=\"-1\">\n"
		 "<h2>%d %s</h2>\n"
		 "<p>\n",
		 status, title,
		 httpd_css_default(),
		 status, title);
	add_response(hc, buf);
	defang(arg, defanged_arg, sizeof(defanged_arg));
	snprintf(buf, sizeof(buf), form, defanged_arg);
	add_response(hc, buf);
#ifdef MSIE_PADDING
	if (match("**MSIE**", hc->useragent)) {
		int n;

		add_response(hc, "<!--\n");
		for (n = 0; n < 6; ++n)
			add_response(hc, "Padding so that MSIE deigns to show this error instead of its own canned one.\n");
		add_response(hc, "-->\n");
	}
#endif
	add_response(hc, "</p>");
	send_response_tail(hc);
}

static char *get_hostname(struct httpd_conn *hc)
{
	char *host;
	static char *fallback = "";

	if (hc->hs->vhost && hc->hostname)
		host = hc->hostname;
	else
		host = hc->hs->server_hostname;
	if (!host)
		host = fallback;

	return host;
}

static void send_response_tail(struct httpd_conn *hc)
{
	char buf[1000];

	snprintf(buf, sizeof(buf),
		 " <address>%s httpd at %s port %d</address>\n"
		 "</div>\n"
		 "</body>\n"
		 "</html>\n", EXPOSED_SERVER_SOFTWARE, get_hostname(hc), (int)hc->hs->port);
	add_response(hc, buf);
}


static void defang(char *str, char *dfstr, int dfsize)
{
	char *cp1;
	char *cp2;

	for (cp1 = str, cp2 = dfstr; *cp1 != '\0' && cp2 - dfstr < dfsize - 8; ++cp1, ++cp2) {
		switch (*cp1) {
		case '<':
			*cp2++ = '&';
			*cp2++ = 'l';
			*cp2++ = 't';
			*cp2 = ';';
			break;

		case '>':
			*cp2++ = '&';
			*cp2++ = 'g';
			*cp2++ = 't';
			*cp2 = ';';
			break;

		case '&':
			*cp2++ = '&';
			*cp2++ = 'a';
			*cp2++ = 'm';
			*cp2++ = 'p';
			*cp2 = ';';
			break;

		case '"':
			*cp2++ = '&';
			*cp2++ = 'q';
			*cp2++ = 'u';
			*cp2++ = 'o';
			*cp2++ = 't';
			*cp2 = ';';
			break;

		case '\'':
			*cp2++ = '&';
			*cp2++ = '#';
			*cp2++ = '3';
			*cp2++ = '9';
			*cp2 = ';';
			break;

		case '?':
			*cp2++ = '&';
			*cp2++ = '#';
			*cp2++ = '6';
			*cp2++ = '3';
			*cp2 = ';';
			break;

		default:
			*cp2 = *cp1;
			break;
		}
	}
	*cp2 = '\0';
}


void httpd_send_err(struct httpd_conn *hc, int status, char *title, const char *extraheads, char *form, char *arg)
{
#ifdef ERR_DIR
	char filename[1000];

	/* Try virtual host error page. */
	if (hc->hs->vhost && hc->hostdir[0] != '\0') {
		snprintf(filename, sizeof(filename), "%s/%s/err%d.html", hc->hostdir, ERR_DIR, status);
		if (send_err_file(hc, status, title, extraheads, filename))
			return;
	}

	/* Try server-wide error page. */
	snprintf(filename, sizeof(filename), "%s/err%d.html", ERR_DIR, status);
	if (send_err_file(hc, status, title, extraheads, filename))
		return;

	/* Fall back on built-in error page. */
	send_response(hc, status, title, extraheads, form, arg);
#else
	send_response(hc, status, title, extraheads, form, arg);
#endif
}


#ifdef ERR_DIR
static int send_err_file(struct httpd_conn *hc, int status, char *title, const char *extraheads, char *filename)
{
	FILE *fp;
	char buf[1000];
	size_t r;

	fp = fopen(filename, "r");
	if (!fp)
		return 0;

	send_mime(hc, status, title, "", extraheads, "text/html; charset=%s", (off_t) - 1, (time_t)0);
	for (;;) {
		r = fread(buf, 1, sizeof(buf) - 1, fp);
		if (r == 0)
			break;
		buf[r] = '\0';
		add_response(hc, buf);
	}
	fclose(fp);

#ifdef ERR_APPEND_SERVER_INFO
	send_response_tail(hc);
#endif

	return 1;
}
#endif /* ERR_DIR */

#if defined(ACCESS_FILE) || defined(AUTH_FILE)
static char *find_htfile(char *topdir, char *dir, char *htfile)
{
	int found = 0;
	char *path;
	size_t len = strlen(dir) + strlen(htfile) + 2;

	path = malloc(len);
	if (!path)
		return NULL;

	snprintf(path, len, "%s/%s", (dir[0] ? dir : "."), htfile);
	while (1) {
		int rc;
		char *ptr, *slash;
		struct stat st;

		rc = stat(path, &st);

		ptr = strstr(path, htfile);
		if (!ptr)
			break;
		*--ptr = 0;

		if (rc == 0) {
			found = 1;
			break;
		}

		/* loop until we hit topdir. */
		if (strcmp(topdir, path) == 0)
			break;

		/* Nope, try up a level. */
		slash = strrchr(path, '/');
		if (!slash)
			break;

		memmove(slash + 1, ptr + 1, strlen(htfile) + 1);
	}

	if (!found) {
		free(path);
		return NULL;
	}

	return path;
}
#endif

#ifdef ACCESS_FILE
/* Returns -1 == unauthorized, 0 == no access file, 1 = authorized. */
static int access_check(struct httpd_conn *hc, char *dir)
{
	int rc = 0;
	char *topdir, *tmp = NULL;

	if (!dir) {
		char *ptr;

		if (strstr(hc->expnfilename, ACCESS_FILE)) {
			syslog(LOG_NOTICE, "%.80s URL \"%.80s\" tried to retrieve access file",
			       httpd_client(hc), hc->encodedurl);
			return -1;
		}

		tmp = strdup(hc->expnfilename);
		if (!tmp) {
			syslog(LOG_ERR, "out of memory in access code; "
			       "Denying access.");
			return -1;
		}

		ptr = strrchr(tmp, '/');
		if (!ptr)
			strcpy(tmp, ".");
		else
			*ptr = '\0';

		dir = tmp;
	}

	if (hc->hs->vhost && hc->hostdir[0] != '\0')
		topdir = hc->hostdir;
	else
		topdir = ".";

	if (!hc->hs->global_passwd) {
		char *path;
	local:
		path = find_htfile(topdir, dir, ACCESS_FILE);
		if (path) {
			rc = access_check2(hc, path);
			free(path);
		}

		if (tmp)
			free(tmp);

		return rc;
	}

	rc = access_check2(hc, topdir);
	if (!rc)
		goto local;
	if (tmp)
		free(tmp);

	return rc;
}

/* Returns -1 == unauthorized, 0 == no access file, 1 = authorized. */
static int access_check2(struct httpd_conn *hc, char *dir)
{
	struct in_addr ipv4_addr, ipv4_mask = { 0xffffffff };
	FILE *fp;
	char line[500];
	struct stat sb;
	char *addr, *addr1, *addr2, *mask;
	size_t l;

	/* Construct access filename. */
	httpd_realloc_str(&hc->accesspath, &hc->maxaccesspath, strlen(dir) + 1 + sizeof(ACCESS_FILE));
	snprintf(hc->accesspath, hc->maxaccesspath, "%s/%s", dir, ACCESS_FILE);

	/* Does this directory have an access file? */
	if (lstat(hc->accesspath, &sb) < 0) {
		/* Nope, let the request go through. */
		return 0;
	}

	/* Open the access file. */
	fp = fopen(hc->accesspath, "r");
	if (!fp) {
		/* The file exists but we can't open it? Disallow access. */
		syslog(LOG_ERR, "%.80s access file %.80s could not be opened: %s",
		       httpd_client(hc), hc->accesspath, strerror(errno));

		httpd_send_err(hc, 403, err403title, "",
			       ERROR_FORM(err403form,
					  "The requested URL '%.80s' is protected by an access file. (2)"),
			       hc->encodedurl);
		return -1;
	}

	/* Read it. */
	while (fgets(line, sizeof(line), fp)) {
		/* Nuke newline. */
		l = strlen(line);
		if (line[l - 1] == '\n') line[l - 1] = '\0';

		addr1 = strrchr(line, ' ');
		addr2 = strrchr(line, '\t');
		if (addr1 > addr2)
			addr = addr1;
		else
			addr = addr2;

		if (!addr) {
		err:
			fclose(fp);
			syslog(LOG_ERR, "%.80s access file %.80s: invalid line: %s",
			       httpd_client(hc), hc->accesspath, line);
			httpd_send_err(hc, 403, err403title, "",
				       ERROR_FORM(err403form,
						  "The requested URL '%.80s' is protected by an access file. (1)"),
				       hc->encodedurl);
			return -1;
		}

		mask = strchr(++addr, '/');
		if (mask) {
			*mask++ = '\0';
			if (!*mask)
				goto err;

			if (!strchr(mask, '.')) {
				long l = atol(mask);

				if ((l < 0) || (l > 32))
					goto err;

				for (l = 32 - l; l > 0; --l)
					ipv4_mask.s_addr ^= 1 << (l - 1);
				ipv4_mask.s_addr = htonl(ipv4_mask.s_addr);
			} else {
				if (!inet_aton(mask, &ipv4_mask))
					goto err;
			}
		}

		if (!inet_aton(addr, &ipv4_addr))
			goto err;

		/*
		 * Does client addr match this rule?
		 * TODO: Generalize and add IPv6 support
		 */
		if ((hc->client_addr.sa_in.sin_addr.s_addr & ipv4_mask.s_addr) ==
		    (ipv4_addr.s_addr & ipv4_mask.s_addr)) {
			/* Yes. */
			switch (line[0]) {
			case 'd':
			case 'D':
				break;

			case 'a':
			case 'A':
				fclose(fp);
				return 1;

			default:
				goto err;
			}
		}
	}

	httpd_send_err(hc, 403, err403title, "",
		       ERROR_FORM(err403form, "The requested URL '%.80s' is protected by an address restriction."),
		       hc->encodedurl);
	fclose(fp);

	return -1;
}
#endif /* ACCESS_FILE */

#ifdef AUTH_FILE
static void send_authenticate(struct httpd_conn *hc, char *realm)
{
	static char *header;
	static size_t maxheader = 0;
	static char headstr[] = "WWW-Authenticate: Basic realm=\"";

	httpd_realloc_str(&header, &maxheader, sizeof(headstr) + strlen(realm) + 3);
	snprintf(header, maxheader, "%s%s\"\r\n", headstr, realm);
	httpd_send_err(hc, 401, err401title, header, err401form, hc->encodedurl);
	/* If the request was a POST then there might still be data to be read,
	** so we need to do a lingering close.
	*/
	if (hc->method == METHOD_POST || hc->method == METHOD_PUT)
		hc->should_linger = 1;
}


/* Returns -1 == unauthorized, 0 == no auth file, 1 = authorized. */
static int auth_check(struct httpd_conn *hc, char *dir)
{
	int rc = 0;
	char *topdir, *tmp = NULL;

	if (!dir) {
		char *ptr;

		if (strstr(hc->expnfilename, AUTH_FILE)) {
			syslog(LOG_NOTICE, "%.80s URL \"%.80s\" tried to retrieve auth file",
			       httpd_client(hc), hc->encodedurl);
			return -1;
		}

		tmp = strdup(hc->expnfilename);
		if (!tmp) {
			syslog(LOG_ERR, "out of memory in authentication code; "
			       "Denying authorization.");
			return -1;
		}

		ptr = strrchr(tmp, '/');
		if (!ptr)
			strcpy(tmp, ".");
		else
			*ptr = '\0';

		dir = tmp;
	}

	if (hc->hs->vhost && hc->hostdir[0] != '\0')
		topdir = hc->hostdir;
	else
		topdir = ".";

	if (!hc->hs->global_passwd) {
		char *path;
	local:
		path = find_htfile(topdir, dir, AUTH_FILE);
		if (path) {
			rc = auth_check2(hc, path);
			free(path);
		}

		if (tmp)
			free(tmp);

		return rc;
	}

	rc = auth_check2(hc, topdir);
	if (!rc)
		goto local;
	if (tmp)
		free(tmp);

	return rc;
}


/* Returns -1 == unauthorized, 0 == no auth file, 1 = authorized. */
static int auth_check2(struct httpd_conn *hc, char *dir)
{
	struct stat sb;
	char authinfo[550];
	char *authpass;
	char *colon;
	int l;
	FILE *fp;
	char line[500];
	char *cryp;
	static time_t prevmtime;
	char *crypt_result;

	/* Construct auth filename. */
	httpd_realloc_str(&hc->authpath, &hc->maxauthpath, strlen(dir) + 1 + sizeof(AUTH_FILE));
	snprintf(hc->authpath, hc->maxauthpath, "%s/%s", dir, AUTH_FILE);

	/* Does this directory have an auth file? */
	if (lstat(hc->authpath, &sb) < 0)
		/* Nope, let the request go through. */
		return 0;

	/* Does this request contain basic authorization info? */
	if (hc->authorization[0] == '\0' || strncmp(hc->authorization, "Basic ", 6) != 0) {
		/* Nope, return a 401 Unauthorized. */
		send_authenticate(hc, dir);
		return -1;
	}

	/* Decode it. */
	l = b64_decode(&(hc->authorization[6]), (unsigned char *)authinfo, sizeof(authinfo) - 1);
	authinfo[l] = '\0';
	/* Split into user and password. */
	authpass = strchr(authinfo, ':');
	if (!authpass) {
		/* No colon?  Bogus auth info. */
		send_authenticate(hc, dir);
		return -1;
	}
	*authpass++ = '\0';

	/* If there are more fields, cut them off. */
	colon = strchr(authpass, ':');
	if (colon)
		*colon = '\0';

	/* See if we have a cached entry and can use it. */
	if (hc->maxprevauthpath != 0 &&
	    strcmp(hc->authpath, hc->prevauthpath) == 0 && sb.st_mtime == prevmtime && strcmp(authinfo, hc->prevuser) == 0) {
		/* Yes.  Check against the cached encrypted password. */
		crypt_result = crypt(authpass, hc->prevcryp);
		if (!crypt_result)
			return -1;

		if (strcmp(crypt_result, hc->prevcryp) == 0) {
			/* Ok! */
			httpd_realloc_str(&hc->remoteuser, &hc->maxremoteuser, strlen(authinfo) + 1);
			strcpy(hc->remoteuser, authinfo);
			return 1;
		}

		/* No. */
		send_authenticate(hc, dir);
		return -1;
	}

	/* Open the password file. */
	fp = fopen(hc->authpath, "r");
	if (!fp) {
		/* The file exists but we can't open it?  Disallow access. */
		syslog(LOG_ERR, "%s auth file %s could not be opened: %s", httpd_client(hc), hc->authpath, strerror(errno));
		httpd_send_err(hc, 403, err403title, "",
			       ERROR_FORM(err403form,
					  "The requested URL '%s' is protected by an authentication file, but the authentication file cannot be opened.\n"),
			       hc->encodedurl);
		return -1;
	}

	/* Read it. */
	while (fgets(line, sizeof(line), fp)) {
		/* Nuke newline. */
		l = strlen(line);
		if (line[l - 1] == '\n')
			line[l - 1] = '\0';

		/* Split into user and encrypted password. */
		cryp = strchr(line, ':');
		if (!cryp)
			continue;
		*cryp++ = '\0';

		/* Is this the right user? */
		if (strcmp(line, authinfo) == 0) {
			/* Yes. */
			fclose(fp);

			/* So is the password right? */
			crypt_result = crypt(authpass, cryp);
			if (!crypt_result)
				return -1;

			if (strcmp(crypt_result, cryp) == 0) {
				/* Ok! */
				httpd_realloc_str(&hc->remoteuser, &hc->maxremoteuser, strlen(line) + 1);
				strcpy(hc->remoteuser, line);

				/* And cache this user's info for next time. */
				prevmtime = sb.st_mtime;

				httpd_realloc_str(&hc->prevauthpath, &hc->maxprevauthpath, strlen(hc->authpath) + 1);
				strcpy(hc->prevauthpath, hc->authpath);

				httpd_realloc_str(&hc->prevuser, &hc->maxprevuser, strlen(authinfo) + 1);
				strcpy(hc->prevuser, authinfo);

				httpd_realloc_str(&hc->prevcryp, &hc->maxprevcryp, strlen(cryp) + 1);
				strcpy(hc->prevcryp, cryp);

				return 1;
			}

			/* No. */
			send_authenticate(hc, dir);
			return -1;
		}
	}

	/* Didn't find that user.  Access denied. */
	fclose(fp);
	send_authenticate(hc, dir);

	return -1;
}

#endif /* AUTH_FILE */


static void send_dirredirect(struct httpd_conn *hc)
{
	static char *location;
	static char *header;
	static size_t maxlocation = 0, maxheader = 0;
	static char headstr[] = "Location: ";

	if (hc->query[0] != '\0') {
		char *cp;

		cp = strchr(hc->encodedurl, '?');
		if (cp)	/* should always find it */
			*cp = '\0';

		httpd_realloc_str(&location, &maxlocation, strlen(hc->encodedurl) + 2 + strlen(hc->query));
		snprintf(location, maxlocation, "%s/?%s", hc->encodedurl, hc->query);
	} else {
		httpd_realloc_str(&location, &maxlocation, strlen(hc->encodedurl) + 1);
		snprintf(location, maxlocation, "%s/", hc->encodedurl);
	}

	httpd_realloc_str(&header, &maxheader, sizeof(headstr) + strlen(location));
	snprintf(header, maxheader, "%s%s\r\n", headstr, location);
	send_response(hc, 302, err302title, header, err302form, location);
}


char *httpd_method_str(int method)
{
	switch (method) {
	case METHOD_GET:
		return "GET";

	case METHOD_HEAD:
		return "HEAD";

	case METHOD_POST:
		return "POST";

	case METHOD_PUT:
		return "PUT";

	case METHOD_DELETE:
		return "DELETE";

	case METHOD_CONNECT:
		return "CONNECT";

	case METHOD_OPTIONS:
		return "OPTIONS";

	case METHOD_TRACE:
		return "TRACE";

	default:
		return "UNKNOWN";
	}
}


static int hexit(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;

	return 0;    /* shouldn't happen, we're guarded by isxdigit() */
}


/* Copies and decodes a string.  It's ok for from and to to be the
** same string.
*/
static void strdecode(char *to, char *from)
{
	for (; *from != '\0'; ++to, ++from) {
		if (from[0] == '%' && isxdigit(from[1]) && isxdigit(from[2])) {
			*to = hexit(from[1]) * 16 + hexit(from[2]);
			from += 2;
		} else {
			*to = *from;
		}
	}
	*to = '\0';
}


#ifdef GENERATE_INDEXES
/* Copies and encodes a string. */
static void strencode(char *to, int tosize, char *from)
{
	int tolen;

	for (tolen = 0; *from != '\0' && tolen + 4 < tosize; ++from) {
		if (isalnum(*from) || strchr("/_.-~", *from)) {
			*to = *from;
			++to;
			++tolen;
		} else {
			sprintf(to, "%%%02x", (int)*from & 0xff);
			to += 3;
			tolen += 3;
		}
	}
	*to = '\0';
}
#endif /* GENERATE_INDEXES */


#ifdef TILDE_MAP_1
/* Map a ~username/whatever URL into <prefix>/username. */
static int tilde_map_1(struct httpd_conn *hc)
{
	static char *temp;
	static size_t maxtemp = 0;
	int len;
	static char *prefix = TILDE_MAP_1;

	len = strlen(hc->expnfilename) - 1;
	httpd_realloc_str(&temp, &maxtemp, len);
	strcpy(temp, &hc->expnfilename[1]);
	httpd_realloc_str(&hc->expnfilename, &hc->maxexpnfilename, strlen(prefix) + 1 + len);
	strcpy(hc->expnfilename, prefix);
	if (prefix[0] != '\0')
		strcat(hc->expnfilename, "/");
	strcat(hc->expnfilename, temp);
	return 1;
}
#endif /* TILDE_MAP_1 */

#ifdef TILDE_MAP_2
/* Map a ~username/whatever URL into <user's homedir>/<postfix>. */
static int tilde_map_2(struct httpd_conn *hc)
{
	static char *temp;
	static size_t maxtemp = 0;
	static char *postfix = TILDE_MAP_2;
	char *cp;
	struct passwd *pw;
	char *alt;
	char *rest;

	/* Get the username. */
	httpd_realloc_str(&temp, &maxtemp, strlen(hc->expnfilename) - 1);
	strcpy(temp, &hc->expnfilename[1]);

	cp = strchr(temp, '/');
	if (cp)
		*cp++ = '\0';
	else
		cp = "";

	/* Get the passwd entry. */
	pw = getpwnam(temp);
	if (!pw)
		return 0;

	/* Set up altdir. */
	httpd_realloc_str(&hc->altdir, &hc->maxaltdir, strlen(pw->pw_dir) + 1 + strlen(postfix));
	strcpy(hc->altdir, pw->pw_dir);
	if (postfix[0] != '\0') {
		strcat(hc->altdir, "/");
		strcat(hc->altdir, postfix);
	}

	alt = expand_symlinks(hc->altdir, &rest, 0, 1);
	if (rest[0] != '\0')
		return 0;

	httpd_realloc_str(&hc->altdir, &hc->maxaltdir, strlen(alt));
	strcpy(hc->altdir, alt);

	/* And the filename becomes altdir plus the post-~ part of the original. */
	httpd_realloc_str(&hc->expnfilename, &hc->maxexpnfilename, strlen(hc->altdir) + 1 + strlen(cp));
	snprintf(hc->expnfilename, hc->maxexpnfilename, "%s/%s", hc->altdir, cp);

	/* For this type of tilde mapping, we want to defeat vhost mapping. */
	hc->tildemapped = 1;

	return 1;
}
#endif /* TILDE_MAP_2 */


/*
 * Allow vhosts to share top-level icons/ and cgi-bin/
 */
static int is_vhost_shared(char *path)
{
	int i;
	char *shared[] = {
		"icons/",
		"cgi-bin/",
		NULL
	};

	if (!path || path[0] == 0)
		return 0;

	for (i = 0; shared[i]; i++) {
		if (!strncmp(path, shared[i], strlen(shared[i])))
			return 1;
	}

	return 0;
}


/* Virtual host mapping. */
static int vhost_map(struct httpd_conn *hc)
{
	httpd_sockaddr sa;
	socklen_t sz;
	char *cp1, *temp;
	size_t len;
#ifdef VHOST_DIRLEVELS
	int i;
	char *cp2;
#endif

	/* Figure out the virtual hostname. */
	if (hc->reqhost[0] != '\0') {
		hc->hostname = hc->reqhost;
	} else if (hc->hdrhost[0] != '\0') {
		hc->hostname = hc->hdrhost;
	} else {
		sz = sizeof(sa);
		if (getsockname(hc->conn_fd, &sa.sa, &sz) < 0) {
			syslog(LOG_ERR, "getsockname: %s", strerror(errno));
			return 0;
		}
		hc->hostname = httpd_ntoa(&sa);
	}
	/* Pound it to lower case. */
	for (cp1 = hc->hostname; *cp1 != '\0'; ++cp1)
		if (isupper(*cp1))
			*cp1 = tolower(*cp1);

	if (hc->tildemapped)
		return 1;

	/* Figure out the host directory. */
#ifdef VHOST_DIRLEVELS
	httpd_realloc_str(&hc->hostdir, &hc->maxhostdir, strlen(hc->hostname) + 2 * VHOST_DIRLEVELS);
	if (strncmp(hc->hostname, "www.", 4) == 0)
		cp1 = &hc->hostname[4];
	else
		cp1 = hc->hostname;

	for (cp2 = hc->hostdir, i = 0; i < VHOST_DIRLEVELS; ++i) {
		/* Skip dots in the hostname.  If we don't, then we get vhost
		** directories in higher level of filestructure if dot gets
		** involved into path construction.  It's `while' used here instead
		** of `if' for it's possible to have a hostname formed with two
		** dots at the end of it.
		*/
		while (*cp1 == '.')
			++cp1;

		/* Copy a character from the hostname, or '_' if we ran out. */
		if (*cp1 != '\0')
			*cp2++ = *cp1++;
		else
			*cp2++ = '_';

		/* Copy a slash. */
		*cp2++ = '/';
	}
	strcpy(cp2, hc->hostname);
#else /* VHOST_DIRLEVELS */
	httpd_realloc_str(&hc->hostdir, &hc->maxhostdir, strlen(hc->hostname));
	strcpy(hc->hostdir, hc->hostname);
#endif /* VHOST_DIRLEVELS */

	/* Prepend hostdir to the filename. */
	len  = strlen(hc->expnfilename);
	temp = strdup(hc->expnfilename);
	httpd_realloc_str(&hc->expnfilename, &hc->maxexpnfilename, strlen(hc->hostdir) + 2 + len);
	strcpy(hc->expnfilename, hc->hostdir);

	/* Skip any port number */
	cp1 = strrchr(hc->expnfilename, ':');
	if (cp1)
		*cp1 = 0;

	strcat(hc->expnfilename, "/");
	strcat(hc->expnfilename, temp);
	free(temp);

	return 1;
}


/* Expands all symlinks in the given filename, eliding ..'s and leading
** /'s.  Returns the expanded path (pointer to static string), or NULL
** on errors.  Also returns, in the string pointed to by trailer, any
** trailing parts of the path that don't exist.
**
** This is a fairly nice little routine.  It handles any size filenames
** without excessive mallocs.
*/
static char *expand_symlinks(char *path, char **trailer, int no_symlink_check, int tildemapped)
{
	static char *checked;
	static char *rest;
	char link[5000];
	static size_t maxchecked = 0, maxrest = 0;
	size_t checkedlen, restlen, prevcheckedlen, prevrestlen;
	ssize_t linklen;
	int nlinks, i;
	char *r;
	char *cp1;
	char *cp2;

	if (no_symlink_check) {
		/* If we are chrooted, we can actually skip the symlink-expansion,
		** since it's impossible to get out of the tree.  However, we still
		** need to do the pathinfo check, and the existing symlink expansion
		** code is a pretty reasonable way to do this.  So, what we do is
		** a single stat() of the whole filename - if it exists, then we
		** return it as is with nothing in trailer.  If it doesn't exist, we
		** fall through to the existing code.
		**
		** One side-effect of this is that users can't symlink to central
		** approved CGIs any more.  The workaround is to use the central
		** URL for the CGI instead of a local symlinked one.
		*/
		struct stat sb;

		if (stat(path, &sb) != -1) {
			checkedlen = strlen(path);
			httpd_realloc_str(&checked, &maxchecked, checkedlen);
			strcpy(checked, path);

			/* Trim trailing slashes. */
			while (checkedlen && checked[checkedlen - 1] == '/') {
				checked[checkedlen - 1] = '\0';
				--checkedlen;
			}

			httpd_realloc_str(&rest, &maxrest, 0);
			rest[0] = '\0';
			*trailer = rest;

			return checked;
		}
	}

	/* Start out with nothing in checked and the whole filename in rest. */
	httpd_realloc_str(&checked, &maxchecked, 1);
	checked[0] = '\0';
	checkedlen = 0;
	restlen = strlen(path);
	httpd_realloc_str(&rest, &maxrest, restlen);
	strcpy(rest, path);
	if (!tildemapped) {
		/* Remove any leading slashes. */
		while (restlen && rest[0] == '/') {
			/* One more for '\0', one less for the eaten first */
			memmove(rest, &(rest[1]), strlen(rest));
			--restlen;
		}
	}
	r = rest;
	nlinks = 0;

	/* While there are still components to check... */
	while (restlen > 0) {
		/* Save current checkedlen in case we get a symlink.  Save current
		** restlen in case we get a non-existant component.
		*/
		prevcheckedlen = checkedlen;
		prevrestlen = restlen;

		/* Grab one component from r and transfer it to checked. */
		cp1 = strchr(r, '/');
		if (cp1) {
			i = cp1 - r;
			if (i == 0) {
				/* Special case for absolute paths. */
				httpd_realloc_str(&checked, &maxchecked, checkedlen + 1);
				strncpy(&checked[checkedlen], r, 1);
				checkedlen += 1;
			} else if (strncmp(r, "..", MAX(i, 2)) == 0) {
				/* Ignore ..'s that go above the start of the path. */
				if (checkedlen != 0) {
					cp2 = strrchr(checked, '/');
					if (!cp2)
						checkedlen = 0;
					else if (cp2 == checked)
						checkedlen = 1;
					else
						checkedlen = cp2 - checked;
				}
			} else {
				httpd_realloc_str(&checked, &maxchecked, checkedlen + 1 + i);
				if (checkedlen > 0 && checked[checkedlen - 1] != '/')
					checked[checkedlen++] = '/';
				strncpy(&checked[checkedlen], r, i);
				checkedlen += i;
			}
			checked[checkedlen] = '\0';
			r += i + 1;
			restlen -= i + 1;
		} else {
			/* No slashes remaining, r is all one component. */
			if (strcmp(r, "..") == 0) {
				/* Ignore ..'s that go above the start of the path. */
				if (checkedlen != 0) {
					cp2 = strrchr(checked, '/');
					if (!cp2)
						checkedlen = 0;
					else if (cp2 == checked)
						checkedlen = 1;
					else
						checkedlen = cp2 - checked;
					checked[checkedlen] = '\0';
				}
			} else {
				httpd_realloc_str(&checked, &maxchecked, checkedlen + 1 + restlen);
				if (checkedlen > 0 && checked[checkedlen - 1] != '/')
					checked[checkedlen++] = '/';
				strcpy(&checked[checkedlen], r);
				checkedlen += restlen;
			}
			r += restlen;
			restlen = 0;
		}

		/* Try reading the current filename as a symlink */
		if (checked[0] == '\0')
			continue;

		linklen = readlink(checked, link, sizeof(link) - 1);
		if (linklen == -1) {
			if (errno == EINVAL)
				continue;	/* not a symlink */

			if (errno == EACCES || errno == ENOENT || errno == ENOTDIR) {
				/* That last component was bogus.  Restore and return. */
				*trailer = r - (prevrestlen - restlen);
				if (prevcheckedlen == 0)
					strcpy(checked, ".");
				else
					checked[prevcheckedlen] = '\0';
				return checked;
			}

			syslog(LOG_ERR, "readlink %s: %s", checked, strerror(errno));
			return NULL;
		}

		++nlinks;
		if (nlinks > MAX_LINKS) {
			syslog(LOG_ERR, "too many symlinks in %s", path);
			return NULL;
		}

		link[linklen] = '\0';
		if (link[linklen - 1] == '/')
			link[--linklen] = '\0';	/* trim trailing slash */

		/* Insert the link contents in front of the rest of the filename. */
		if (restlen != 0) {
			memmove(rest, r, strlen(r) + 1);
			httpd_realloc_str(&rest, &maxrest, restlen + linklen + 1);
			for (i = restlen; i >= 0; --i)
				rest[i + linklen + 1] = rest[i];
			memmove(rest, link, strlen(link) + 1);
			rest[linklen] = '/';
			restlen += linklen + 1;
			r = rest;
		} else {
			/* There's nothing left in the filename, so the link contents
			** becomes the rest.
			*/
			httpd_realloc_str(&rest, &maxrest, linklen);
			memmove(rest, link, strlen(link) + 1);
			restlen = linklen;
			r = rest;
		}

		if (rest[0] == '/') {
			/* There must have been an absolute symlink - zero out checked. */
			checked[0] = '\0';
			checkedlen = 0;
		} else {
			/* Re-check this component. */
			checkedlen = prevcheckedlen;
			checked[checkedlen] = '\0';
		}
	}

	*trailer = r;
	if (checked[0] == '\0')
		strcpy(checked, ".");

	return checked;
}


void httpd_close_conn(struct httpd_conn *hc, struct timeval *now)
{
	if (hc->file_address) {
		mmc_unmap(hc->file_address, &(hc->sb), now);
		hc->file_address = NULL;
	}

	if (hc->conn_fd >= 0) {
		httpd_ssl_close(hc);
		hc->conn_fd = -1;
	}
}


void httpd_destroy_conn(struct httpd_conn *hc)
{
	if (hc->initialized) {
		free(hc->read_buf);
		free(hc->decodedurl);
		free(hc->origfilename);
		free(hc->indexname);
		free(hc->expnfilename);
		free(hc->encodings);
		free(hc->pathinfo);
		free(hc->query);
		free(hc->accept);
		free(hc->accepte);
		free(hc->reqhost);
		free(hc->hostdir);
		free(hc->remoteuser);
		free(hc->response);
#ifdef TILDE_MAP_2
		free(hc->altdir);
#endif
#ifdef ACCESS_FILE
		free(hc->accesspath);
#endif
#ifdef AUTH_FILE
		free(hc->authpath);
		free(hc->prevauthpath);
		free(hc->prevuser);
		free(hc->prevcryp);
#endif
		httpd_ssl_shutdown(hc);
		hc->initialized = 0;
	}
}

void httpd_init_conn_mem(struct httpd_conn *hc)
{
	if (hc->initialized)
		return;

	hc->read_size = 0;
	httpd_realloc_str(&hc->read_buf, &hc->read_size, 16384);
	hc->maxdecodedurl = hc->maxorigfilename =  hc->maxindexname =
		hc->maxexpnfilename = hc->maxencodings = hc->maxpathinfo = hc->maxquery = hc->maxaccept =
		hc->maxaccepte = hc->maxreqhost = hc->maxhostdir = hc->maxremoteuser = hc->maxresponse = 0;

	httpd_realloc_str(&hc->decodedurl, &hc->maxdecodedurl, 1);
	httpd_realloc_str(&hc->origfilename, &hc->maxorigfilename, 1);
	httpd_realloc_str(&hc->indexname,  &hc->maxindexname, 1);
	httpd_realloc_str(&hc->expnfilename, &hc->maxexpnfilename, 0);
	httpd_realloc_str(&hc->encodings, &hc->maxencodings, 1);
	httpd_realloc_str(&hc->pathinfo, &hc->maxpathinfo, 0);
	httpd_realloc_str(&hc->query, &hc->maxquery, 0);
	httpd_realloc_str(&hc->accept, &hc->maxaccept, 0);
	httpd_realloc_str(&hc->accepte, &hc->maxaccepte, 0);
	httpd_realloc_str(&hc->reqhost, &hc->maxreqhost, 0);
	httpd_realloc_str(&hc->hostdir, &hc->maxhostdir, 0);
	httpd_realloc_str(&hc->remoteuser, &hc->maxremoteuser, 0);
	httpd_realloc_str(&hc->response, &hc->maxresponse, 0);

#ifdef TILDE_MAP_2
	hc->maxaltdir = 0;
	httpd_realloc_str(&hc->altdir, &hc->maxaltdir, 0);
#endif

#ifdef ACCESS_FILE
	hc->maxaccesspath = 0;
	httpd_realloc_str(&hc->accesspath, &hc->maxaccesspath, 0);
#endif
#ifdef AUTH_FILE
	hc->maxauthpath = 0;
	hc->maxprevauthpath = 0;
	hc->maxprevuser = 0;
	hc->maxprevcryp = 0;
	httpd_realloc_str(&hc->authpath, &hc->maxauthpath, 0);
	httpd_realloc_str(&hc->prevauthpath, &hc->maxprevauthpath, 0);
	httpd_realloc_str(&hc->prevuser, &hc->maxprevuser, 0);
	httpd_realloc_str(&hc->prevcryp, &hc->maxprevcryp, 0);
#endif

	hc->initialized = 1;
}


void httpd_init_conn_content(struct httpd_conn *hc)
{
	hc->read_idx = 0;
	hc->checked_idx = 0;
	hc->checked_state = CHST_FIRSTWORD;
	hc->method = METHOD_UNKNOWN;
	hc->status = 0;
	hc->bytes_to_send = 0;
	hc->bytes_sent = 0;
	hc->encodedurl = "";
	hc->decodedurl[0] = '\0';
	hc->protocol = "UNKNOWN";
	hc->origfilename[0] = '\0';
	hc->expnfilename[0] = '\0';
	hc->encodings[0] = '\0';
	hc->pathinfo[0] = '\0';
	hc->query[0] = '\0';
	hc->referer = "";
	hc->useragent = "";
	hc->accept[0] = '\0';
	hc->accepte[0] = '\0';
	hc->acceptl = "";
	hc->cookie = "";
	hc->contenttype = "";
	hc->reqhost[0] = '\0';
	hc->hdrhost = "";
	hc->hostdir[0] = '\0';
	hc->authorization = "";
	hc->remoteuser[0] = '\0';
	hc->response[0] = '\0';
#ifdef TILDE_MAP_2
	hc->altdir[0] = '\0';
#endif
	hc->responselen = 0;
	hc->if_modified_since = (time_t)-1;
	hc->range_if = (time_t)-1;
	hc->contentlength = 0;
	hc->type = "";
	hc->hostname = NULL;
	hc->mime_flag = 1;
	hc->one_one = 0;
	hc->got_range = 0;
	hc->tildemapped = 0;
	hc->first_byte_index = 0;
	hc->last_byte_index = -1;
	hc->keep_alive = 0;
	hc->do_keep_alive = 0;
	hc->should_linger = 0;
	hc->file_address = NULL;
	hc->compression_type = COMPRESSION_NONE;
}


int httpd_get_conn(struct httpd_server *hs, int listen_fd, struct httpd_conn *hc)
{
	httpd_sockaddr sa;
	socklen_t sz;
	char *real_ip;

	httpd_init_conn_mem(hc);

	/* Accept the new connection. */
	sz = sizeof(sa);
	hc->conn_fd = accept(listen_fd, &sa.sa, &sz);
	if (hc->conn_fd < 0) {
		if (errno == EWOULDBLOCK)
			return GC_NO_MORE;

		syslog(LOG_ERR, "accept: %s", strerror(errno));
		return GC_FAIL;
	}

	if (!sockaddr_check(&sa)) {
		syslog(LOG_ERR, "unknown sockaddr family");
		close(hc->conn_fd);
		hc->conn_fd = -1;

		return GC_FAIL;
	}

	fcntl(hc->conn_fd, F_SETFD, 1);
	hc->hs = hs;
	memset(&hc->client_addr, 0, sizeof(hc->client_addr));
	memmove(&hc->client_addr, &sa, sockaddr_len(&sa));

	/*
	 * Slightly ugly workaround to handle X-Forwarded-For better for IPv6
	 * Idea from https://blog.steve.fi/IPv6_and_thttpd.html
	 */
	real_ip = httpd_ntoa(&hc->client_addr);
	memset(hc->client_addr.real_ip, 0, sizeof(hc->client_addr.real_ip));
	strncpy(hc->client_addr.real_ip, real_ip, sizeof(hc->client_addr.real_ip));

	if (httpd_ssl_open(hc)) {
		syslog(LOG_CRIT, "Failed creating new SSL connection");
		return GC_FAIL;
	}
	httpd_init_conn_content(hc);

	return GC_OK;
}


/* Checks hc->read_buf to see whether a complete request has been read so far;
** either the first line has two words (an HTTP/0.9 request), or the first
** line has three words and there's a blank line present.
**
** hc->read_idx is how much has been read in; hc->checked_idx is how much we
** have checked so far; and hc->checked_state is the current state of the
** finite state machine.
*/
int httpd_got_request(struct httpd_conn *hc)
{
	char c;

	for (; hc->checked_idx < hc->read_idx; ++hc->checked_idx) {
		c = hc->read_buf[hc->checked_idx];
		switch (hc->checked_state) {
		case CHST_FIRSTWORD:
			switch (c) {
			case ' ':
			case '\t':
				hc->checked_state = CHST_FIRSTWS;
				break;

			case '\n':
			case '\r':
				hc->checked_state = CHST_BOGUS;
				return GR_BAD_REQUEST;
			}
			break;

		case CHST_FIRSTWS:
			switch (c) {
			case ' ':
			case '\t':
				break;

			case '\n':
			case '\r':
				hc->checked_state = CHST_BOGUS;
				return GR_BAD_REQUEST;

			default:
				hc->checked_state = CHST_SECONDWORD;
				break;
			}
			break;

		case CHST_SECONDWORD:
			switch (c) {
			case ' ':
			case '\t':
				hc->checked_state = CHST_SECONDWS;
				break;

			case '\n':
			case '\r':
				/* The first line has only two words - an HTTP/0.9 request. */
				return GR_GOT_REQUEST;
			}
			break;

		case CHST_SECONDWS:
			switch (c) {
			case ' ':
			case '\t':
				break;

			case '\n':
			case '\r':
				hc->checked_state = CHST_BOGUS;
				return GR_BAD_REQUEST;

			default:
				hc->checked_state = CHST_THIRDWORD;
				break;
			}
			break;

		case CHST_THIRDWORD:
			switch (c) {
			case ' ':
			case '\t':
				hc->checked_state = CHST_THIRDWS;
				break;

			case '\n':
				hc->checked_state = CHST_LF;
				break;

			case '\r':
				hc->checked_state = CHST_CR;
				break;
			}
			break;

		case CHST_THIRDWS:
			switch (c) {
			case ' ':
			case '\t':
				break;

			case '\n':
				hc->checked_state = CHST_LF;
				break;

			case '\r':
				hc->checked_state = CHST_CR;
				break;

			default:
				hc->checked_state = CHST_BOGUS;
				return GR_BAD_REQUEST;
			}
			break;

		case CHST_LINE:
			switch (c) {
			case '\n':
				hc->checked_state = CHST_LF;
				break;

			case '\r':
				hc->checked_state = CHST_CR;
				break;
			}
			break;

		case CHST_LF:
			switch (c) {
			case '\n':
				/* Two newlines in a row - a blank line - end of request. */
				return GR_GOT_REQUEST;

			case '\r':
				hc->checked_state = CHST_CR;
				break;

			default:
				hc->checked_state = CHST_LINE;
				break;
			}
			break;

		case CHST_CR:
			switch (c) {
			case '\n':
				hc->checked_state = CHST_CRLF;
				break;

			case '\r':
				/* Two returns in a row - end of request. */
				return GR_GOT_REQUEST;

			default:
				hc->checked_state = CHST_LINE;
				break;
			}
			break;

		case CHST_CRLF:
			switch (c) {
			case '\n':
				/* Two newlines in a row - end of request. */
				return GR_GOT_REQUEST;

			case '\r':
				hc->checked_state = CHST_CRLFCR;
				break;

			default:
				hc->checked_state = CHST_LINE;
				break;
			}
			break;

		case CHST_CRLFCR:
			switch (c) {
			case '\n':
			case '\r':
				/* Two CRLFs or two CRs in a row - end of request. */
				return GR_GOT_REQUEST;

			default:
				hc->checked_state = CHST_LINE;
				break;
			}
			break;

		case CHST_BOGUS:
			return GR_BAD_REQUEST;
		}
	}

	return GR_NO_REQUEST;
}


int httpd_parse_request(struct httpd_conn *hc)
{
	char *buf;
	char *method_str;
	char *url;
	char *protocol;
	char *reqhost;
	char *eol;
	char *cp;
	char *pi;

	hc->checked_idx = 0;	/* reset */
	method_str = bufgets(hc);
	url = strpbrk(method_str, " \t\n\r");
	if (!url) {
		httpd_send_err(hc, 400, httpd_err400title, "", httpd_err400form, "1");
		return -1;
	}

	*url++ = '\0';
	url += strspn(url, " \t\n\r");

	protocol = strpbrk(url, " \t\n\r");
	if (!protocol) {
		protocol = "HTTP/0.9";
		hc->mime_flag = 0;
	} else {
		*protocol++ = '\0';
		protocol += strspn(protocol, " \t\n\r");
		if (*protocol != '\0') {
			eol = strpbrk(protocol, " \t\n\r");
			if (eol)
				*eol = '\0';

			if (strcasecmp(protocol, "HTTP/1.0") != 0)
				hc->one_one = 1;
		}
	}
	hc->protocol = protocol;

	/* Check for HTTP/1.1 absolute URL. */
	if (strncasecmp(url, "http://", 7) == 0) {
		if (!hc->one_one) {
			httpd_send_err(hc, 400, httpd_err400title, "", httpd_err400form, "2");
			return -1;
		}

		reqhost = url + 7;
		url = strchr(reqhost, '/');
		if (!url) {
			httpd_send_err(hc, 400, httpd_err400title, "", httpd_err400form, "3");
			return -1;
		}
		*url = '\0';

		if (strchr(reqhost, '/') || reqhost[0] == '.') {
			httpd_send_err(hc, 400, httpd_err400title, "", httpd_err400form, "4");
			return -1;
		}

		httpd_realloc_str(&hc->reqhost, &hc->maxreqhost, strlen(reqhost));
		strcpy(hc->reqhost, reqhost);
		*url = '/';
	}

	if (*url != '/') {
		httpd_send_err(hc, 400, httpd_err400title, "", httpd_err400form, "5");
		return -1;
	}

	if (strcasecmp(method_str, httpd_method_str(METHOD_GET)) == 0)
		hc->method = METHOD_GET;
	else if (strcasecmp(method_str, httpd_method_str(METHOD_HEAD)) == 0)
		hc->method = METHOD_HEAD;
	else if (strcasecmp(method_str, httpd_method_str(METHOD_POST)) == 0)
		hc->method = METHOD_POST;
	else if (strcasecmp(method_str, httpd_method_str(METHOD_PUT)) == 0)
		hc->method = METHOD_PUT;
	else if (strcasecmp(method_str, httpd_method_str(METHOD_DELETE)) == 0)
		hc->method = METHOD_DELETE;
	else if (strcasecmp(method_str, httpd_method_str(METHOD_CONNECT)) == 0)
		hc->method = METHOD_CONNECT;
	else if (strcasecmp(method_str, httpd_method_str(METHOD_OPTIONS)) == 0)
		hc->method = METHOD_OPTIONS;
	else if (strcasecmp(method_str, httpd_method_str(METHOD_TRACE)) == 0)
		hc->method = METHOD_TRACE;
	else {
		httpd_send_err(hc, 501, err501title, "", err501form, method_str);
		return -1;
	}

	hc->encodedurl = url;
	httpd_realloc_str(&hc->decodedurl, &hc->maxdecodedurl, strlen(hc->encodedurl));
	strdecode(hc->decodedurl, hc->encodedurl);

	httpd_realloc_str(&hc->origfilename, &hc->maxorigfilename, strlen(hc->decodedurl));
	strcpy(hc->origfilename, &hc->decodedurl[1]);
	/* Special case for top-level URL. */
	if (hc->origfilename[0] == '\0')
		strcpy(hc->origfilename, ".");

	/* Extract query string from encoded URL. */
	cp = strchr(hc->encodedurl, '?');
	if (cp) {
		++cp;
		httpd_realloc_str(&hc->query, &hc->maxquery, strlen(cp));
		strcpy(hc->query, cp);
		/* Remove query from (decoded) origfilename. */
		cp = strchr(hc->origfilename, '?');
		if (cp)
			*cp = '\0';
	}

	de_dotdot(hc->origfilename);
	if (hc->origfilename[0] == '/' ||
	    (hc->origfilename[0] == '.' && hc->origfilename[1] == '.' &&
	     (hc->origfilename[2] == '\0' || hc->origfilename[2] == '/'))) {
		httpd_send_err(hc, 400, httpd_err400title, "", httpd_err400form, "6");
		return -1;
	}

	if (hc->mime_flag) {
		/* Read the MIME headers. */
		while ((buf = bufgets(hc))) {
			if (buf[0] == '\0')
				break;

			if (strncasecmp(buf, "Referer:", 8) == 0) {
				cp = &buf[8];
				cp += strspn(cp, " \t");
				hc->referer = cp;
			} else if (strncasecmp(buf, "User-Agent:", 11) == 0) {
				cp = &buf[11];
				cp += strspn(cp, " \t");
				hc->useragent = cp;
			} else if (strncasecmp(buf, "Host:", 5) == 0) {
				cp = &buf[5];
				cp += strspn(cp, " \t");
				hc->hdrhost = cp;
				if (strchr(hc->hdrhost, '/') || hc->hdrhost[0] == '.') {
					httpd_send_err(hc, 400, httpd_err400title, "", httpd_err400form, "7");
					return -1;
				}
			} else if (strncasecmp(buf, "Accept:", 7) == 0) {
				cp = &buf[7];
				cp += strspn(cp, " \t");
				if (hc->accept[0] != '\0') {
					if (strlen(hc->accept) > 5000) {
						syslog(LOG_ERR, "%s way too much Accept: data", httpd_client(hc));
						continue;
					}
					httpd_realloc_str(&hc->accept, &hc->maxaccept, strlen(hc->accept) + 2 + strlen(cp));
					strcat(hc->accept, ", ");
				} else
					httpd_realloc_str(&hc->accept, &hc->maxaccept, strlen(cp));
				strcat(hc->accept, cp);
			} else if (strncasecmp(buf, "Accept-Encoding:", 16) == 0) {
				cp = &buf[16];
				cp += strspn(cp, " \t");
				if (hc->accepte[0] != '\0') {
					if (strlen(hc->accepte) > 5000) {
						syslog(LOG_ERR, "%s way too much Accept-Encoding: data", httpd_client(hc));
						continue;
					}
					httpd_realloc_str(&hc->accepte, &hc->maxaccepte, strlen(hc->accepte) + 2 + strlen(cp));
					strcat(hc->accepte, ", ");
				} else {
					httpd_realloc_str(&hc->accepte, &hc->maxaccepte, strlen(cp));
				}
				strcpy(hc->accepte, cp);
			} else if (strncasecmp(buf, "Accept-Language:", 16) == 0) {
				cp = &buf[16];
				cp += strspn(cp, " \t");
				hc->acceptl = cp;
			} else if (strncasecmp(buf, "If-Modified-Since:", 18) == 0) {
				cp = &buf[18];
				hc->if_modified_since = tdate_parse(cp);
				if (hc->if_modified_since == (time_t)-1)
					syslog(LOG_DEBUG, "unparsable time: %s", cp);
			} else if (strncasecmp(buf, "Cookie:", 7) == 0) {
				cp = &buf[7];
				cp += strspn(cp, " \t");
				hc->cookie = cp;
			} else if (strncasecmp(buf, "Range:", 6) == 0) {
				/* Only support %d- and %d-%d, not %d-%d,%d-%d or -%d. */
				if (!strchr(buf, ',')) {
					char *cp_dash;

					cp = strpbrk(buf, "=");
					if (cp) {
						cp_dash = strchr(cp + 1, '-');
						if (cp_dash && cp_dash != cp + 1) {
							*cp_dash = '\0';
							hc->got_range = 1;
							hc->first_byte_index = atoll(cp + 1);
							if (hc->first_byte_index < 0)
								hc->first_byte_index = 0;
							if (isdigit((int)cp_dash[1])) {
								hc->last_byte_index = atoll(cp_dash + 1);
								if (hc->last_byte_index < 0)
									hc->last_byte_index = -1;
							}
						}
					}
				}
			} else if (strncasecmp(buf, "Range-If:", 9) == 0 || strncasecmp(buf, "If-Range:", 9) == 0) {
				cp = &buf[9];
				hc->range_if = tdate_parse(cp);
				if (hc->range_if == (time_t)-1)
					syslog(LOG_DEBUG, "unparsable time: %s", cp);
			} else if (strncasecmp(buf, "Content-Type:", 13) == 0) {
				cp = &buf[13];
				cp += strspn(cp, " \t");
				hc->contenttype = cp;
			} else if (strncasecmp(buf, "Content-Length:", 15) == 0) {
				cp = &buf[15];
				hc->contentlength = (size_t)atol(cp);
			} else if (strncasecmp(buf, "Authorization:", 14) == 0) {
				cp = &buf[14];
				cp += strspn(cp, " \t");
				hc->authorization = cp;
			} else if (strncasecmp(buf, "Connection:", 11) == 0) {
				cp = &buf[11];
				cp += strspn(cp, " \t");
				if (strcasecmp(cp, "keep-alive") == 0) {
					hc->keep_alive = 1;     /* Client signaling */
					hc->do_keep_alive = 10; /* Our intention, which might change later */
				}
			} else if (strncasecmp(buf, "X-Forwarded-For:", 16) == 0) {
				int i;

				/* Syntax: X-Forwarded-For: client[, proxy1, proxy2, ...] */
				cp = &buf[16];
				cp += strspn(cp, " \t");
				for (i = 0; cp[i]; i++) {
					hc->client_addr.real_ip[i] = cp[i];
					if (isblank(cp[i]))
						break;
				}
				hc->client_addr.real_ip[i] = 0;
			}
			/*
			 * Possibly add support for X-Real-IP: here?
			 * http://distinctplace.com/infrastructure/2014/04/23/story-behind-x-forwarded-for-and-x-real-ip-headers/
			 */
#ifdef LOG_UNKNOWN_HEADERS
			else if (strncasecmp(buf, "Accept-Charset:", 15)   == 0 ||
				 strncasecmp(buf, "Accept-Language:", 16)  == 0 ||
				 strncasecmp(buf, "Agent:", 6)             == 0 ||
				 strncasecmp(buf, "Cache-Control:", 14)    == 0 ||
				 strncasecmp(buf, "Cache-Info:", 11)       == 0 ||
				 strncasecmp(buf, "Charge-To:", 10)        == 0 ||
				 strncasecmp(buf, "Client-IP:", 10)        == 0 ||
				 strncasecmp(buf, "Date:", 5)              == 0 ||
				 strncasecmp(buf, "Extension:", 10)        == 0 ||
				 strncasecmp(buf, "Forwarded:", 10)        == 0 ||
				 strncasecmp(buf, "From:", 5)              == 0 ||
				 strncasecmp(buf, "HTTP-Version:", 13)     == 0 ||
				 strncasecmp(buf, "Max-Forwards:", 13)     == 0 ||
				 strncasecmp(buf, "Message-Id:", 11)       == 0 ||
				 strncasecmp(buf, "MIME-Version:", 13)     == 0 ||
				 strncasecmp(buf, "Negotiate:", 10)        == 0 ||
				 strncasecmp(buf, "Pragma:", 7)            == 0 ||
				 strncasecmp(buf, "Proxy-Agent:", 12)      == 0 ||
				 strncasecmp(buf, "Proxy-Connection:", 17) == 0 ||
				 strncasecmp(buf, "Security-Scheme:", 16)  == 0 ||
				 strncasecmp(buf, "Session-Id:", 11)       == 0 ||
				 strncasecmp(buf, "UA-Color:", 9)          == 0 ||
				 strncasecmp(buf, "UA-CPU:", 7)            == 0 ||
				 strncasecmp(buf, "UA-Disp:", 8)           == 0 ||
				 strncasecmp(buf, "UA-OS:", 6)             == 0 ||
				 strncasecmp(buf, "UA-Pixels:", 10)        == 0 ||
				 strncasecmp(buf, "User:", 5)              == 0 ||
				 strncasecmp(buf, "Via:", 4)               == 0 ||
				 strncasecmp(buf, "X-", 2)                 == 0) {
				; /* ignore */
			} else {
				syslog(LOG_DEBUG, "unknown request header: %s", buf);
			}
#endif /* LOG_UNKNOWN_HEADERS */
		}
	}

	if (hc->one_one) {
		/* Check that HTTP/1.1 requests specify a host, as required. */
		if (hc->reqhost[0] == '\0' && hc->hdrhost[0] == '\0') {
			httpd_send_err(hc, 400, httpd_err400title, "", httpd_err400form, "8");
			return -1;
		}

		/* If the client wants to do keep-alives, it might also be doing
		** pipelining.  There's no way for us to tell.  Since we don't
		** implement keep-alives yet, if we close such a connection there
		** might be unread pipelined requests waiting.  So, we have to
		** do a lingering close.
		*/
		if (hc->keep_alive)
			hc->should_linger = 1;
	}

	/* Look for a gzip accept-encoding */
	if (hc->accepte[0] != '\0') {
		char *gz;

		gz = strstr(hc->accepte, "gzip");
		if (gz) {
			char *c, *q;
			float qval = 0.0f;

			c = strstr(gz, ",");
			q = strstr(gz, "q=");
			if (q)
				qval = strtof(q + 2, 0);

			if (!q || c < q || ((!c || q < c) && qval > 0.0f))
				hc->compression_type = COMPRESSION_GZIP;
		}
	}

	/*
	**  Disable keep alive support for bad browsers,
	**    list taken from Apache 1.3.19
	*/
	if (hc->do_keep_alive &&
	    (strstr(hc->useragent, "Mozilla/2")  ||
	     strstr(hc->useragent, "MSIE 4.0b2;")))
		hc->do_keep_alive = 0;

	/* Ok, the request has been parsed.  Now we resolve stuff that
	** may require the entire request.
	*/

	/* Copy original filename to expanded filename. */
	httpd_realloc_str(&hc->expnfilename, &hc->maxexpnfilename, strlen(hc->origfilename));
	strcpy(hc->expnfilename, hc->origfilename);

	/* Tilde mapping. */
	if (hc->expnfilename[0] == '~') {
#ifdef TILDE_MAP_1
		if (!tilde_map_1(hc)) {
			httpd_send_err(hc, 404, err404title, "", err404form, hc->encodedurl);
			return -1;
		}
#endif
#ifdef TILDE_MAP_2
		if (!tilde_map_2(hc)) {
			httpd_send_err(hc, 404, err404title, "", err404form, hc->encodedurl);
			return -1;
		}
#endif
	}

	/* Virtual host mapping. */
	if (hc->hs->vhost) {
		if (!vhost_map(hc)) {
			httpd_send_err(hc, 500, err500title, "", err500form, hc->encodedurl);
			return -1;
		}
	}

	/* Expand all symbolic links in the filename.  This also gives us
	** any trailing non-existing components, for pathinfo.
	*/
	cp = expand_symlinks(hc->expnfilename, &pi, hc->hs->no_symlink_check, hc->tildemapped);
	if (!cp) {
		httpd_send_err(hc, 500, err500title, "", err500form, hc->encodedurl);
		return -1;
	}

	/* Fall back to shared (restricted) top-level directory for missing files */
	if (hc->hs->vhost && is_vhost_shared(pi)) {
		httpd_realloc_str(&hc->expnfilename, &hc->maxexpnfilename, strlen(pi));
		strcpy(hc->expnfilename, pi);
		httpd_realloc_str(&hc->pathinfo, &hc->maxpathinfo, 1);
		strcpy(hc->pathinfo, "");
	} else {
		httpd_realloc_str(&hc->expnfilename, &hc->maxexpnfilename, strlen(cp));
		strcpy(hc->expnfilename, cp);
		httpd_realloc_str(&hc->pathinfo, &hc->maxpathinfo, strlen(pi));
		strcpy(hc->pathinfo, pi);
	}

	/* Remove pathinfo stuff from the original filename too. */
	if (hc->pathinfo[0] != '\0') {
		int i;

		i = strlen(hc->origfilename) - strlen(hc->pathinfo);
		if (strcmp(&hc->origfilename[i], hc->pathinfo) == 0) {
			if (i == 0)
				hc->origfilename[0] = '\0';
			else
				hc->origfilename[i - 1] = '\0';
		}
	}

	/* If the expanded filename is an absolute path, check that it's still
	** within the current directory or the alternate directory.
	*/
	if (hc->expnfilename[0] == '/') {
		if (strncmp(hc->expnfilename, hc->hs->cwd, strlen(hc->hs->cwd)) == 0) {
			/* Elide the current directory. */
			memmove(hc->expnfilename, &hc->expnfilename[strlen(hc->hs->cwd)],
				      strlen(hc->expnfilename) - strlen(hc->hs->cwd) + 1);
		}
#ifdef TILDE_MAP_2
		else if (hc->altdir[0] != '\0' &&
			 (strncmp(hc->expnfilename, hc->altdir,
				  strlen(hc->altdir)) == 0 &&
			  (hc->expnfilename[strlen(hc->altdir)] == '\0' || hc->expnfilename[strlen(hc->altdir)] == '/'))) {
		}
#endif
		else if (hc->hs->no_symlink_check) {
			httpd_send_err(hc, 404, err404title, "", err404form, hc->encodedurl);
			return -1;
		} else {
			syslog(LOG_NOTICE, "%s URL \"%s\" goes outside the web tree", httpd_client(hc), hc->encodedurl);
			httpd_send_err(hc, 403, err403title, "",
				       ERROR_FORM(err403form,
						  "The requested URL '%s' resolves to a file outside the permitted web server directory tree.\n"),
				       hc->encodedurl);
			return -1;
		}
	}

	return 0;
}


static char *bufgets(struct httpd_conn *hc)
{
	int i;
	char c;

	for (i = hc->checked_idx; hc->checked_idx < hc->read_idx; ++hc->checked_idx) {
		c = hc->read_buf[hc->checked_idx];
		if (c == '\n' || c == '\r') {
			hc->read_buf[hc->checked_idx] = '\0';
			++hc->checked_idx;
			if (c == '\r' && hc->checked_idx < hc->read_idx && hc->read_buf[hc->checked_idx] == '\n') {
				hc->read_buf[hc->checked_idx] = '\0';
				++hc->checked_idx;
			}

			return &(hc->read_buf[i]);
		}
	}

	return NULL;
}


static void de_dotdot(char *file)
{
	char *cp;
	char *cp2;
	int l;

	/* Collapse any multiple / sequences. */
	while ((cp = strstr(file, "//"))) {
		for (cp2 = cp + 2; *cp2 == '/'; ++cp2)
			continue;

		memmove(cp + 1, cp2, strlen(cp2) + 1);
	}

	/* Collapse leading // (first one is lost prior to this fn) */
	if (file[0] == '/')
		memmove(file, &file[1], strlen(file));

	/* Remove leading ./ and any /./ sequences. */
	while (strncmp(file, "./", 2) == 0)
		memmove(file, file + 2, strlen(file) - 1);
	while ((cp = strstr(file, "/./")))
		memmove(cp, cp + 2, strlen(cp) - 1);

	/* Alternate between removing leading ../ and removing xxx/../ */
	for (;;) {
		while (strncmp(file, "../", 3) == 0)
			memmove(file, file + 3, strlen(file) - 2);
		cp = strstr(file, "/../");
		if (!cp)
			break;

		for (cp2 = cp - 1; cp2 >= file && *cp2 != '/'; --cp2)
			continue;

		memmove(cp2 + 1, cp + 4, strlen(cp + 3));
	}

	/* Also elide any xxx/.. at the end. */
	while ((l = strlen(file)) > 3 && strcmp((cp = file + l - 3), "/..") == 0) {
		for (cp2 = cp - 1; cp2 >= file && *cp2 != '/'; --cp2)
			continue;
		if (cp2 < file)
			break;
		*cp2 = '\0';
	}
}


struct mime_entry {
	char *ext;
	size_t ext_len;
	char *val;
	size_t val_len;
};

static struct mime_entry enc_tab[] = {
#include "mime_encodings.h"
};

static const int n_enc_tab = sizeof(enc_tab) / sizeof(*enc_tab);

static struct mime_entry typ_tab[] = {
#include "mime_types.h"
};

static const int n_typ_tab = sizeof(typ_tab) / sizeof(*typ_tab);


/* qsort comparison routine - declared old-style on purpose, for portability. */
static int ext_compare(a, b)
struct mime_entry *a;
struct mime_entry *b;
{
	return strcmp(a->ext, b->ext);
}


static void init_mime(void)
{
	int i;

	/* Sort the tables so we can do binary search. */
	qsort(enc_tab, n_enc_tab, sizeof(*enc_tab), ext_compare);
	qsort(typ_tab, n_typ_tab, sizeof(*typ_tab), ext_compare);

	/* Fill in the lengths. */
	for (i = 0; i < n_enc_tab; ++i) {
		enc_tab[i].ext_len = strlen(enc_tab[i].ext);
		enc_tab[i].val_len = strlen(enc_tab[i].val);
	}
	for (i = 0; i < n_typ_tab; ++i) {
		typ_tab[i].ext_len = strlen(typ_tab[i].ext);
		typ_tab[i].val_len = strlen(typ_tab[i].val);
	}
}


/* Figure out MIME encodings and type based on the filename.  Multiple
** encodings are separated by commas, and are listed in the order in
** which they were applied to the file.
*/
static void figure_mime(struct httpd_conn *hc)
{
	char *prev_dot;
	char *dot;
	char *ext;
	int me_indexes[100];
	size_t ext_len, encodings_len, n_me_indexes;
	int i, top, bot, mid;
	int r;
	const char *default_type = "text/plain; charset=%s";

	/* Peel off encoding extensions until there aren't any more. */
	n_me_indexes = 0;
	hc->type = default_type;
	for (prev_dot = &hc->expnfilename[strlen(hc->expnfilename)];; prev_dot = dot) {
		for (dot = prev_dot - 1; dot >= hc->expnfilename && *dot != '.'; --dot)
			;
		if (dot < hc->expnfilename) {
			/* No dot found.  No more extensions.  */
			goto done;
		}
		ext = dot + 1;
		ext_len = prev_dot - ext;
		/* Search the encodings table.  Linear search is fine here, there
		** are only a few entries.
		*/
		for (i = 0; i < n_enc_tab; ++i) {
			if (ext_len == enc_tab[i].ext_len && strncasecmp(ext, enc_tab[i].ext, ext_len) == 0) {
				if (n_me_indexes < sizeof(me_indexes) / sizeof(*me_indexes)) {
					me_indexes[n_me_indexes] = i;
					++n_me_indexes;
				}
				break;
			}
		}
		/* Binary search for a matching type extension. */
		top = n_typ_tab - 1;
		bot = 0;
		while (top >= bot) {
			mid = (top + bot) / 2;
			r = strncasecmp(ext, typ_tab[mid].ext, ext_len);
			if (r < 0)
				top = mid - 1;
			else if (r > 0)
				bot = mid + 1;
			else if (ext_len < typ_tab[mid].ext_len)
				top = mid - 1;
			else if (ext_len > typ_tab[mid].ext_len)
				bot = mid + 1;
			else {
				hc->type = typ_tab[mid].val;
				goto done;
			}
		}
	}

done:

	/* The last thing we do is actually generate the mime-encoding header. */
	hc->encodings[0] = '\0';
	encodings_len = 0;
	for (i = n_me_indexes - 1; i >= 0; --i) {
		httpd_realloc_str(&hc->encodings, &hc->maxencodings, encodings_len + enc_tab[me_indexes[i]].val_len + 1);
		if (hc->encodings[0] != '\0') {
			strcpy(&hc->encodings[encodings_len], ",");
			++encodings_len;
		}
		strcpy(&hc->encodings[encodings_len], enc_tab[me_indexes[i]].val);
		encodings_len += enc_tab[me_indexes[i]].val_len;
	}

}


#ifdef CGI_TIMELIMIT
static void cgi_kill2(arg_t arg, struct timeval *now)
{
	pid_t pid;

	pid = (pid_t)arg.i;
	if (kill(pid, SIGKILL) == 0)
		syslog(LOG_ERR, "hard-killed CGI process %d", pid);
}

static void cgi_kill(arg_t arg, struct timeval *now)
{
	pid_t pid;

	pid = (pid_t)arg.i;
	if (kill(pid, SIGINT) == 0) {
		syslog(LOG_ERR, "killed CGI process %d", pid);
		/* In case this isn't enough, schedule an uncatchable kill. */
		if (!tmr_create(now, cgi_kill2, arg, 5 * 1000L, 0)) {
			syslog(LOG_CRIT, "tmr_create(cgi_kill2) failed");
			exit(1);
		}
	}
}
#endif /* CGI_TIMELIMIT */


#ifdef GENERATE_INDEXES

/* Convert byte size to kiB, MiB, GiB */
static char *humane_size(struct stat *st)
{
	size_t i = 0;
	off_t bytes;
	char *mult[] = { "", "k", "M", "G", "T", "P" };
	static char str[42];

	if (S_ISDIR(st->st_mode)) {
		snprintf(str, sizeof(str), "  - ");
		return str;
	}

	bytes = st->st_size;
	while (bytes > 1000 && i < NELEMS(mult)) {
		bytes /= 1000;
		i++;
	}

	snprintf(str, sizeof(str), "  %ld%s", (long int)bytes, mult[i]);

	return str;
}


static int is_reserved_htfile(const char *fn)
{
	int i;
	const char *res[] = {
#ifdef AUTH_FILE
		AUTH_FILE,
#else
		".htpasswd",
#endif
#ifdef ACCESS_FILE
		ACCESS_FILE,
#else
		".htaccess",
#endif
		NULL
	};

	for (i = 0; res[i]; i++) {
		if (!strcmp(res[i], fn))
			return 1;
	}

	return 0;
}

/* qsort comparison routine - declared old-style on purpose, for portability. */
static int name_compare(a, b)
char **a;
char **b;
{
	return strcmp(*a, *b);
}

static int child_ls_read_names(struct httpd_conn *hc, DIR *dirp, FILE *fp, int onlydir)
{
	int i, namlen, nnames = 0;
	static int maxnames = 0;
	struct dirent *de;
	static char *names;
	static char **nameptrs;
	static char *name;
	static size_t maxname = 0;
	static char *rname;
	static size_t maxrname = 0;
	static char *encrname;
	static size_t maxencrname = 0;

	while ((de = readdir(dirp))) {
		char *path;

		if (!strcmp(".", de->d_name))
			continue;
		if (!strcmp("..", de->d_name))
			continue;

		path = realpath(de->d_name, NULL);
		if (!path) {
			struct stat st;

			httpd_realloc_str(&name, &maxname, strlen(hc->expnfilename) + 1 + strlen(de->d_name));
			snprintf(name, maxname, "%s/%s", hc->expnfilename, de->d_name);

			if (stat(name, &st))
				continue;
			if (!(st.st_mode & (S_IROTH | S_IXOTH)))
				continue;

		fallback:
			if (onlydir && de->d_type != DT_DIR)
				continue;
			if (!onlydir && de->d_type == DT_DIR)
				continue;
		} else {
			struct stat st;

			if (stat(path, &st)) {
				free(path);
				goto fallback;
			}

			if (!(st.st_mode & (S_IROTH | S_IXOTH)))
				continue;

			free(path);
			if (onlydir && !S_ISDIR(st.st_mode))
				continue;
			if (!onlydir && S_ISDIR(st.st_mode))
				continue;
		}
			
		if (nnames >= maxnames) {
			if (maxnames == 0) {
				maxnames = 100;
				names = NEW(char, maxnames * (MAXPATHLEN + 1));
				nameptrs = NEW(char *, maxnames);
			} else {
				maxnames *= 2;
				names = RENEW(names, char, maxnames * (MAXPATHLEN + 1));
				nameptrs = RENEW(nameptrs, char *, maxnames);
			}

			if (!names || !nameptrs) {
				syslog(LOG_ERR, "out of memory reallocating directory names");
				return 1;
			}

			for (i = 0; i < maxnames; ++i)
				nameptrs[i] = &names[i * (MAXPATHLEN + 1)];
		}
		namlen = NAMLEN(de);
		strncpy(nameptrs[nnames], de->d_name, namlen);
		nameptrs[nnames][namlen] = '\0';
		++nnames;
	}

	/* Sort the names. */
	qsort(nameptrs, nnames, sizeof(*nameptrs), name_compare);

	/* Generate output. */
	for (i = 0; i < nnames; ++i) {
		struct stat sb;
		struct stat lsb;
		char buf[256];
		char timestr[42];
		char *icon, *alt;

		if (!strcmp(nameptrs[i], "."))
			continue;
		if (!strcmp(nameptrs[i], "..")) {
			if (!strcmp(hc->encodedurl, "/"))
				continue;

			fprintf(fp,
				" <tr>\n"
				"  <td class=\"icon\"><img src=\"/icons/back.gif\" alt=\"&#8617;\" width=\"20\" height=\"22\"></td>\n"
				"  <td><a href=\"..\">Parent Directory</a></td>\n"
				"  <td class=\"right\">&nbsp;</td>\n"
				"  <td>&nbsp;</td>\n"
				" </tr>\n");
			continue;
		}

		/* Skip listing dotfiles unless enabled in .conf file */
		if (!hc->hs->list_dotfiles && nameptrs[i][0] == '.' && strlen(nameptrs[i]) > 2)
			continue;

		/* Do not show .htpasswd and .htaccess files */
		if (is_reserved_htfile(nameptrs[i]))
			continue;

		httpd_realloc_str(&name, &maxname, strlen(hc->expnfilename) + 1 + strlen(nameptrs[i]));
		httpd_realloc_str(&rname, &maxrname, strlen(hc->origfilename) + 1 + strlen(nameptrs[i]));
		if (hc->expnfilename[0] == '\0' || strcmp(hc->expnfilename, ".") == 0) {
			strcpy(name, nameptrs[i]);
			strcpy(rname, nameptrs[i]);
		} else {
			snprintf(name, maxname, "%s/%s", hc->expnfilename, nameptrs[i]);
			if (strcmp(hc->origfilename, ".") == 0)
				snprintf(rname, maxrname, "%s", nameptrs[i]);
			else
				snprintf(rname, maxrname, "%s%s", hc->origfilename, nameptrs[i]);
		}
		httpd_realloc_str(&encrname, &maxencrname, 3 * strlen(rname) + 1);
		strencode(encrname, maxencrname, rname);

		if (stat(name, &sb) < 0 || lstat(name, &lsb) < 0)
			continue;

		/* Get time string. */
		strftime(timestr, sizeof(timestr), "%F&nbsp;&nbsp;%R", localtime(&lsb.st_mtime));

		/* The ls -F file class. */
		switch (sb.st_mode & S_IFMT) {
		case S_IFDIR:
			icon = "/icons/folder.gif";
			alt  = "&#128193;";
			break;

		default:
			icon = "/icons/generic.gif";
			alt  = "&#128196;";
			break;
		}

		defang(nameptrs[i], buf, sizeof(buf));
		fprintf(fp,
			" <tr>\n"
			"  <td class=\"icon\"><img src=\"%s\" alt=\"%s\" width=\"20\" height=\"22\"></td>\n"
			"  <td><a href=\"/%s%s\">%s</a></td>\n"
			"  <td class=\"right\">%s</td>\n"
			"  <td>%s</td>\n"
			" </tr>\n", icon, alt,
			encrname, S_ISDIR(sb.st_mode) ? "/" : "", buf,
			humane_size(&lsb), timestr);
	}

	return 0;
}

/* Forked child process from ls() */
static int child_ls(struct httpd_conn *hc, DIR *dirp)
{
	FILE *fp;
	long len;
	char *buf;

	fp = tmpfile();
	if (!fp) {
		syslog(LOG_ERR, "tmpfile: %s", strerror(errno));
error:
		httpd_send_err(hc, 500, err500title, "", err500form, hc->encodedurl);
		httpd_send_response(hc);
		return 1;
	}

	fprintf(fp, "<!DOCTYPE html>\n"
		"<html>\n"
		" <head>\n"
		"  <title>Index of http://%s%s</title>\n"
		"  <link rel=\"icon\" type=\"image/x-icon\" href=\"/icons/favicon.ico\">\n"
		"  <script type=\"text/javascript\">window.onload = function() { document.getElementById('table').focus();} </script>\n"
		"%s"
		" </head>\n"
		" <body>\n"
		"<div id=\"wrapper\" tabindex=\"-1\">\n"
		"<h2>Index of http://%s%s</h2>\n"
		"<input type=\"hidden\" autofocus />\n"
		"<div id=\"table\">"
		"<table width=\"100%%\">\n"
		" <tr>"
		"  <th class=\"icon\" style=\"width:20px;\"><img src=\"/icons/blank.gif\" alt=\"&#8195;\" width=\"20\" height=\"22\"></th>\n"
		"  <th style=\"width:35em;\">Name</th>\n"
		"  <th class=\"right\" style=\"width: 3em;\">Size</th>\n"
		"  <th style=\"width: 7em;\">Last modified</th>\n"
		" </tr>\n",
		get_hostname(hc), hc->encodedurl,
		httpd_css_default(),
		get_hostname(hc), hc->encodedurl);

	/* Read in names. */
	child_ls_read_names(hc, dirp, fp, 1);
	rewinddir(dirp);
	child_ls_read_names(hc, dirp, fp, 0);

	fprintf(fp, " </table></div>\n");
	fprintf(fp, " <address>%s httpd at %s port %d</address>\n", EXPOSED_SERVER_SOFTWARE, get_hostname(hc), (int)hc->hs->port);
	fprintf(fp, "</div></body>\n</html>\n");

	len = ftell(fp);
	if (len == -1) {
		syslog(LOG_ERR, "ftell: %s", strerror(errno));
		fclose(fp);
		goto error;
	}

	buf = malloc((size_t)len);
	if (!buf) {
		fclose(fp);
		goto error;
	}

	send_mime(hc, 200, ok200title, "", "", "text/html; charset=%s", (off_t) - 1, hc->sb.st_mtime);
	httpd_send_response(hc);

	rewind(fp);
	fread(buf, (size_t)len, 1, fp);
	if (httpd_write(hc, buf, (size_t)len) <= 0)
		syslog(LOG_ERR, "Failed sending dirlisting to client: %s", strerror(errno));

	free(buf);
	fclose(fp);

	return 0;
}

static int ls(struct httpd_conn *hc)
{
	int r;
	DIR *dirp;
	arg_t arg;

	hc->compression_type = COMPRESSION_NONE;

	dirp = opendir(hc->expnfilename);
	if (!dirp) {
		syslog(LOG_ERR, "opendir %s: %s", hc->expnfilename, strerror(errno));
		httpd_send_err(hc, 404, err404title, "", err404form, hc->encodedurl);
		return -1;
	}

	if (hc->method == METHOD_HEAD) {
		closedir(dirp);
		send_mime(hc, 200, ok200title, "", "", "text/html; charset=%s", (off_t) - 1, hc->sb.st_mtime);
	} else if (hc->method == METHOD_GET) {
		child_ls(hc, dirp);

		closedir(dirp);
		syslog(LOG_INFO, "%s: LST[%d] /%.200s \"%s\" \"%s\"",
		       httpd_client(hc), r, hc->expnfilename, hc->referer, hc->useragent);

		hc->status = 200;
		hc->bytes_sent = CGI_BYTECOUNT;
		hc->should_linger = 0;
	} else {
		closedir(dirp);
		httpd_send_err(hc, 501, err501title, "", err501form, httpd_method_str(hc->method));
		return -1;
	}

	return 0;
}

#endif /* GENERATE_INDEXES */


static char *build_env(char *fmt, char *arg)
{
	char *cp;
	size_t size;
	static char *buf;
	static size_t maxbuf = 0;

	size = strlen(fmt) + strlen(arg);
	if (size > maxbuf)
		httpd_realloc_str(&buf, &maxbuf, size);
	snprintf(buf, maxbuf, fmt, arg);

	cp = strdup(buf);
	if (!cp) {
		syslog(LOG_ERR, "out of memory copying environment variable");
		exit(1);
	}

	return cp;
}


#ifdef SERVER_NAME_LIST
static char *hostname_map(char *hostname)
{
	int len, n;
	static char *list[] = { SERVER_NAME_LIST };

	len = strlen(hostname);
	for (n = sizeof(list) / sizeof(*list) - 1; n >= 0; --n) {
		if (strncasecmp(hostname, list[n], len) == 0) {
			if (list[n][len] == '/')	/* check in case of a substring match */
				return &list[n][len + 1];
		}
	}

	return NULL;
}
#endif /* SERVER_NAME_LIST */


/* Set up environment variables. Be real careful here to avoid
** letting malicious clients overrun a buffer.  We don't have
** to worry about freeing stuff since we're a sub-process.
*/
static char **make_envp(struct httpd_conn *hc)
{
	static char *envp[50];
	int envn;
	char *cp;
	char buf[256];

	envn = 0;
	envp[envn++] = build_env("PATH=%s", CGI_PATH);
#ifdef CGI_LD_LIBRARY_PATH
	envp[envn++] = build_env("LD_LIBRARY_PATH=%s", CGI_LD_LIBRARY_PATH);
#endif
	envp[envn++] = build_env("SERVER_SOFTWARE=%s", SERVER_SOFTWARE);
	/* If vhosting, use that server-name here. */
	cp = get_hostname(hc);
	if (cp[0])
		envp[envn++] = build_env("SERVER_NAME=%s", cp);
	envp[envn++] = "GATEWAY_INTERFACE=CGI/1.1";
	envp[envn++] = build_env("SERVER_PROTOCOL=%s", hc->protocol);
	snprintf(buf, sizeof(buf), "%d", (int)hc->hs->port);
	envp[envn++] = build_env("SERVER_PORT=%s", buf);
	envp[envn++] = build_env("REQUEST_METHOD=%s", httpd_method_str(hc->method));
	if (hc->pathinfo[0] != '\0') {
		char *cp2;
		size_t l;

		envp[envn++] = build_env("PATH_INFO=/%s", hc->pathinfo);
		l = strlen(hc->hs->cwd) + strlen(hc->pathinfo) + 1;
		cp2 = NEW(char, l);

		if (cp2) {
			snprintf(cp2, l, "%s%s", hc->hs->cwd, hc->pathinfo);
			envp[envn++] = build_env("PATH_TRANSLATED=%s", cp2);
		}
	}
	envp[envn++] = build_env("SCRIPT_NAME=/%s", strcmp(hc->origfilename, ".") == 0 ? "" : hc->origfilename);

	/*
	 * php-cgi needs SCRIPT_FILENAME environement variable to be
	 * defined to detect it was invoqued as CGI script.  Patch by
	 * Fanfan <francois@cerbelle.net>
	 */
	if (hc->expnfilename[0] == '/')
		snprintf(buf, sizeof(buf), "%s", strcmp(hc->expnfilename, ".") == 0 ? "" : hc->expnfilename);
	else
		snprintf(buf, sizeof(buf), "%s%s", hc->hs->cwd, strcmp(hc->expnfilename, ".") == 0 ? "" : hc->expnfilename);
	envp[envn++] = build_env("SCRIPT_FILENAME=%s", buf);

	if (hc->query[0] != '\0')
		envp[envn++] = build_env("QUERY_STRING=%s", hc->query);
	envp[envn++] = build_env("REMOTE_ADDR=%s", httpd_client(hc));

	if (hc->referer[0] != '\0')
		envp[envn++] = build_env("HTTP_REFERER=%s", hc->referer);
	if (hc->useragent[0] != '\0')
		envp[envn++] = build_env("HTTP_USER_AGENT=%s", hc->useragent);
	if (hc->accept[0] != '\0')
		envp[envn++] = build_env("HTTP_ACCEPT=%s", hc->accept);
	if (hc->accepte[0] != '\0')
		envp[envn++] = build_env("HTTP_ACCEPT_ENCODING=%s", hc->accepte);
	if (hc->acceptl[0] != '\0')
		envp[envn++] = build_env("HTTP_ACCEPT_LANGUAGE=%s", hc->acceptl);
	if (hc->cookie[0] != '\0')
		envp[envn++] = build_env("HTTP_COOKIE=%s", hc->cookie);
	if (hc->contenttype[0] != '\0')
		envp[envn++] = build_env("CONTENT_TYPE=%s", hc->contenttype);
	if (hc->hdrhost[0] != '\0')
		envp[envn++] = build_env("HTTP_HOST=%s", hc->hdrhost);
	if (hc->contentlength > 0) {
		snprintf(buf, sizeof(buf), "%lu", (unsigned long)hc->contentlength);
		envp[envn++] = build_env("CONTENT_LENGTH=%s", buf);
	}
	if (hc->remoteuser[0] != '\0')
		envp[envn++] = build_env("REMOTE_USER=%s", hc->remoteuser);
	if (hc->authorization[0] != '\0')
		envp[envn++] = build_env("AUTH_TYPE=%s", "Basic");
	/* We only support Basic auth at the moment. */
	if (getenv("TZ"))
		envp[envn++] = build_env("TZ=%s", getenv("TZ"));
	envp[envn++] = build_env("CGI_PATTERN=%s", hc->hs->cgi_pattern);
	envp[envn] = NULL;

	return envp;
}


/* Set up argument vector.  Again, we don't have to worry about freeing stuff
** since we're a sub-process.  This gets done after make_envp() because we
** scribble on hc->query.
*/
static char **make_argp(struct httpd_conn *hc)
{
	char **argp;
	int argn;
	char *cp1;
	char *cp2;

	/* By allocating an arg slot for every character in the query, plus
	** one for the filename and one for the NULL, we are guaranteed to
	** have enough.  We could actually use strlen/2.
	*/
	argp = NEW(char *, strlen(hc->query) + 2);

	if (!argp)
		return NULL;

	argp[0] = strrchr(hc->expnfilename, '/');
	if (argp[0])
		++argp[0];
	else
		argp[0] = hc->expnfilename;

	argn = 1;
	/* According to the CGI spec at http://hoohoo.ncsa.uiuc.edu/cgi/cl.html,
	** "The server should search the query information for a non-encoded =
	** character to determine if the command line is to be used, if it finds
	** one, the command line is not to be used."
	*/
	if (!strchr(hc->query, '=')) {
		for (cp1 = cp2 = hc->query; *cp2 != '\0'; ++cp2) {
			if (*cp2 == '+') {
				*cp2 = '\0';
				strdecode(cp1, cp1);
				argp[argn++] = cp1;
				cp1 = cp2 + 1;
			}
		}
		if (cp2 != cp1) {
			strdecode(cp1, cp1);
			argp[argn++] = cp1;
		}
	}

	argp[argn] = NULL;

	return argp;
}


/* This routine is used only for POST requests.  It reads the data
** from the request and sends it to the child process.  The only reason
** we need to do it this way instead of just letting the child read
** directly is that we have already read part of the data into our
** buffer.
*/
static void cgi_interpose_input(struct httpd_conn *hc, int wfd)
{
	size_t c;
	ssize_t r;
	char buf[1024];

	c = hc->read_idx - hc->checked_idx;
	if (c > 0) {
		r = file_write(wfd, &(hc->read_buf[hc->checked_idx]), c);
		if ((size_t)r != c)
			return;
	}

	while (c < hc->contentlength) {
		r = httpd_read(hc, buf, MIN(sizeof(buf), hc->contentlength - c));
		if (r < 0 && (errno == EINTR || errno == EAGAIN)) {
			sleep(1);
			continue;
		}

		if (r <= 0)
			return;

		if (file_write(wfd, buf, r) != r)
			return;
		c += r;
	}
	post_post_garbage_hack(hc);
}


/* Special hack to deal with broken browsers that send a LF or CRLF
** after POST data, causing TCP resets - we just read and discard up
** to 2 bytes.  Unfortunately this doesn't fix the problem for CGIs
** which avoid the interposer process due to their POST data being
** short.  Creating an interposer process for all POST CGIs is
** unacceptably expensive.  The eventual fix will come when interposing
** gets integrated into the main loop as a tasklet instead of a process.
*/
static void post_post_garbage_hack(struct httpd_conn *hc)
{
	char buf[2];

	/* If we are in a sub-process, turn on no-delay mode in case we
	** previously cleared it.
	*/
	if (sub_process)
		httpd_set_ndelay(hc->conn_fd);

	/* And read up to 2 bytes. */
	httpd_read(hc, buf, sizeof(buf));
}


/* This routine is used for parsed-header CGIs.  The idea here is that the
** CGI can return special headers such as "Status:" and "Location:" which
** change the return status of the response.  Since the return status has to
** be the very first line written out, we have to accumulate all the headers
** and check for the special ones before writing the status.  Then we write
** out the saved headers and proceed to echo the rest of the response.
*/
static void cgi_interpose_output(struct httpd_conn *hc, int rfd)
{
	ssize_t r;
	char buf[1024];
	size_t headers_size, headers_len;
	char *headers;
	char *br;
	int status;
	char *title;
	char *cp;

	/* Make sure the connection is in blocking mode.  It should already
	** be blocking, but we might as well be sure.
	*/
	httpd_clear_ndelay(hc->conn_fd);

	/* Slurp in all headers. */
	headers_size = 0;
	httpd_realloc_str(&headers, &headers_size, 500);
	headers_len = 0;
	for (;;) {
		r = file_read(rfd, buf, sizeof(buf));
		if (r <= 0) {
			br = &(headers[headers_len]);
			break;
		}

		httpd_realloc_str(&headers, &headers_size, headers_len + r);
		memmove(&(headers[headers_len]), buf, r);
		headers_len += r;
		headers[headers_len] = '\0';
		if ((br = strstr(headers, "\r\n\r\n")) || (br = strstr(headers, "\n\n")))
			break;
	}

	/* If there were no headers, bail. */
	if (headers[0] == '\0')
		return;

	/* Figure out the status.  Look for a Status: or Location: header;
	** else if there's an HTTP header line, get it from there; else
	** default to 200.
	*/
	status = 200;
	if (strncmp(headers, "HTTP/", 5) == 0) {
		cp = headers;
		cp += strcspn(cp, " \t");
		status = atoi(cp);
	}
	if ((cp = strstr(headers, "Status:")) && cp < br && (cp == headers || *(cp - 1) == '\n')) {
		cp += 7;
		cp += strspn(cp, " \t");
		status = atoi(cp);
	} else if ((cp = strstr(headers, "Location:")) && cp < br && (cp == headers || *(cp - 1) == '\n')) {
		status = 302;
	}

	/* Write the status line. */
	switch (status) {
	case 200:
		title = ok200title;
		break;
	case 302:
		title = err302title;
		break;
	case 304:
		title = err304title;
		break;
	case 400:
		title = httpd_err400title;
		break;
#ifdef AUTH_FILE
	case 401:
		title = err401title;
		break;
#endif
	case 403:
		title = err403title;
		break;
	case 404:
		title = err404title;
		break;
	case 408:
		title = httpd_err408title;
		break;
	case 500:
		title = err500title;
		break;
	case 501:
		title = err501title;
		break;
	case 503:
		title = httpd_err503title;
		break;
	default:
		title = "Something";
		break;
	}

	snprintf(buf, sizeof(buf), "HTTP/1.0 %d %s\r\n", status, title);
	httpd_write(hc, buf, strlen(buf));

	/* Write the saved headers. */
	httpd_write(hc, headers, headers_len);

	/* Echo the rest of the output. */
	for (;;) {
		r = file_read(rfd, buf, sizeof(buf));
		if (r <= 0)
			break;

		if (httpd_write(hc, buf, r) != r)
			break;
	}

	shutdown(hc->conn_fd, SHUT_WR);
}


/* CGI child process. */
static void cgi_child(struct httpd_conn *hc)
{
	int r;
	char **argp;
	char **envp;
	char *binary;
	char *directory;

	/* Unset close-on-exec flag for this socket.  This actually shouldn't
	** be necessary, according to POSIX a dup()'d file descriptor does
	** *not* inherit the close-on-exec flag, its flag is always clear.
	** However, Linux messes this up and does copy the flag to the
	** dup()'d descriptor, so we have to clear it.  This could be
	** ifdeffed for Linux only.
	*/
	fcntl(hc->conn_fd, F_SETFD, 0);

	/* If the connection happens to be using one of the stdio
	** descriptors move it to another descriptor so that the
	** dup2() calls below don't screw things up.
	*/
	if (hc->conn_fd == STDIN_FILENO || hc->conn_fd == STDOUT_FILENO || hc->conn_fd == STDERR_FILENO) {
		int newfd = dup(hc->conn_fd);

		if (newfd >= 0)
			hc->conn_fd = newfd;
		/* If the dup() fails, shrug.  We'll just take our
		** chances.  Shouldn't happen though.
		*/
	}

	/* Make the environment vector. */
	envp = make_envp(hc);

	/* Make the argument vector. */
	argp = make_argp(hc);

	/* Set up stdin.  For POSTs we may have to set up a pipe from an
	** interposer process, depending on if we've read some of the data
	** into our buffer.
	*/
	if ((hc->method == METHOD_POST || hc->method == METHOD_PUT) && hc->read_idx >= hc->checked_idx) {
		int p[2];

		if (pipe(p) < 0) {
			syslog(LOG_ERR, "pipe: %s", strerror(errno));
			httpd_send_err(hc, 500, err500title, "", err500form, hc->encodedurl);
			httpd_send_response(hc);
			exit(1);
		}
		r = fork();
		if (r < 0) {
			syslog(LOG_ERR, "fork: %s", strerror(errno));
			httpd_send_err(hc, 500, err500title, "", err500form, hc->encodedurl);
			httpd_send_response(hc);
			exit(1);
		}
		if (r == 0) {
			/* Interposer process. */
			sub_process = 1;
			close(p[0]);
			cgi_interpose_input(hc, p[1]);
			exit(0);
		}

		/* Need to schedule a kill for process r; but in the main process! */
		close(p[1]);
		if (p[0] != STDIN_FILENO) {
			dup2(p[0], STDIN_FILENO);
			close(p[0]);
		}
	} else {
		/* Otherwise, the request socket is stdin. */
		if (hc->conn_fd != STDIN_FILENO)
			dup2(hc->conn_fd, STDIN_FILENO);
	}

	/* Set up stdout/stderr.  If we're doing CGI header parsing,
	** we need an output interposer too.
	*/
	if (strncmp(argp[0], "nph-", 4) != 0 && hc->mime_flag) {
		int p[2];

		if (pipe(p) < 0) {
			syslog(LOG_ERR, "pipe: %s", strerror(errno));
			httpd_send_err(hc, 500, err500title, "", err500form, hc->encodedurl);
			httpd_send_response(hc);
			exit(1);
		}
		r = fork();
		if (r < 0) {
			syslog(LOG_ERR, "fork: %s", strerror(errno));
			httpd_send_err(hc, 500, err500title, "", err500form, hc->encodedurl);
			httpd_send_response(hc);
			exit(1);
		}
		if (r == 0) {
			/* Interposer process. */
			sub_process = 1;
			close(p[1]);
			cgi_interpose_output(hc, p[0]);
			exit(0);
		}
		/* Need to schedule a kill for process r; but in the main process! */
		close(p[0]);
		if (p[1] != STDOUT_FILENO)
			dup2(p[1], STDOUT_FILENO);
		if (p[1] != STDERR_FILENO)
			dup2(p[1], STDERR_FILENO);
		if (p[1] != STDOUT_FILENO && p[1] != STDERR_FILENO)
			close(p[1]);
	} else {
		/* Otherwise, the request socket is stdout/stderr. */
		if (hc->conn_fd != STDOUT_FILENO)
			dup2(hc->conn_fd, STDOUT_FILENO);
		if (hc->conn_fd != STDERR_FILENO)
			dup2(hc->conn_fd, STDERR_FILENO);
	}

	/* At this point we would like to set close-on-exec again for hc->conn_fd
	** (see previous comments on Linux's broken behavior re: close-on-exec
	** and dup.)  Unfortunately there seems to be another Linux problem, or
	** perhaps a different aspect of the same problem - if we do this
	** close-on-exec in Linux, the socket stays open but stderr gets
	** closed - the last fd duped from the socket.  What a mess.  So we'll
	** just leave the socket as is, which under other OSs means an extra
	** file descriptor gets passed to the child process.  Since the child
	** probably already has that file open via stdin stdout and/or stderr,
	** this is not a problem.
	*/
	/* fcntl(hc->conn_fd, F_SETFD, 1); */

#ifdef CGI_NICE
	/* Set priority. */
	nice(CGI_NICE);
#endif

	/* Split the program into directory and binary, so we can chdir()
	** to the program's own directory.  This isn't in the CGI 1.1
	** spec, but it's what other HTTP servers do.
	*/
	directory = strdup(hc->expnfilename);
	if (!directory) {
		binary = hc->expnfilename;	/* ignore errors */
	} else {
		binary = strrchr(directory, '/');
		if (!binary) {
			binary = hc->expnfilename;
		} else {
			*binary++ = '\0';
			chdir(directory);	/* ignore errors */
		}
	}

	/* Default behavior for SIGPIPE. */
	signal(SIGPIPE, SIG_DFL);

	/* Close the syslog descriptor so that the CGI program can't
	** mess with it.  All other open descriptors should be either
	** the listen socket(s), sockets from accept(), or the file-logging
	** fd, and all of those are set to close-on-exec, so we don't
	** have to close anything else.
	*/
	closelog();

	/* Run the program. */
	execve(binary, argp, envp);

	/* Something went wrong, in a chroot() we may not get this syslog() msg. */
	syslog(LOG_ERR, "execve %s(%s): %s", binary, hc->expnfilename, strerror(errno));
	httpd_send_err(hc, 500, err500title, "", err500form, hc->encodedurl);
	httpd_send_response(hc);
	exit(1);
}

int httpd_cgi_track(struct httpd_server *hs, pid_t pid)
{
	int i;

	for (i = 0; i < hs->cgi_limit; i++) {
		if (hs->cgi_tracker[i] != 0)
			continue;

		hs->cgi_tracker[i] = pid;
		hs->cgi_count++;

		return 0;
	}

	return 1;		/* Error, no space? */
}

int httpd_cgi_untrack(struct httpd_server *hs, pid_t pid)
{
	int i;

	for (i = 0; i < hs->cgi_limit; i++) {
		if (hs->cgi_tracker[i] != pid)
			continue;

		hs->cgi_tracker[i] = 0;
		hs->cgi_count--;

		return 0;
	}

	return 1;		/* Not found in this server */
}

static int cgi(struct httpd_conn *hc)
{
	int r;
	arg_t arg;

	/*
	** We are not going to leave the socket open after a CGI ... too difficult
	*/
	hc->do_keep_alive = 0;

	if (hc->method == METHOD_GET || hc->method == METHOD_POST ||
	    hc->method == METHOD_PUT || hc->method == METHOD_DELETE) {
		if (hc->hs->cgi_limit != 0 && hc->hs->cgi_count >= hc->hs->cgi_limit) {
			httpd_send_err(hc, 503, httpd_err503title, "", httpd_err503form, hc->encodedurl);
			return -1;
		}

		httpd_clear_ndelay(hc->conn_fd);
		r = fork();
		if (r < 0) {
			syslog(LOG_ERR, "fork: %s", strerror(errno));
			httpd_send_err(hc, 500, err500title, "", err500form, hc->encodedurl);
			return -1;
		}
		if (r == 0) {
			/* Child process. */
			sub_process = 1;
			httpd_unlisten(hc->hs);
			cgi_child(hc);
		}

		/* Parent process spawned CGI process PID. */
		syslog(LOG_INFO, "%s: CGI[%d] /%.200s%s \"%s\" \"%s\"",
		       httpd_client(hc), r, hc->expnfilename, hc->encodedurl, hc->referer, hc->useragent);

		httpd_cgi_track(hc->hs, r);

#ifdef CGI_TIMELIMIT
		/* Schedule a kill for the child process, in case it runs too long */
		arg.i = r;
		if (!tmr_create(NULL, cgi_kill, arg, CGI_TIMELIMIT * 1000L, 0)) {
			syslog(LOG_CRIT, "tmr_create(cgi_kill child) failed");
			exit(1);
		}
#endif

		hc->status = 200;
		hc->bytes_sent = CGI_BYTECOUNT;
		hc->should_linger = 0;
	} else {
		httpd_send_err(hc, 501, err501title, "", err501form, httpd_method_str(hc->method));
		return -1;
	}

	return 0;
}


/*
 * This function checks the requested (expanded) filename against the
 * CGI pattern, taking into account if the filename is prefixed with
 * a VHOST.
 */
static int is_cgi(struct httpd_conn *hc)
{
	char *fn = hc->expnfilename;

	if (hc->hs->vhost) {
		int len;
		char buf[256];

		len = snprintf(buf, sizeof(buf), "%s/**", hc->hostdir) - 2;
		if (match(buf, fn))
			fn += len;
	}

	/* With the vhost prefix out of the way we can match CGI patterns */
	if (hc->hs->cgi_pattern && match(hc->hs->cgi_pattern, fn))
		return 1;

	return 0;
}

/*
** Adds Vary: Accept-Encoding to relevant files.  For details, see
** https://www.maxcdn.com/blog/accept-encoding-its-vary-important/
** TODO: Refactor, too much focus on finding .gz files
*/
static char *mod_headers(struct httpd_conn *hc)
{
	int serve_dotgz = 0;
	char *fn = NULL;
	char *header = "";
	char *match[] = { ".js", ".css", ".xml", ".gz", ".html" };
	size_t i, len;
	struct stat st;

	if (hc->compression_type != COMPRESSION_GZIP)
		goto done;

	/* construct .gz filename, remember NUL */
	len = strlen(hc->expnfilename) + 4;
	fn = malloc(len);
	snprintf(fn, len, "%s.gz", hc->expnfilename);

	/* is there a .gz file */
	if (!stat(fn, &st)) {
		/* Is it world-readable or world-executable? and newer than original */
		if (st.st_mode & (S_IROTH | S_IXOTH) && st.st_mtime >= hc->sb.st_mtime)
			serve_dotgz = 1;
	}

	/* can serve .gz file and there is no previous encodings */
	if (serve_dotgz && hc->encodings[0] == 0) {
		httpd_realloc_str(&hc->expnfilename, &hc->maxexpnfilename, strlen(fn) + 1);
		strncpy(hc->expnfilename, fn, hc->maxexpnfilename);
		hc->sb.st_size = st.st_size;
		hc->compression_type = COMPRESSION_NONE; /* Compressed already, do not call zlib */
		httpd_realloc_str(&hc->encodings, &hc->maxencodings, 5);
		strncpy(hc->encodings, "gzip", hc->maxencodings);
	}

	free(fn);
done:
	/* no zlib */
	if (!hc->has_deflate)
		hc->compression_type = COMPRESSION_NONE;
	/* don't try to compress non-text files unless it's javascript */
	else if (strncmp(hc->type, "text/", 5) && strcmp(hc->type, "application/javascript"))
		hc->compression_type = COMPRESSION_NONE;
        /* don't try to compress really small things */
	else if (hc->sb.st_size < 256)
		hc->compression_type = COMPRESSION_NONE;

	fn = strrchr(hc->expnfilename, '.');
	if (fn || strstr(hc->encodings, "gzip")) {
		for (i = 0; i < NELEMS(match); i++) {
			if (strcmp(fn, match[i]))
				continue;

			header = "Vary: Accept-Encoding\r\n";
			break;
		}
	}

	return header;
}

static int really_start_request(struct httpd_conn *hc, struct timeval *now)
{
	int is_icon;
	char *cp, *pi;
	static const char *index_names[] = { INDEX_NAMES };
	size_t expnlen, indxlen, i;

	expnlen = strlen(hc->expnfilename);

	if (hc->method != METHOD_GET && hc->method != METHOD_HEAD && hc->method != METHOD_POST &&
	    hc->method != METHOD_OPTIONS && hc->method != METHOD_PUT && hc->method != METHOD_DELETE) {
		httpd_send_err(hc, 501, err501title, "", err501form, httpd_method_str(hc->method));
		return -1;
	}

	is_icon = mmc_icon_check(hc->pathinfo, &hc->sb);
	if (is_icon) {
		strcpy(hc->expnfilename, hc->pathinfo);
		strcpy(hc->pathinfo, "");
		goto sneaky;
	}

	/* Stat the file. */
	if (stat(hc->expnfilename, &hc->sb) < 0) {
		httpd_send_err(hc, 500, err500title, "", err500form, hc->encodedurl);
		return -1;
	}

	/* Is it world-readable or world-executable?  We check explicitly instead
	** of just trying to open it, so that no one ever gets surprised by
	** a file that's not set world-readable and yet somehow is
	** readable by the HTTP server and therefore the *whole* world.
	*/
	if (!(hc->sb.st_mode & (S_IROTH | S_IXOTH))) {
		syslog(LOG_INFO, "%s URL \"%s\" resolves to a non world-readable file", httpd_client(hc), hc->encodedurl);
		httpd_send_err(hc, 403, err403title, "",
			       ERROR_FORM(err403form, "The requested URL '%s' resolves to a file that is not world-readable.\n"),
			       hc->encodedurl);
		return -1;
	}

	/* Is it a directory? */
	if (S_ISDIR(hc->sb.st_mode)) {
		/* If there's pathinfo, it's just a non-existent file. */
		if (hc->pathinfo[0] != '\0') {
			httpd_send_err(hc, 404, err404title, "", err404form, hc->encodedurl);
			return -1;
		}

		/* Special handling for directory URLs that don't end in a slash.
		** We send back an explicit redirect with the slash, because
		** otherwise many clients can't build relative URLs properly.
		*/
		if (strcmp(hc->origfilename, "") != 0 &&
		    strcmp(hc->origfilename, ".") != 0 && hc->origfilename[strlen(hc->origfilename) - 1] != '/') {
			send_dirredirect(hc);
			return -1;
		}

		/* Check for an index file. */
		for (i = 0; i < NELEMS(index_names); ++i) {
			/* +2 = extra slash plus \0, strlen() returns length without \0 */
			httpd_realloc_str(&hc->indexname, &hc->maxindexname, expnlen + 2 + strlen(index_names[i]));
			strcpy(hc->indexname, hc->expnfilename);
			indxlen = strlen(hc->indexname);
			if (indxlen == 0 || hc->indexname[indxlen - 1] != '/')
				strcat(hc->indexname, "/");
			if (strcmp(hc->indexname, "./") == 0)
				hc->indexname[0] = '\0';
			strcat(hc->indexname, index_names[i]);
			if (stat(hc->indexname, &hc->sb) >= 0)
				goto got_one;
		}

		/* Nope, no index file, so it's an actual directory request. */
#ifdef GENERATE_INDEXES
		/* Directories must be readable for indexing. */
		if (!(hc->sb.st_mode & S_IROTH)) {
			syslog(LOG_INFO, "%s URL \"%s\" tried to index a directory with indexing disabled",
			       httpd_client(hc), hc->encodedurl);
			httpd_send_err(hc, 403, err403title, "",
				       ERROR_FORM(err403form,
						  "The requested URL '%s' resolves to a directory that has indexing disabled.\n"),
				       hc->encodedurl);
			return -1;
		}

#ifdef ACCESS_FILE
		/* Check access file for this directory. */
		if (access_check(hc, hc->expnfilename) == -1)
			return -1;
#endif

#ifdef AUTH_FILE
		/* Check authorization for this directory. */
		if (auth_check(hc, hc->expnfilename) == -1)
			return -1;
#endif

		/* Referer check. */
		if (!check_referer(hc))
			return -1;
		/* Ok, generate an index. */
		return ls(hc);
#else /* GENERATE_INDEXES */
		syslog(LOG_INFO, "%s URL \"%s\" tried to index a directory", httpd_client(hc), hc->encodedurl);
		httpd_send_err(hc, 403, err403title, "",
			       ERROR_FORM(err403form,
					  "The requested URL '%s' is a directory, and directory indexing is disabled on this server.\n"),
			       hc->encodedurl);
		return -1;
#endif /* GENERATE_INDEXES */

	got_one:
		/* Got an index file.  Expand symlinks again.
		** More pathinfo means something went wrong.
		*/
		cp = expand_symlinks(hc->indexname, &pi, hc->hs->no_symlink_check, hc->tildemapped);
		if (!cp || pi[0] != '\0') {
			httpd_send_err(hc, 500, err500title, "", err500form, hc->encodedurl);
			return -1;
		}

		expnlen = strlen(cp);
		httpd_realloc_str(&hc->expnfilename, &hc->maxexpnfilename, expnlen);
		strcpy(hc->expnfilename, cp);

		/* Now, is the index version world-readable or world-executable? */
		if (!(hc->sb.st_mode & (S_IROTH | S_IXOTH))) {
			syslog(LOG_INFO, "%s URL \"%s\" resolves to a non-world-readable index file",
			       httpd_client(hc), hc->encodedurl);
			httpd_send_err(hc, 403, err403title, "",
				       ERROR_FORM(err403form,
						  "The requested URL '%s' resolves to an index file that is not world-readable.\n"),
				       hc->encodedurl);
			return -1;
		}
	} else if (!S_ISREG(hc->sb.st_mode)) {
		/* Err, not a regular file and not a directory? */
		httpd_send_err(hc, 404, err404title, "", err404form, hc->encodedurl);
		return -1;
	}

#ifdef ACCESS_FILE
	/* Check access for this directory. */
	if (access_check(hc, NULL) == -1)
		return -1;
#endif /* ACCESS_FILE */

#ifdef AUTH_FILE
	/* Check authorization for this directory. */
	if (auth_check(hc, NULL) == -1)
		return -1;
#endif /* AUTH_FILE */

sneaky:
	/* Referer check. */
	if (!check_referer(hc))
		return -1;

	if (hc->method == METHOD_OPTIONS) {
		time_t now;
		char buf[1000];
		char nowbuf[100];
		const char *rfc1123fmt = "%a, %d %b %Y %H:%M:%S GMT";

		now = time(NULL);
		strftime(nowbuf, sizeof(nowbuf), rfc1123fmt, gmtime(&now));

		snprintf(buf, sizeof(buf),
			 "%.20s %d %s\r\n"
			 "Date: %s\r\n"
			 "Server: %s\r\n"
			 "Allow: %sOPTIONS,GET,HEAD\r\n"
			 "Cache-control: max-age=%d\r\n"
			 "Content-Length: 0\r\n"
			 "Content-Type: text/html\r\n"
			 "\r\n",
			 hc->protocol, 200, "OK", nowbuf,
			 EXPOSED_SERVER_SOFTWARE,
			 is_cgi(hc) ? "POST," : "",
			 hc->hs->max_age);
		add_response(hc, buf);
		return 0;
	}

	/* Is it world-executable and in the CGI area? */
	if (is_cgi(hc)) {
		if (hc->sb.st_mode & S_IXOTH)
			return cgi(hc);

		syslog(LOG_DEBUG, "%s URL \"%s\" is a CGI but not executable, rejecting.", httpd_client(hc), hc->encodedurl);
		httpd_send_err(hc, 403, err403title, "",
			       ERROR_FORM(err403form,
					  "The requested URL '%s' resolves to a file which matches a CGI but is not executable; retrieving it is forbidden.\n"),
			       hc->encodedurl);
		return -1;
	}

	if (hc->pathinfo[0] != '\0') {
		syslog(LOG_INFO, "%s URL \"%s\" has pathinfo but isn't CGI", httpd_client(hc), hc->encodedurl);
		httpd_send_err(hc, 403, err403title, "",
			       ERROR_FORM(err403form,
					  "The requested URL '%s' resolves to a file plus CGI-style pathinfo, but the file is not a valid CGI file.\n"),
			       hc->encodedurl);
		return -1;
	}

	/* Fill in last_byte_index, if necessary. */
	if (hc->got_range && (hc->last_byte_index == -1 || hc->last_byte_index >= hc->sb.st_size))
		hc->last_byte_index = hc->sb.st_size - 1;

	figure_mime(hc);

	if (hc->method == METHOD_HEAD) {
		char *extra = mod_headers(hc);

		send_mime(hc, 200, ok200title, hc->encodings, extra, hc->type, hc->sb.st_size, hc->sb.st_mtime);
	} else if (hc->if_modified_since != (time_t)-1 && hc->if_modified_since >= hc->sb.st_mtime) {
		send_mime(hc, 304, err304title, hc->encodings, "", hc->type, (off_t) - 1, hc->sb.st_mtime);
	} else {
		char *extra = mod_headers(hc);

		hc->file_address = mmc_map(hc->expnfilename, &(hc->sb), now);
		if (!hc->file_address) {
			if (is_icon)
				httpd_send_err(hc, 404, err404title, "", err404form, hc->encodedurl);
			else
				httpd_send_err(hc, 500, err500title, "", err500form, hc->encodedurl);
			return -1;
		}

		send_mime(hc, 200, ok200title, hc->encodings, extra, hc->type, hc->sb.st_size, hc->sb.st_mtime);
	}

	return 0;
}


int httpd_start_request(struct httpd_conn *hc, struct timeval *now)
{
	int r;

	/* Really start the request. */
	r = really_start_request(hc, now);

	/* And return the status. */
	return r;
}


static void make_log_entry(struct httpd_conn *hc)
{
	char *ru;
	char url[305];
	char bytes[40];

	if (hc->hs->no_log)
		return;

	/* This is straight CERN Combined Log Format - the only tweak
	** being that if we're using syslog() we leave out the date, because
	** syslogd puts it in.  The included syslogtocern script turns the
	** results into true CERN format.
	*/

	/* Format remote user. */
	if (hc->remoteuser[0] != '\0')
		ru = hc->remoteuser;
	else
		ru = "-";

	/* If we're vhosting, prepend the hostname to the url.  This is
	** a little weird, perhaps writing separate log files for
	** each vhost would make more sense.
	*/
	if (hc->hs->vhost && !hc->tildemapped)
		snprintf(url, sizeof(url), "/%.100s%.200s", get_hostname(hc), hc->encodedurl);
	else
		snprintf(url, sizeof(url), "%.200s", hc->encodedurl);

	/* Format the bytes. */
	if (hc->bytes_sent >= 0)
		snprintf(bytes, sizeof(bytes), "%" PRId64, (int64_t)hc->bytes_sent);
	else
		strcpy(bytes, "-");

	syslog(LOG_INFO, "%s: %s \"%s %.200s %s\" %d %s \"%.200s\" \"%.200s\"",
	       httpd_client(hc), ru, httpd_method_str(hc->method), url, hc->protocol,
	       hc->status, bytes, hc->referer, hc->useragent);
}


/* Returns 1 if ok to serve the url, 0 if not. */
static int check_referer(struct httpd_conn *hc)
{
	int r;

	/* Are we doing referer checking at all? */
	if (!hc->hs->url_pattern)
		return 1;

	r = really_check_referer(hc);
	if (!r) {
		syslog(LOG_INFO, "%s non-local referer \"%s%s\" \"%s\"",
		       httpd_client(hc), get_hostname(hc), hc->encodedurl, hc->referer);
		httpd_send_err(hc, 403, err403title, "",
			       ERROR_FORM(err403form, "You must supply a local referer to get URL '%s' from this server.\n"),
			       hc->encodedurl);
	}

	return r;
}


/* Returns 1 if ok to serve the url, 0 if not. */
static int really_check_referer(struct httpd_conn *hc)
{
	struct httpd_server *hs;
	char *cp1;
	char *cp2;
	char *cp3;
	static char *refhost = NULL;
	static size_t refhost_size = 0;
	char *lp;

	hs = hc->hs;

	/* Check for an empty referer. */
	if (!hc->referer || hc->referer[0] == '\0' || (cp1 = strstr(hc->referer, "//")) == NULL) {
		/* Disallow if we require a referer and the url matches. */
		if (hs->no_empty_referers && match(hs->url_pattern, hc->origfilename))
			return 0;

		/* Otherwise ok. */
		return 1;
	}

	/* Extract referer host. */
	cp1 += 2;
	for (cp2 = cp1; *cp2 != '/' && *cp2 != ':' && *cp2 != '\0'; ++cp2)
		continue;
	httpd_realloc_str(&refhost, &refhost_size, cp2 - cp1);
	for (cp3 = refhost; cp1 < cp2; ++cp1, ++cp3)
		if (isupper(*cp1))
			*cp3 = tolower(*cp1);
		else
			*cp3 = *cp1;
	*cp3 = '\0';

	/* Local pattern? */
	if (hs->local_pattern) {
		lp = hs->local_pattern;
	} else {
		/* No local pattern.  What's our hostname? */
		if (!hs->vhost) {
			/* Not vhosting, use the server name. */
			lp = hs->server_hostname;
			if (!lp)
				/* Couldn't figure out local hostname - give up. */
				return 1;
		} else {
			/* We are vhosting, use the hostname on this connection. */
			lp = hc->hostname;
			if (!lp)
				/* Oops, no hostname.  Maybe it's an old browser that
				** doesn't send a Host: header.  We could figure out
				** the default hostname for this IP address, but it's
				** not worth it for the few requests like this.
				*/
				return 1;
		}
	}

	/* If the referer host doesn't match the local host pattern, and
	** the filename does match the url pattern, it's an illegal reference.
	*/
	if (!match(lp, refhost) && match(hs->url_pattern, hc->origfilename))
		return 0;

	/* Otherwise ok. */
	return 1;
}


char *httpd_ntoa(httpd_sockaddr *hsa)
{
#ifdef USE_IPV6
	static char str[200];

	if (getnameinfo(&hsa->sa, sockaddr_len(hsa), str, sizeof(str), 0, 0, NI_NUMERICHOST) != 0) {
		str[0] = '?';
		str[1] = '\0';
	} else if (IN6_IS_ADDR_V4MAPPED(&hsa->sa_in6.sin6_addr) && strncmp(str, "::ffff:", 7) == 0) {
		/* Elide IPv6ish prefix for IPv4 addresses. */
		memmove(str, &str[7], strlen(str) - 6);
	}

	return str;
#else
	return inet_ntoa(hsa->sa_in.sin_addr);
#endif
}

short httpd_port(httpd_sockaddr *hsa)
{
    if (hsa->sa.sa_family == AF_INET)
	    return ntohs(hsa->sa_in.sin_port);

    return htons(hsa->sa_in6.sin6_port);
}

char *httpd_client(struct httpd_conn *hc)
{
	return hc->client_addr.real_ip;
}

static int sockaddr_check(httpd_sockaddr *hsa)
{
	switch (hsa->sa.sa_family) {
	case AF_INET:
		return 1;

#ifdef USE_IPV6
	case AF_INET6:
		return 1;
#endif

	default:
		return 0;
	}
}


static size_t sockaddr_len(httpd_sockaddr *hsa)
{
	switch (hsa->sa.sa_family) {
	case AF_INET:
		return sizeof(struct sockaddr_in);

#ifdef USE_IPV6
	case AF_INET6:
		return sizeof(struct sockaddr_in6);
#endif

	default:
		return 0;	/* shouldn't happen */
	}
}


#ifndef HAVE_ATOLL
static long long atoll(const char *str)
{
	long long value;
	long long sign;

	while (isspace(*str))
		++str;

	switch (*str) {
	case '-':
		sign = -1;
		++str;
		break;

	case '+':
		sign = 1;
		++str;
		break;

	default:
		sign = 1;
		break;
	}

	value = 0;
	while (isdigit(*str)) {
		value = value * 10 + (*str - '0');
		++str;
	}

	return sign * value;
}
#endif /* HAVE_ATOLL */


/* Read the requested buffer completely, accounting for interruptions. */
ssize_t httpd_read(struct httpd_conn *hc, void *buf, size_t len)
{
	return httpd_ssl_read(hc, buf, len);
}


/* Write the requested buffer completely, accounting for interruptions. */
ssize_t httpd_write(struct httpd_conn *hc, void *buf, size_t len)
{
	return httpd_ssl_write(hc, buf, len);
}

ssize_t httpd_writev(struct httpd_conn *hc, struct iovec *iov, size_t num)
{
	return httpd_ssl_writev(hc, iov, num);
}


/* Generate debugging statistics syslog message. */
void httpd_logstats(long secs)
{
	if (str_alloc_count <= 0)
		return;

	syslog(LOG_INFO, "  libhttpd - %d strings allocated, %lu bytes (%g bytes/str)",
	       str_alloc_count, (unsigned long)str_alloc_size, (float)str_alloc_size / str_alloc_count);
}

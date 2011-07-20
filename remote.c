/*
 * remote.c - remote control for the NSD daemon.
 *
 * Copyright (c) 2008, NLnet Labs. All rights reserved.
 *
 * This software is open source.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 
 * Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 * 
 * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 * 
 * Neither the name of the NLNET LABS nor the names of its contributors may
 * be used to endorse or promote products derived from this software without
 * specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * \file
 *
 * This file contains the remote control functionality for the daemon.
 * The remote control can be performed using either the commandline
 * nsd-control tool, or a SSLv3/TLS capable web browser. 
 * The channel is secured using SSLv3 or TLSv1, and certificates.
 * Both the server and the client(control tool) have their own keys.
 */
#include "config.h"
#if defined(HAVE_SSL)

#ifdef HAVE_OPENSSL_SSL_H
#include "openssl/ssl.h"
#endif
#ifdef HAVE_OPENSSL_ERR_H
#include <openssl/err.h>
#endif
#include <ctype.h>
#include <unistd.h>
#include <assert.h>
#include <fcntl.h>
#include "remote.h"
#include "util.h"
#include "xfrd.h"
#include "xfrd-notify.h"
#include "nsd.h"
#include "netio.h"
#include "options.h"
#include "difffile.h"

#ifdef HAVE_SYS_TYPES_H
#  include <sys/types.h>
#endif
#ifdef HAVE_NETDB_H
#include <netdb.h>
#endif

/** number of seconds timeout on incoming remote control handshake */
#define REMOTE_CONTROL_TCP_TIMEOUT 120

/** if you want zero to be inhibited in stats output.
 * it omits zeroes for types that have no acronym and unused-rcodes */
const int inhibit_zero = 1;

/**
 * a busy control command connection, SSL state
 * Defined here to keep the definition private, and keep SSL out of the .h
 */
struct rc_state {
	/** the next item in list */
	struct rc_state* next, *prev;
	/** the commpoint */
	struct netio_handler* c;
	/** timeout for this state */
	struct timespec tval;
	/** in the handshake part */
	enum { rc_none, rc_hs_read, rc_hs_write } shake_state;
	/** the ssl state */
	SSL* ssl;
	/** the rc this is part of */
	struct daemon_remote* rc;
	/** stats list next item */
	struct rc_state* stats_next;
	/** stats list indicator (0 is not part of stats list, 1 is stats,
	 * 2 is stats_noreset. */
	int in_stats_list;
};

/**
 * The remote control state.
 */
struct daemon_remote {
	/** the master process for this remote control */
	struct xfrd_state* xfrd;
	/** commpoints for accepting remote control connections */
	struct netio_handler_list* accept_list;
	/** number of active commpoints that are handling remote control */
	int active;
	/** max active commpoints */
	int max_active;
	/** current commpoints busy; double linked, malloced */
	struct rc_state* busy_list;
	/** commpoints waiting for stats to complete (also in busy_list) */
	struct rc_state* stats_list;
	/** last time stats was reported */
	struct timeval stats_time, boot_time;
	/** the SSL context for creating new SSL streams */
	SSL_CTX* ctx;
};

/** 
 * Print fixed line of text over ssl connection in blocking mode
 * @param ssl: print to
 * @param text: the text.
 * @return false on connection failure.
 */
static int ssl_print_text(SSL* ssl, const char* text);

/** 
 * printf style printing to the ssl connection
 * @param ssl: the SSL connection to print to. Blocking.
 * @param format: printf style format string.
 * @return success or false on a network failure.
 */
static int ssl_printf(SSL* ssl, const char* format, ...)
        ATTR_FORMAT(printf, 2, 3);

/**
 * Read until \n is encountered
 * If SSL signals EOF, the string up to then is returned (without \n).
 * @param ssl: the SSL connection to read from. blocking.
 * @param buf: buffer to read to.
 * @param max: size of buffer.
 * @return false on connection failure.
 */
static int ssl_read_line(SSL* ssl, char* buf, size_t max);

/** perform the accept of a new remote control connection */
static void
remote_accept_callback(netio_type *netio, netio_handler_type *handler,
	netio_event_types_type event_types);

/** perform remote control */
static void
remote_control_callback(netio_type *netio, netio_handler_type *handler,
	netio_event_types_type event_types);


/** ---- end of private defines ---- **/


/** log ssl crypto err */
static void
log_crypto_err(const char* str)
{
	/* error:[error code]:[library name]:[function name]:[reason string] */
	char buf[128];
	unsigned long e;
	ERR_error_string_n(ERR_get_error(), buf, sizeof(buf));
	log_msg(LOG_ERR, "%s crypto %s", str, buf);
	while( (e=ERR_get_error()) ) {
		ERR_error_string_n(e, buf, sizeof(buf));
		log_msg(LOG_ERR, "and additionally crypto %s", buf);
	}
}

#ifdef BIND8_STATS
/** subtract timers and the values do not overflow or become negative */
static void
timeval_subtract(struct timeval* d, const struct timeval* end, 
	const struct timeval* start)
{
#ifndef S_SPLINT_S
	time_t end_usec = end->tv_usec;
	d->tv_sec = end->tv_sec - start->tv_sec;
	if(end_usec < start->tv_usec) {
		end_usec += 1000000;
		d->tv_sec--;
	}
	d->tv_usec = end_usec - start->tv_usec;
#endif
}
#endif /* BIND8_STATS */

struct daemon_remote*
daemon_remote_create(nsd_options_t* cfg)
{
	char* s_cert;
	char* s_key;
	struct daemon_remote* rc = (struct daemon_remote*)xalloc_zero(
		sizeof(*rc));
	rc->max_active = 10;
	assert(cfg->control_enable);

	/* init SSL library */
	ERR_load_crypto_strings();
	ERR_load_SSL_strings();
	OpenSSL_add_all_algorithms();
	(void)SSL_library_init();

	rc->ctx = SSL_CTX_new(SSLv23_server_method());
	if(!rc->ctx) {
		log_crypto_err("could not SSL_CTX_new");
		free(rc);
		return NULL;
	}
	/* no SSLv2 because has defects */
	if(!(SSL_CTX_set_options(rc->ctx, SSL_OP_NO_SSLv2) & SSL_OP_NO_SSLv2)){
		log_crypto_err("could not set SSL_OP_NO_SSLv2");
		daemon_remote_delete(rc);
		return NULL;
	}
	s_cert = cfg->server_cert_file;
	s_key = cfg->server_key_file;
	VERBOSITY(2, (LOG_INFO, "setup SSL certificates"));
	if (!SSL_CTX_use_certificate_file(rc->ctx,s_cert,SSL_FILETYPE_PEM)) {
		log_msg(LOG_ERR, "Error for server-cert-file: %s", s_cert);
		log_crypto_err("Error in SSL_CTX use_certificate_file");
		goto setup_error;
	}
	if(!SSL_CTX_use_PrivateKey_file(rc->ctx,s_key,SSL_FILETYPE_PEM)) {
		log_msg(LOG_ERR, "Error for server-key-file: %s", s_key);
		log_crypto_err("Error in SSL_CTX use_PrivateKey_file");
		goto setup_error;
	}
	if(!SSL_CTX_check_private_key(rc->ctx)) {
		log_msg(LOG_ERR, "Error for server-key-file: %s", s_key);
		log_crypto_err("Error in SSL_CTX check_private_key");
		goto setup_error;
	}
	if(!SSL_CTX_load_verify_locations(rc->ctx, s_cert, NULL)) {
		log_crypto_err("Error setting up SSL_CTX verify locations");
	setup_error:
		daemon_remote_delete(rc);
		return NULL;
	}
	SSL_CTX_set_client_CA_list(rc->ctx, SSL_load_client_CA_file(s_cert));
	SSL_CTX_set_verify(rc->ctx, SSL_VERIFY_PEER, NULL);

	/* and try to open the ports */
	if(!daemon_remote_open_ports(rc, cfg)) {
		log_msg(LOG_ERR, "could not open remote control port");
		goto setup_error;
	}

	if(gettimeofday(&rc->boot_time, NULL) == -1)
		log_msg(LOG_ERR, "gettimeofday: %s", strerror(errno));
	rc->stats_time = rc->boot_time;

	return rc;
}

void daemon_remote_close(struct daemon_remote* rc)
{
	struct rc_state* p, *np;
	netio_handler_list_type* h, *nh;
	if(!rc) return;

	/* close listen sockets */
	h = rc->accept_list;
	while(h) {
		nh = h->next;
		close(h->handler->fd);
		free(h->handler);
		free(h);
		h = nh;
	}
	rc->accept_list = NULL;

	/* close busy connection sockets */
	p = rc->busy_list;
	while(p) {
		np = p->next;
		if(p->ssl)
			SSL_free(p->ssl);
		close(p->c->fd);
		free(p->c);
		free(p);
		p = np;
	}
	rc->busy_list = NULL;
	rc->active = 0;
}

void daemon_remote_delete(struct daemon_remote* rc)
{
	if(!rc) return;
	daemon_remote_close(rc);
	if(rc->ctx) {
		SSL_CTX_free(rc->ctx);
	}
	free(rc);
}

static int
create_tcp_accept_sock(struct addrinfo* addr, int* noproto)
{
#if defined(SO_REUSEADDR) || (defined(INET6) && (defined(IPV6_V6ONLY) || defined(IPV6_USE_MIN_MTU) || defined(IPV6_MTU)))
	int on = 1;
#endif
	int s;
	*noproto = 0;
	if ((s = socket(addr->ai_family, addr->ai_socktype, 0)) == -1) {
#if defined(INET6)
		if (addr->ai_family == AF_INET6 &&
			errno == EAFNOSUPPORT) {
			*noproto = 1;
			log_msg(LOG_WARNING, "fallback to TCP4, no IPv6: not supported");
			return -1;
		}
#endif /* INET6 */
		log_msg(LOG_ERR, "can't create a socket: %s", strerror(errno));
		return -1;
	}
#ifdef  SO_REUSEADDR
	if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0) {
		log_msg(LOG_ERR, "setsockopt(..., SO_REUSEADDR, ...) failed: %s", strerror(errno));
	}
#endif /* SO_REUSEADDR */
#if defined(INET6) && defined(IPV6_V6ONLY)
	if (addr->ai_family == AF_INET6 &&
		setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &on, sizeof(on)) < 0)
	{
		log_msg(LOG_ERR, "setsockopt(..., IPV6_V6ONLY, ...) failed: %s", strerror(errno));
		return -1;
	}
#endif
	/* set it nonblocking */
	/* (StevensUNP p463), if tcp listening socket is blocking, then
	   it may block in accept, even if select() says readable. */
	if (fcntl(s, F_SETFL, O_NONBLOCK) == -1) {
		log_msg(LOG_ERR, "cannot fcntl tcp: %s", strerror(errno));
	}
	/* Bind it... */
	if (bind(s, (struct sockaddr *)addr->ai_addr, addr->ai_addrlen) != 0) {
		log_msg(LOG_ERR, "can't bind tcp socket: %s", strerror(errno));
		return -1;
	}
	/* Listen to it... */
	if (listen(s, TCP_BACKLOG) == -1) {
		log_msg(LOG_ERR, "can't listen: %s", strerror(errno));
		return -1;
	}
	return s;
}

/**
 * Add and open a new control port
 * @param rc: rc with result list.
 * @param ip: ip str
 * @param nr: port nr
 * @param noproto_is_err: if lack of protocol support is an error.
 * @return false on failure.
 */
static int
add_open(struct daemon_remote* rc, const char* ip, int nr, int noproto_is_err)
{
	struct addrinfo hints;
	struct addrinfo* res;
	netio_handler_list_type* hl;
	int noproto;
	int fd, r;
	char port[15];
	snprintf(port, sizeof(port), "%d", nr);
	port[sizeof(port)-1]=0;
	memset(&hints, 0, sizeof(hints));
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE | AI_NUMERICHOST;
	if((r = getaddrinfo(ip, port, &hints, &res)) != 0 || !res) {
                log_msg(LOG_ERR, "control interface %s:%s getaddrinfo: %s %s",
			ip?ip:"default", port, gai_strerror(r),
#ifdef EAI_SYSTEM
			r==EAI_SYSTEM?(char*)strerror(errno):""
#else
			""
#endif
			);
		return 0;
	}

	/* open fd */
	fd = create_tcp_accept_sock(res, &noproto);
	freeaddrinfo(res);
	if(fd == -1 && noproto) {
		if(!noproto_is_err)
			return 1; /* return success, but do nothing */
		log_msg(LOG_ERR, "cannot open control interface %s %d : "
			"protocol not supported", ip, nr);
		return 0;
	}
	if(fd == -1) {
		log_msg(LOG_ERR, "cannot open control interface %s %d", ip, nr);
		return 0;
	}

	/* alloc */
	hl = (netio_handler_list_type*)xalloc_zero(sizeof(*hl));
	hl->handler = (netio_handler_type*)xalloc_zero(sizeof(*hl->handler));
	hl->next = rc->accept_list;
	rc->accept_list = hl;
	hl->handler->fd = fd;
	return 1;
}

int daemon_remote_open_ports(struct daemon_remote* rc, nsd_options_t* cfg)
{
	assert(cfg->control_enable && cfg->control_port);
	if(cfg->control_interface) {
		ip_address_option_t* p;
		for(p = cfg->control_interface; p; p = p->next) {
			if(!add_open(rc, p->address, cfg->control_port, 1)) {
				return 0;
			}
		}
	} else {
		/* defaults */
		if(!cfg->ip4_only &&
			!add_open(rc, "::1", cfg->control_port, 0)) {
			return 0;
		}
		if(!cfg->ip6_only &&
			!add_open(rc, "127.0.0.1", cfg->control_port, 1)) {
			return 0;
		}
	}
	return 1;
}

void daemon_remote_attach(struct daemon_remote* rc, struct xfrd_state* xfrd)
{
	netio_handler_list_type* p;
	if(!rc) return;
	rc->xfrd = xfrd;
	for(p = rc->accept_list; p; p = p->next) {
		/* add to netio */
		p->handler->timeout = NULL;
		p->handler->user_data = rc;
		p->handler->event_types = NETIO_EVENT_READ;
		p->handler->event_handler = &remote_accept_callback;
		netio_add_handler(xfrd->netio, p->handler);
	}
}

static void
remote_accept_callback(netio_type *netio, netio_handler_type *handler,
	netio_event_types_type event_types)
{
	struct daemon_remote *rc = (struct daemon_remote*) handler->user_data;
	struct sockaddr_storage addr;
	socklen_t addrlen;
	int newfd;
	struct rc_state* n;

	if (!(event_types & NETIO_EVENT_READ)) {
		return;
	}

	/* perform the accept */
	addrlen = sizeof(addr);
	newfd = accept(handler->fd, (struct sockaddr*)&addr, &addrlen);
	if(newfd == -1) {
		if (    errno != EINTR
			&& errno != EWOULDBLOCK
#ifdef ECONNABORTED
			&& errno != ECONNABORTED
#endif /* ECONNABORTED */
#ifdef EPROTO
			&& errno != EPROTO
#endif /* EPROTO */
			) {
			log_msg(LOG_ERR, "accept failed: %s", strerror(errno));
		}
		return;
	}

	/* create new commpoint unless we are servicing already */
	if(rc->active >= rc->max_active) {
		log_msg(LOG_WARNING, "drop incoming remote control: "
			"too many connections");
	close_exit:
		close(newfd);
		return;
	}
	if (fcntl(newfd, F_SETFL, O_NONBLOCK) == -1) {
		log_msg(LOG_ERR, "fcntl failed: %s", strerror(errno));
		goto close_exit;
	}

	/* setup state to service the remote control command */
	n = (struct rc_state*)calloc(1, sizeof(*n));
	if(!n) {
		log_msg(LOG_ERR, "out of memory");
		goto close_exit;
	}
	n->c = (struct netio_handler*)calloc(1, sizeof(*n->c));
	if(!n->c) {
		log_msg(LOG_ERR, "out of memory");
		free(n);
		goto close_exit;
	}
	n->c->fd = newfd;
	n->c->timeout = &n->tval;
	n->tval.tv_sec = REMOTE_CONTROL_TCP_TIMEOUT;
	n->tval.tv_nsec = 0L;
	timespec_add(&n->tval, netio_current_time(netio));
	n->c->event_types = NETIO_EVENT_READ | NETIO_EVENT_TIMEOUT;
	n->c->event_handler = &remote_control_callback;
	n->c->user_data = n;

	if(2 <= verbosity) {
		char s[128];
		addr2str(&addr, s, sizeof(s));
		VERBOSITY(2, (LOG_INFO, "new control connection from %s", s));
	}

	n->shake_state = rc_hs_read;
	n->ssl = SSL_new(rc->ctx);
	if(!n->ssl) {
		log_crypto_err("could not SSL_new");
		free(n->c);
		free(n);
		goto close_exit;
	}
	SSL_set_accept_state(n->ssl);
        (void)SSL_set_mode(n->ssl, SSL_MODE_AUTO_RETRY);
	if(!SSL_set_fd(n->ssl, newfd)) {
		log_crypto_err("could not SSL_set_fd");
		SSL_free(n->ssl);
		free(n->c);
		free(n);
		goto close_exit;
	}

	n->rc = rc;
	n->stats_next = NULL;
	n->in_stats_list = 0;
	n->prev = NULL;
	n->next = rc->busy_list;
	if(n->next) n->next->prev = n;
	rc->busy_list = n;
	rc->active ++;
	netio_add_handler(netio, n->c);

	/* perform the first nonblocking read already, for windows, 
	 * so it can return wouldblock. could be faster too. */
	remote_control_callback(netio, n->c, NETIO_EVENT_READ);
}

/** delete from list */
static void
state_list_remove_elem(struct rc_state** list, struct rc_state* todel)
{
	if(todel->prev) todel->prev->next = todel->next;
	else	*list = todel->next;
	if(todel->next) todel->next->prev = todel->prev;
}

/** delete from stats list */
static void
stats_list_remove_elem(struct rc_state** list, struct rc_state* todel)
{
	while(*list) {
		if( (*list) == todel) {
			*list = (*list)->stats_next;
			return;
		}
		list = &(*list)->stats_next;
	}
}

/** decrease active count and remove commpoint from busy list */
static void
clean_point(netio_type* netio, struct daemon_remote* rc, struct rc_state* s)
{
	if(s->in_stats_list)
		stats_list_remove_elem(&rc->stats_list, s);
	state_list_remove_elem(&rc->busy_list, s);
	rc->active --;
	if(s->ssl) {
		SSL_shutdown(s->ssl);
		SSL_free(s->ssl);
	}
	netio_remove_handler(netio, s->c);
	close(s->c->fd);
	free(s->c);
	free(s);
}

static int
ssl_print_text(SSL* ssl, const char* text)
{
	int r;
	if(!ssl) 
		return 0;
	ERR_clear_error();
	if((r=SSL_write(ssl, text, (int)strlen(text))) <= 0) {
		if(SSL_get_error(ssl, r) == SSL_ERROR_ZERO_RETURN) {
			VERBOSITY(2, (LOG_WARNING, "in SSL_write, peer "
				"closed connection"));
			return 0;
		}
		log_crypto_err("could not SSL_write");
		return 0;
	}
	return 1;
}

/** print text over the ssl connection */
static int
ssl_print_vmsg(SSL* ssl, const char* format, va_list args)
{
	char msg[1024];
	vsnprintf(msg, sizeof(msg), format, args);
	return ssl_print_text(ssl, msg);
}

/** printf style printing to the ssl connection */
static int ssl_printf(SSL* ssl, const char* format, ...)
{
	va_list args;
	int ret;
	va_start(args, format);
	ret = ssl_print_vmsg(ssl, format, args);
	va_end(args);
	return ret;
}

static int
ssl_read_line(SSL* ssl, char* buf, size_t max)
{
	int r;
	size_t len = 0;
	if(!ssl)
		return 0;
	while(len < max) {
		ERR_clear_error();
		if((r=SSL_read(ssl, buf+len, 1)) <= 0) {
			if(SSL_get_error(ssl, r) == SSL_ERROR_ZERO_RETURN) {
				buf[len] = 0;
				return 1;
			}
			log_crypto_err("could not SSL_read");
			return 0;
		}
		if(buf[len] == '\n') {
			/* return string without \n */
			buf[len] = 0;
			return 1;
		}
		len++;
	}
	buf[max-1] = 0;
	log_msg(LOG_ERR, "control line too long (%d): %s", (int)max, buf);
	return 0;
}

/** skip whitespace, return new pointer into string */
static char*
skipwhite(char* str)
{
	/* EOS \0 is not a space */
	while( isspace(*str) ) 
		str++;
	return str;
}

/** send the OK to the control client */
static void send_ok(SSL* ssl)
{
	(void)ssl_printf(ssl, "ok\n");
}

/** get zone argument (if any) or NULL, false on error */
static int get_zone_arg(SSL* ssl, xfrd_state_t* xfrd, char* arg,
	zone_options_t** zo)
{
	const dname_type* dname;
	if(!arg[0]) {
		/* no argument present, return NULL */
		*zo = NULL;
		return 1;
	}
	dname = dname_parse(xfrd->region, arg);
	if(!dname) {
		ssl_printf(ssl, "error cannot parse zone name '%s'\n", arg);
		*zo = NULL;
		return 0;
	}
	*zo = zone_options_find(xfrd->nsd->options, dname);
	region_recycle(xfrd->region, (void*)dname, dname_total_size(dname));
	if(!*zo) {
		ssl_printf(ssl, "error zone %s not configured\n", arg);
		return 0;
	}
	return 1;
}

/** do the stop command */
static void
do_stop(SSL* ssl, xfrd_state_t* xfrd)
{
	xfrd->need_to_send_shutdown = 1;
	xfrd->ipc_handler.event_types |= NETIO_EVENT_WRITE;
	send_ok(ssl);
}

/** do the log_reopen command, it only needs reload_now */
static void
do_log_reopen(SSL* ssl, xfrd_state_t* xfrd)
{
	xfrd_set_reload_now(xfrd);
	send_ok(ssl);
}

/** do the reload command */
static void
do_reload(SSL* ssl, xfrd_state_t* xfrd, char* arg)
{
	zone_options_t* zo;
	if(!get_zone_arg(ssl, xfrd, arg, &zo))
		return;
	task_new_check_zonefiles(xfrd->nsd->task[xfrd->nsd->mytask],
		xfrd->last_task, (const dname_type*)zo->node.key);
	xfrd_set_reload_now(xfrd);
	send_ok(ssl);
}

/** do the verbosity command */
static void
do_verbosity(SSL* ssl, char* str)
{
	int val = atoi(str);
	if(val == 0 && strcmp(str, "0") != 0) {
		ssl_printf(ssl, "error in verbosity number syntax: %s\n", str);
		return;
	}
	verbosity = val;
	task_new_set_verbosity(xfrd->nsd->task[xfrd->nsd->mytask],
		xfrd->last_task, val);
	xfrd_set_reload_now(xfrd);
	send_ok(ssl);
}

/** find second argument, modifies string */
static int
find_arg2(SSL* ssl, char* arg, char** arg2)
{
	char* as = strrchr(arg, ' ');
	if(as) {
		as[0]=0;
		*arg2 = as+1;
		while(isspace(*as) && as > arg)
			as--;
		as[0]=0;
		return 1;
	}
	ssl_printf(ssl, "error could not find next argument "
		"after %s\n", arg);
	return 0;
}

/** do the status command */
static void
do_status(SSL* ssl)
{
	if(!ssl_printf(ssl, "version: %s\n", PACKAGE_VERSION))
		return;
	if(!ssl_printf(ssl, "verbosity: %d\n", verbosity))
		return;
}

/** do the stats command */
static void
do_stats(struct daemon_remote* rc, int peek, struct rc_state* rs)
{
#ifdef BIND8_STATS
	/* queue up to get stats after a reload is done (to gather statistics
	 * from the servers) */
	assert(!rs->in_stats_list);
	if(peek) rs->in_stats_list = 2;
	else	rs->in_stats_list = 1;
	rs->stats_next = rc->stats_list;
	rc->stats_list = rs;
	/* block the tcp waiting for the reload */
	rs->c->event_types = NETIO_EVENT_NONE;
	/* force a reload */
	xfrd_set_reload_now(xfrd);
#else
	(void)rc; (void)peek;
	(void)ssl_printf(rs->ssl, "error no stats enabled at compile time\n");
#endif /* BIND8_STATS */
}

/** do the addzone command */
static void
do_addzone(SSL* ssl, xfrd_state_t* xfrd, char* arg)
{
	const dname_type* dname;
	zone_options_t* zopt;
	char* arg2 = NULL;
	if(!find_arg2(ssl, arg, &arg2))
		return;

	/* if we add it to the xfrd now, then xfrd could download AXFR and
	 * store it and the NSD-reload would see it in the difffile before
	 * it sees the add-config task.
	 */
	/* thus: AXFRs and IXFRs must store the pattern name in the
	 * difffile, so that it can be added when the AXFR or IXFR is seen.
	 */

	/* check that the pattern exists */
	if(!rbtree_search(xfrd->nsd->options->patterns, arg2)) {
		(void)ssl_printf(ssl, "error pattern does not exist\n");
		return;
	}

	/* attempt to parse zone name and refuse if not possible */
	dname = dname_parse(xfrd->region, arg);
	if(!dname) {
		(void)ssl_printf(ssl, "error cannot parse zone name\n");
		return;
	}

	/* add to zonelist and adds to config in memory */
	zopt = zone_list_add(xfrd->nsd->options, arg, arg2);
	if(!zopt) {
		region_recycle(xfrd->region, (void*)dname,
			dname_total_size(dname));
		(void)ssl_printf(ssl, "error could not add zonelist entry\n");
		return;
	}
	/* make addzone task and schedule reload */
	task_new_add_zone(xfrd->nsd->task[xfrd->nsd->mytask],
		xfrd->last_task, arg, arg2);
	xfrd_set_reload_now(xfrd);
	/* add to xfrd - notify (for master and slaves) */
	init_notify_send(xfrd->notify_zones, xfrd->netio, xfrd->region,
		dname, zopt);
	/* add to xfrd - slave */
	if(zone_is_slave(zopt)) {
		xfrd_init_slave_zone(xfrd, dname, zopt);
	}

	send_ok(ssl);
}

/** do the delzone command */
static void
do_delzone(SSL* ssl, xfrd_state_t* xfrd, char* arg)
{
	const dname_type* dname;
	zone_options_t* zopt;

	dname = dname_parse(xfrd->region, arg);
	if(!dname) {
		(void)ssl_printf(ssl, "error cannot parse zone name\n");
		return;
	}

	/* see if we have the zone in question */
	zopt = zone_options_find(xfrd->nsd->options, dname);
	if(!zopt) {
		region_recycle(xfrd->region, (void*)dname,
			dname_total_size(dname));
		/* nothing to do */
		if(!ssl_printf(ssl, "warning zone %s not present\n", arg))
			return;
		send_ok(ssl);
		return;
	}

	/* see if it can be deleted */
	if(zopt->part_of_config) {
		region_recycle(xfrd->region, (void*)dname,
			dname_total_size(dname));
		(void)ssl_printf(ssl, "error zone defined in nsd.conf, "
			"cannot delete it\n");
		return;
	}

	/* create deletion task */
	task_new_del_zone(xfrd->nsd->task[xfrd->nsd->mytask],
		xfrd->last_task, dname);
	xfrd_set_reload_now(xfrd);
	/* delete it in xfrd */
	if(zone_is_slave(zopt)) {
		xfrd_del_slave_zone(xfrd, dname);
	}
	xfrd_del_notify(xfrd, dname);
	/* delete from config */
	zone_list_del(xfrd->nsd->options, zopt);

	region_recycle(xfrd->region, (void*)dname, dname_total_size(dname));
	send_ok(ssl);
}

/** check for name with end-of-string, space or tab after it */
static int
cmdcmp(char* p, const char* cmd, size_t len)
{
	return strncmp(p,cmd,len)==0 && (p[len]==0||p[len]==' '||p[len]=='\t');
}

/** execute a remote control command */
static void
execute_cmd(struct daemon_remote* rc, SSL* ssl, char* cmd, struct rc_state* rs)
{
	char* p = skipwhite(cmd);
	/* compare command */
	if(cmdcmp(p, "stop", 4)) {
		do_stop(ssl, rc->xfrd);
	} else if(cmdcmp(p, "reload", 6)) {
		do_reload(ssl, rc->xfrd, skipwhite(p+6));
	} else if(cmdcmp(p, "status", 6)) {
		do_status(ssl);
	} else if(cmdcmp(p, "stats_noreset", 13)) {
		do_stats(rc, 1, rs);
	} else if(cmdcmp(p, "stats", 5)) {
		do_stats(rc, 0, rs);
	} else if(cmdcmp(p, "log_reopen", 10)) {
		do_log_reopen(ssl, rc->xfrd);
	} else if(cmdcmp(p, "addzone", 7)) {
		do_addzone(ssl, rc->xfrd, skipwhite(p+7));
	} else if(cmdcmp(p, "delzone", 7)) {
		do_delzone(ssl, rc->xfrd, skipwhite(p+7));
	} else if(cmdcmp(p, "verbosity", 9)) {
		do_verbosity(ssl, skipwhite(p+9));
	} else {
		(void)ssl_printf(ssl, "error unknown command '%s'\n", p);
	}
}

/** handle remote control request */
static void
handle_req(struct daemon_remote* rc, struct rc_state* s, SSL* ssl)
{
	int r;
	char pre[10];
	char magic[8];
	char buf[1024];
	if (fcntl(s->c->fd, F_SETFL, 0) == -1) { /* set blocking */
		log_msg(LOG_ERR, "cannot fcntl rc: %s", strerror(errno));
	}

	/* try to read magic UBCT[version]_space_ string */
	ERR_clear_error();
	if((r=SSL_read(ssl, magic, (int)sizeof(magic)-1)) <= 0) {
		if(SSL_get_error(ssl, r) == SSL_ERROR_ZERO_RETURN)
			return;
		log_crypto_err("could not SSL_read");
		return;
	}
	magic[7] = 0;
	if( r != 7 || strncmp(magic, "NSDCT", 5) != 0) {
		VERBOSITY(2, (LOG_INFO, "control connection has bad header"));
		/* probably wrong tool connected, ignore it completely */
		return;
	}

	/* read the command line */
	if(!ssl_read_line(ssl, buf, sizeof(buf))) {
		return;
	}
	snprintf(pre, sizeof(pre), "NSDCT%d ", NSD_CONTROL_VERSION);
	if(strcmp(magic, pre) != 0) {
		VERBOSITY(2, (LOG_INFO, "control connection had bad "
			"version %s, cmd: %s", magic, buf));
		ssl_printf(ssl, "error version mismatch\n");
		return;
	}
	VERBOSITY(2, (LOG_INFO, "control cmd: %s", buf));

	/* figure out what to do */
	execute_cmd(rc, ssl, buf, s);
}

static void
remote_control_callback(netio_type* ATTR_UNUSED(netio),
	netio_handler_type *handler, netio_event_types_type event_types)
{
	struct rc_state* s = (struct rc_state*)handler->user_data;
	struct daemon_remote* rc = s->rc;
	int r;
	if( (event_types&NETIO_EVENT_TIMEOUT) ) {
		log_msg(LOG_ERR, "remote control timed out");
		clean_point(netio, rc, s);
		return;
	}
	/* (continue to) setup the SSL connection */
	ERR_clear_error();
	r = SSL_do_handshake(s->ssl);
	if(r != 1) {
		int r2 = SSL_get_error(s->ssl, r);
		if(r2 == SSL_ERROR_WANT_READ) {
			if(s->shake_state == rc_hs_read) {
				/* try again later */
				return;
			}
			s->shake_state = rc_hs_read;
			handler->event_types =
				NETIO_EVENT_READ|NETIO_EVENT_TIMEOUT;
			return;
		} else if(r2 == SSL_ERROR_WANT_WRITE) {
			if(s->shake_state == rc_hs_write) {
				/* try again later */
				return;
			}
			s->shake_state = rc_hs_write;
			handler->event_types =
				NETIO_EVENT_WRITE|NETIO_EVENT_TIMEOUT;
			return;
		} else {
			if(r == 0)
				log_msg(LOG_ERR, "remote control connection closed prematurely");
			log_crypto_err("remote control failed ssl");
			clean_point(netio, rc, s);
			return;
		}
	}
	s->shake_state = rc_none;

	/* once handshake has completed, check authentication */
	if(SSL_get_verify_result(s->ssl) == X509_V_OK) {
		X509* x = SSL_get_peer_certificate(s->ssl);
		if(!x) {
			VERBOSITY(2, (LOG_INFO, "remote control connection "
				"provided no client certificate"));
			clean_point(netio, rc, s);
			return;
		}
		VERBOSITY(3, (LOG_INFO, "remote control connection authenticated"));
		X509_free(x);
	} else {
		VERBOSITY(2, (LOG_INFO, "remote control connection failed to "
			"authenticate with client certificate"));
		clean_point(netio, rc, s);
		return;
	}

	/* if OK start to actually handle the request */
	handle_req(rc, s, s->ssl);

	if(!s->in_stats_list) {
		VERBOSITY(3, (LOG_INFO, "remote control operation completed"));
		clean_point(netio, rc, s);
	}
}

#ifdef BIND8_STATS
static const char*
opcode2str(int o)
{
	switch(o) {
		case OPCODE_QUERY: return "QUERY";
		case OPCODE_IQUERY: return "IQUERY";
		case OPCODE_STATUS: return "STATUS";
		case OPCODE_NOTIFY: return "NOTIFY";
		case OPCODE_UPDATE: return "UPDATE";
		default: return "OTHER";
	}
}

/** print long number */
static int
print_longnum(SSL* ssl, char* desc, uint64_t x)
{
	if(x > (uint64_t)1024*1024*1024) {
		/* more than a Gb */
		size_t front = (size_t)(x / (uint64_t)1000000);
		size_t back = (size_t)(x % (uint64_t)1000000);
		return ssl_printf(ssl, "%s%u%6.6u\n", desc, 
			(unsigned)front, (unsigned)back);
	} else {
		return ssl_printf(ssl, "%s%u\n", desc, (unsigned)x);
	}
}

static void
print_stats(SSL* ssl, xfrd_state_t* xfrd, struct timeval* now)
{
	const char* rcstr[] = {"NOERROR", "FORMERR", "SERVFAIL", "NXDOMAIN",
	    "NOTIMP", "REFUSED", "YXDOMAIN", "YXRRSET", "NXRRSET", "NOTAUTH",
	    "NOTZONE", "RCODE11", "RCODE12", "RCODE13", "RCODE14", "RCODE15",
	    "BADVERS"
	};
	size_t i;
	stc_t total = 0;
	struct timeval elapsed, uptime;

	/* per CPU and total */
	for(i=0; i<xfrd->nsd->child_count; i++) {
		if(!ssl_printf(ssl, "server%d.queries=%u\n", (int)i,
			(unsigned)xfrd->nsd->children[i].query_count))
			return;
		total += xfrd->nsd->children[i].query_count;
	}
	if(!ssl_printf(ssl, "num.queries=%u\n", (unsigned)total))
		return;

	/* time elapsed and uptime (in seconds) */
	timeval_subtract(&uptime, now, &xfrd->nsd->rc->boot_time);
	timeval_subtract(&elapsed, now, &xfrd->nsd->rc->stats_time);
	if(!ssl_printf(ssl, "time.boot=%u.%6.6u\n",
		(unsigned)uptime.tv_sec, (unsigned)uptime.tv_usec))
		return;
	if(!ssl_printf(ssl, "time.elapsed=%u.%6.6u\n",
		(unsigned)elapsed.tv_sec, (unsigned)elapsed.tv_usec))
		return;

	/* mem info, database on disksize */
	if(!print_longnum(ssl, "size.db.disk=", xfrd->nsd->st.db_disk))
		return;
	if(!print_longnum(ssl, "size.db.mem=", xfrd->nsd->st.db_mem))
		return;
	if(!print_longnum(ssl, "size.xfrd.mem=", region_get_mem(xfrd->region)))
		return;
	if(!print_longnum(ssl, "size.config.disk=", 
		xfrd->nsd->options->zonelist_off))
		return;
	if(!print_longnum(ssl, "size.config.mem=", region_get_mem(
		xfrd->nsd->options->region)))
		return;

	for(i=0; i<= 255; i++) {
		if(inhibit_zero && xfrd->nsd->st.qtype[i] == 0 &&
			strncmp(rrtype_to_string(i), "TYPE", 4) == 0)
			continue;
		if(!ssl_printf(ssl, "num.type.%s=%u\n", 
			rrtype_to_string(i), (unsigned)xfrd->nsd->st.qtype[i]))
			return;
	}

	/* opcode */
	for(i=0; i<6; i++) {
		if(inhibit_zero && xfrd->nsd->st.opcode[i] == 0 &&
			i != OPCODE_QUERY)
			continue;
		if(!ssl_printf(ssl, "num.opcode.%s=%u\n", opcode2str(i),
			(unsigned)xfrd->nsd->st.opcode[i]))
			return;
	}

	/* qclass */
	for(i=0; i<4; i++) {
		if(inhibit_zero && xfrd->nsd->st.qclass[i] == 0 &&
			i != CLASS_IN)
			continue;
		if(!ssl_printf(ssl, "num.class.%s=%u\n", rrclass_to_string(i),
			(unsigned)xfrd->nsd->st.qclass[i]))
			return;
	}

	/* rcode */
	for(i=0; i<17; i++) {
		if(inhibit_zero && xfrd->nsd->st.rcode[i] == 0 &&
			i > RCODE_YXDOMAIN) /* NSD does not use larger */
			continue;
		if(!ssl_printf(ssl, "num.rcode.%s=%u\n", rcstr[i],
			(unsigned)xfrd->nsd->st.rcode[i]))
			return;
	}

	/* edns */
	if(!ssl_printf(ssl, "num.edns=%u\n", (unsigned)xfrd->nsd->st.edns))
		return;

	/* ednserr */
	if(!ssl_printf(ssl, "num.ednserr=%u\n",
		(unsigned)xfrd->nsd->st.ednserr))
		return;

	/* qudp */
	if(!ssl_printf(ssl, "num.udp=%u\n", (unsigned)xfrd->nsd->st.qudp))
		return;
	/* qudp6 */
	if(!ssl_printf(ssl, "num.udp6=%u\n", (unsigned)xfrd->nsd->st.qudp6))
		return;
	/* ctcp */
	if(!ssl_printf(ssl, "num.tcp=%u\n", (unsigned)xfrd->nsd->st.ctcp))
		return;
	/* ctcp6 */
	if(!ssl_printf(ssl, "num.tcp6=%u\n", (unsigned)xfrd->nsd->st.ctcp6))
		return;

	/* nona */
	if(!ssl_printf(ssl, "num.answer_wo_aa=%u\n",
		(unsigned)xfrd->nsd->st.nona))
		return;

	/* rxerr */
	if(!ssl_printf(ssl, "num.rxerr=%u\n", (unsigned)xfrd->nsd->st.rxerr))
		return;

	/* txerr */
	if(!ssl_printf(ssl, "num.txerr=%u\n", (unsigned)xfrd->nsd->st.txerr))
		return;

	/* number of requested-axfr, number of times axfr served to clients */
	if(!ssl_printf(ssl, "num.raxfr=%u\n", (unsigned)xfrd->nsd->st.raxfr))
		return;

	/* truncated */
	if(!ssl_printf(ssl, "num.truncated=%u\n",
		(unsigned)xfrd->nsd->st.truncated))
		return;

	/* dropped */
	if(!ssl_printf(ssl, "num.dropped=%u\n",
		(unsigned)xfrd->nsd->st.dropped))
		return;

	/* zone statistics */
	if(!ssl_printf(ssl, "zone.master=%u\n",
		(unsigned)(xfrd->notify_zones->count - xfrd->zones->count)))
		return;
	if(!ssl_printf(ssl, "zone.slave=%u\n", (unsigned)xfrd->zones->count))
		return;
}

static void
clear_stats(xfrd_state_t* xfrd)
{
	size_t i;
	uint64_t dbd = xfrd->nsd->st.db_disk;
	uint64_t dbm = xfrd->nsd->st.db_mem;
	for(i=0; i<xfrd->nsd->child_count; i++) {
		xfrd->nsd->children[i].query_count = 0;
	}
	memset(&xfrd->nsd->st, 0, sizeof(struct nsdst));
	xfrd->nsd->st.db_disk = dbd;
	xfrd->nsd->st.db_mem = dbm;
}

void daemon_remote_process_stats(struct daemon_remote* rc)
{
	struct rc_state* s;
	struct timeval now;
	if(!rc) return;
	if(gettimeofday(&now, NULL) == -1)
		log_msg(LOG_ERR, "gettimeofday: %s", strerror(errno));
	/* pop one and give it stats */
	while((s = rc->stats_list)) {
		assert(s->in_stats_list);
		print_stats(s->ssl, rc->xfrd, &now);
		if(s->in_stats_list == 1) {
			clear_stats(rc->xfrd);
			rc->stats_time = now;
		}
		VERBOSITY(3, (LOG_INFO, "remote control stats printed"));
		rc->stats_list = s->next;
		s->in_stats_list = 0;
		clean_point(rc->xfrd->netio, rc, s);
	}
}
#endif /* BIND8_STATS */

#endif /* HAVE_SSL */

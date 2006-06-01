/*
 * xfrd.c - XFR (transfer) Daemon source file. Coordinates SOA updates.
 *
 * Copyright (c) 2001-2006, NLnet Labs. All rights reserved.
 *
 * See LICENSE for the license.
 *
 */

#include <config.h>
#include <assert.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include "xfrd.h"
#include "xfrd-tcp.h"
#include "xfrd-disk.h"
#include "options.h"
#include "util.h"
#include "netio.h"
#include "region-allocator.h"
#include "nsd.h"
#include "packet.h"
#include "difffile.h"
#include "ipc.h"

#define XFRD_TRANSFER_TIMEOUT 10 /* empty zone timeout is between x and 2*x seconds */
#define XFRD_TCP_TIMEOUT TCP_TIMEOUT /* seconds, before a tcp connectin is stopped */
#define XFRD_UDP_TIMEOUT 10 /* seconds, before a udp request times out */
#define XFRD_LOWERBOUND_REFRESH 1 /* seconds, smallest refresh timeout */
#define XFRD_LOWERBOUND_RETRY 1 /* seconds, smallest retry timeout */
#define XFRD_MAX_ROUNDS 3 /* number of rounds along the masters */
#define XFRD_TSIG_MAX_UNSIGNED 103 /* max number of packets without tsig in a tcp stream. */
			/* rfc recommends 100, +3 for offbyone errors/interoperability. */
#define XFRD_NOTIFY_RETRY_TIMOUT 15 /* seconds between retries sending NOTIFY */
#define XFRD_NOTIFY_MAX_NUM 5 /* number of attempts to send NOTIFY */

/* the daemon state */
static xfrd_state_t* xfrd = 0;

/* main xfrd loop */
static void xfrd_main();
/* shut down xfrd, close sockets. */
static void xfrd_shutdown();
/* create zone rbtree at start */
static void xfrd_init_zones();
/* free up memory used by main database */
static void xfrd_free_namedb();

/* handle zone timeout, event */
static void xfrd_handle_zone(netio_type *netio, 
	netio_handler_type *handler, netio_event_types_type event_types);
/* handle incoming notification message. soa can be NULL. true if transfer needed. */
static int xfrd_handle_incoming_notify(xfrd_zone_t* zone, xfrd_soa_t* soa);
/* handle zone notify send */
static void xfrd_handle_notify_send(netio_type *netio, 
	netio_handler_type *handler, netio_event_types_type event_types);

/* call with buffer just after the soa dname. returns 0 on error. */
static int xfrd_parse_soa_info(buffer_type* packet, xfrd_soa_t* soa);
/* copy SOA info from rr to soa struct. */
static void xfrd_copy_soa(xfrd_soa_t* soa, rr_type* rr);
/* set the zone state to a new state (takes care of expiry messages) */
static void xfrd_set_zone_state(xfrd_zone_t* zone, enum xfrd_zone_state new_zone_state);
/* set timer for retry amount (depends on zone_state) */
static void xfrd_set_timer_retry(xfrd_zone_t* zone);
/* set timer for refresh timeout (depends on zone_state) */
static void xfrd_set_timer_refresh(xfrd_zone_t* zone);

/* set reload timeout */
static void xfrd_set_reload_timeout();
/* handle reload timeout */
static void xfrd_handle_reload(netio_type *netio, 
	netio_handler_type *handler, netio_event_types_type event_types);

/* send notifications to all in the notify list */
static void xfrd_send_notify(xfrd_zone_t* zone);
/* send expiry notifications to nsd */
static void xfrd_send_expire_notification(xfrd_zone_t* zone);
/* send ixfr request, returns fd of connection to read on */
static int xfrd_send_ixfr_request_udp(xfrd_zone_t* zone);

/* send packet via udp (returns UDP fd source socket) to acl addr. -1 on failure. */
static int xfrd_send_udp(acl_options_t* acl, buffer_type* packet);
/* read data via udp */
static void xfrd_udp_read(xfrd_zone_t* zone);
/* read from udp port packet into buffer, 0 on failure */
static int xfrd_udp_read_packet(buffer_type* packet, int fd);

/* find master by notify number */
static int find_same_master_notify(xfrd_zone_t* zone, int acl_num_nfy);

void 
xfrd_init(int socket, struct nsd* nsd)
{
	region_type* region;

	assert(xfrd == 0);
	/* to setup signalhandling */
	nsd->server_kind = NSD_SERVER_BOTH;

	region = region_create(xalloc, free);
	xfrd = (xfrd_state_t*)region_alloc(region, sizeof(xfrd_state_t));
	memset(xfrd, 0, sizeof(xfrd_state_t));
	xfrd->region = region;
	xfrd->xfrd_start_time = time(0);
	xfrd->netio = netio_create(xfrd->region);
	xfrd->nsd = nsd;
	xfrd->packet = buffer_create(xfrd->region, QIOBUFSZ);
	xfrd->ipc_pass = buffer_create(xfrd->region, QIOBUFSZ);
	xfrd->parent_soa_info_pass = 0;

	/* add the handlers already, because this involves allocs */
	xfrd->reload_handler.fd = -1;
	xfrd->reload_handler.timeout = NULL;
	xfrd->reload_handler.user_data = xfrd;
	xfrd->reload_handler.event_types = NETIO_EVENT_TIMEOUT;
	xfrd->reload_handler.event_handler = xfrd_handle_reload;
	netio_add_handler(xfrd->netio, &xfrd->reload_handler);
	xfrd->reload_timeout.tv_sec = 0;
	xfrd->reload_cmd_last_sent = xfrd->xfrd_start_time;

	xfrd->ipc_handler.fd = socket;
	xfrd->ipc_handler.timeout = NULL;
	xfrd->ipc_handler.user_data = xfrd;
	xfrd->ipc_handler.event_types = NETIO_EVENT_READ;
	xfrd->ipc_handler.event_handler = xfrd_handle_ipc;
	xfrd->ipc_conn = xfrd_tcp_create(xfrd->region);
	xfrd->ipc_conn->is_reading = 0; /* not reading using ipc_conn yet */
	xfrd->ipc_conn->fd = xfrd->ipc_handler.fd;
	xfrd->ipc_conn_write = xfrd_tcp_create(xfrd->region);
	xfrd->ipc_conn_write->fd = xfrd->ipc_handler.fd;
	xfrd->need_to_send_reload = 0;
	xfrd->sending_zone_state = 0;
	xfrd->dirty_zones = stack_create(xfrd->region, 
		nsd_options_num_zones(nsd->options));
	netio_add_handler(xfrd->netio, &xfrd->ipc_handler);

	xfrd->tcp_set = xfrd_tcp_set_create(xfrd->region);

	log_msg(LOG_INFO, "xfrd pre-startup");
	diff_snip_garbage(nsd->db, nsd->options);
	xfrd_init_zones();
	xfrd_free_namedb();
	xfrd_read_state(xfrd);
	xfrd_send_expy_all_zones();

	log_msg(LOG_INFO, "xfrd startup");
	xfrd_main();
}

static void 
xfrd_main()
{
	xfrd->shutdown = 0;
	while(!xfrd->shutdown)
	{
		/* dispatch may block for a longer period, so current is gone */
		xfrd->got_time = 0;
		if(netio_dispatch(xfrd->netio, NULL, 0) == -1) {
			if (errno != EINTR) {
				log_msg(LOG_ERR, 
					"xfrd netio_dispatch failed: %s", 
					strerror(errno));
			}
		}
		if(xfrd->nsd->signal_hint_quit || xfrd->nsd->signal_hint_shutdown)
			xfrd->shutdown = 1;
	}
	xfrd_shutdown();
}

static void 
xfrd_shutdown()
{
	xfrd_zone_t* zone;
	int i;

	log_msg(LOG_INFO, "xfrd shutdown");
	xfrd_write_state(xfrd);
	close(xfrd->ipc_handler.fd);
	/* close tcp sockets */
	for(i=0; i<XFRD_MAX_TCP; i++)
	{
		if(xfrd->tcp_set->tcp_state[i]->fd != -1) {
			close(xfrd->tcp_set->tcp_state[i]->fd);
			xfrd->tcp_set->tcp_state[i]->fd = -1;
		}
	}
	/* close udp sockets */
	RBTREE_FOR(zone, xfrd_zone_t*, xfrd->zones)
	{
		if(zone->tcp_conn==-1 && zone->zone_handler.fd != -1) {
			close(zone->zone_handler.fd);
			zone->zone_handler.fd = -1;
		}
		if(zone->notify_send_handler.fd != -1) {
			close(zone->notify_send_handler.fd);
			zone->notify_send_handler.fd = -1;
		}
	}
	exit(0);
}

static void 
xfrd_init_zones()
{
	zone_type *dbzone;
	zone_options_t *zone_opt;
	xfrd_zone_t *xzone;
	const dname_type* dname;

	assert(xfrd->zones == 0);
	assert(xfrd->nsd->db != 0);

	xfrd->zones = rbtree_create(xfrd->region, 
		(int (*)(const void *, const void *)) dname_compare);
	
	RBTREE_FOR(zone_opt, zone_options_t*, xfrd->nsd->options->zone_options)
	{
		log_msg(LOG_INFO, "Zone %s\n", zone_opt->name);
		if(!zone_is_slave(zone_opt)) {
			log_msg(LOG_INFO, "xfrd: zone %s, master zone has no outgoing xfr requests", zone_opt->name);
			continue;
		}

		dname = dname_parse(xfrd->region, zone_opt->name);
		if(!dname) {
			log_msg(LOG_ERR, "xfrd: Could not parse zone name %s.", zone_opt->name);
			continue;
		}

		dbzone = domain_find_zone(domain_table_find(xfrd->nsd->db->domains, dname));
		if(dbzone && dname_compare(dname, domain_dname(dbzone->apex)) != 0)
			dbzone = 0; /* we found a parent zone */
		if(!dbzone)
			log_msg(LOG_INFO, "xfrd: adding empty zone %s\n", zone_opt->name);
		else log_msg(LOG_INFO, "xfrd: adding filled zone %s\n", zone_opt->name);
		
		xzone = (xfrd_zone_t*)region_alloc(xfrd->region, sizeof(xfrd_zone_t));
		memset(xzone, 0, sizeof(xfrd_zone_t));
		xzone->apex = dname;
		xzone->apex_str = zone_opt->name;
		xzone->state = xfrd_zone_expired;
		xzone->dirty = 0;
		xzone->zone_options = zone_opt;
		xzone->master = 0; /* first retry will use first master */
		xzone->master_num = 0;
		xzone->next_master = 0;

		xzone->soa_nsd_acquired = 0;
		xzone->soa_disk_acquired = 0;
		xzone->soa_notified_acquired = 0;
		xzone->soa_nsd.prim_ns[0] = 1; /* [0]=1, [1]=0; "." domain name */
		xzone->soa_nsd.email[0] = 1;
		xzone->soa_disk.prim_ns[0]=1;
		xzone->soa_disk.email[0]=1;
		xzone->soa_notified.prim_ns[0]=1;
		xzone->soa_notified.email[0]=1;

		xzone->zone_handler.fd = -1;
		xzone->zone_handler.timeout = 0;
		xzone->zone_handler.user_data = xzone;
		xzone->zone_handler.event_types = NETIO_EVENT_READ|NETIO_EVENT_TIMEOUT;
		xzone->zone_handler.event_handler = xfrd_handle_zone;
		netio_add_handler(xfrd->netio, &xzone->zone_handler);
		xzone->tcp_waiting = 0;
		xzone->tcp_conn = -1;

		xzone->notify_send_handler.fd = -1;
		xzone->notify_send_handler.timeout = 0;
		xzone->notify_send_handler.user_data = xzone;
		xzone->notify_send_handler.event_types = NETIO_EVENT_READ|NETIO_EVENT_TIMEOUT;
		xzone->notify_send_handler.event_handler = xfrd_handle_notify_send;
		netio_add_handler(xfrd->netio, &xzone->notify_send_handler);

#ifdef TSIG
		tsig_create_record(&xzone->tsig, xfrd->region);
		tsig_create_record(&xzone->notify_tsig, xfrd->region);
#endif /* TSIG */
		
		if(dbzone && dbzone->soa_rrset && dbzone->soa_rrset->rrs) {
			xzone->soa_nsd_acquired = xfrd_time();
			xzone->soa_disk_acquired = xfrd_time();
			/* we only use the first SOA in the rrset */
			xfrd_copy_soa(&xzone->soa_nsd, dbzone->soa_rrset->rrs);
			xfrd_copy_soa(&xzone->soa_disk, dbzone->soa_rrset->rrs);
			/* set refreshing anyway, we have data but it may be old */
		}
		xfrd_set_refresh_now(xzone);

		xzone->node.key = dname;
		rbtree_insert(xfrd->zones, (rbnode_t*)xzone);
	}
	log_msg(LOG_INFO, "xfrd: started server %d secondary zones", (int)xfrd->zones->count);
}

void
xfrd_send_expy_all_zones()
{
	xfrd_zone_t* zone;
	RBTREE_FOR(zone, xfrd_zone_t*, xfrd->zones)
	{
		xfrd_send_expire_notification(zone);
	}
}

static void 
xfrd_free_namedb()
{
	namedb_close(xfrd->nsd->db);
	xfrd->nsd->db = 0;
}

static void 
xfrd_set_timer_refresh(xfrd_zone_t* zone)
{
	time_t set_refresh;
	time_t set_expire;
	time_t set_min;
	time_t set;
	if(zone->soa_disk_acquired == 0 || zone->state != xfrd_zone_ok) {
		xfrd_set_timer_retry(zone);
		return;
	}
	/* refresh or expire timeout, whichever is earlier */
	set_refresh = zone->soa_disk_acquired + ntohl(zone->soa_disk.refresh);
	set_expire = zone->soa_disk_acquired + ntohl(zone->soa_disk.expire);
	if(set_refresh < set_expire)
		set = set_refresh;
	else set = set_expire;
	set_min = zone->soa_disk_acquired + XFRD_LOWERBOUND_REFRESH;
	if(set < set_min)
		set = set_min;
	xfrd_set_timer(zone, set);
}

static void 
xfrd_set_timer_retry(xfrd_zone_t* zone)
{
	/* set timer for next retry or expire timeout if earlier. */
	if(zone->soa_disk_acquired == 0) {
		/* if no information, use reasonable timeout */
		xfrd_set_timer(zone, xfrd_time() + XFRD_TRANSFER_TIMEOUT
			+ random()%XFRD_TRANSFER_TIMEOUT);
	} else if(zone->state == xfrd_zone_expired ||
		xfrd_time() + ntohl(zone->soa_disk.retry) <
		zone->soa_disk_acquired + ntohl(zone->soa_disk.expire)) 
	{
		if(ntohl(zone->soa_disk.retry) < XFRD_LOWERBOUND_RETRY)
			xfrd_set_timer(zone, xfrd_time() + XFRD_LOWERBOUND_RETRY);
		else 	
			xfrd_set_timer(zone, xfrd_time() + ntohl(zone->soa_disk.retry));
	} else {
		if(ntohl(zone->soa_disk.expire) < XFRD_LOWERBOUND_RETRY)
			xfrd_set_timer(zone, xfrd_time() + XFRD_LOWERBOUND_RETRY);
		else
			xfrd_set_timer(zone, zone->soa_disk_acquired + 
				ntohl(zone->soa_disk.expire));
	}
}

static void 
xfrd_handle_zone(netio_type* ATTR_UNUSED(netio), 
	netio_handler_type *handler, netio_event_types_type event_types)
{
	xfrd_zone_t* zone = (xfrd_zone_t*)handler->user_data;

	if(zone->tcp_conn != -1) {
		/* busy in tcp transaction */
		if(xfrd_tcp_is_reading(xfrd->tcp_set, zone->tcp_conn) &&
			event_types & NETIO_EVENT_READ) { 
			xfrd_set_timer(zone, xfrd_time() + XFRD_TCP_TIMEOUT);
			xfrd_tcp_read(xfrd->tcp_set, zone); 
			return;
		} else if(!xfrd_tcp_is_reading(xfrd->tcp_set, zone->tcp_conn) &&
			event_types & NETIO_EVENT_WRITE) { 
			xfrd_set_timer(zone, xfrd_time() + XFRD_TCP_TIMEOUT);
			xfrd_tcp_write(xfrd->tcp_set, zone); 
			return;
		} else if(event_types & NETIO_EVENT_TIMEOUT) {
			/* tcp connection timed out. Stop it. */
			xfrd_tcp_release(xfrd->tcp_set, zone);
			/* continue to retry; as if a timeout happened */
			event_types = NETIO_EVENT_TIMEOUT;
		}
	}

	if(event_types & NETIO_EVENT_READ) {
		/* busy in udp transaction */
		log_msg(LOG_INFO, "xfrd: zone %s event udp read", zone->apex_str);
		xfrd_set_refresh_now(zone);
		xfrd_udp_read(zone);
		return;
	}

	/* timeout */
	log_msg(LOG_INFO, "xfrd: zone %s timeout", zone->apex_str);
	if(handler->fd != -1) {
		close(handler->fd);
		handler->fd = -1;
	}

	if(zone->tcp_waiting) {
		log_msg(LOG_ERR, "xfrd: zone %s skips retry, TCP connections full",
			zone->apex_str);
		xfrd_set_timer_retry(zone);
		return;
	}

	if(zone->soa_disk_acquired)
	{
		if(	zone->state != xfrd_zone_expired &&
			(uint32_t)xfrd_time() >=
			zone->soa_disk_acquired + ntohl(zone->soa_disk.expire))
		{
			/* zone expired */
			log_msg(LOG_ERR, "xfrd: zone %s has expired", zone->apex_str);
			xfrd_set_zone_state(zone, xfrd_zone_expired);
		}
		else if(zone->state == xfrd_zone_ok &&
			(uint32_t)xfrd_time() >=
			zone->soa_disk_acquired + ntohl(zone->soa_disk.refresh))
		{
			/* zone goes to refreshing state. */
			log_msg(LOG_INFO, "xfrd: zone %s is refreshing", zone->apex_str);
			xfrd_set_zone_state(zone, xfrd_zone_refreshing);
		}
	}
	/* make a new request */
	xfrd_make_request(zone);
}

void
xfrd_make_request(xfrd_zone_t* zone)
{
	/* cycle master */
	if(zone->next_master != -1) {
		zone->master_num = zone->next_master;
		zone->master = acl_find_num(
			zone->zone_options->request_xfr, zone->master_num);
		if(!zone->master) {
			zone->master = zone->zone_options->request_xfr;
			zone->master_num = 0;
		}
		zone->next_master = -1;
		zone->round_num = 0; /* fresh set of retries after notify */
	} else {
		if(zone->round_num != -1 && zone->master && 
			zone->master->next) {
			zone->master = zone->master->next;
			zone->master_num++;
		} else {
			zone->master = zone->zone_options->request_xfr;
			zone->master_num = 0;
			zone->round_num++;
		}
		if(zone->round_num >= XFRD_MAX_ROUNDS) {
			/* tried all servers that many times, wait */
			zone->round_num = -1;
			xfrd_set_timer_retry(zone);
			log_msg(LOG_INFO, "xfrd zone %s makereq wait_retry, rd %d mr %d nx %d", 
				zone->apex_str, zone->round_num, zone->master_num, zone->next_master);
			return;
		}
	}

	log_msg(LOG_INFO, "xfrd zone %s make request round %d mr %d nx %d", 
		zone->apex_str, zone->round_num, zone->master_num, zone->next_master);
	/* perform xfr request */
	if(zone->soa_disk_acquired == 0 || zone->master->use_axfr_only) {
		/* request axfr */
		xfrd_set_timer(zone, xfrd_time() + XFRD_TCP_TIMEOUT);
		xfrd_tcp_obtain(xfrd->tcp_set, zone);
	} else {
		/* request ixfr ; start by udp */
		xfrd_set_timer(zone, xfrd_time() + XFRD_UDP_TIMEOUT);
		zone->zone_handler.fd = xfrd_send_ixfr_request_udp(zone);
	}
}

time_t 
xfrd_time()
{
	if(!xfrd->got_time) {
		xfrd->current_time = time(0);
		xfrd->got_time = 1;
	}
	return xfrd->current_time;
}

/* stop sending notifies */
static void 
xfrd_notify_disable(xfrd_zone_t* zone)
{
	if(zone->notify_send_handler.fd != -1) {
		close(zone->notify_send_handler.fd);
	}
	zone->notify_current = 0;
	zone->notify_send_handler.fd = -1;
	zone->notify_send_handler.timeout = 0;
}

/* returns if the notify send is done for the notify_current acl */
static int 
xfrd_handle_notify_reply(xfrd_zone_t* zone, buffer_type* packet) 
{
	if((OPCODE(packet) != OPCODE_NOTIFY) ||
		(QR(packet) == 0)) {
		log_msg(LOG_ERR, "xfrd: zone %s: received bad notify reply opcode/flags",
			zone->apex_str);
		return 0;
	}
	/* we know it is OPCODE NOTIFY, QUERY_REPLY and for this zone */
	if(ID(packet) != zone->notify_query_id) {
		log_msg(LOG_ERR, "xfrd: zone %s: received notify-ack with bad ID",
			zone->apex_str);
		return 0;
	}
	/* could check tsig, but why. The reply does not cause processing. */
	if(RCODE(packet) != RCODE_OK) {
		log_msg(LOG_ERR, "xfrd: zone %s: received notify response error %s from %s",
			zone->apex_str, rcode2str(RCODE(packet)),
			zone->notify_current->ip_address_spec);
		if(RCODE(packet) == RCODE_IMPL)
			return 1; /* rfc1996: notimpl notify reply: consider retries done */
		return 0;
	}
	log_msg(LOG_INFO, "xfrd: zone %s: host %s acknowledges notify",
		zone->apex_str, zone->notify_current->ip_address_spec);
	return 1;
}

static void
xfrd_notify_next(xfrd_zone_t* zone)
{
	/* advance to next in acl */
	zone->notify_current = zone->notify_current->next;
	zone->notify_retry = 0;
	if(zone->notify_current == 0) {
		log_msg(LOG_INFO, "xfrd: zone %s: no more notify-send acls. stop notify.", 
			zone->apex_str);
		xfrd_notify_disable(zone);
		return;
	}
}

static void 
xfrd_notify_send_udp(xfrd_zone_t* zone)
{
	if(zone->notify_send_handler.fd != -1)
		close(zone->notify_send_handler.fd);
	zone->notify_send_handler.fd = -1;
	/* Set timeout for next reply */
	zone->notify_timeout.tv_sec = xfrd_time() + XFRD_NOTIFY_RETRY_TIMOUT;
	/* send NOTIFY to secondary. */
	xfrd_setup_packet(xfrd->packet, TYPE_SOA, CLASS_IN, zone->apex);
	zone->notify_query_id = ID(xfrd->packet);
	OPCODE_SET(xfrd->packet, OPCODE_NOTIFY);
	AA_SET(xfrd->packet);
	if(zone->soa_nsd_acquired != 0) {
		/* add current SOA to answer section */
		ANCOUNT_SET(xfrd->packet, 1);
		xfrd_write_soa_buffer(xfrd->packet, zone, &zone->soa_nsd);
	}
#ifdef TSIG
	if(zone->notify_current->key_options) {
		xfrd_tsig_sign_request(xfrd->packet, &zone->notify_tsig, zone->notify_current);
	}
#endif /* TSIG */
	buffer_flip(xfrd->packet);
	zone->notify_send_handler.fd = xfrd_send_udp(zone->notify_current, xfrd->packet);
	if(zone->notify_send_handler.fd == -1) {
		log_msg(LOG_ERR, "xfrd: zone %s: could not send notify #%d to %s",
			zone->apex_str, zone->notify_retry,
			zone->notify_current->ip_address_spec);
		return;
	}
	log_msg(LOG_INFO, "xfrd: zone %s: sent notify #%d to %s",
		zone->apex_str, zone->notify_retry,
		zone->notify_current->ip_address_spec);
}

static void 
xfrd_handle_notify_send(netio_type* ATTR_UNUSED(netio), 
	netio_handler_type *handler, netio_event_types_type event_types)
{
	xfrd_zone_t* zone = (xfrd_zone_t*)handler->user_data;
	assert(zone->notify_current);
	if(event_types & NETIO_EVENT_READ) {
		log_msg(LOG_INFO, "xfrd: zone %s: read notify ACK", zone->apex_str);
		assert(handler->fd != -1);
		if(xfrd_udp_read_packet(xfrd->packet, zone->zone_handler.fd)) {
			if(xfrd_handle_notify_reply(zone, xfrd->packet))
				xfrd_notify_next(zone);
		}
	} else if(event_types & NETIO_EVENT_TIMEOUT) {
		log_msg(LOG_INFO, "xfrd: zone %s: notify timeout", zone->apex_str);
		zone->notify_retry++; /* timeout, try again */
		if(zone->notify_retry > XFRD_NOTIFY_MAX_NUM) {
			log_msg(LOG_ERR, "xfrd: zone %s: max notify send count reached, %s unreachable", 
				zone->apex_str, zone->notify_current->ip_address_spec);
			xfrd_notify_next(zone);
		}
	}
	/* see if notify is still enabled */
	if(zone->notify_current) {
		/* try again */
		xfrd_notify_send_udp(zone);
	}
}

static void 
xfrd_copy_soa(xfrd_soa_t* soa, rr_type* rr)
{
	const uint8_t* rr_ns_wire = dname_name(domain_dname(rdata_atom_domain(rr->rdatas[0])));
	uint8_t rr_ns_len = domain_dname(rdata_atom_domain(rr->rdatas[0]))->name_size;
	const uint8_t* rr_em_wire = dname_name(domain_dname(rdata_atom_domain(rr->rdatas[1])));
	uint8_t rr_em_len = domain_dname(rdata_atom_domain(rr->rdatas[1]))->name_size;

	if(rr->type != TYPE_SOA || rr->rdata_count != 7) {
		log_msg(LOG_ERR, "xfrd: copy_soa called with bad rr, type %d rrs %d.", 
			rr->type, rr->rdata_count);
		return;
	}
	log_msg(LOG_INFO, "xfrd: copy_soa rr, type %d rrs %d, ttl %d.", 
			rr->type, rr->rdata_count, rr->ttl);
	soa->type = htons(rr->type);
	soa->klass = htons(rr->klass);
	soa->ttl = htonl(rr->ttl);
	soa->rdata_count = htons(rr->rdata_count);
	
	/* copy dnames */
	soa->prim_ns[0] = rr_ns_len;
	memcpy(soa->prim_ns+1, rr_ns_wire, rr_ns_len);
	soa->email[0] = rr_em_len;
	memcpy(soa->email+1, rr_em_wire, rr_em_len);

	/* already in network format */
	memcpy(&soa->serial, rdata_atom_data(rr->rdatas[2]), sizeof(uint32_t));
	memcpy(&soa->refresh, rdata_atom_data(rr->rdatas[3]), sizeof(uint32_t));
	memcpy(&soa->retry, rdata_atom_data(rr->rdatas[4]), sizeof(uint32_t));
	memcpy(&soa->expire, rdata_atom_data(rr->rdatas[5]), sizeof(uint32_t));
	memcpy(&soa->minimum, rdata_atom_data(rr->rdatas[6]), sizeof(uint32_t));
	log_msg(LOG_INFO, "xfrd: copy_soa rr, serial %d refresh %d retry %d expire %d", 
			ntohl(soa->serial), ntohl(soa->refresh), ntohl(soa->retry),
			ntohl(soa->expire));
}

static void 
xfrd_set_zone_state(xfrd_zone_t* zone, enum xfrd_zone_state s)
{
	if(s != zone->state) {
		enum xfrd_zone_state old = zone->state;
		zone->state = s;
		if(s == xfrd_zone_expired || old == xfrd_zone_expired) {
			xfrd_send_expire_notification(zone);
		}
	}
}

void 
xfrd_set_refresh_now(xfrd_zone_t* zone) 
{
	xfrd_set_timer(zone, xfrd_time());
	log_msg(LOG_INFO, "xfrd zone %s sets timeout right now, state %d",
		zone->apex_str, zone->state);
}

void 
xfrd_set_timer(xfrd_zone_t* zone, time_t t)
{
	/* randomize the time, within 90%-100% of original */
	/* not later so zones cannot expire too late */
	/* only for times far in the future */
	if(t > xfrd_time() + 10) {
		time_t extra = t - xfrd_time();
		time_t base = extra*9/10;
		t = xfrd_time() + base + random()%(extra-base);
	}

	zone->zone_handler.timeout = &zone->timeout;
	zone->timeout.tv_sec = t;
	zone->timeout.tv_nsec = 0;
}

void 
xfrd_handle_incoming_soa(xfrd_zone_t* zone, 
	xfrd_soa_t* soa, time_t acquired)
{
	if(soa == NULL) {
		/* nsd no longer has a zone in memory */
		zone->soa_nsd_acquired = 0;
		xfrd_set_zone_state(zone, xfrd_zone_refreshing);
		xfrd_set_refresh_now(zone);
		return;
	}
	if(zone->soa_nsd_acquired && soa->serial == zone->soa_nsd.serial)
		return;

	if(zone->soa_disk_acquired && soa->serial == zone->soa_disk.serial)
	{
		/* soa in disk has been loaded in memory */
		log_msg(LOG_INFO, "Zone %s serial %d is updated to %d.",
			zone->apex_str, ntohl(zone->soa_nsd.serial),
			ntohl(soa->serial));
		zone->soa_nsd = zone->soa_disk;
		zone->soa_nsd_acquired = zone->soa_disk_acquired;
		if((uint32_t)xfrd_time() - zone->soa_disk_acquired 
			< ntohl(zone->soa_disk.refresh))
		{
			/* zone ok, wait for refresh time */
			xfrd_set_zone_state(zone, xfrd_zone_ok);
			zone->round_num = -1;
			xfrd_set_timer_refresh(zone);
		} else if((uint32_t)xfrd_time() - zone->soa_disk_acquired 
			< ntohl(zone->soa_disk.expire))
		{
			/* zone refreshing */
			xfrd_set_zone_state(zone, xfrd_zone_refreshing);
			xfrd_set_refresh_now(zone);
		} 
		if((uint32_t)xfrd_time() - zone->soa_disk_acquired
			>= ntohl(zone->soa_disk.expire)) {
			/* zone expired */
			xfrd_set_zone_state(zone, xfrd_zone_expired);
			xfrd_set_refresh_now(zone);
		}

		if(zone->soa_notified_acquired != 0 &&
			(zone->soa_notified.serial == 0 ||
		   	compare_serial(ntohl(zone->soa_disk.serial),
				ntohl(zone->soa_notified.serial)) >= 0))
		{	/* read was in response to this notification */
			zone->soa_notified_acquired = 0;
		}
		if(zone->soa_notified_acquired && zone->state == xfrd_zone_ok)
		{
			/* refresh because of notification */
			xfrd_set_zone_state(zone, xfrd_zone_refreshing);
			xfrd_set_refresh_now(zone);
		}
		xfrd_send_notify(zone);
		return;
	}

	/* user must have manually provided zone data */
	log_msg(LOG_INFO, "xfrd: zone %s serial %d from unknown source. refreshing", 
		zone->apex_str, ntohl(soa->serial));
	zone->soa_nsd = *soa;
	zone->soa_disk = *soa;
	zone->soa_nsd_acquired = acquired;
	zone->soa_disk_acquired = acquired;
	if(zone->soa_notified_acquired != 0 &&
		(zone->soa_notified.serial == 0 ||
	   	compare_serial(ntohl(zone->soa_disk.serial),
			ntohl(zone->soa_notified.serial)) >= 0))
	{	/* user provided in response to this notification */
		zone->soa_notified_acquired = 0;
	}
	xfrd_set_zone_state(zone, xfrd_zone_refreshing);
	xfrd_set_refresh_now(zone);
	xfrd_send_notify(zone);
}

static void 
xfrd_send_notify(xfrd_zone_t* zone)
{
	if(!zone->zone_options->notify) {
		return; /* no notify acl, nothing to do */
	}
	zone->notify_retry = 0;
	zone->notify_current = zone->zone_options->notify;
	zone->notify_send_handler.timeout = &zone->notify_timeout;
	zone->notify_timeout.tv_sec = xfrd_time();
	zone->notify_timeout.tv_nsec = 0;
}

static void 
xfrd_send_expire_notification(xfrd_zone_t* zone)
{
	if(zone->dirty)
		return; /* already queued */
	/* enqueue */
	assert(xfrd->dirty_zones->num < xfrd->dirty_zones->capacity);
	zone->dirty = 1;
	stack_push(xfrd->dirty_zones, (void*)zone);
	xfrd->ipc_handler.event_types |= NETIO_EVENT_WRITE;
}

static int 
xfrd_udp_read_packet(buffer_type* packet, int fd)
{
	ssize_t received;

	/* read the data */
	buffer_clear(packet);
	received = recvfrom(fd, buffer_begin(packet), buffer_remaining(packet),
		0, NULL, NULL);
	if(received == -1) {
		log_msg(LOG_ERR, "xfrd: recvfrom failed: %s",
			strerror(errno));
		return 0;
	}
	buffer_set_limit(packet, received);
	return 1;
}

static void 
xfrd_udp_read(xfrd_zone_t* zone)
{
	log_msg(LOG_INFO, "xfrd: zone %s read udp data", zone->apex_str);
	if(!xfrd_udp_read_packet(xfrd->packet, zone->zone_handler.fd)) {
		close(zone->zone_handler.fd);
		zone->zone_handler.fd = -1;
		return;
	}
	close(zone->zone_handler.fd);
	zone->zone_handler.fd = -1;
	switch(xfrd_handle_received_xfr_packet(zone, xfrd->packet)) {
		case xfrd_packet_tcp:
			xfrd_set_timer(zone, xfrd_time() + XFRD_TCP_TIMEOUT);
			xfrd_tcp_obtain(xfrd->tcp_set, zone);
			break;
		case xfrd_packet_transfer:
		case xfrd_packet_newlease:
			/* nothing more to do */
			assert(zone->round_num == -1);
			break;
		case xfrd_packet_more:
		case xfrd_packet_bad:
		default:
			/* drop packet */
			/* query next server */
			xfrd_make_request(zone);
			break;
	}
}

static int 
xfrd_send_udp(acl_options_t* acl, buffer_type* packet)
{
	struct sockaddr_storage to;
	int fd, family;
	socklen_t to_len = xfrd_acl_sockaddr(acl, &to);

	if(acl->is_ipv6) {
#ifdef INET6
		family = PF_INET6;
#else
		return -1;
#endif
	} else {
		family = PF_INET;
	}

	fd = socket(family, SOCK_DGRAM, IPPROTO_UDP);
	if(fd == -1) {
		log_msg(LOG_ERR, "xfrd: cannot create udp socket to %s: %s",
			acl->ip_address_spec, strerror(errno));
		return -1;
	}

	/* send it (udp) */
	if(sendto(fd,
		buffer_current(packet),
		buffer_remaining(packet), 0,
		(struct sockaddr*)&to, to_len) == -1)
	{
		log_msg(LOG_ERR, "xfrd: sendto %s failed %s",
			acl->ip_address_spec, strerror(errno));
		return -1;
	}
	return fd;
}

void
xfrd_tsig_sign_request(buffer_type* packet, tsig_record_type* tsig, 
	acl_options_t* acl)
{
#ifdef TSIG
	tsig_algorithm_type* algo;
	assert(acl->key_options && acl->key_options->tsig_key);
	algo = tsig_get_algorithm_by_name(acl->key_options->algorithm);
	if(!algo) {
		log_msg(LOG_ERR, "tsig unknown algorithm %s", 
			acl->key_options->algorithm);
		return;
	}
	assert(algo);
	tsig_init_record(tsig, algo, acl->key_options->tsig_key);
	tsig_init_query(tsig, ID(packet));
	tsig_prepare(tsig);
	tsig_update(tsig, packet, buffer_position(packet));
	tsig_sign(tsig);
	tsig_append_rr(tsig, packet);
	ARCOUNT_SET(packet, ARCOUNT(packet) + 1);
	log_msg(LOG_INFO, "appending tsig to packet");
	/* prepare for validating tsigs */
	tsig_prepare(tsig);
#endif
}

static int 
xfrd_send_ixfr_request_udp(xfrd_zone_t* zone)
{
	int fd;
	assert(zone->master);
	if(zone->tcp_conn != -1) {
		/* tcp is using the zone_handler.fd */
		log_msg(LOG_ERR, "xfrd: %s tried to send udp whilst tcp engaged",
			zone->apex_str);
		return -1;
	}
	xfrd_setup_packet(xfrd->packet, TYPE_IXFR, CLASS_IN, zone->apex);
	zone->query_id = ID(xfrd->packet);
	zone->msg_seq_nr = 0;
	zone->msg_rr_count = 0;
	log_msg(LOG_INFO, "sent query with ID %d", zone->query_id);
        NSCOUNT_SET(xfrd->packet, 1);
	xfrd_write_soa_buffer(xfrd->packet, zone, &zone->soa_disk);
	if(zone->master->key_options) {
#ifdef TSIG
		xfrd_tsig_sign_request(xfrd->packet, &zone->tsig, zone->master);
#endif /* TSIG */
	}
	buffer_flip(xfrd->packet);

	if((fd = xfrd_send_udp(zone->master, xfrd->packet)) == -1) 
		return -1;

	log_msg(LOG_INFO, "xfrd sent udp request for ixfr=%d for zone %s to %s", 
		ntohl(zone->soa_disk.serial),
		zone->apex_str, zone->master->ip_address_spec);
	return fd;
}

static int xfrd_parse_soa_info(buffer_type* packet, xfrd_soa_t* soa)
{
	if(!buffer_available(packet, 10))
		return 0;
	soa->type = htons(buffer_read_u16(packet));
	soa->klass = htons(buffer_read_u16(packet));
	soa->ttl = htonl(buffer_read_u32(packet));
	if(ntohs(soa->type) != TYPE_SOA || ntohs(soa->klass) != CLASS_IN)
	{
		return 0;
	}

	if(!buffer_available(packet, buffer_read_u16(packet)) /* rdata length */ ||
		!(soa->prim_ns[0] = dname_make_wire_from_packet(soa->prim_ns+1, packet, 1)) ||
		!(soa->email[0] = dname_make_wire_from_packet(soa->email+1, packet, 1)))
	{
		return 0;
	}
	soa->serial = htonl(buffer_read_u32(packet));
	soa->refresh = htonl(buffer_read_u32(packet));
	soa->retry = htonl(buffer_read_u32(packet));
	soa->expire = htonl(buffer_read_u32(packet));
	soa->minimum = htonl(buffer_read_u32(packet));

	return 1;
}


/* 
 * Check the RRs in an IXFR/AXFR reply.
 * returns 0 on error, 1 on correct parseable packet.
 * done = 1 if the last SOA in an IXFR/AXFR has been seen.
 * soa then contains that soa info.
 * (soa contents is modified by the routine) 
 */
static int
xfrd_xfr_check_rrs(xfrd_zone_t* zone, buffer_type* packet, size_t count, 
	int *done, xfrd_soa_t* soa)
{
	/* first RR has already been checked */
	uint16_t type, klass, rrlen;
	uint32_t ttl;
	size_t i, soapos;
	for(i=0; i<count; ++i,++zone->msg_rr_count)
	{
		if(!packet_skip_dname(packet))
			return 0;
		if(!buffer_available(packet, 10))
			return 0;
		soapos = buffer_position(packet);
		type = buffer_read_u16(packet);
		klass = buffer_read_u16(packet);
		ttl = buffer_read_u32(packet);
		rrlen = buffer_read_u16(packet);
		if(!buffer_available(packet, rrlen))
			return 0;
		if(type == TYPE_SOA) {
			/* check the SOAs */
			size_t mempos = buffer_position(packet);
			buffer_set_position(packet, soapos);
			if(!xfrd_parse_soa_info(packet, soa))
				return 0;
			if(zone->msg_rr_count == 1 && 
				ntohl(soa->serial) != zone->msg_new_serial) {
				/* 2nd RR is SOA with lower serial, this is an IXFR */
				zone->msg_is_ixfr = 1;
				if(!zone->soa_disk_acquired)
					return 0; /* got IXFR but need AXFR */
				if(ntohl(soa->serial) != ntohl(zone->soa_disk.serial))
					return 0; /* bad start serial in IXFR */
				zone->msg_old_serial = ntohl(soa->serial);
			}
			else if(ntohl(soa->serial) == zone->msg_new_serial) {
				/* saw another SOA of new serial. */
				if(zone->msg_is_ixfr == 1) {
					zone->msg_is_ixfr = 2; /* seen middle SOA in ixfr */
				} else {
					/* 2nd SOA for AXFR or 3rd newSOA for IXFR */
					*done = 1;
				}
			}
			buffer_set_position(packet, mempos);
		}
		buffer_skip(packet, rrlen);
	}
	/* packet seems to have a valid DNS RR structure */
	return 1;
}

#ifdef TSIG
static int
xfrd_xfr_process_tsig(xfrd_zone_t* zone, buffer_type* packet)
{
	int have_tsig = 0;
	assert(zone && zone->master && zone->master->key_options 
		&& zone->master->key_options->tsig_key && packet);
	if(!tsig_find_rr(&zone->tsig, packet)) {
		log_msg(LOG_ERR, "xfrd: zone %s, from %s: malformed tsig RR",
			zone->apex_str, zone->master->ip_address_spec);
		return 0;
	} 
	if(zone->tsig.status == TSIG_OK) {
		have_tsig = 1;
	}
	if(have_tsig) {
		/* strip the TSIG resource record off... */
		buffer_set_limit(packet, zone->tsig.position);
		ARCOUNT_SET(packet, ARCOUNT(packet) - 1);
	}

	/* keep running the TSIG hash */
	tsig_update(&zone->tsig, packet, buffer_limit(packet));
	if(have_tsig) {
		if (!tsig_verify(&zone->tsig)) {
			log_msg(LOG_ERR, "xfrd: zone %s, from %s: bad tsig signature",
				zone->apex_str, zone->master->ip_address_spec);
			return 0;
		}
		log_msg(LOG_INFO, "xfrd: zone %s, from %s: good tsig signature",
			zone->apex_str, zone->master->ip_address_spec);
		/* prepare for next tsigs */
		tsig_prepare(&zone->tsig);
	}
	else if(zone->tsig.updates_since_last_prepare > XFRD_TSIG_MAX_UNSIGNED) {
		/* we allow a number of non-tsig signed packets */
		log_msg(LOG_INFO, "xfrd: zone %s, from %s: too many consecutive "
			"packets without TSIG", zone->apex_str, 
			zone->master->ip_address_spec);
		return 0;
	}

	if(!have_tsig && zone->msg_seq_nr == 0) {
		log_msg(LOG_ERR, "xfrd: zone %s, from %s: no tsig in first packet of reply",
			zone->apex_str, zone->master->ip_address_spec);
		return 0;
	}
	return 1;
}
#endif

/* parse the received packet. returns xfrd packet result code. */
static enum xfrd_packet_result 
xfrd_parse_received_xfr_packet(xfrd_zone_t* zone, buffer_type* packet, 
	xfrd_soa_t* soa)
{
	size_t rr_count;
	size_t qdcount = QDCOUNT(packet);
	size_t ancount = ANCOUNT(packet), ancount_todo;
	int done = 0;

	/* has to be axfr / ixfr reply */
	if(!buffer_available(packet, QHEADERSZ)) {
		log_msg(LOG_INFO, "packet too small");
		return xfrd_packet_bad;
	}

	/* only check ID in first response message. Could also check that
	 * AA bit and QR bit are set, but not needed.
	 */
	log_msg(LOG_INFO, "got query with ID %d and %d needed", ID(packet), zone->query_id);
	if(ID(packet) != zone->query_id) {
		log_msg(LOG_ERR, "xfrd: zone %s received bad query id from %s, dropped",
			zone->apex_str, zone->master->ip_address_spec);
		return xfrd_packet_bad;
	}
	/* check RCODE in all response messages */
	if(RCODE(packet) != RCODE_OK) {
		log_msg(LOG_ERR, "xfrd: zone %s received error code %s from %s",
			zone->apex_str, rcode2str(RCODE(packet)), 
			zone->master->ip_address_spec);
		return xfrd_packet_bad;
	}
#ifdef TSIG
	/* check TSIG */
	if(zone->master->key_options) {
		if(!xfrd_xfr_process_tsig(zone, packet)) {
			log_msg(LOG_ERR, "dropping xfr reply due to bad TSIG");
			return xfrd_packet_bad;
		}
	}
#endif
	buffer_skip(packet, QHEADERSZ);

	/* skip question section */
	for(rr_count = 0; rr_count < qdcount; ++rr_count) {
		if (!packet_skip_rr(packet, 1)) {
			log_msg(LOG_ERR, "xfrd: zone %s, from %s: bad RR in question section",
				zone->apex_str, zone->master->ip_address_spec);
			return xfrd_packet_bad;
		}
	}
	if(ancount == 0) {
		log_msg(LOG_INFO, "xfrd: too short xfr packet: no answer");
		return xfrd_packet_bad;
	}
	ancount_todo = ancount;

	if(zone->msg_rr_count == 0) {
		/* parse the first RR, see if it is a SOA */
		if(!packet_skip_dname(packet) ||
			!xfrd_parse_soa_info(packet, soa))
		{
			log_msg(LOG_ERR, "xfrd: zone %s, from %s: no SOA begins answer section",
				zone->apex_str, zone->master->ip_address_spec);
			return xfrd_packet_bad;
		}
		if(zone->soa_disk_acquired != 0 &&
			zone->state != xfrd_zone_expired /* if expired - accept anything */ &&
			compare_serial(ntohl(zone->soa_disk.serial), ntohl(soa->serial)) > 0) {
			log_msg(LOG_INFO, "xfrd: zone %s ignoring old serial from %s",
				zone->apex_str, zone->master->ip_address_spec);
			return xfrd_packet_bad;
		}
		if(zone->soa_disk_acquired != 0 && zone->soa_disk.serial == soa->serial) {
			log_msg(LOG_INFO, "xfrd: zone %s got update indicating current serial",
				zone->apex_str);
			if(zone->soa_notified_acquired == 0) {
				/* we got a new lease on the SOA */
				zone->soa_disk_acquired = xfrd_time();
				if(zone->soa_nsd.serial == soa->serial)
					zone->soa_nsd_acquired = xfrd_time();
				xfrd_set_zone_state(zone, xfrd_zone_ok);
				log_msg(LOG_INFO, "xfrd: zone %s is ok", zone->apex_str);
				zone->round_num = -1; /* next try start anew */
				xfrd_set_timer_refresh(zone);
				return xfrd_packet_newlease;
			}
			/* try next master */
			return xfrd_packet_bad;
		}
		log_msg(LOG_INFO, "IXFR reply has newer serial (have %d, reply %d)",
			ntohl(zone->soa_disk.serial), ntohl(soa->serial));
		/* serial is newer than soa_disk */
		if(ancount == 1) {
			/* single record means it is like a notify */
			(void)xfrd_handle_incoming_notify(zone, soa);
		}
		else if(zone->soa_notified_acquired && zone->soa_notified.serial &&
			compare_serial(ntohl(zone->soa_notified.serial), ntohl(soa->serial)) < 0) {
			/* this AXFR/IXFR notifies me that an even newer serial exists */
			zone->soa_notified.serial = soa->serial;
		}
		zone->msg_new_serial = ntohl(soa->serial);
		zone->msg_rr_count = 1;
		zone->msg_is_ixfr = 0;
		if(zone->soa_disk_acquired)
			zone->msg_old_serial = ntohl(zone->soa_disk.serial);
		else zone->msg_old_serial = 0;
		ancount_todo = ancount - 1;
	}

	if(zone->tcp_conn == -1 && TC(packet)) {
		log_msg(LOG_INFO, "xfrd: zone %s received TC from %s. retry tcp.",
			zone->apex_str, zone->master->ip_address_spec);
		return xfrd_packet_tcp;
	}

	if(zone->tcp_conn == -1 && ancount < 2) {
		/* too short to be a real ixfr/axfr data transfer */
		/* The serial is newer, so try tcp to this master. */
		log_msg(LOG_INFO, "xfrd: udp reply is short. Try tcp anyway.");
		return xfrd_packet_tcp;
	}

	if(!xfrd_xfr_check_rrs(zone, packet, ancount_todo, &done, soa))
	{
		log_msg(LOG_INFO, "xfrd: zone %s sent bad xfr reply.",
			zone->apex_str);
		return xfrd_packet_bad;
	}
	if(zone->tcp_conn == -1 && done == 0) {
		log_msg(LOG_INFO, "xfrd: udp reply incomplete");
		return xfrd_packet_bad;
	}
	if(done == 0)
		return xfrd_packet_more;
#ifdef TSIG
	if(zone->master->key_options) {
		if(zone->tsig.updates_since_last_prepare != 0) {
			log_msg(LOG_INFO, "xfrd: last packet of reply has no TSIG");
			return xfrd_packet_bad;
		}
	}
#endif
	return xfrd_packet_transfer;
}

enum xfrd_packet_result 
xfrd_handle_received_xfr_packet(xfrd_zone_t* zone, buffer_type* packet)
{
	xfrd_soa_t soa;
	enum xfrd_packet_result res;

	/* parse and check the packet - see if it ends the xfr */
	switch((res=xfrd_parse_received_xfr_packet(zone, packet, &soa)))
	{
		case xfrd_packet_more:
		case xfrd_packet_transfer:
			/* continue with commit */
			break;
		case xfrd_packet_newlease:
			return xfrd_packet_newlease;
		case xfrd_packet_tcp:
			return xfrd_packet_tcp;
		case xfrd_packet_bad:
		default:
			/* rollback */
			if(zone->msg_seq_nr > 0) {
				/* do not process xfr - if only one part simply ignore it. */
				/* rollback previous parts of commit */
				buffer_clear(packet);
				buffer_printf(packet, "xfrd: zone %s xfr rollback serial %d at time %d "
					"from %s of %d parts",
					zone->apex_str, (int)zone->msg_new_serial, (int)xfrd_time(), 
					zone->master->ip_address_spec, zone->msg_seq_nr);
				buffer_flip(packet);
				diff_write_commit(zone->apex_str, zone->msg_old_serial, zone->msg_new_serial,
					zone->query_id, zone->msg_seq_nr, 0, (char*)buffer_begin(packet),
					xfrd->nsd->options);
				log_msg(LOG_INFO, "xfrd: zone %s xfr reverted \"%s\"", zone->apex_str,
					(char*)buffer_begin(packet));
			}
			return xfrd_packet_bad;
	}

	/* dump reply on disk to diff file */
	diff_write_packet(zone->apex_str, zone->msg_new_serial, zone->query_id, zone->msg_seq_nr,
		buffer_begin(packet), buffer_limit(packet), xfrd->nsd->options);
	log_msg(LOG_INFO, "xfrd: zone %s written %d received XFR to serial %d from %s to disk (part %d)",
		zone->apex_str, (int)buffer_limit(packet), (int)zone->msg_new_serial, 
		zone->master->ip_address_spec, zone->msg_seq_nr);
	zone->msg_seq_nr++;
	if(res == xfrd_packet_more) {
		/* wait for more */
		return xfrd_packet_more;
	}

	/* done. we are completely sure of this */
	buffer_clear(packet);
	buffer_printf(packet, "xfrd: zone %s received update to serial %d at time %d from %s in %d parts",
		zone->apex_str, (int)zone->msg_new_serial, (int)xfrd_time(), 
		zone->master->ip_address_spec, zone->msg_seq_nr);
#ifdef TSIG
	if(zone->master->key_options) {
		buffer_printf(packet, " TSIG verified with key %s",
			zone->master->key_options->name);
	}
#endif /* TSIG */
	buffer_flip(packet);
	diff_write_commit(zone->apex_str, zone->msg_old_serial, zone->msg_new_serial,
		zone->query_id, zone->msg_seq_nr, 1, (char*)buffer_begin(packet),
		xfrd->nsd->options);
	log_msg(LOG_INFO, "xfrd: zone %s committed \"%s\"", zone->apex_str,
		(char*)buffer_begin(packet));
	/* update the disk serial no. */
	zone->soa_disk_acquired = xfrd_time();
	zone->soa_disk = soa;
	if(zone->soa_notified_acquired && (
		zone->soa_notified.serial == 0 ||
		compare_serial(htonl(zone->soa_disk.serial), 
		htonl(zone->soa_notified.serial)) >= 0))
	{
		zone->soa_notified_acquired = 0;
	}
	if(!zone->soa_notified_acquired) {
		/* do not set expired zone to ok:
		 * it would cause nsd to start answering
		 * bad data, since the zone is not loaded yet.
		 * if nsd does not reload < retry time, more 
		 * queries (for even newer versions) are made.
		 * For expired zone after reload it is set ok (SOAINFO ipc). */
		if(zone->state != xfrd_zone_expired)
			xfrd_set_zone_state(zone, xfrd_zone_ok);
		log_msg(LOG_INFO, "xfrd: zone %s is waiting for reload", zone->apex_str);
		zone->round_num = -1; /* next try start anew */
		xfrd_set_timer_refresh(zone);
		xfrd_set_reload_timeout();
		return xfrd_packet_transfer;
	} else {
		/* try to get an even newer serial */
		/* pretend it was bad to continue queries */
		xfrd_set_reload_timeout();
		return xfrd_packet_bad;
	}
}

static void 
xfrd_set_reload_timeout()
{
	if(xfrd->nsd->options->xfrd_reload_timeout == -1)
		return; /* automatic reload disabled. */
	if(xfrd->reload_timeout.tv_sec == 0 ||
		xfrd_time() >= xfrd->reload_timeout.tv_sec ) {
		/* no reload wait period (or it passed), do it right away */
		xfrd->need_to_send_reload = 1;
		xfrd->ipc_handler.event_types |= NETIO_EVENT_WRITE;
		/* start reload wait period */
		xfrd->reload_timeout.tv_sec = xfrd_time() +
			xfrd->nsd->options->xfrd_reload_timeout;
		xfrd->reload_timeout.tv_nsec = 0;
		return;
	}
	/* cannot reload now, set that after the timeout a reload has to happen */
	xfrd->reload_handler.timeout = &xfrd->reload_timeout;
}

static void 
xfrd_handle_reload(netio_type *ATTR_UNUSED(netio), 
	netio_handler_type *handler, netio_event_types_type event_types)
{
	/* reload timeout */
	assert(event_types & NETIO_EVENT_TIMEOUT);
	/* timeout wait period after this request is sent */
	handler->timeout = NULL;
	xfrd->reload_timeout.tv_sec = xfrd_time() +
		xfrd->nsd->options->xfrd_reload_timeout;
	xfrd->need_to_send_reload = 1;
	xfrd->ipc_handler.event_types |= NETIO_EVENT_WRITE;
}

void 
xfrd_handle_passed_packet(buffer_type* packet, int acl_num)
{
	uint8_t qnamebuf[MAXDOMAINLEN];
	uint16_t qtype, qclass;
	const dname_type* dname;
	region_type* tempregion = region_create(xalloc, free);
	xfrd_zone_t* zone;
	buffer_skip(packet, QHEADERSZ);
	if(!packet_read_query_section(packet, qnamebuf, &qtype, &qclass))
		return; /* drop bad packet */

	dname = dname_make(tempregion, qnamebuf, 1);
	log_msg(LOG_INFO, "xfrd: got passed packet for %s, acl %d", 
		dname_to_string(dname,0), acl_num);

	/* find the zone */
	zone = (xfrd_zone_t*)rbtree_search(xfrd->zones, dname);
	if(!zone) {
		log_msg(LOG_INFO, "xfrd: incoming packet for unknown zone %s", 
			dname_to_string(dname,0));
		region_destroy(tempregion);
		return; /* drop packet for unknown zone */
	}
	region_destroy(tempregion);

	/* handle */
	if(OPCODE(packet) == OPCODE_NOTIFY) {
		xfrd_soa_t soa;
		int have_soa = 0;
		int next;
		/* get serial from a SOA */
		if(ANCOUNT(packet) == 1 && packet_skip_dname(packet) &&
			xfrd_parse_soa_info(packet, &soa))
			have_soa = 1;
		if(xfrd_handle_incoming_notify(zone, have_soa?&soa:NULL))
			xfrd_set_refresh_now(zone);
		next = find_same_master_notify(zone, acl_num);
		if(next != -1) {
			zone->next_master = next;
			log_msg(LOG_INFO, "xfrd: notify set next master to query %d", next);
		}
	}
	else {
		/* TODO handle incoming IXFR udp reply via port 53 */
	}
}

static int 
xfrd_handle_incoming_notify(xfrd_zone_t* zone, xfrd_soa_t* soa)
{
	if(soa && zone->soa_disk_acquired && zone->state != xfrd_zone_expired
		&& compare_serial(ntohl(soa->serial), ntohl(zone->soa_disk.serial)) <= 0)
		return 0; /* ignore notify with old serial, we have a valid zone */
	if(soa == 0) {
		zone->soa_notified.serial = 0;
	}
	else if(zone->soa_notified_acquired == 0 || 
		zone->soa_notified.serial == 0 ||
		compare_serial(ntohl(soa->serial), ntohl(zone->soa_notified.serial)) > 0)
	{
		zone->soa_notified = *soa;
	}
	zone->soa_notified_acquired = xfrd_time();
	if(zone->state == xfrd_zone_ok) {
		xfrd_set_zone_state(zone, xfrd_zone_refreshing);
	}
	/* transfer right away */
	return 1;
}

static int
find_same_master_notify(xfrd_zone_t* zone, int acl_num_nfy)
{
	acl_options_t* nfy_acl = acl_find_num(
		zone->zone_options->allow_notify, acl_num_nfy);
	int num = 0;
	acl_options_t* master = zone->zone_options->request_xfr;
	if(!nfy_acl) 
		return -1;
	while(master)
	{
		if(acl_same_host(nfy_acl, master))
			return num;
		master = master->next;
		num++;
	}
	return -1;
}

void
xfrd_check_failed_updates()
{
	/* see if updates have not come through */
	xfrd_zone_t* zone;
	RBTREE_FOR(zone, xfrd_zone_t*, xfrd->zones)
	{
		/* zone has a disk soa, and no nsd soa or a different nsd soa */
		if(zone->soa_disk_acquired != 0 &&
			(zone->soa_nsd_acquired == 0 ||
			zone->soa_disk.serial != zone->soa_nsd.serial)) 
		{
			if(zone->soa_disk_acquired < xfrd->reload_cmd_last_sent) {
				/* this zone should have been loaded, since its disk
				   soa time is before the time of the reload cmd. */
				xfrd_soa_t dumped_soa = zone->soa_disk;
				log_msg(LOG_ERR, "xfrd: zone %s: soa serial %d update failed"
					" restarting transfer (notified zone)",
					zone->apex_str, ntohl(zone->soa_disk.serial));
				/* revert the soa; it has not been acquired properly */
				zone->soa_disk_acquired = zone->soa_nsd_acquired;
				zone->soa_disk = zone->soa_nsd;
				/* pretend we are notified with disk soa.
				   This will cause a refetch of the data, and reload. */
				xfrd_handle_incoming_notify(zone, &dumped_soa);
			} else if(zone->soa_disk_acquired >= xfrd->reload_cmd_last_sent) {
				/* this zone still has to be loaded,
				   make sure reload is set to be sent. */
				if(xfrd->need_to_send_reload == 0 &&
					xfrd->reload_handler.timeout == NULL) {
					log_msg(LOG_ERR, "xfrd: zone %s: needs to be loaded."
						" reload lost? try again", zone->apex_str);
					xfrd_set_reload_timeout();
				}
			}
		}
	}
}

void
xfrd_prepare_zones_for_reload()
{
	xfrd_zone_t* zone;
	RBTREE_FOR(zone, xfrd_zone_t*, xfrd->zones)
	{
		/* zone has a disk soa, and no nsd soa or a different nsd soa */
		if(zone->soa_disk_acquired != 0 &&
			(zone->soa_nsd_acquired == 0 ||
			zone->soa_disk.serial != zone->soa_nsd.serial)) 
		{
			if(zone->soa_disk_acquired == xfrd_time()) {
				/* antedate by one second. */
				/* this makes sure that the zone time is before reload,
				   so that check_failed_zones() is certain of the result */
				zone->soa_disk_acquired--;
			}
		}
	}
}

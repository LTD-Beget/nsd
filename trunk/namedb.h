/*
 * namedb.h -- nsd(8) internal namespace database definitions
 *
 * Copyright (c) 2001-2004, NLnet Labs. All rights reserved.
 *
 * See LICENSE for the license.
 *
 */

#ifndef _NAMEDB_H_
#define	_NAMEDB_H_

#include <stdio.h>

#include "dname.h"
#include "dns.h"
#include "heap.h"

#define	NAMEDB_MAGIC		"NSDdbV06"
#define	NAMEDB_MAGIC_SIZE	8

#if defined(NAMEDB_UPPERCASE) || defined(USE_NAMEDB_UPPERCASE)
#define	NAMEDB_NORMALIZE	toupper
#else
#define	NAMEDB_NORMALIZE	tolower
#endif


typedef union rdata_atom rdata_atom_type;
typedef struct rrset rrset_type;
typedef struct rr rr_type;

/*
 * A domain name table supporting fast insert and search operations.
 */
typedef struct domain_table domain_table_type;
typedef struct domain domain_type;
typedef struct zone zone_type;

struct domain_table
{
	region_type *region;
	heap_t      *names_to_domains;
	domain_type *root;
};

struct domain
{
	rbnode_t     node;
	domain_type *parent;
	domain_type *wildcard_child_closest_match;
	rrset_type  *rrsets;
	uint32_t     number; /* Unique domain name number.  */
#ifdef PLUGINS
	void       **plugin_data;
#endif
	
	/*
	 * This domain name exists (see wildcard clarification draft).
	 */
	unsigned     is_existing : 1;
};

struct zone
{
	zone_type   *next;
	domain_type *apex;
	rrset_type  *soa_rrset;
	rrset_type  *ns_rrset;
	uint32_t     number;
	unsigned     is_secure : 1;
};

/* a RR in DNS */
struct rr {
	domain_type     *owner;
	uint16_t         type;
	uint16_t         klass;
	uint32_t         ttl;
	uint16_t         rdata_count;
	rdata_atom_type *rdatas;
};

/*
 * An RRset consists of at least one RR.  All RRs are from the same
 * zone.
 */
struct rrset
{
	rrset_type *next;
	zone_type  *zone;
	uint16_t    rr_count;
	rr_type    *rrs;
};

/*
 * The field used is based on the wireformat the atom is stored in.
 * The allowed wireformats are defined by the rdata_wireformat_type
 * enumeration.
 */
union rdata_atom
{
	/* RDATA_WF_COMPRESSED_DNAME, RDATA_WF_UNCOMPRESSED_DNAME.  */
	domain_type *domain;

	/* Default.  */
	uint16_t    *data;
};

/*
 * Create a new domain_table containing only the root domain.
 */
domain_table_type *domain_table_create(region_type *region);

/*
 * Search the domain table for a match and the closest encloser.
 */
int domain_table_search(domain_table_type *table,
			const dname_type  *dname,
			domain_type      **closest_match,
			domain_type      **closest_encloser);

/*
 * The number of domains stored in the table (minimum is one for the
 * root domain).
 */
static inline uint32_t
domain_table_count(domain_table_type *table)
{
	return table->names_to_domains->count;
}

/*
 * Find the specified dname in the domain_table.  NULL is returned if
 * there is no exact match.
 */
domain_type *domain_table_find(domain_table_type *table,
			       const dname_type  *dname);

/*
 * Insert a domain name in the domain table.  If the domain name is
 * not yet present in the table it is copied and a new dname_info node
 * is created (as well as for the missing parent domain names, if
 * any).  Otherwise the domain_type that is already in the
 * domain_table is returned.
 */
domain_type *domain_table_insert(domain_table_type *table,
				 const dname_type  *dname);


/*
 * Iterate over all the domain names in the domain tree.
 */
typedef void (*domain_table_iterator_type)(domain_type *node,
					   void *user_data);

void domain_table_iterate(domain_table_type *table,
			  domain_table_iterator_type iterator,
			  void *user_data);

/*
 * Add an RRset to the specified domain.  Updates the is_existing flag
 * as required.
 */
void domain_add_rrset(domain_type *domain, rrset_type *rrset);

rrset_type *domain_find_rrset(domain_type *domain, zone_type *zone, uint16_t type);
rrset_type *domain_find_any_rrset(domain_type *domain, zone_type *zone);

zone_type *domain_find_zone(domain_type *domain);
zone_type *domain_find_parent_zone(zone_type *zone);

domain_type *domain_find_ns_rrsets(domain_type *domain, zone_type *zone, rrset_type **ns);

int domain_is_glue(domain_type *domain, zone_type *zone);

domain_type *domain_wildcard_child(domain_type *domain);

int zone_is_secure(zone_type *zone);

static inline const dname_type *
domain_dname(domain_type *domain)
{
	return (const dname_type *) domain->node.key;
}

/*
 * The type covered by the signature in the specified RR in the RRSIG
 * RRset.
 */
uint16_t rrset_rrsig_type_covered(rrset_type *rrset, uint16_t rr);

typedef struct namedb namedb_type;
struct namedb
{
	region_type       *region;
	domain_table_type *domains;
	zone_type         *zones;
	char              *filename;
	FILE              *fd;
};

static inline int rdata_atom_is_domain(uint16_t type, size_t index);

static inline domain_type *
rdata_atom_domain(rdata_atom_type atom)
{
	return atom.domain;
}

static inline uint16_t
rdata_atom_size(rdata_atom_type atom)
{
	return *atom.data;
}

static inline void *
rdata_atom_data(rdata_atom_type atom)
{
	return atom.data + 1;
}


/*
 * Find the zone for the specified DOMAIN in DB.
 */
zone_type *namedb_find_zone(namedb_type *db, domain_type *domain);

/* dbcreate.c */
struct namedb *namedb_new(const char *filename);
int namedb_save(struct namedb *db);
void namedb_discard(struct namedb *db);


/* dbaccess.c */
int namedb_lookup (struct namedb    *db,
		   const dname_type *dname,
		   domain_type     **closest_match,
		   domain_type     **closest_encloser);
struct namedb *namedb_open(const char *filename);
void namedb_close(struct namedb *db);

static inline int
rdata_atom_is_domain(uint16_t type, size_t index)
{
	const rrtype_descriptor_type *descriptor
		= rrtype_descriptor_by_type(type);
	return (index < descriptor->maximum
		&& (descriptor->wireformat[index] == RDATA_WF_COMPRESSED_DNAME
		    || descriptor->wireformat[index] == RDATA_WF_UNCOMPRESSED_DNAME));
}

static inline rdata_wireformat_type
rdata_atom_wireformat_type(uint16_t type, size_t index)
{
	const rrtype_descriptor_type *descriptor
		= rrtype_descriptor_by_type(type);
	assert(index < descriptor->maximum);
	return (rdata_wireformat_type) descriptor->wireformat[index];
}

#endif

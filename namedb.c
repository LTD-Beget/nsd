/*
 * namedb.c -- common namedb operations.
 *
 * Copyright (c) 2001-2006, NLnet Labs. All rights reserved.
 *
 * See LICENSE for the license.
 *
 */

#include "config.h"

#include <sys/types.h>

#include <assert.h>
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "namedb.h"


static domain_type *
allocate_domain_info(domain_table_type *table,
		     const dname_type *dname,
		     domain_type *parent)
{
	domain_type *result;

	assert(table);
	assert(dname);
	assert(parent);

	result = (domain_type *) region_alloc(table->region,
					      sizeof(domain_type));
	result->dname = dname_partial_copy(
		table->region, dname, domain_dname(parent)->label_count + 1);
	result->parent = parent;
	result->wildcard_child_closest_match = result;
	result->rrsets = NULL;
	result->usage = 0;
#ifdef NSEC3
	result->nsec3_cover = NULL;
	result->nsec3_wcard_child_cover = NULL;
	result->nsec3_ds_parent_cover = NULL;
	result->nsec3_lookup = NULL;
	result->nsec3_is_exact = 0;
	result->nsec3_ds_parent_is_exact = 0;
	result->have_nsec3_hash = 0;
	result->have_nsec3_wc_hash = 0;
	result->have_nsec3_ds_parent_hash = 0;
#endif
	result->is_existing = 0;
	result->is_apex = 0;
	assert(table->numlist_last); /* it exists because root exists */
	/* push this domain at the end of the numlist */
	result->number = table->numlist_last->number+1;
	result->numlist_next = NULL;
	result->numlist_prev = table->numlist_last;
	table->numlist_last->numlist_next = result;
	table->numlist_last = result;

	return result;
}

/** make the domain last in the numlist, changes numbers of domains */
static void
numlist_make_last(domain_table_type *table, domain_type* domain)
{
	size_t sw;
	domain_type *last = table->numlist_last;
	if(domain == last)
		return;
	/* swap numbers with the last element */
	sw = domain->number;
	domain->number = last->number;
	last->number = sw;
	/* swap list position with the last element */
	assert(domain->numlist_next);
	assert(last->numlist_prev);
	if(domain->numlist_next != last) {
		/* case 1: there are nodes between domain .. last */
		domain_type* span_start = domain->numlist_next;
		domain_type* span_end = last->numlist_prev;
		/* these assignments walk the new list from start to end */
		if(domain->numlist_prev)
			domain->numlist_prev->numlist_next = last;
		last->numlist_prev = domain->numlist_prev;
		last->numlist_next = span_start;
		span_start->numlist_prev = last;
		span_end->numlist_next = domain;
		domain->numlist_prev = span_end;
		domain->numlist_next = NULL;
	} else {
		/* case 2: domain and last are neighbors */
		/* these assignments walk the new list from start to end */
		if(domain->numlist_prev)
			domain->numlist_prev->numlist_next = last;
		last->numlist_prev = domain->numlist_prev;
		last->numlist_next = domain;
		domain->numlist_prev = last;
		domain->numlist_next = NULL;
	}
	table->numlist_last = domain;
}

/** pop the biggest domain off the numlist */
static domain_type*
numlist_pop_last(domain_table_type *table)
{
	domain_type* d = table->numlist_last;
	table->numlist_last = table->numlist_last->numlist_prev;
	if(table->numlist_last)
		table->numlist_last->numlist_next = NULL;
	return d;
}

/** see if a domain is eligible to be deleted, and thus is not used */
static int
domain_can_be_deleted(domain_type* domain)
{
	domain_type* n;
	/* it has data or it has usage, do not delete it */
	if(domain->rrsets) return 0;
	if(domain->usage) return 0;
	n = domain_next(domain);
	/* it has children domains, do not delete it */
	if(n && dname_is_subdomain(domain_dname(n), domain_dname(domain)))
		return 0;
	return 1;
}

/** perform domain name deletion */
static void
do_deldomain(domain_table_type *table, domain_type* domain)
{
	assert(domain && domain->parent); /* exists and not root */
	/* first adjust the number list so that domain is the last one */
	numlist_make_last(table, domain);
	/* pop off the domain from the number list */
	(void)numlist_pop_last(table);

	/* see if this domain is someones wildcard-child-closest-match,
	 * which can only be the parent, and then it should use the
	 * one-smaller than this domain as closest-match. */
	if(domain->parent->wildcard_child_closest_match == domain)
		domain->parent->wildcard_child_closest_match =
			domain_previous(domain);

	/* see if nsec3-pointers point here */
	/* TODO */

	/* actual removal */
	radix_delete(table->nametree, domain->rnode);
	region_recycle(table->region, (dname_type*)domain->dname,
		dname_total_size(domain->dname));
	region_recycle(table->region, domain, sizeof(domain_type));
}

void domain_table_deldomain(domain_table_type *table, domain_type* domain)
{
	while(domain_can_be_deleted(domain)) {
		/* delete it */
		do_deldomain(table, domain);
		/* test parent */
		domain = domain->parent;
	}
}

/** delete radix tree as a region cleanup */
void del_radix_tree(void* arg)
{
	radix_tree_delete((struct radtree*)arg);
}

domain_table_type *
domain_table_create(region_type *region)
{
	const dname_type *origin;
	domain_table_type *result;
	domain_type *root;

	assert(region);

	origin = dname_make(region, (uint8_t *) "", 0);

	root = (domain_type *) region_alloc(region, sizeof(domain_type));
	root->dname = origin;
	root->parent = NULL;
	root->wildcard_child_closest_match = root;
	root->rrsets = NULL;
	root->number = 1; /* 0 is used for after header */
	root->usage = 1; /* do not delete root, ever */
	root->is_existing = 0;
	root->is_apex = 0;
	root->numlist_prev = NULL;
	root->numlist_next = NULL;
#ifdef NSEC3
	root->nsec3_is_exact = 0;
	root->nsec3_ds_parent_is_exact = 0;
	root->nsec3_cover = NULL;
	root->nsec3_wcard_child_cover = NULL;
	root->nsec3_ds_parent_cover = NULL;
	root->nsec3_lookup = NULL;
	root->have_nsec3_hash = 0;
	root->have_nsec3_wc_hash = 0;
	root->have_nsec3_ds_parent_hash = 0;
#endif

	result = (domain_table_type *) region_alloc(region,
						    sizeof(domain_table_type));
	result->region = region;
	result->nametree = radix_tree_create();
	region_add_cleanup(region, &del_radix_tree, result->nametree);
	root->rnode = radname_insert(result->nametree, dname_name(root->dname),
		root->dname->name_size, root);

	result->root = root;
	result->numlist_last = root;

	return result;
}

int
domain_table_search(domain_table_type *table,
		   const dname_type   *dname,
		   domain_type       **closest_match,
		   domain_type       **closest_encloser)
{
	int exact;
	uint8_t label_match_count;

	assert(table);
	assert(dname);
	assert(closest_match);
	assert(closest_encloser);

	exact = radname_find_less_equal(table->nametree, dname_name(dname),
		dname->name_size, (struct radnode**)closest_match);
	*closest_match = (domain_type*)((*(struct radnode**)closest_match)->elem);
	assert(*closest_match);

	*closest_encloser = *closest_match;

	if (!exact) {
		label_match_count = dname_label_match_count(
			domain_dname(*closest_encloser),
			dname);
		assert(label_match_count < dname->label_count);
		while (label_match_count < domain_dname(*closest_encloser)->label_count) {
			(*closest_encloser) = (*closest_encloser)->parent;
			assert(*closest_encloser);
		}
	}

	return exact;
}

domain_type *
domain_table_find(domain_table_type *table,
		  const dname_type *dname)
{
	domain_type *closest_match;
	domain_type *closest_encloser;
	int exact;

	exact = domain_table_search(
		table, dname, &closest_match, &closest_encloser);
	return exact ? closest_encloser : NULL;
}


domain_type *
domain_table_insert(domain_table_type *table,
		    const dname_type  *dname)
{
	domain_type *closest_match;
	domain_type *closest_encloser;
	domain_type *result;
	int exact;

	assert(table);
	assert(dname);

	exact = domain_table_search(
		table, dname, &closest_match, &closest_encloser);
	if (exact) {
		result = closest_encloser;
	} else {
		assert(domain_dname(closest_encloser)->label_count < dname->label_count);

		/* Insert new node(s).  */
		do {
			result = allocate_domain_info(table,
						      dname,
						      closest_encloser);
			result->rnode = radname_insert(table->nametree,
				dname_name(result->dname),
				result->dname->name_size, result);

			/*
			 * If the newly added domain name is larger
			 * than the parent's current
			 * wildcard_child_closest_match but smaller or
			 * equal to the wildcard domain name, update
			 * the parent's wildcard_child_closest_match
			 * field.
			 */
			if (label_compare(dname_name(domain_dname(result)),
					  (const uint8_t *) "\001*") <= 0
			    && dname_compare(domain_dname(result),
					     domain_dname(closest_encloser->wildcard_child_closest_match)) > 0)
			{
				closest_encloser->wildcard_child_closest_match
					= result;
			}
			closest_encloser = result;
		} while (domain_dname(closest_encloser)->label_count < dname->label_count);
	}

	return result;
}

int
domain_table_iterate(domain_table_type *table,
		    domain_table_iterator_type iterator,
		    void *user_data)
{
	int error = 0;
	struct radnode* n;
	for(n = radix_first(table->nametree); n; n = radix_next(n)) {
		error += iterator((domain_type*)n->elem, user_data);
	}
	return error;
}


void
domain_add_rrset(domain_type *domain, rrset_type *rrset)
{
#if 0 	/* fast */
	rrset->next = domain->rrsets;
	domain->rrsets = rrset;
#else
	/* preserve ordering, add at end */
	rrset_type** p = &domain->rrsets;
	while(*p)
		p = &((*p)->next);
	*p = rrset;
	rrset->next = 0;
#endif

	while (domain && !domain->is_existing) {
		domain->is_existing = 1;
		domain = domain->parent;
	}
}


rrset_type *
domain_find_rrset(domain_type *domain, zone_type *zone, uint16_t type)
{
	rrset_type *result = domain->rrsets;

	while (result) {
		if (result->zone == zone && rrset_rrtype(result) == type) {
			return result;
		}
		result = result->next;
	}
	return NULL;
}

rrset_type *
domain_find_any_rrset(domain_type *domain, zone_type *zone)
{
	rrset_type *result = domain->rrsets;

	while (result) {
		if (result->zone == zone) {
			return result;
		}
		result = result->next;
	}
	return NULL;
}

zone_type *
domain_find_zone(domain_type *domain)
{
	rrset_type *rrset;
	while (domain) {
		for (rrset = domain->rrsets; rrset; rrset = rrset->next) {
			if (rrset_rrtype(rrset) == TYPE_SOA) {
				return rrset->zone;
			}
		}
		domain = domain->parent;
	}
	return NULL;
}

zone_type *
domain_find_parent_zone(zone_type *zone)
{
	rrset_type *rrset;

	assert(zone);

	for (rrset = zone->apex->rrsets; rrset; rrset = rrset->next) {
		if (rrset->zone != zone && rrset_rrtype(rrset) == TYPE_NS) {
			return rrset->zone;
		}
	}
	return NULL;
}

domain_type *
domain_find_ns_rrsets(domain_type *domain, zone_type *zone, rrset_type **ns)
{
	while (domain && domain != zone->apex) {
		*ns = domain_find_rrset(domain, zone, TYPE_NS);
		if (*ns)
			return domain;
		domain = domain->parent;
	}

	*ns = NULL;
	return NULL;
}

int
domain_is_glue(domain_type *domain, zone_type *zone)
{
	rrset_type *unused;
	domain_type *ns_domain = domain_find_ns_rrsets(domain, zone, &unused);
	return (ns_domain != NULL &&
		domain_find_rrset(ns_domain, zone, TYPE_SOA) == NULL);
}

domain_type *
domain_wildcard_child(domain_type *domain)
{
	domain_type *wildcard_child;

	assert(domain);
	assert(domain->wildcard_child_closest_match);

	wildcard_child = domain->wildcard_child_closest_match;
	if (wildcard_child != domain
	    && label_is_wildcard(dname_name(domain_dname(wildcard_child))))
	{
		return wildcard_child;
	} else {
		return NULL;
	}
}

int
zone_is_secure(zone_type *zone)
{
	assert(zone);
	return zone->is_secure;
}

uint16_t
rr_rrsig_type_covered(rr_type *rr)
{
	assert(rr->type == TYPE_RRSIG);
	assert(rr->rdata_count > 0);
	assert(rdata_atom_size(rr->rdatas[0]) == sizeof(uint16_t));

	return ntohs(* (uint16_t *) rdata_atom_data(rr->rdatas[0]));
}

zone_type *
namedb_find_zone(namedb_type *db, const dname_type *dname)
{
	struct radnode* n = radname_search(db->zonetree, dname_name(dname),
		dname->name_size);
	if(n) return (zone_type*)n->elem;
	return NULL;
}

void namedb_wipe_updated_flag(namedb_type *db)
{
	struct radnode* n;
        for(n=radix_first(db->zonetree); n; n=radix_next(n)) {
                ((zone_type*)n->elem)->updated = 0;
        }
}

rrset_type *
domain_find_non_cname_rrset(domain_type *domain, zone_type *zone)
{
	/* find any rrset type that is not allowed next to a CNAME */
	/* nothing is allowed next to a CNAME, except RRSIG, NSEC, NSEC3 */
	rrset_type *result = domain->rrsets;

	while (result) {
		if (result->zone == zone && /* here is the list of exceptions*/
			rrset_rrtype(result) != TYPE_CNAME &&
			rrset_rrtype(result) != TYPE_RRSIG &&
			rrset_rrtype(result) != TYPE_NXT &&
			rrset_rrtype(result) != TYPE_SIG &&
			rrset_rrtype(result) != TYPE_NSEC &&
			rrset_rrtype(result) != TYPE_NSEC3 ) {
			return result;
		}
		result = result->next;
	}
	return NULL;
}

int
namedb_lookup(struct namedb    *db,
	      const dname_type *dname,
	      domain_type     **closest_match,
	      domain_type     **closest_encloser)
{
	return domain_table_search(
		db->domains, dname, closest_match, closest_encloser);
}

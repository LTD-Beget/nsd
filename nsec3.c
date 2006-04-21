/*
 * nsec3.c -- nsec3 handling.
 *
 * Copyright (c) 2001-2006, NLnet Labs. All rights reserved.
 *
 * See LICENSE for the license.
 *
 */
#include <config.h>
#ifdef NSEC3
#include <stdio.h>

#include "nsec3.h"
#include "iterated_hash.h"
#include "namedb.h"
#include "nsd.h"
#include "answer.h"

static void detect_nsec3_params(rrset_type* nsec3_apex,
	const unsigned char** salt, int* salt_len, int* iter)
{
	/* always uses first NSEC3 record with SOA bit set */
	assert(salt && salt_len && iter);
	assert(nsec3_apex);
	*salt_len = rdata_atom_data(nsec3_apex->rrs[0].rdatas[2])[0];
	*salt = (unsigned char*)(rdata_atom_data(nsec3_apex->rrs[0].rdatas[2])+1);
	*iter = (rdata_atom_data(nsec3_apex->rrs[0].rdatas[1])[0]<<16) |
		(rdata_atom_data(nsec3_apex->rrs[0].rdatas[1])[1]<<8) |
		(rdata_atom_data(nsec3_apex->rrs[0].rdatas[1])[2]);
	*iter &= 0x7fffff;
}

const dname_type *
nsec3_hash_dname(region_type *region, zone_type *zone,
	const dname_type *dname)
{
	unsigned char hash[SHA_DIGEST_LENGTH];
	char b32[SHA_DIGEST_LENGTH*2+1];
	const unsigned char* nsec3_salt = NULL;
	int nsec3_saltlength = 0;
	int nsec3_iterations = 0;

	detect_nsec3_params(zone->nsec3_rrset, &nsec3_salt, 
		&nsec3_saltlength, &nsec3_iterations);
	iterated_hash(hash, nsec3_salt, nsec3_saltlength, dname_name(dname),
		dname->name_size, nsec3_iterations);
	b32_ntop(hash, sizeof(hash), b32, sizeof(b32));
	dname=dname_parse(region, b32);
	dname=dname_concatenate(region, dname, domain_dname(zone->apex));
	return dname;
}

static int nsec3_has_soa(rr_type* rr)
{
	if(rdata_atom_size(rr->rdatas[4]) > 0 && /* has types in bitmap */
		rdata_atom_data(rr->rdatas[4])[0] == 0 && /* first window = 0, */
						/* [1]: windowlen must be >= 1 */
		rdata_atom_data(rr->rdatas[4])[2]&0x02)  /* SOA bit set */
		return 1;
	return 0;
}

static rrset_type* find_zone_nsec3(zone_type *zone)
{
	domain_type *domain = zone->apex;
	while(domain && dname_is_subdomain(
		domain_dname(domain), domain_dname(zone->apex)))
	{
		rrset_type *rrset = domain_find_rrset(domain, zone, TYPE_NSEC3);
		if(rrset && nsec3_has_soa(rrset->rrs))
		{
			const unsigned char* salt;
			int slen, iter;
			detect_nsec3_params(rrset, &salt, &slen, &iter);
			log_msg(LOG_INFO, "detected NSEC3 for zone %s saltlen=%d iter=%d",
				dname_to_string(domain_dname(zone->apex),0), slen, iter);
			return rrset;
		}
		domain = domain_next(domain);
	}
	return 0;
}

static domain_type* nsec3_find_last(zone_type* zone)
{
	/* this is the longest possible walk to get to the end, but is precomputed */
	/* could also tree-find the <max_val>.domain name */
	domain_type* walk = zone->apex;
	domain_type* result = 0;
	while(walk && 
		dname_is_subdomain(domain_dname(walk), domain_dname(zone->apex)))
	{
		/* remember last domain with an NSEC3 rrset */
		if(domain_find_rrset(walk, zone, TYPE_NSEC3)) {
			result = walk;
		}
		walk = domain_next(walk);
	}
	return result;
}

int nsec3_find_cover(namedb_type* db, zone_type* zone, 
	const dname_type* hashname, domain_type** result)
{
	rrset_type *rrset;
	domain_type *walk;
	domain_type *closest_match;
	domain_type *closest_encloser;
	int exact;

	assert(result);
	assert(zone->nsec3_rrset);

	exact = domain_table_search(
		db->domains, hashname, &closest_match, &closest_encloser);
	/* exact match of hashed domain name + it has an NSEC3? */
	if(exact && (rrset = domain_find_rrset(closest_encloser, zone, TYPE_NSEC3))) {
		*result = closest_encloser;
		assert(*result != 0);
		return 1;
	}

	/* find covering NSEC3 record, lexicographically before the closest match */
	walk = closest_match;
	rrset = 0;
	while(walk && dname_is_subdomain(domain_dname(walk), domain_dname(zone->apex))
		&& !(rrset = domain_find_rrset(walk, zone, TYPE_NSEC3)))
	{
		walk = domain_previous(walk);
	}
	if(rrset)
		*result = walk;
	else 	{
		/* 
		 * There are no NSEC3s before the closest match.
		 * so the hash name is before the first NSEC3 record in the zone.
		 * use last NSEC3, which covers the wraparound in hash space 
		 *
		 * Since the zone has an NSEC3 with the SOA bit set for NSEC3 to turn on,
		 * there is also a last nsec3, so find_cover always assigns *result!=0.
		 */
		*result = zone->nsec3_last;
	}
	assert(*result != 0);
	return 0;
}

static void prehash_domain(namedb_type* db, zone_type* zone, 
	domain_type* domain, region_type* region)
{
	/* find it */
	domain_type* result = 0;
	const dname_type *wcard, *wcard_child, *hashname;
	int exact;

	if(!zone->nsec3_rrset)
	{
		/* set to 0 (in case NSEC3 removed after an update) */
		domain->nsec3_exact = 0;
		domain->nsec3_cover = 0;
		domain->nsec3_wcard_child_cover = 0;
		return;
	}

	hashname = nsec3_hash_dname(region, zone, domain_dname(domain));
	exact = nsec3_find_cover(db, zone, hashname, &result);
	domain->nsec3_cover = result;
	if(exact)
		domain->nsec3_exact = result;
	else	domain->nsec3_exact = 0;

	/*
	printf("prehash for %s ", dname_to_string(domain_dname(domain),0));
	printf("found prehash %s %s", exact?"exact":"cover",
		dname_to_string(domain_dname(result),0));
	*/

	/* find cover for *.domain for wildcard denial */
	wcard = dname_parse(region, "*");
	wcard_child = dname_concatenate(region, wcard, domain_dname(domain));
	hashname = nsec3_hash_dname(region, zone, wcard_child);
	exact = nsec3_find_cover(db, zone, hashname, &result);
	domain->nsec3_wcard_child_cover = result;

	if(exact && !domain_wildcard_child(domain))
	{
		log_msg(LOG_ERR, "prehash: collision of wildcard denial for %s."
			" Sign zone with different salt to remove collision.",
			dname_to_string(domain_dname(domain),0));
	}
	/*
	printf(" wcard denial %s %s\n", exact?"exact":"cover",
		dname_to_string(domain_dname(result),0));
	*/
}

static void prehash_ds(namedb_type* db, zone_type* zone, 
	domain_type* domain, region_type* region)
{
	domain_type* result = 0;
	const dname_type* hashname;
	int exact;

	if(!zone->nsec3_rrset) {
		domain->nsec3_ds_parent_exact = NULL;
		return;
	}

	/* hash again, other zone could have different hash parameters */
	hashname = nsec3_hash_dname(region, zone, domain_dname(domain));
	exact = nsec3_find_cover(db, zone, hashname, &result);
	if(exact)
		domain->nsec3_ds_parent_exact = result;
	else 	domain->nsec3_ds_parent_exact = 0;
	domain->nsec3_ds_parent_cover = result;

	/*
	printf("prehash_ds for %s ", dname_to_string(domain_dname(domain),0));
	printf("found prehash %s\n", dname_to_string(domain_dname(result),0));
	*/
}

static void prehash_zone(struct namedb* db, struct zone* zone)
{
	domain_type *walk;
	region_type *temp_region = region_create(xalloc, free);
	assert(db && zone);

	/* find zone settings */
	zone->nsec3_rrset = find_zone_nsec3(zone);
	zone->nsec3_last = nsec3_find_last(zone); 
	assert((zone->nsec3_rrset&&zone->nsec3_last) ||
		(!zone->nsec3_rrset&&!zone->nsec3_last));
	if(zone->nsec3_rrset) {
		/* check that hashed, the apex name equals the found nsec3 domain */
		const dname_type* checkname = nsec3_hash_dname(temp_region, 
			zone, domain_dname(zone->apex));
		assert(zone->nsec3_rrset->rr_count > 0);
		if(dname_compare(checkname, domain_dname(
			zone->nsec3_rrset->rrs[0].owner)) != 0) {
			log_msg(LOG_ERR, "NSEC3 record with SOA bit on %s is bad."
				" name!=hash(zone). disabling NSEC3 for zone",
				dname_to_string(domain_dname(
				zone->nsec3_rrset->rrs[0].owner),0));
			zone->nsec3_rrset = 0;
			zone->nsec3_last = 0;
		}
	}

	/* go through entire zone */
	walk = zone->apex;
	while(walk && dname_is_subdomain(
		domain_dname(walk), domain_dname(zone->apex)))
	{
		zone_type* z = domain_find_zone(walk);
		if(z && z==zone)
		{
			prehash_domain(db, zone, walk, temp_region);
			region_free_all(temp_region);
		}
		/* prehash the DS (parent zone) */
		/* only if there is a DS (so z==parent side of zone cut) */
		if(domain_find_rrset(walk, zone, TYPE_DS))
		{
			assert(walk != zone->apex /* DS must be above zone cut */);
			prehash_ds(db, zone, walk, temp_region);	
			region_free_all(temp_region);
		}
		walk = domain_next(walk);
	}
	region_destroy(temp_region);
}

void prehash(struct namedb* db, struct zone* zone)
{
	time_t start = time(0), end;
	if(zone) {
		prehash_zone(db, zone);
	} else {
		zone_type *z;
		for(z = db->zones; z; z = z->next)
		{
			prehash_zone(db, z);
		}
	}
	end = time(0);
	log_msg(LOG_INFO, "prehash took %d seconds", (int)(end-start));
}

/* add the NSEC3 rrset at the given domain */
static void nsec3_add_rrset(struct query *query, struct answer *answer, 
	rr_section_type section, struct domain* domain)
{
	if(domain) {
		rrset_type* rrset = domain_find_rrset(domain, query->zone, TYPE_NSEC3);
		if(rrset)
			answer_add_rrset(answer, section, domain, rrset);
	}
}

/* this routine does hashing at query-time. slow. */
static void nsec3_add_nonexist_proof(struct query *query, struct answer *answer,
        struct domain *encloser, struct namedb* db)
{
	const dname_type *to_prove, *hashed; 
	domain_type *cover=0;
	assert(encloser);
	to_prove = dname_partial_copy(query->region, query->qname,
		dname_label_match_count(query->qname, domain_dname(encloser))+1);
	/* generate proof that one label below closest encloser does not exist */
	hashed = nsec3_hash_dname(query->region, query->zone, to_prove);
	if(nsec3_find_cover(db, query->zone, hashed, &cover))
	{
		/* exact match, hash collision */
		/* the hashed name of the query corresponds to an existing name. */
		RCODE_SET(query->packet, RCODE_SERVFAIL);
		return;
	}
	else
	{
		/* cover proves the qname does not exist */
		nsec3_add_rrset(query, answer, AUTHORITY_SECTION, cover);
	}
}

static void nsec3_add_closest_encloser_proof(
	struct query *query, struct answer *answer,
	struct domain *closest_encloser, struct namedb* db)
{
	if(!closest_encloser) return;
	/* prove that below closest encloser nothing exists */
	nsec3_add_nonexist_proof(query, answer, closest_encloser, db);
	/* proof that closest encloser exists */
	nsec3_add_rrset(query, answer, AUTHORITY_SECTION, closest_encloser->nsec3_exact);
}

void nsec3_answer_wildcard(struct query *query, struct answer *answer,
        struct domain *wildcard, struct namedb* db)
{
	if(!wildcard) return;
	nsec3_add_nonexist_proof(query, answer, wildcard, db);
}

void nsec3_add_ds_proof(struct query *query, struct answer *answer,
	struct domain *domain)
{
	assert(domain != query->zone->apex);
	if(domain->nsec3_ds_parent_exact) {
		/* use NSEC3 record from above the zone cut. */
		nsec3_add_rrset(query, answer, AUTHORITY_SECTION, 
			domain->nsec3_ds_parent_exact);
	} else {
		/* prove closest provable encloser */
		domain_type* par = domain->parent;
		domain_type* prev_par = 0;
		while(par && !par->nsec3_exact)
		{
			prev_par = par;
			par = par->parent;
		}
		assert(par); /* parent zone apex must be provable, thus this ends */	
		nsec3_add_rrset(query, answer, AUTHORITY_SECTION,
			par->nsec3_exact);
		/* we took several steps to go to the provable parent, so
		   the one below it has no exact nsec3, disprove it */
		if(prev_par) {
			assert(prev_par != domain && !prev_par->nsec3_exact);
			nsec3_add_rrset(query, answer, AUTHORITY_SECTION,
				prev_par->nsec3_cover);
		}
		/* add optout range from parent zone */
		nsec3_add_rrset(query, answer, AUTHORITY_SECTION, 
			domain->nsec3_ds_parent_cover);
	}
}

void nsec3_answer_nodata(struct query *query, struct answer *answer,
	struct domain *original)
{
	if(query->qtype == TYPE_DS)
	{
		if(original == query->zone->apex) {
			/* DS at zone apex, but server not authoritative for parent zone */
			/* so answer at the child zone level */
			nsec3_add_rrset(query, answer, AUTHORITY_SECTION, 
				original->nsec3_exact);
			return;
		}
		/* query->zone must be the parent zone */
		nsec3_add_ds_proof(query, answer, original);
	}
	else if (original==original->wildcard_child_closest_match
		&& label_is_wildcard(dname_name(domain_dname(original)))) {
		/* denial for wildcard is already there */
		/* add parent proof to have a closest encloser proof for wildcard parent */
		if(original->parent)
			nsec3_add_rrset(query, answer, AUTHORITY_SECTION, 
				original->parent->nsec3_exact);
		/* proof for wildcard itself */
		nsec3_add_rrset(query, answer, AUTHORITY_SECTION, 
			original->nsec3_cover);
	}
	else	/* add nsec3 to prove rrset does not exist */
		nsec3_add_rrset(query, answer, AUTHORITY_SECTION, 
			original->nsec3_exact);
}

void nsec3_answer_delegation(struct query *query, struct answer *answer)
{
	nsec3_add_ds_proof(query, answer, query->delegation_domain);
}

static int domain_has_only_NSEC3(struct domain* domain, struct zone* zone)
{
	rrset_type* rrset = domain->rrsets;
	int nsec3_seen = 0;
	while(rrset)
	{
		if(rrset->zone == zone)
		{
			if(rrset->rrs[0].type != TYPE_NSEC3)
				return 0;
			nsec3_seen = 1;
		}
		rrset = rrset->next;
	}
	return nsec3_seen;
}

void nsec3_answer_authoritative(struct domain** match, struct query *query,
	struct answer *answer, struct domain* closest_encloser, 
	struct namedb* db)
{
	assert(match);
	/* there is a match, this has 1 RRset, which is NSEC3, but qtype is not. */
	if(query->qtype != TYPE_NSEC3 && *match && 
		domain_has_only_NSEC3(*match, query->zone))
	{
		/* act as if the NSEC3 domain did not exist, name error */
		*match = 0;
	}
	if(!*match) {
		/* name error */
		if(query->qtype == TYPE_NSEC3) {
			/* query for NSEC3, but that domain did not exist */
			/* include covering nsec3 found *without hashing* */
			domain_type* cover=0;
			if(nsec3_find_cover(db, query->zone, query->qname, &cover))
			{
				/* impossible, this is an NXDomain, but there is an NSEC3... */
				assert(0);
			} 
			nsec3_add_rrset(query, answer, AUTHORITY_SECTION, cover);
		}
		else {
			/* name error, domain does not exist */
			if(closest_encloser->nsec3_wcard_child_cover ==
				closest_encloser->nsec3_exact) {
				/* collision of wcard denial and cover */
				RCODE_SET(query->packet, RCODE_SERVFAIL);
			}
			nsec3_add_closest_encloser_proof(query, answer, closest_encloser, db);	
			nsec3_add_rrset(query, answer, AUTHORITY_SECTION, 
				closest_encloser->nsec3_wcard_child_cover);
		}
	}
}

#endif /* NSEC3 */

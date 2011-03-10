/*
 * pktc.h -- packet compiler definitions.
 *
 * Copyright (c) 2011, NLnet Labs. All rights reserved.
 *
 * See LICENSE for the license.
 *
 */

#ifndef PKTC_H
#define PKTC_H
#include "answer.h"
struct radtree;
struct radnode;
struct cpkt;
struct zone;
struct domain_table;
struct domain;
struct region;
struct rr;

/**
 * Tree with compiled packets
 */
struct comptree {
	/** radix tree by name to a struct compname */
	struct radtree* nametree;
	/** tree of zones, for every zone an nsec3 tree, to compzone */
	struct radtree* zonetree;
};

/**
 * Compiled info for a zone
 * There are pointers to this structure for nsec3 content (NXDOMAINs),
 * from the compname structure.
 */
struct compzone {
	/** radix node for this element */
	struct radnode* rnode;
	/** zone name */
	uint8_t* name;
	/** unsigned nxdomain packet */
	struct cpkt* nx;
	/** unsigned nodata packet */
	struct cpkt* nodata;
	/** the tree of nsec3 hashes to compnsec3, for this zone */
	struct radtree* nsec3tree;
	/** nsec3 parameters for hashing */
	unsigned char* n3_salt;
	int n3_saltlen;
	int n3_iterations;
	/** soa serial number to insert into negative answers, network order,
	 * negative compiled packets have pointers to this value. */
	uint32_t serial;
	/** length of the zone name */
	uint8_t namelen;
};

/**
 * Compiled packets for an NSEC3-hash.
 */
struct compnsec3 {
	/** radix node for this element */
	struct radnode* rnode;
	/** the nsec3 node that covers the wildcard for the *.thisname,
	 * reference, can be NULL(thisname is never a CE),
	 * can be a pointer to this very node. */
	struct compnsec3* wc;
	/** the original node that hashed to this value, set for nodata
	 * answers (for wildcards) so that the wildcard-denial-nsec3 cannot
	 * be added twice to the answer */
	struct compname* rev;
	/** the denial NSEC3 packet for this hash span, for concatenation,
	 * contains only the authority section NSEC3(denial). */
	struct cpkt* denial;
};

#define BELOW_NORMAL 0 /* below is a cpkt */
#define BELOW_NSEC3NX 1 /* below is compzone, do nsec3 hashed nxdomain */
#define BELOW_WILDCARD 2 /* below is compname, do wildcard processing */
#define BELOW_SYNTHC 3 /* below is cpkt with DNAME. perform CNAME synth */

/**
 * Compiled packets for a domain name.
 * irrespective of zone.
 */
struct compname {
	/** radix node for this element */
	struct radnode* rnode;
	/** the compiled zone for (most of) these answers */
	struct compzone* cz;
	/** DEBUG: name of the node */
	uint8_t* name;
	/** length of specifics array */
	size_t typelen;
	/** length of nonDO specifics array */
	size_t typelen_nondo;
	/** specifics array, by qtype, to compiled packets for this qtype.
	 * includes TYPE_ANY, TYPE_RRSIG, ...  The array is sorted by qtype.
	 * also contains separate DS-denial if parent-zone, or referral-here.
	 * or DS-positive if secure-referral here. */
	struct cpkt** types;
	/** no type match, have name match, packet, to nodata or referral
	 *  if it is null, use the zone to get shared unsigned nodata cpkt. */
	struct cpkt* notype;
	/** the nonDO answers by type */
	struct cpkt** types_nondo;
	/** notype packet for nonDO queries */
	struct cpkt* notype_nondo;
	/** match below the name, qname is below this name, to nxdomain,
	 * dname or referral packet.
	 * For nsec3 need to go hash at compzone, for wildcard special todo */
	struct cpkt* below;
	/** belowptr for nonDO queries */
	struct cpkt* below_nondo;
	/** side match, the qname is after this name, for NSEC nxdomains.
	 * side is NULL if the closest-encloser below is (wildcard,nsec3nx). 
	 * for side, use the namelen of the ce for compressionadjustment. */
	struct cpkt* side;
	/** sidewc entry: used for NSEC wildcard qname denial, concatenated,
	 * thus its qname is the zone, noanswersection.  Used if
	 * a wildcard is instantiated, so that main cpkt cannot have rrsets
	 * in the additional section. */
	struct cpkt* sidewc;
	/** length of the wirefmt of this name, to calculate the prefix of
	 * the qname for nsec3 hashing and wildcards */
	uint8_t namelen;
	/** type of the below pointer.
	 *  BELOW_NORMAL use it, unless you have a side-match
	 *      set for referrals, and for nsec, nsec3 zones.
	 *      for the zone apex the below has the NSEC for first NSEC,
	 *      and the lower side ptrs have the other NSECs for nxdomain.
	 *  BELOW_NSEC3NX below is nxdomain cpkt with SOA,NSEC3(ce,wc). 
	 *	and below_nondo is ptr to compnsec3 matching hash for ce. 
	 *	so, that the cz->nsec3tree can be used to find qname denial,
	 *	and then ce and ce->wc can be used to check for duplicates.
	 *  BELOW_WILDCARD ptr to the *.x name below this.
	 *  BELOW_SYNTHC  ptr to cpkt with DNAME, perform CNAME synthesis.
	 *  if it is null, use the zone to get shared unsigned nxdomain cpkt.
	 */
	uint8_t belowtype;
	/** type of the below_nondo pointer */
	uint8_t belowtype_nondo;
};

/**
 * precompiled packet, the answer to a given name, type, class.
 * It needs to be adjusted for
 * 	o the qname
 * 	o the EDNS-OPT record.
 * 	o length (TC).
 * 	o flags RD, CD.
 * 	o serial number (in nodata, nxdomain).
 * Allocated in packed format in the order.
 * 	cpktstruct, truncpkts_u16, ptrs_u16, pktdata_u8
 */
struct cpkt {
	/** ptr to soa serial number to use, in network format (or NULL) */
	uint32_t* serial;
	/** packet data (often allocated behind this struct),
	 * contains answer,authority,additional section octets. */
	uint8_t* data;
	/** array of truncation points: length, arcount,
	 * goes down, first one is the whole packet. */
	uint16_t* truncpts;
	/** array of compression ptrs to adjust in the packet, offset in data.
	 * ends with a 0.  They point to host-order u16 offset values. */
	uint16_t* ptrs;
	/** qtype of the packet, 0 for nxdomains, referrals */
	uint16_t qtype;
	/** length of the original qname (for compression adjustment */
	uint16_t qnamelen;
	/** length of data segment */
	uint16_t datalen;
	/** flagcode, the u16 with flags, rcode, opcode for the result,
	 * needs to have RD,CD flags copied from query */
	uint16_t flagcode;
	/** the answer count. */
	uint16_t ancount;
	/** the authority count. Note flagcode, ancount, nscount are
	 * consecutive so a memcpy can do them at once */
	uint16_t nscount;
	/** truncation points, and the additional count that goes with it,
	 * if none fit, set TC flag on answer.
	 * number of truncation points. */
	uint16_t numtrunc;
	/** soa serial location in packet data (or 0 if none) */
	uint16_t serial_pos;
};

/** create comp tree, empty,
 * @return the new commptree. */
struct comptree* comptree_create(void);

/** delete comptree, frees all contents. */
void comptree_delete(struct comptree* ct);

/** create comp zone, add to tree,
 * @param ct: comptree to add it into.
 * @param zname: zone name, dname.
 * @return compzone object that has been added. */
struct compzone* compzone_create(struct comptree* ct, uint8_t* zname);

/** delete compzone, frees all contents. does not edit the zonetree. */
void compzone_delete(struct compzone* cz);

/** find a compzone by name, NULL if not found */
struct compzone* compzone_search(struct comptree* ct, uint8_t* name);

/** find a compzone by name, also returns closest-encloser if not found */
struct compzone* compzone_find(struct comptree* ct, uint8_t* name, int* ce);

/** add a new name to the nametree.
 * @param ct: comptree to add it into.
 * @param name: name, dname to add.
 * @param cz: zone of name (for most answers; specifically NXDOMAIN, NODATA).
 * @return compname object that has been added. */
struct compname* compname_create(struct comptree* ct, uint8_t* name,
	struct compzone* cz);

/** delete compname, frees all contents. does not edit the nametree. */
void compname_delete(struct compname* cn);

/** find a compname by name, NULL if not found */
struct compname* compname_search(struct comptree* ct, uint8_t* name);

/** add a new nsec3 to the nsec3tree.
 * @param cz: zone with tree to add it to.
 * @param hash: hash to add it for.
 * @param hashlen: length of hash.
 * @return compnsec3 object that has been added (empty). */
struct compnsec3* compnsec3_create(struct compzone* cz, unsigned char* hash,
	size_t hashlen);

/** find a compnsec3 by hash, NULL if not found */
struct compnsec3* compnsec3_search(struct compzone* cz, unsigned char* hash,
	size_t hashlen);

/** delete compnsec3, frees contents, does not edit tree */
void compnsec3_delete(struct compnsec3* c3);

/** find a compnsec3 denial by hash, NULL if not found or exact match,
 * returns covering nsec3 if one exists. */
struct compnsec3* compnsec3_find_denial(struct compzone* cz,
	unsigned char* hash, size_t hashlen);

/** packer compiling input, the answer to compile */
struct answer_info {
	/** qname, or ce */
	uint8_t* qname;
	/** qtype or 0 */
	uint16_t qtype;
	/** can this answer compressptrs be adjusted after compilation */
	int adjust;
	/** perform special wildcard adjustment:
	 * *.bla owner names are changed to qname compression ptrs(noajust)
	 * other ptrs are adjusted towards the qname (the parent of the wc) */
	int wildcard;
	/** does this answer have DO (DNSSEC resource records) added */
	int withdo;
	/** flags and rcode */
	uint16_t flagcode;
	/** rrsets in sections */
	struct answer answer;
	/** temp region during answer compilation (for wildcards in the
	 * additional and so on) */
	struct region* region;
};

/** precompile environment */
struct prec_env {
	/** the compile tree */
	struct comptree* ct;
	/** the compile zone */
	struct compzone* cz;
	/** the compiled name */ 
	struct compname* cn;

	/** the domain table */
	struct domain_table* table;
	/** the  */

	/** the current answer under development */
	struct answer_info ai;
	
};

/** create a compiled packet structure, encode from RR data.
 * creates compression pointers.
 * @param qname: the qname for this packet.
 * @param qtype: qtype or 0.
 * @param adjust: if true, a compression pointer adjustment list is created.
 * 	set this to true for NXDOMAINs, DNAME, referrals, wildcard.
 * @param wildcard: if true, owner name must be a wildcard (changed to
 *   its parent), and answers are compressed so that wildcards are instantiated
 * @param flagcode: flagcode to set on packet
 * @param num_an: number an rrs.
 * @param num_ns: number ns rrs.
 * @param num_ar: number ar rrs.
 * @param rrname: array of pointers to RR name
 * @param rrinfo: array of RR data elements.
 * @param cz: compiled zone for soa serial.
 * @return compiled packet, allocated.
 */
struct cpkt* compile_packet(uint8_t* qname, uint16_t qtype, int adjust,
	int wildcard, uint16_t flagcode, uint16_t num_an, uint16_t num_ns,
	uint16_t num_ar, uint8_t** rrname, struct rr** rrinfo,
	struct compzone* cz);

/** delete a compiled packet structure, frees its contents */
void cpkt_delete(struct cpkt* cp);

/** compare two cpkts and return if -, 0, + for sort order by qtype */
int cpkt_compare_qtype(const void* a, const void* b);

/** determine packets to compile, based on zonelist and nametree to lookup.
 * @param ct: the compiled packet tree that is filled up.
 * @param zonelist: list of zones.
 */
void compile_zones(struct comptree* ct, struct zone* zonelist);

/** add a zone and determine packets to compile for this zone.
 * @param ct: compiled packet tree that is filled up.
 * @param cz: compiled zone structure for the zone.
 * @param zone: the zone.
 */
void compile_zone(struct comptree* ct, struct compzone* cz, struct zone* zone);

/** compile the packets for one name in one zone. It may or may not add
 * the compiled-name to the tree (not for occluded items, glue).
 * @param ct: compiled packet tree.
 * @param cz: the zone to compile the name for.
 * @param zone: the zone
 * @param domain: the named domain in the namelookup structure.
 * @param is_signed: true if the zone is signed.
 */
void compile_name(struct comptree* ct, struct compzone* cz, struct zone* zone,
	struct domain* domain, int is_signed);

enum domain_type_enum {
	dtype_normal, /* a normal domain name */
	dtype_notexist, /* notexist, nsec3, glue, occluded */
	dtype_delegation, /* not apex, has type NS */
	dtype_cname, /* has CNAME */
	dtype_dname /* has DNAME */
};

/** determine the type of the domain */
enum domain_type_enum determine_domain_type(struct domain* domain,
        struct zone* zone, int* apex);
/** length of wireformat dname (including end0). */
size_t dname_length(uint8_t* dname);
/** return static buffer with dname, formatted with escapecodes */
char* dname2str(uint8_t* dname);
/** create a compression pointer to the given offset. */
#define PTR_CREATE(offset) ((uint16_t)(0xc000 | (offset)))
#define MAX_COMPRESS_PTRS 10000
/** Check if label length is first octet of a compression pointer, pass u8. */
#define LABEL_IS_PTR(x) ( ((x)&0xc0) == 0xc0 )
/** Calculate destination offset of a compression pointer. pass first and
 *  * second octets of the compression pointer. */
#define PTR_OFFSET(x, y) ( ((x)&0x3f)<<8 | (y) )
/** determine uncompressed length of (compressed)name at position */
size_t pkt_dname_len_at(buffer_type* pkt, size_t pos);
/** lowercase dname, canonicalise, dname is uncompressed. */
void dname_tolower(uint8_t* dname);

#endif /* PKTC_H */

%{
/*
 * $Id: zyparser.y,v 1.7 2003/08/18 16:20:03 miekg Exp $
 *
 * zyparser.y -- yacc grammar for (DNS) zone files
 *
 * Copyright (c) 2001-2003, NLnet Labs. All rights reserved.
 *
 * See LICENSE for the license
 */

#include "zparser2.h"
#include<stdio.h>
#include<string.h>

/* these are global, otherwise they cannot be used inside yacc */
unsigned int lineno;
struct zdefault_t * zdefault;
struct node_t * root;       /* root of the list */
struct node_t * rrlist;     /* hold all RRs as a linked list */
struct RR * current_rr;

/* [XXX] needs to be local */
unsigned int error = 0;
int progress = 5;
int yydebug = 1;

%}
/* this list must be in exactly the same order as *RRtypes[] in zlparser.lex. 
 * The only changed are:
 * - NSAP-PRT is named NSAP_PTR
 * - NULL which is named YYNULL.
 */
%token A NS MX TXT CNAME AAAA PTR NXT KEY SOA SIG SRV CERT LOC MD MF MB
%token MG MR YYNULL WKS HINFO MINFO RP AFSDB X25 ISDN RT NSAP NSAP_PTR PX GPOS 
%token EID NIMLOC ATMA NAPTR KX A6 DNAME SINK OPT APL UINFO UID GID 
%token UNSPEC TKEY TSIG IXFR AXFR MAILB MAILA
/* other tokens */
%token ORIGIN NL SP STR DIR_TTL DIR_ORIG PREV IN CH HS 
/* unknown RR */
%token UN_RR UN_CLASS UN_TYPE

%%
lines:  /* empty line */
    |   lines line
    { if ( lineno % progress == 0 )
        printf("\nzonec: reading zone \"%s\": %d\n", "configured zone name",
        lineno);
    }
    |    error      { yyerrok; }
    ;

line:   NL
    |   DIR_TTL dir_ttl
    |   DIR_ORIG dir_orig
    |   rr
    {   /* rr should be fully parsed */
        rrlist = list_add(rrlist, current_rr);
        current_rr = xalloc(sizeof(struct RR));
        zreset_current_rr(zdefault);
    }
    ;

dir_ttl:    SP STR NL
    { 
        if ($2->len > MAXDNAME ) {
            yyerror("TTL thingy too large");
            return 1;
        } 
        printf("\nttl-directive parsed: %s\n",  $2->str);
        /* perform TTL conversion */
        if ( ( zdefault->ttl = zparser_ttl2int($2->str)) == -1 )
            zdefault->ttl = DEFAULT_TTL;
    }
    ;

dir_orig:   SP dname NL
    {
        /* [xxx] does $origin not effect previous */
        if ( $2->len > MAXDNAME ) { 
            yyerror("origin thingy too large");
            return 1;
        } 
        zdefault->origin = (uint8_t *)dnamedup($2->str);
        zdefault->origin_len = $2->len;
    }
    ;

rr:     ORIGIN SP rrrest NL
    {
        /* starts with @, use the origin */
        current_rr->dname = (uint8_t *) dnamedup(zdefault->origin);
        printf("dname  %s",dnamestr(current_rr->dname));

        /* also set this as the prev_dname */
        zdefault->prev_dname = zdefault->origin;
        zdefault->prev_dname_len = zdefault->origin_len; /* what about
        this len? */
    }
    |   PREV rrrest NL
    {
        /* a tab, use previously defined dname */
        /* [XXX] is null -> error, not checked (yet) MIEK */
        current_rr->dname = (uint8_t *) dnamedup(zdefault->prev_dname);
    }
    |   dname SP rrrest NL
    {
        printf("dname: %s\n", $1->str);

        current_rr->dname = $1->str;

        /* set this as previous */
        zdefault->prev_dname = $1->str;
        zdefault->prev_dname_len = $1->len;
    }
    ;
 
ttl:    STR
    {
        /* set the ttl */
        if ( (current_rr->ttl = zparser_ttl2int($1->str) ) == -1 )
            current_rr->ttl = DEFAULT_TTL;
    }
    ;

in:     IN
    {
        /* set the class */
        current_rr->class =  zdefault->class;
    }
    ;

rrrest: classttl rtype 
    {
        /* terminate the rdata list - NULL does not have rdata */
        zadd_rdata_finalize(zdefault);
    }
    ;

classttl:   /* empty - fill in the default, def ttl and in */
    |   in SP         /* no ttl */
    {
        current_rr->ttl = zdefault->ttl;
    }
    |   ttl SP in SP  /* the lot */
    |   in SP ttl SP  /* the lot - reversed */
    |   ttl SP        /* no class */
    {   
        current_rr->class = zdefault->class;
    }
    |   CH SP         { yyerror("chaos class not supported"); }
    |   HS SP         { yyerror("hesiod class not supported"); }
    |   ttl CH SP         { yyerror("chaos class not supported"); }
    |   ttl HS SP         { yyerror("hesiod class not supported"); }
    |   CH ttl SP         { yyerror("chaos class not supported"); }
    |   HS ttl SP         { yyerror("hesiod class not supported"); }
    ;

dname:  abs_dname
    {
        $$->str = $1->str;
        $$->len = $1->len;  /* length really not important anymore */
        printf("NEW ABS\n");
    }
    |   rel_dname
    {
        /* append origin */
        $$->str = (uint8_t *)cat_dname($$->str, zdefault->origin);
        $$->len = $1->len;
        printf("NEW REL\n");
    }
    ;

abs_dname:  '.'
    {
            $$->str = (uint8_t *)dnamedup(ROOT);
            $$->len = 1;
    }
    |       rel_dname '.'
    {
            $$->str = $1->str;
            $$->len = $1->len;
    }
    ;

rel_dname:  STR
    {
        $$->str = (uint8_t *) creat_dname($1->str, $1->len);
        $$->len = $1->len + 2; /* total length, label + len byte */
    }
    |       rel_dname '.' STR
    {  
        $$->str = (uint8_t *) cat_dname ($1->str, creat_dname($3->str,
                    $3->len));
        $$->len = $1->len + $3->len;
    }
    ;

/* define what we can parse */

rtype:  SOA SP rdata_soa
    {   
        zadd_rtype("soa");
    }
    |   A SP rdata_a
    {
        zadd_rtype("a");
    }
    |   NS SP rdata_ns
    {
        zadd_rtype("ns");
    }
    |   TXT SP rdata_txt
    {
        zadd_rtype("txt");
    }
    ;


/* 
 * below are all the definition for all the different rdata 
 */

rdata_soa:  dname SP dname SP STR STR STR STR STR
    {
        /* convert the soa data */
        zadd_rdata2( zdefault, zparser_conv_dname($1->str) );   /* prim. ns */
        zadd_rdata2( zdefault, zparser_conv_dname($3->str) );   /* email */
        zadd_rdata2( zdefault, zparser_conv_rdata_period($5->str) ); /* serial */
        zadd_rdata2( zdefault, zparser_conv_rdata_period($6->str) ); /* obscure item */
        zadd_rdata2( zdefault, zparser_conv_rdata_period($7->str) ); /* obscure item */
        zadd_rdata2( zdefault, zparser_conv_rdata_period($8->str) ); /* obscure item */
        zadd_rdata2( zdefault, zparser_conv_rdata_period($9->str) ); /* minimum */

        /* [XXX] also store the minium in case of no TTL? */
        if ( (zdefault->minimum = zparser_ttl2int($9->str) ) == -1 )
            zdefault->minimum = DEFAULT_TTL;
    }
    ;

rdata_ns:   dname
    {
        /* convert a nameserver record */
        zadd_rdata2( zdefault, zparser_conv_dname($1->str) ); /* nameserver */
    }
    ;

rdata_a:    STR '.' STR '.' STR '.' STR
    {
        /* setup the string suitable for parsing */
        uint8_t * ipv4 = xalloc($1->len + $3->len + $5->len +
                            $7->len + 3);
        sprintf(ipv4,"%s.%s.%s.%s",$1->str, $3->str, $5->str,
                            $7->str);
        zadd_rdata2(zdefault, zparser_conv_A((char*)ipv4));
        free(ipv4);
    }
    ;

rdata_txt:  STR
    {
        zadd_rdata2( zdefault, zparser_conv_text($1->str));
    }
    ;


%%

int yywrap(){
    return 1;
}

int yyerror(char *s) {
    fprintf(stderr,"\n[%d]error: %s: %s\n", lineno, s, yylval);
    if ( error++ > 50 ) {
        fprintf(stderr,"too many errors (50+)\n");
        exit(1);
    }
}

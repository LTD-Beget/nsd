%{
/*
 * zlexer.lex - lexical analyzer for (DNS) zone files
 * 
 * Copyright (c) 2001-2004, NLnet Labs. All rights reserved
 *
 * See LICENSE for the license.
 *
 */

#include <config.h>

#include <ctype.h>
#include <errno.h>
#include <string.h>
#include <strings.h>

#include "zonec.h"
#include "dname.h"
#include "zparser.h"

#if 0
#define LEXOUT(s)  printf s /* used ONLY when debugging */
#else
#define LEXOUT(s)
#endif

enum lexer_state {
	EXPECT_OWNER,
	PARSING_OWNER,
	PARSING_TTL_CLASS_TYPE,
	PARSING_RDATA
};

static int parse_token(int token, char *yytext, enum lexer_state *lexer_state);

static YY_BUFFER_STATE include_stack[MAXINCLUDES];
static zparser_type zparser_stack[MAXINCLUDES];
static int include_stack_ptr = 0;

static void
push_parser_state(FILE *input)
{
	/*
	 * Save file specific variables on the include stack.
	 */
	zparser_stack[include_stack_ptr].filename = parser->filename;
	zparser_stack[include_stack_ptr].line = parser->line;
	zparser_stack[include_stack_ptr].origin = parser->origin;
	include_stack[include_stack_ptr] = YY_CURRENT_BUFFER;
	yy_switch_to_buffer(yy_create_buffer(input, YY_BUF_SIZE));
	++include_stack_ptr;
}

static void
pop_parser_state(void)
{
	--include_stack_ptr;
	parser->filename = zparser_stack[include_stack_ptr].filename;
	parser->line = zparser_stack[include_stack_ptr].line;
	parser->origin = zparser_stack[include_stack_ptr].origin;
	yy_delete_buffer(YY_CURRENT_BUFFER);
	yy_switch_to_buffer(include_stack[include_stack_ptr]);
}
	
%}

SPACE   [ \t]
LETTER  [a-zA-Z]
NEWLINE \n
ZONESTR [a-zA-Z0-9+/=:_!\-\*#%&^\[\]?]
DOLLAR  \$
COMMENT ;
DOT     \.
BIT	[^\]\n]|\\.
ANY     [^\"\n]|\\.
Q       \"

%x	incl bitlabel quotedstring

%%
	static int paren_open = 0;
	static enum lexer_state lexer_state = EXPECT_OWNER;
{SPACE}*{COMMENT}.*	/* ignore */
^{DOLLAR}TTL            { lexer_state = PARSING_RDATA; return DOLLAR_TTL; }
^{DOLLAR}ORIGIN         { lexer_state = PARSING_RDATA; return DOLLAR_ORIGIN; }

	/*
	 * Handle $INCLUDE directives.  See
	 * http://dinosaur.compilertools.net/flex/flex_12.html#SEC12.
	 */
^{DOLLAR}INCLUDE        {
	BEGIN(incl);
}
<incl>\n 		|
<incl><<EOF>>		{
	int error_occurred = parser->error_occurred;
	BEGIN(INITIAL);
	zc_error("Missing file name in $INCLUDE directive");
	yy_set_bol(1); /* Set beginning of line, so "^" rules match.  */
	++parser->line;
	parser->error_occurred = error_occurred;
}
<incl>.+ 		{ 	
	char *tmp;
	domain_type *origin = parser->origin;
	int error_occurred = parser->error_occurred;
	
	BEGIN(INITIAL);
	if (include_stack_ptr >= MAXINCLUDES ) {
		zc_error("Includes nested too deeply, skipped (>%d)",
			 MAXINCLUDES);
	} else {
		/* Remove trailing comment.  */
		tmp = strrchr(yytext, ';');
		if (tmp) {
			*tmp = '\0';
		}
		strip_string(yytext);
		
		/* Parse origin for include file.  */
		tmp = strrchr(yytext, ' ');
		if (!tmp) {
			tmp = strrchr(yytext, '\t');
		}
		if (tmp) {
			const dname_type *dname;
			
			/* split the original yytext */
			*tmp = '\0';
			strip_string(yytext);
			
			dname = dname_parse(parser->region, tmp + 1);
			if (!dname) {
				zc_error("incorrect include origin '%s'",
					 tmp + 1);
			} else {
				origin = domain_table_insert(
					parser->db->domains, dname);
			}
		}
		
		if (strlen(yytext) == 0) {
			zc_error("Missing file name in $INCLUDE directive");
		} else if (!(yyin = fopen(yytext, "r"))) {
			zc_error("Cannot open include file '%s': %s",
				 yytext, strerror(errno));
		} else {
			/* Initialize parser for include file.  */
			char *filename = region_strdup(parser->region, yytext);
			push_parser_state(yyin); /* Destroys yytext.  */
			parser->filename = filename;
			parser->line = 1;
			parser->origin = origin;
			lexer_state = EXPECT_OWNER;
		}
	}

	parser->error_occurred = error_occurred;
}
<INITIAL><<EOF>>			{
	yy_set_bol(1); /* Set beginning of line, so "^" rules match.  */
	if (include_stack_ptr == 0) {
		yyterminate();
	} else {
		pop_parser_state();
	}
}
^{DOLLAR}{LETTER}+      { zc_warning("Unknown $directive: %s", yytext); }
{DOT}                   {
	LEXOUT((". "));
	return parse_token('.', yytext, &lexer_state);
}
@ 			{
	LEXOUT(("@ "));
	return parse_token('@', yytext, &lexer_state);
}
\\# {
	LEXOUT(("\\# "));
	return parse_token(URR, yytext, &lexer_state);
}
{NEWLINE}               {
	++parser->line;
	if (!paren_open) { 
		lexer_state = EXPECT_OWNER;
		LEXOUT(("NL\n"));
		return NL;
	} else {
		LEXOUT(("SP "));
		return SP;
	}
}
\( {
	if (paren_open) {
		zc_error("Nested parentheses");
		yyterminate();
	}
	LEXOUT(("( "));
	paren_open = 1;
	return SP;
}
\) {
	if (!paren_open) {
		zc_error("Unterminated parentheses");
		yyterminate();
	}
	LEXOUT((") "));
	paren_open = 0;
	return SP;
}
{SPACE}+                {
	if (!paren_open && lexer_state == EXPECT_OWNER) {
		lexer_state = PARSING_TTL_CLASS_TYPE;
		LEXOUT(("PREV "));
		return PREV;
	}
	if (lexer_state == PARSING_OWNER) {
		lexer_state = PARSING_TTL_CLASS_TYPE;
	}
	LEXOUT(("SP "));
	return SP;
}

	/* Bitlabels.  Strip leading and ending brackets.  */
\\\[			{ BEGIN(bitlabel); }
<bitlabel><<EOF>> 	{
	zc_error("EOF inside bitlabel");
	BEGIN(INITIAL);
}
<bitlabel>{BIT}*	{ yymore(); }
<bitlabel>\n 		{ ++parser->line; yymore(); }
<bitlabel>\] 		{
	BEGIN(INITIAL);
	yytext[yyleng - 1] = '\0';
	return parse_token(BITLAB, yytext, &lexer_state);
}

	/* Quoted strings.  Strip leading and ending quotes.  */
{Q}			{ BEGIN(quotedstring); }
<quotedstring><<EOF>> 	{
	zc_error("EOF inside quoted string");
	BEGIN(INITIAL);
}
<quotedstring>{ANY}*	{ yymore(); }
<quotedstring>\n 	{ ++parser->line; yymore(); }
<quotedstring>\" {
	BEGIN(INITIAL);
	yytext[strlen(yytext) - 1] = '\0';
	return parse_token(STR, yytext, &lexer_state);
}

({ZONESTR}|\\.)+ {
	/* Any allowed word.  */
	return parse_token(STR, yytext, &lexer_state);
}
. {
	zc_error("Unknown character '%c' (\\%03d) seen - is this a zonefile?",
		 (int) yytext[0], (int) yytext[0]);
}
%%

/*
 * Analyze "word" to see if it matches an RR type, possibly by using
 * the "TYPExxx" notation.  If it matches, the corresponding token is
 * returned and the TYPE parameter is set to the RR type value.
 */
static int
rrtype_to_token(const char *word, uint16_t *type) 
{
	uint16_t t = rrtype_from_string(word);
	if (t != 0) {
		rrtype_descriptor_type *entry = rrtype_descriptor_by_type(t);
		*type = t;
		return entry->token;
	}

	return 0;
}


/*
 * Remove \DDD constructs from the input. See RFC 1035, section 5.1.
 */
static size_t
zoctet(char *text) 
{
	/*
	 * s follows the string, p lags behind and rebuilds the new
	 * string
	 */
	char *s;
	char *p;
	
	for (s = p = text; *s; ++s, ++p) {
		assert(p <= s);
		if (s[0] != '\\') {
			/* Ordinary character.  */
			*p = *s;
		} else if (isdigit(s[1]) && isdigit(s[2]) && isdigit(s[3])) {
			/* \DDD escape.  */
			int val = (digittoint(s[1]) * 100 +
				   digittoint(s[2]) * 10 +
				   digittoint(s[3]));
			if (0 <= val && val <= 255) {
				s += 3;
				*p = val;
			} else {
				zc_warning("text escape \\DDD overflow");
				*p = *++s;
			}
		} else if (s[1] != '\0') {
			/* \X where X is any character, keep X.  */
			*p = *++s;
		} else {
			/* Trailing backslash, ignore it.  */
			zc_warning("trailing backslash ignored");
			--p;
		}
	}
	return p - text;
}

static int
parse_token(int token, char *yytext, enum lexer_state *lexer_state)
{
	if (*lexer_state == EXPECT_OWNER) {
		*lexer_state = PARSING_OWNER;
	} else if (*lexer_state == PARSING_TTL_CLASS_TYPE) {
		const char *t;
		int token;
		uint16_t rrclass;
		
		/* type */
		token = rrtype_to_token(yytext, &yylval.type);
		if (token != 0) {
			*lexer_state = PARSING_RDATA;
			return token;
		}

		/* class */
		rrclass = rrclass_from_string(yytext);
		if (rrclass != 0) {
			yylval.klass = rrclass;
			LEXOUT(("CLASS "));
			return T_RRCLASS;
		}

		/* ttl */
		yylval.ttl = strtottl(yytext, &t);
		if (*t == '\0') {
			LEXOUT(("TTL "));
			return T_TTL;
		}
	}
	
	yylval.data.str = region_strdup(parser->rr_region, yytext);
	yylval.data.len = zoctet(yylval.data.str);
	LEXOUT(("%d ", token));
	return token;
}

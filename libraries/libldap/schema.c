/* $OpenLDAP$ */
/*
 * Copyright 1999-2000 The OpenLDAP Foundation, All Rights Reserved.
 * COPYING RESTRICTIONS APPLY, see COPYRIGHT file
 */
/*
 * schema.c:  parsing routines used by servers and clients to process
 *	schema definitions
 */

#include "portable.h"

#include <stdio.h>
#include <ac/stdlib.h>

#include <ac/ctype.h>
#include <ac/string.h>
#include <ac/time.h>

#include "ldap-int.h"

#include <ldap_schema.h>


static LDAP_CONST char *
choose_name( char *names[], LDAP_CONST char *fallback )
{
	return( (names != NULL && names[0] != NULL) ? names[0] : fallback );
}

LDAP_CONST char *
ldap_syntax2name( LDAP_SYNTAX * syn )
{
	return( syn->syn_oid );
}

LDAP_CONST char *
ldap_matchingrule2name( LDAP_MATCHING_RULE * mr )
{
	return( choose_name( mr->mr_names, mr->mr_oid ) );
}

LDAP_CONST char *
ldap_attributetype2name( LDAP_ATTRIBUTE_TYPE * at )
{
	return( choose_name( at->at_names, at->at_oid ) );
}

LDAP_CONST char *
ldap_objectclass2name( LDAP_OBJECT_CLASS * oc )
{
	return( choose_name( oc->oc_names, oc->oc_oid ) );
}


/*
 * When pretty printing the entities we will be appending to a buffer.
 * Since checking for overflow, realloc'ing and checking if no error
 * is extremely boring, we will use a protection layer that will let
 * us blissfully ignore the error until the end.  This layer is
 * implemented with the help of the next type.
 */

typedef struct safe_string {
	char * val;
	ber_len_t size;
	ber_len_t pos;
	int at_whsp;
} safe_string;

static safe_string *
new_safe_string(int size)
{
	safe_string * ss;
	
	ss = LDAP_MALLOC(sizeof(safe_string));
	if ( !ss )
		return(NULL);

	ss->val = LDAP_MALLOC(size);
	if ( !ss->val ) {
		LDAP_FREE(ss);
		return(NULL);
	}

	ss->size = size;
	ss->pos = 0;
	ss->at_whsp = 0;

	return ss;
}

static void
safe_string_free(safe_string * ss)
{
	if ( !ss )
		return;
	LDAP_FREE(ss->val);
	LDAP_FREE(ss);
}

static char *
safe_string_val(safe_string * ss)
{
	ss->val[ss->pos] = '\0';
	return(ss->val);
}

static int
append_to_safe_string(safe_string * ss, char * s)
{
	int l = strlen(s);
	char * temp;

	/*
	 * Some runaway process is trying to append to a string that
	 * overflowed and we could not extend.
	 */
	if ( !ss->val )
		return -1;

	/* We always make sure there is at least one position available */
	if ( ss->pos + l >= ss->size-1 ) {
		ss->size *= 2;
		temp = LDAP_REALLOC(ss->val, ss->size);
		if ( !temp ) {
			/* Trouble, out of memory */
			LDAP_FREE(ss->val);
			return -1;
		}
		ss->val = temp;
	}
	strncpy(&ss->val[ss->pos], s, l);
	ss->pos += l;
	if ( ss->pos > 0 && isspace(ss->val[ss->pos-1]) )
		ss->at_whsp = 1;
	else
		ss->at_whsp = 0;

	return 0;
}

static int
print_literal(safe_string *ss, char *s)
{
	return(append_to_safe_string(ss,s));
}

static int
print_whsp(safe_string *ss)
{
	if ( ss->at_whsp )
		return(append_to_safe_string(ss,""));
	else
		return(append_to_safe_string(ss," "));
}

static int
print_numericoid(safe_string *ss, char *s)
{
	if ( s )
		return(append_to_safe_string(ss,s));
	else
		return(append_to_safe_string(ss,""));
}

/* This one is identical to print_qdescr */
static int
print_qdstring(safe_string *ss, char *s)
{
	print_whsp(ss);
	print_literal(ss,"'");
	append_to_safe_string(ss,s);
	print_literal(ss,"'");
	return(print_whsp(ss));
}

static int
print_qdescr(safe_string *ss, char *s)
{
	print_whsp(ss);
	print_literal(ss,"'");
	append_to_safe_string(ss,s);
	print_literal(ss,"'");
	return(print_whsp(ss));
}

static int
print_qdescrlist(safe_string *ss, char **sa)
{
	char **sp;
	int ret = 0;
	
	for (sp=sa; *sp; sp++) {
		ret = print_qdescr(ss,*sp);
	}
	/* If the list was empty, we return zero that is potentially
	 * incorrect, but since we will be still appending things, the
	 * overflow will be detected later.  Maybe FIX.
	 */
	return(ret);
}

static int
print_qdescrs(safe_string *ss, char **sa)
{
	/* The only way to represent an empty list is as a qdescrlist
	 * so, if the list is empty we treat it as a long list.
	 * Really, this is what the syntax mandates.  We should not
	 * be here if the list was empty, but if it happens, a label
	 * has already been output and we cannot undo it.
	 */
	if ( !sa[0] || ( sa[0] && sa[1] ) ) {
		print_whsp(ss);
		print_literal(ss,"(");
		print_qdescrlist(ss,sa);
		print_literal(ss,")");
		return(print_whsp(ss));
	} else {
	  return(print_qdescr(ss,*sa));
	}
}

static int
print_woid(safe_string *ss, char *s)
{
	print_whsp(ss);
	append_to_safe_string(ss,s);
	return print_whsp(ss);
}

static int
print_oidlist(safe_string *ss, char **sa)
{
	char **sp;

	for (sp=sa; *(sp+1); sp++) {
		print_woid(ss,*sp);
		print_literal(ss,"$");
	}
	return(print_woid(ss,*sp));
}

static int
print_oids(safe_string *ss, char **sa)
{
	if ( sa[0] && sa[1] ) {
		print_literal(ss,"(");
		print_oidlist(ss,sa);
		print_whsp(ss);
		return(print_literal(ss,")"));
	} else {
		return(print_woid(ss,*sa));
	}
}

static int
print_noidlen(safe_string *ss, char *s, int l)
{
	char buf[64];
	int ret;

	ret = print_numericoid(ss,s);
	if ( l ) {
		sprintf(buf,"{%d}",l);
		ret = print_literal(ss,buf);
	}
	return(ret);
}

static int
print_extensions(safe_string *ss, LDAP_SCHEMA_EXTENSION_ITEM **extensions)
{
	LDAP_SCHEMA_EXTENSION_ITEM **ext;

	if ( extensions ) {
		print_whsp(ss);
		for ( ext = extensions; *ext != NULL; ext++ ) {
			print_literal(ss, (*ext)->lsei_name);
			print_whsp(ss);
			print_qdescrs(ss, (*ext)->lsei_values);
			print_whsp(ss);
		}
	}
}

char *
ldap_syntax2str( const LDAP_SYNTAX * syn )
{
	safe_string * ss;
	char * retstring;
	
	ss = new_safe_string(256);
	if ( !ss )
		return NULL;

	print_literal(ss,"(");
	print_whsp(ss);

	print_numericoid(ss, syn->syn_oid);
	print_whsp(ss);

	if ( syn->syn_desc ) {
		print_literal(ss,"DESC");
		print_qdstring(ss,syn->syn_desc);
	}

	print_whsp(ss);

	print_extensions(ss, syn->syn_extensions);

	print_literal(ss,")");

	retstring = LDAP_STRDUP(safe_string_val(ss));
	safe_string_free(ss);
	return(retstring);
}

char *
ldap_matchingrule2str( const LDAP_MATCHING_RULE * mr )
{
	safe_string * ss;
	char * retstring;
	
	ss = new_safe_string(256);
	if ( !ss )
		return NULL;

	print_literal(ss,"(");
	print_whsp(ss);

	print_numericoid(ss, mr->mr_oid);
	print_whsp(ss);

	if ( mr->mr_names ) {
		print_literal(ss,"NAME");
		print_qdescrs(ss,mr->mr_names);
	}

	if ( mr->mr_desc ) {
		print_literal(ss,"DESC");
		print_qdstring(ss,mr->mr_desc);
	}

	if ( mr->mr_obsolete == LDAP_SCHEMA_YES ) {
		print_literal(ss, "OBSOLETE");
		print_whsp(ss);
	}

	if ( mr->mr_syntax_oid ) {
		print_literal(ss,"SYNTAX");
		print_whsp(ss);
		print_literal(ss, mr->mr_syntax_oid);
		print_whsp(ss);
	}

	print_whsp(ss);

	print_extensions(ss, mr->mr_extensions);

	print_literal(ss,")");

	retstring = LDAP_STRDUP(safe_string_val(ss));
	safe_string_free(ss);
	return(retstring);
}

char *
ldap_objectclass2str( const LDAP_OBJECT_CLASS * oc )
{
	safe_string * ss;
	char * retstring;
	
	ss = new_safe_string(256);
	if ( !ss )
		return NULL;

	print_literal(ss,"(");
	print_whsp(ss);

	print_numericoid(ss, oc->oc_oid);
	print_whsp(ss);

	if ( oc->oc_names ) {
		print_literal(ss,"NAME");
		print_qdescrs(ss,oc->oc_names);
	}

	if ( oc->oc_desc ) {
		print_literal(ss,"DESC");
		print_qdstring(ss,oc->oc_desc);
	}

	if ( oc->oc_obsolete == LDAP_SCHEMA_YES ) {
		print_literal(ss, "OBSOLETE");
		print_whsp(ss);
	}

	if ( oc->oc_sup_oids ) {
		print_literal(ss,"SUP");
		print_whsp(ss);
		print_oids(ss,oc->oc_sup_oids);
		print_whsp(ss);
	}

	switch (oc->oc_kind) {
	case LDAP_SCHEMA_ABSTRACT:
		print_literal(ss,"ABSTRACT");
		break;
	case LDAP_SCHEMA_STRUCTURAL:
		print_literal(ss,"STRUCTURAL");
		break;
	case LDAP_SCHEMA_AUXILIARY:
		print_literal(ss,"AUXILIARY");
		break;
	default:
		print_literal(ss,"KIND-UNKNOWN");
		break;
	}
	print_whsp(ss);
	
	if ( oc->oc_at_oids_must ) {
		print_literal(ss,"MUST");
		print_whsp(ss);
		print_oids(ss,oc->oc_at_oids_must);
		print_whsp(ss);
	}

	if ( oc->oc_at_oids_may ) {
		print_literal(ss,"MAY");
		print_whsp(ss);
		print_oids(ss,oc->oc_at_oids_may);
		print_whsp(ss);
	}

	print_whsp(ss);

	print_extensions(ss, oc->oc_extensions);

	print_literal(ss,")");

	retstring = LDAP_STRDUP(safe_string_val(ss));
	safe_string_free(ss);
	return(retstring);
}

char *
ldap_attributetype2str( const LDAP_ATTRIBUTE_TYPE * at )
{
	safe_string * ss;
	char * retstring;
	
	ss = new_safe_string(256);
	if ( !ss )
		return NULL;

	print_literal(ss,"(");
	print_whsp(ss);

	print_numericoid(ss, at->at_oid);
	print_whsp(ss);

	if ( at->at_names ) {
		print_literal(ss,"NAME");
		print_qdescrs(ss,at->at_names);
	}

	if ( at->at_desc ) {
		print_literal(ss,"DESC");
		print_qdstring(ss,at->at_desc);
	}

	if ( at->at_obsolete == LDAP_SCHEMA_YES ) {
		print_literal(ss, "OBSOLETE");
		print_whsp(ss);
	}

	if ( at->at_sup_oid ) {
		print_literal(ss,"SUP");
		print_woid(ss,at->at_sup_oid);
	}

	if ( at->at_equality_oid ) {
		print_literal(ss,"EQUALITY");
		print_woid(ss,at->at_equality_oid);
	}

	if ( at->at_ordering_oid ) {
		print_literal(ss,"ORDERING");
		print_woid(ss,at->at_ordering_oid);
	}

	if ( at->at_substr_oid ) {
		print_literal(ss,"SUBSTR");
		print_woid(ss,at->at_substr_oid);
	}

	if ( at->at_syntax_oid ) {
		print_literal(ss,"SYNTAX");
		print_whsp(ss);
		print_noidlen(ss,at->at_syntax_oid,at->at_syntax_len);
		print_whsp(ss);
	}

	if ( at->at_single_value == LDAP_SCHEMA_YES ) {
		print_literal(ss,"SINGLE-VALUE");
		print_whsp(ss);
	}

	if ( at->at_collective == LDAP_SCHEMA_YES ) {
		print_literal(ss,"COLLECTIVE");
		print_whsp(ss);
	}

	if ( at->at_no_user_mod == LDAP_SCHEMA_YES ) {
		print_literal(ss,"NO-USER-MODIFICATION");
		print_whsp(ss);
	}

	if ( at->at_usage != LDAP_SCHEMA_USER_APPLICATIONS ) {
		print_literal(ss,"USAGE");
		print_whsp(ss);
		switch (at->at_usage) {
		case LDAP_SCHEMA_DIRECTORY_OPERATION:
			print_literal(ss,"directoryOperation");
			break;
		case LDAP_SCHEMA_DISTRIBUTED_OPERATION:
			print_literal(ss,"distributedOperation");
			break;
		case LDAP_SCHEMA_DSA_OPERATION:
			print_literal(ss,"dSAOperation");
			break;
		default:
			print_literal(ss,"UNKNOWN");
			break;
		}
	}
	
	print_whsp(ss);

	print_extensions(ss, at->at_extensions);

	print_literal(ss,")");

	retstring = LDAP_STRDUP(safe_string_val(ss));
	safe_string_free(ss);
	return(retstring);
}

/*
 * Now come the parsers.  There is one parser for each entity type:
 * objectclasses, attributetypes, etc.
 *
 * Each of them is written as a recursive-descent parser, except that
 * none of them is really recursive.  But the idea is kept: there
 * is one routine per non-terminal that eithers gobbles lexical tokens
 * or calls lower-level routines, etc.
 *
 * The scanner is implemented in the routine get_token.  Actually,
 * get_token is more than a scanner and will return tokens that are
 * in fact non-terminals in the grammar.  So you can see the whole
 * approach as the combination of a low-level bottom-up recognizer
 * combined with a scanner and a number of top-down parsers.  Or just
 * consider that the real grammars recognized by the parsers are not
 * those of the standards.  As a matter of fact, our parsers are more
 * liberal than the spec when there is no ambiguity.
 *
 * The difference is pretty academic (modulo bugs or incorrect
 * interpretation of the specs).
 */

#define TK_NOENDQUOTE	-2
#define TK_OUTOFMEM	-1
#define TK_EOS		0
#define TK_UNEXPCHAR	1
#define TK_BAREWORD	2
#define TK_QDSTRING	3
#define TK_LEFTPAREN	4
#define TK_RIGHTPAREN	5
#define TK_DOLLAR	6
#define TK_QDESCR	TK_QDSTRING

struct token {
	int type;
	char *sval;
};

static int
get_token(const char ** sp, char ** token_val)
{
	int kind;
	const char * p;
	const char * q;
	char * res;

	*token_val = NULL;
	switch (**sp) {
	case '\0':
		kind = TK_EOS;
		(*sp)++;
		break;
	case '(':
		kind = TK_LEFTPAREN;
		(*sp)++;
		break;
	case ')':
		kind = TK_RIGHTPAREN;
		(*sp)++;
		break;
	case '$':
		kind = TK_DOLLAR;
		(*sp)++;
		break;
	case '\'':
		kind = TK_QDSTRING;
		(*sp)++;
		p = *sp;
		while ( **sp != '\'' && **sp != '\0' )
			(*sp)++;
		if ( **sp == '\'' ) {
			q = *sp;
			res = LDAP_MALLOC(q-p+1);
			if ( !res ) {
				kind = TK_OUTOFMEM;
			} else {
				strncpy(res,p,q-p);
				res[q-p] = '\0';
				*token_val = res;
			}
			(*sp)++;
		} else {
			kind = TK_NOENDQUOTE;
		}
		break;
	default:
		kind = TK_BAREWORD;
		p = *sp;
		while ( !isspace(**sp) &&
			**sp != '(' &&
			**sp != ')' &&
			**sp != '$' &&
			**sp != '\'' &&
			**sp != '\0' )
			(*sp)++;
		q = *sp;
		res = LDAP_MALLOC(q-p+1);
		if ( !res ) {
			kind = TK_OUTOFMEM;
		} else {
			strncpy(res,p,q-p);
			res[q-p] = '\0';
			*token_val = res;
		}
		break;
/*  		kind = TK_UNEXPCHAR; */
/*  		break; */
	}
	
	return kind;
}

/* Gobble optional whitespace */
static void
parse_whsp(const char **sp)
{
	while (isspace(**sp))
		(*sp)++;
}

/* TBC:!!
 * General note for all parsers: to guarantee the algorithm halts they
 * must always advance the pointer even when an error is found.  For
 * this one is not that important since an error here is fatal at the
 * upper layers, but it is a simple strategy that will not get in
 * endless loops.
 */

/* Parse a sequence of dot-separated decimal strings */
static char *
parse_numericoid(const char **sp, int *code, const int allow_quoted)
{
	char * res;
	const char * start = *sp;
	int len;
	int quoted = 0;

	/* Netscape puts the SYNTAX value in quotes (incorrectly) */
	if ( allow_quoted && **sp == '\'' ) {
		quoted = 1;
		(*sp)++;
		start++;
	}
	/* Each iteration of this loop gets one decimal string */
	while (**sp) {
		if ( !isdigit(**sp) ) {
			/*
			 * Initial char is not a digit or char after dot is
			 * not a digit
			 */
			*code = LDAP_SCHERR_NODIGIT;
			return NULL;
		}
		(*sp)++;
		while ( isdigit(**sp) )
			(*sp)++;
		if ( **sp != '.' )
			break;
		/* Otherwise, gobble the dot and loop again */
		(*sp)++;
	}
	/* Now *sp points at the char past the numericoid. Perfect. */
	len = *sp - start;
	res = LDAP_MALLOC(len+1);
	if (!res) {
		*code = LDAP_SCHERR_OUTOFMEM;
		return(NULL);
	}
	strncpy(res,start,len);
	res[len] = '\0';
	if ( allow_quoted && quoted ) {
		if ( **sp == '\'' ) {
			(*sp)++;
		} else {
			*code = LDAP_SCHERR_UNEXPTOKEN;
			LDAP_FREE(res);
			return NULL;
		}
	}
	return(res);
}

/* Parse a qdescr or a list of them enclosed in () */
static char **
parse_qdescrs(const char **sp, int *code)
{
	char ** res;
	char ** res1;
	int kind;
	char * sval;
	int size;
	int pos;

	parse_whsp(sp);
	kind = get_token(sp,&sval);
	if ( kind == TK_LEFTPAREN ) {
		/* Let's presume there will be at least 2 entries */
		size = 3;
		res = LDAP_CALLOC(3,sizeof(char *));
		if ( !res ) {
			*code = LDAP_SCHERR_OUTOFMEM;
			return NULL;
		}
		pos = 0;
		while (1) {
			parse_whsp(sp);
			kind = get_token(sp,&sval);
			if ( kind == TK_RIGHTPAREN )
				break;
			if ( kind == TK_QDESCR ) {
				if ( pos == size-2 ) {
					size++;
					res1 = LDAP_REALLOC(res,size*sizeof(char *));
					if ( !res1 ) {
						LDAP_VFREE(res);
						LDAP_FREE(sval);
						*code = LDAP_SCHERR_OUTOFMEM;
						return(NULL);
					}
					res = res1;
				}
				res[pos] = sval;
				pos++;
				parse_whsp(sp);
			} else {
				LDAP_VFREE(res);
				LDAP_FREE(sval);
				*code = LDAP_SCHERR_UNEXPTOKEN;
				return(NULL);
			}
		}
		res[pos] = NULL;
		parse_whsp(sp);
		return(res);
	} else if ( kind == TK_QDESCR ) {
		res = LDAP_CALLOC(2,sizeof(char *));
		if ( !res ) {
			*code = LDAP_SCHERR_OUTOFMEM;
			return NULL;
		}
		res[0] = sval;
		res[1] = NULL;
		parse_whsp(sp);
		return res;
	} else {
		LDAP_FREE(sval);
		*code = LDAP_SCHERR_BADNAME;
		return NULL;
	}
}

/* Parse a woid */
static char *
parse_woid(const char **sp, int *code)
{
	char * sval;
	int kind;

	parse_whsp(sp);
	kind = get_token(sp, &sval);
	if ( kind != TK_BAREWORD ) {
		LDAP_FREE(sval);
		*code = LDAP_SCHERR_UNEXPTOKEN;
		return NULL;
	}
	parse_whsp(sp);
	return sval;
}

/* Parse a noidlen */
static char *
parse_noidlen(const char **sp, int *code, int *len, int allow_quoted)
{
	char * sval;
	int quoted = 0;

	*len = 0;
	/* Netscape puts the SYNTAX value in quotes (incorrectly) */
	if ( allow_quoted && **sp == '\'' ) {
		quoted = 1;
		(*sp)++;
	}
	sval = parse_numericoid(sp, code, 0);
	if ( !sval ) {
		return NULL;
	}
	if ( **sp == '{' ) {
		(*sp)++;
		*len = atoi(*sp);
		while ( isdigit(**sp) )
			(*sp)++;
		if ( **sp != '}' ) {
			*code = LDAP_SCHERR_UNEXPTOKEN;
			LDAP_FREE(sval);
			return NULL;
		}
		(*sp)++;
	}		
	if ( allow_quoted && quoted ) {
		if ( **sp == '\'' ) {
			(*sp)++;
		} else {
			*code = LDAP_SCHERR_UNEXPTOKEN;
			LDAP_FREE(sval);
			return NULL;
		}
	}
	return sval;
}

/*
 * Next routine will accept a qdstring in place of an oid if
 * allow_quoted is set.  This is necessary to interoperate with
 * Netscape Directory server that will improperly quote each oid (at
 * least those of the descr kind) in the SUP clause.
 */

/* Parse a woid or a $-separated list of them enclosed in () */
static char **
parse_oids(const char **sp, int *code, const int allow_quoted)
{
	char ** res;
	char ** res1;
	int kind;
	char * sval;
	int size;
	int pos;

	/*
	 * Strictly speaking, doing this here accepts whsp before the
	 * ( at the begining of an oidlist, but this is harmless.  Also,
	 * we are very liberal in what we accept as an OID.  Maybe
	 * refine later.
	 */
	parse_whsp(sp);
	kind = get_token(sp,&sval);
	if ( kind == TK_LEFTPAREN ) {
		/* Let's presume there will be at least 2 entries */
		size = 3;
		res = LDAP_CALLOC(3,sizeof(char *));
		if ( !res ) {
			*code = LDAP_SCHERR_OUTOFMEM;
			return NULL;
		}
		pos = 0;
		parse_whsp(sp);
		kind = get_token(sp,&sval);
		if ( kind == TK_BAREWORD ||
		     ( allow_quoted && kind == TK_QDSTRING ) ) {
			res[pos] = sval;
			pos++;
		} else {
			*code = LDAP_SCHERR_UNEXPTOKEN;
			LDAP_FREE(sval);
			LDAP_VFREE(res);
			return NULL;
		}
		parse_whsp(sp);
		while (1) {
			kind = get_token(sp,&sval);
			if ( kind == TK_RIGHTPAREN )
				break;
			if ( kind == TK_DOLLAR ) {
				parse_whsp(sp);
				kind = get_token(sp,&sval);
				if ( kind == TK_BAREWORD ||
				     ( allow_quoted &&
				       kind == TK_QDSTRING ) ) {
					if ( pos == size-2 ) {
						size++;
						res1 = LDAP_REALLOC(res,size*sizeof(char *));
						if ( !res1 ) {
							LDAP_FREE(sval);
							LDAP_VFREE(res);
							*code = LDAP_SCHERR_OUTOFMEM;
							return(NULL);
						}
						res = res1;
					}
					res[pos] = sval;
					pos++;
				} else {
					*code = LDAP_SCHERR_UNEXPTOKEN;
					LDAP_FREE(sval);
					LDAP_VFREE(res);
					return NULL;
				}
				parse_whsp(sp);
			} else {
				*code = LDAP_SCHERR_UNEXPTOKEN;
				LDAP_FREE(sval);
				LDAP_VFREE(res);
				return NULL;
			}
		}
		res[pos] = NULL;
		parse_whsp(sp);
		return(res);
	} else if ( kind == TK_BAREWORD ||
		    ( allow_quoted && kind == TK_QDSTRING ) ) {
		res = LDAP_CALLOC(2,sizeof(char *));
		if ( !res ) {
			LDAP_FREE(sval);
			*code = LDAP_SCHERR_OUTOFMEM;
			return NULL;
		}
		res[0] = sval;
		res[1] = NULL;
		parse_whsp(sp);
		return res;
	} else {
		LDAP_FREE(sval);
		*code = LDAP_SCHERR_BADNAME;
		return NULL;
	}
}

static int
add_extension(LDAP_SCHEMA_EXTENSION_ITEM ***extensions,
	      char * name, char ** values)
{
	int n;
	LDAP_SCHEMA_EXTENSION_ITEM **tmp, *ext;

	ext = LDAP_CALLOC(1, sizeof(LDAP_SCHEMA_EXTENSION_ITEM));
	if ( !ext )
		return 1;
	ext->lsei_name = name;
	ext->lsei_values = values;

	if ( !*extensions ) {
		*extensions =
		  LDAP_CALLOC(2, sizeof(LDAP_SCHEMA_EXTENSION_ITEM *));
		if ( !*extensions )
		  return 1;
		n = 0;
	} else {
		for ( n=0; (*extensions)[n] != NULL; n++ )
	  		;
		tmp = LDAP_REALLOC(*extensions,
				   (n+2)*sizeof(LDAP_SCHEMA_EXTENSION_ITEM *));
		if ( !tmp )
			return 1;
		*extensions = tmp;
	}
	(*extensions)[n] = ext;
	(*extensions)[n+1] = NULL;
	return 0;
}

static void
free_extensions(LDAP_SCHEMA_EXTENSION_ITEM **extensions)
{
	LDAP_SCHEMA_EXTENSION_ITEM **ext;

	if ( extensions ) {
		for ( ext = extensions; *ext != NULL; ext++ ) {
			LDAP_FREE((*ext)->lsei_name);
			LDAP_VFREE((*ext)->lsei_values);
			LDAP_FREE(*ext);
		}
		LDAP_FREE(extensions);
	}
}

void
ldap_syntax_free( LDAP_SYNTAX * syn )
{
	LDAP_FREE(syn->syn_oid);
	LDAP_FREE(syn->syn_desc);
	free_extensions(syn->syn_extensions);
	LDAP_FREE(syn);
}

LDAP_SYNTAX *
ldap_str2syntax( const char * s, int * code, const char ** errp )
{
	int kind;
	const char * ss = s;
	char * sval;
	int seen_desc = 0;
	LDAP_SYNTAX * syn;
	char ** ssdummy;

	if ( !s ) {
		*code = LDAP_SCHERR_EMPTY;
		*errp = "";
		return NULL;
	}

	*errp = s;
	syn = LDAP_CALLOC(1,sizeof(LDAP_SYNTAX));

	if ( !syn ) {
		*code = LDAP_SCHERR_OUTOFMEM;
		return NULL;
	}

	kind = get_token(&ss,&sval);
	if ( kind != TK_LEFTPAREN ) {
		LDAP_FREE(sval);
		*code = LDAP_SCHERR_NOLEFTPAREN;
		ldap_syntax_free(syn);
		return NULL;
	}

	parse_whsp(&ss);
	syn->syn_oid = parse_numericoid(&ss,code,0);
	if ( !syn->syn_oid ) {
		*errp = ss;
		ldap_syntax_free(syn);
		return NULL;
	}
	parse_whsp(&ss);

	/*
	 * Beyond this point we will be liberal and accept the items
	 * in any order.
	 */
	while (1) {
		kind = get_token(&ss,&sval);
		switch (kind) {
		case TK_EOS:
			*code = LDAP_SCHERR_NORIGHTPAREN;
			*errp = ss;
			ldap_syntax_free(syn);
			return NULL;
		case TK_RIGHTPAREN:
			return syn;
		case TK_BAREWORD:
			if ( !strcmp(sval,"DESC") ) {
				LDAP_FREE(sval);
				if ( seen_desc ) {
					*code = LDAP_SCHERR_DUPOPT;
					*errp = ss;
					ldap_syntax_free(syn);
					return(NULL);
				}
				seen_desc = 1;
				parse_whsp(&ss);
				kind = get_token(&ss,&sval);
				if ( kind != TK_QDSTRING ) {
					*code = LDAP_SCHERR_UNEXPTOKEN;
					*errp = ss;
					LDAP_FREE(sval);
					ldap_syntax_free(syn);
					return NULL;
				}
				syn->syn_desc = sval;
				parse_whsp(&ss);
			} else if ( sval[0] == 'X' && sval[1] == '-' ) {
				LDAP_FREE(sval);
				/* Should be parse_qdstrings */
				ssdummy = parse_qdescrs(&ss, code);
				if ( !ssdummy ) {
					*errp = ss;
					ldap_syntax_free(syn);
					return NULL;
				}
			} else {
				*code = LDAP_SCHERR_UNEXPTOKEN;
				*errp = ss;
				LDAP_FREE(sval);
				ldap_syntax_free(syn);
				return NULL;
			}
			break;
		default:
			*code = LDAP_SCHERR_UNEXPTOKEN;
			*errp = ss;
			LDAP_FREE(sval);
			ldap_syntax_free(syn);
			return NULL;
		}
	}
}

void
ldap_matchingrule_free( LDAP_MATCHING_RULE * mr )
{
	LDAP_FREE(mr->mr_oid);
	LDAP_VFREE(mr->mr_names);
	LDAP_FREE(mr->mr_desc);
	LDAP_FREE(mr->mr_syntax_oid);
	free_extensions(mr->mr_extensions);
	LDAP_FREE(mr);
}

LDAP_MATCHING_RULE *
ldap_str2matchingrule( const char * s, int * code, const char ** errp )
{
	int kind;
	const char * ss = s;
	char * sval;
	int be_liberal = 1;	/* Future additional argument */
	int seen_name = 0;
	int seen_desc = 0;
	int seen_obsolete = 0;
	int seen_syntax = 0;
	LDAP_MATCHING_RULE * mr;
	char ** ssdummy;
	const char * savepos;

	if ( !s ) {
		*code = LDAP_SCHERR_EMPTY;
		*errp = "";
		return NULL;
	}

	*errp = s;
	mr = LDAP_CALLOC(1,sizeof(LDAP_MATCHING_RULE));

	if ( !mr ) {
		*code = LDAP_SCHERR_OUTOFMEM;
		return NULL;
	}

	kind = get_token(&ss,&sval);
	if ( kind != TK_LEFTPAREN ) {
		*code = LDAP_SCHERR_NOLEFTPAREN;
		LDAP_FREE(sval);
		ldap_matchingrule_free(mr);
		return NULL;
	}

	parse_whsp(&ss);
	savepos = ss;
	mr->mr_oid = parse_numericoid(&ss,code,be_liberal);
	if ( !mr->mr_oid ) {
		if ( be_liberal ) {
			/* Backtracking */
			ss = savepos;
			kind = get_token(&ss,&sval);
			if ( kind == TK_BAREWORD ) {
				if ( !strcmp(sval, "NAME") ||
				     !strcmp(sval, "DESC") ||
				     !strcmp(sval, "OBSOLETE") ||
				     !strcmp(sval, "SYNTAX") ||
				     !strncmp(sval, "X-", 2) ) {
					/* Missing OID, backtrack */
					ss = savepos;
				} else {
					/* Non-numerical OID, ignore */
				}
			}
			LDAP_FREE(sval);
		} else {
			*errp = ss;
			ldap_matchingrule_free(mr);
			return NULL;
		}
	}
	parse_whsp(&ss);

	/*
	 * Beyond this point we will be liberal and accept the items
	 * in any order.
	 */
	while (1) {
		kind = get_token(&ss,&sval);
		switch (kind) {
		case TK_EOS:
			*code = LDAP_SCHERR_NORIGHTPAREN;
			*errp = ss;
			ldap_matchingrule_free(mr);
			return NULL;
		case TK_RIGHTPAREN:
			return mr;
		case TK_BAREWORD:
			if ( !strcmp(sval,"NAME") ) {
				LDAP_FREE(sval);
				if ( seen_name ) {
					*code = LDAP_SCHERR_DUPOPT;
					*errp = ss;
					ldap_matchingrule_free(mr);
					return(NULL);
				}
				seen_name = 1;
				mr->mr_names = parse_qdescrs(&ss,code);
				if ( !mr->mr_names ) {
					if ( *code != LDAP_SCHERR_OUTOFMEM )
						*code = LDAP_SCHERR_BADNAME;
					*errp = ss;
					ldap_matchingrule_free(mr);
					return NULL;
				}
			} else if ( !strcmp(sval,"DESC") ) {
				LDAP_FREE(sval);
				if ( seen_desc ) {
					*code = LDAP_SCHERR_DUPOPT;
					*errp = ss;
					ldap_matchingrule_free(mr);
					return(NULL);
				}
				seen_desc = 1;
				parse_whsp(&ss);
				kind = get_token(&ss,&sval);
				if ( kind != TK_QDSTRING ) {
					*code = LDAP_SCHERR_UNEXPTOKEN;
					*errp = ss;
					LDAP_FREE(sval);
					ldap_matchingrule_free(mr);
					return NULL;
				}
				mr->mr_desc = sval;
				parse_whsp(&ss);
			} else if ( !strcmp(sval,"OBSOLETE") ) {
				LDAP_FREE(sval);
				if ( seen_obsolete ) {
					*code = LDAP_SCHERR_DUPOPT;
					*errp = ss;
					ldap_matchingrule_free(mr);
					return(NULL);
				}
				seen_obsolete = 1;
				mr->mr_obsolete = LDAP_SCHEMA_YES;
				parse_whsp(&ss);
			} else if ( !strcmp(sval,"SYNTAX") ) {
				LDAP_FREE(sval);
				if ( seen_syntax ) {
					*code = LDAP_SCHERR_DUPOPT;
					*errp = ss;
					ldap_matchingrule_free(mr);
					return(NULL);
				}
				seen_syntax = 1;
				parse_whsp(&ss);
				mr->mr_syntax_oid =
					parse_numericoid(&ss,code,be_liberal);
				if ( !mr->mr_syntax_oid ) {
					*errp = ss;
					ldap_matchingrule_free(mr);
					return NULL;
				}
				parse_whsp(&ss);
			} else if ( sval[0] == 'X' && sval[1] == '-' ) {
				LDAP_FREE(sval);
				/* Should be parse_qdstrings */
				ssdummy = parse_qdescrs(&ss, code);
				if ( !ssdummy ) {
					*errp = ss;
					ldap_matchingrule_free(mr);
					return NULL;
				}
			} else {
				*code = LDAP_SCHERR_UNEXPTOKEN;
				*errp = ss;
				LDAP_FREE(sval);
				ldap_matchingrule_free(mr);
				return NULL;
			}
			break;
		default:
			*code = LDAP_SCHERR_UNEXPTOKEN;
			*errp = ss;
			LDAP_FREE(sval);
			ldap_matchingrule_free(mr);
			return NULL;
		}
	}
}

void
ldap_attributetype_free(LDAP_ATTRIBUTE_TYPE * at)
{
	LDAP_FREE(at->at_oid);
	LDAP_VFREE(at->at_names);
	LDAP_FREE(at->at_desc);
	LDAP_FREE(at->at_sup_oid);
	LDAP_FREE(at->at_equality_oid);
	LDAP_FREE(at->at_ordering_oid);
	LDAP_FREE(at->at_substr_oid);
	LDAP_FREE(at->at_syntax_oid);
	free_extensions(at->at_extensions);
	LDAP_FREE(at);
}

LDAP_ATTRIBUTE_TYPE *
ldap_str2attributetype( const char * s, int * code, const char ** errp )
{
	int kind;
	const char * ss = s;
	char * sval;
	int be_liberal = 1;	/* Future additional argument */
	int seen_name = 0;
	int seen_desc = 0;
	int seen_obsolete = 0;
	int seen_sup = 0;
	int seen_equality = 0;
	int seen_ordering = 0;
	int seen_substr = 0;
	int seen_syntax = 0;
	int seen_usage = 0;
	LDAP_ATTRIBUTE_TYPE * at;
	char ** ext_vals;
	const char * savepos;

	if ( !s ) {
		*code = LDAP_SCHERR_EMPTY;
		*errp = "";
		return NULL;
	}

	*errp = s;
	at = LDAP_CALLOC(1,sizeof(LDAP_ATTRIBUTE_TYPE));

	if ( !at ) {
		*code = LDAP_SCHERR_OUTOFMEM;
		return NULL;
	}

	kind = get_token(&ss,&sval);
	if ( kind != TK_LEFTPAREN ) {
		*code = LDAP_SCHERR_NOLEFTPAREN;
		LDAP_FREE(sval);
		ldap_attributetype_free(at);
		return NULL;
	}

	/*
	 * Definitions MUST begin with an OID in the numericoid format.
	 * However, this routine is used by clients to parse the response
	 * from servers and very well known servers will provide an OID
	 * in the wrong format or even no OID at all.  We do our best to
	 * extract info from those servers.
	 */
	parse_whsp(&ss);
	savepos = ss;
	at->at_oid = parse_numericoid(&ss,code,0);
	if ( !at->at_oid ) {
		if ( be_liberal ) {
			/* Backtracking */
			ss = savepos;
			kind = get_token(&ss,&sval);
			if ( kind == TK_BAREWORD ) {
				if ( !strcmp(sval, "NAME") ||
				     !strcmp(sval, "DESC") ||
				     !strcmp(sval, "OBSOLETE") ||
				     !strcmp(sval, "SUP") ||
				     !strcmp(sval, "EQUALITY") ||
				     !strcmp(sval, "ORDERING") ||
				     !strcmp(sval, "SUBSTR") ||
				     !strcmp(sval, "SYNTAX") ||
				     !strcmp(sval, "SINGLE-VALUE") ||
				     !strcmp(sval, "COLLECTIVE") ||
				     !strcmp(sval, "NO-USER-MODIFICATION") ||
				     !strcmp(sval, "USAGE") ||
				     !strncmp(sval, "X-", 2) ) {
					/* Missing OID, backtrack */
					ss = savepos;
				} else {
					/* Non-numerical OID, ignore */
				}
			}
			LDAP_FREE(sval);
		} else {
			*errp = ss;
			ldap_attributetype_free(at);
			return NULL;
		}
	}
	parse_whsp(&ss);

	/*
	 * Beyond this point we will be liberal and accept the items
	 * in any order.
	 */
	while (1) {
		kind = get_token(&ss,&sval);
		switch (kind) {
		case TK_EOS:
			*code = LDAP_SCHERR_NORIGHTPAREN;
			*errp = ss;
			ldap_attributetype_free(at);
			return NULL;
		case TK_RIGHTPAREN:
			return at;
		case TK_BAREWORD:
			if ( !strcmp(sval,"NAME") ) {
				LDAP_FREE(sval);
				if ( seen_name ) {
					*code = LDAP_SCHERR_DUPOPT;
					*errp = ss;
					ldap_attributetype_free(at);
					return(NULL);
				}
				seen_name = 1;
				at->at_names = parse_qdescrs(&ss,code);
				if ( !at->at_names ) {
					if ( *code != LDAP_SCHERR_OUTOFMEM )
						*code = LDAP_SCHERR_BADNAME;
					*errp = ss;
					ldap_attributetype_free(at);
					return NULL;
				}
			} else if ( !strcmp(sval,"DESC") ) {
				LDAP_FREE(sval);
				if ( seen_desc ) {
					*code = LDAP_SCHERR_DUPOPT;
					*errp = ss;
					ldap_attributetype_free(at);
					return(NULL);
				}
				seen_desc = 1;
				parse_whsp(&ss);
				kind = get_token(&ss,&sval);
				if ( kind != TK_QDSTRING ) {
					*code = LDAP_SCHERR_UNEXPTOKEN;
					*errp = ss;
					LDAP_FREE(sval);
					ldap_attributetype_free(at);
					return NULL;
				}
				at->at_desc = sval;
				parse_whsp(&ss);
			} else if ( !strcmp(sval,"OBSOLETE") ) {
				LDAP_FREE(sval);
				if ( seen_obsolete ) {
					*code = LDAP_SCHERR_DUPOPT;
					*errp = ss;
					ldap_attributetype_free(at);
					return(NULL);
				}
				seen_obsolete = 1;
				at->at_obsolete = LDAP_SCHEMA_YES;
				parse_whsp(&ss);
			} else if ( !strcmp(sval,"SUP") ) {
				LDAP_FREE(sval);
				if ( seen_sup ) {
					*code = LDAP_SCHERR_DUPOPT;
					*errp = ss;
					ldap_attributetype_free(at);
					return(NULL);
				}
				seen_sup = 1;
				at->at_sup_oid = parse_woid(&ss,code);
				if ( !at->at_sup_oid ) {
					*errp = ss;
					ldap_attributetype_free(at);
					return NULL;
				}
			} else if ( !strcmp(sval,"EQUALITY") ) {
				LDAP_FREE(sval);
				if ( seen_equality ) {
					*code = LDAP_SCHERR_DUPOPT;
					*errp = ss;
					ldap_attributetype_free(at);
					return(NULL);
				}
				seen_equality = 1;
				at->at_equality_oid = parse_woid(&ss,code);
				if ( !at->at_equality_oid ) {
					*errp = ss;
					ldap_attributetype_free(at);
					return NULL;
				}
			} else if ( !strcmp(sval,"ORDERING") ) {
				LDAP_FREE(sval);
				if ( seen_ordering ) {
					*code = LDAP_SCHERR_DUPOPT;
					*errp = ss;
					ldap_attributetype_free(at);
					return(NULL);
				}
				seen_ordering = 1;
				at->at_ordering_oid = parse_woid(&ss,code);
				if ( !at->at_ordering_oid ) {
					*errp = ss;
					ldap_attributetype_free(at);
					return NULL;
				}
			} else if ( !strcmp(sval,"SUBSTR") ) {
				LDAP_FREE(sval);
				if ( seen_substr ) {
					*code = LDAP_SCHERR_DUPOPT;
					*errp = ss;
					ldap_attributetype_free(at);
					return(NULL);
				}
				seen_substr = 1;
				at->at_substr_oid = parse_woid(&ss,code);
				if ( !at->at_substr_oid ) {
					*errp = ss;
					ldap_attributetype_free(at);
					return NULL;
				}
			} else if ( !strcmp(sval,"SYNTAX") ) {
				LDAP_FREE(sval);
				if ( seen_syntax ) {
					*code = LDAP_SCHERR_DUPOPT;
					*errp = ss;
					ldap_attributetype_free(at);
					return(NULL);
				}
				seen_syntax = 1;
				parse_whsp(&ss);
				at->at_syntax_oid =
					parse_noidlen(&ss,
						      code,
						      &at->at_syntax_len,
						      be_liberal);
				if ( !at->at_syntax_oid ) {
					*errp = ss;
					ldap_attributetype_free(at);
					return NULL;
				}
				parse_whsp(&ss);
			} else if ( !strcmp(sval,"SINGLE-VALUE") ) {
				LDAP_FREE(sval);
				if ( at->at_single_value ) {
					*code = LDAP_SCHERR_DUPOPT;
					*errp = ss;
					ldap_attributetype_free(at);
					return(NULL);
				}
				at->at_single_value = LDAP_SCHEMA_YES;
				parse_whsp(&ss);
			} else if ( !strcmp(sval,"COLLECTIVE") ) {
				LDAP_FREE(sval);
				if ( at->at_collective ) {
					*code = LDAP_SCHERR_DUPOPT;
					*errp = ss;
					ldap_attributetype_free(at);
					return(NULL);
				}
				at->at_collective = LDAP_SCHEMA_YES;
				parse_whsp(&ss);
			} else if ( !strcmp(sval,"NO-USER-MODIFICATION") ) {
				LDAP_FREE(sval);
				if ( at->at_no_user_mod ) {
					*code = LDAP_SCHERR_DUPOPT;
					*errp = ss;
					ldap_attributetype_free(at);
					return(NULL);
				}
				at->at_no_user_mod = LDAP_SCHEMA_YES;
				parse_whsp(&ss);
			} else if ( !strcmp(sval,"USAGE") ) {
				LDAP_FREE(sval);
				if ( seen_usage ) {
					*code = LDAP_SCHERR_DUPOPT;
					*errp = ss;
					ldap_attributetype_free(at);
					return(NULL);
				}
				seen_usage = 1;
				parse_whsp(&ss);
				kind = get_token(&ss,&sval);
				if ( kind != TK_BAREWORD ) {
					*code = LDAP_SCHERR_UNEXPTOKEN;
					*errp = ss;
					LDAP_FREE(sval);
					ldap_attributetype_free(at);
					return NULL;
				}
				if ( !strcasecmp(sval,"userApplications") )
					at->at_usage =
					    LDAP_SCHEMA_USER_APPLICATIONS;
				else if ( !strcasecmp(sval,"directoryOperation") )
					at->at_usage =
					    LDAP_SCHEMA_DIRECTORY_OPERATION;
				else if ( !strcasecmp(sval,"distributedOperation") )
					at->at_usage =
					    LDAP_SCHEMA_DISTRIBUTED_OPERATION;
				else if ( !strcasecmp(sval,"dSAOperation") )
					at->at_usage =
					    LDAP_SCHEMA_DSA_OPERATION;
				else {
					*code = LDAP_SCHERR_UNEXPTOKEN;
					*errp = ss;
					LDAP_FREE(sval);
					ldap_attributetype_free(at);
					return NULL;
				}
				LDAP_FREE(sval);
				parse_whsp(&ss);
			} else if ( sval[0] == 'X' && sval[1] == '-' ) {
				/* Should be parse_qdstrings */
				ext_vals = parse_qdescrs(&ss, code);
				if ( !ext_vals ) {
					*errp = ss;
					ldap_attributetype_free(at);
					return NULL;
				}
				if ( add_extension(&at->at_extensions,
						    sval, ext_vals) ) {
					*code = LDAP_SCHERR_OUTOFMEM;
					*errp = ss;
					LDAP_FREE(sval);
					ldap_attributetype_free(at);
					return NULL;
				}
			} else {
				*code = LDAP_SCHERR_UNEXPTOKEN;
				*errp = ss;
				LDAP_FREE(sval);
				ldap_attributetype_free(at);
				return NULL;
			}
			break;
		default:
			*code = LDAP_SCHERR_UNEXPTOKEN;
			*errp = ss;
			LDAP_FREE(sval);
			ldap_attributetype_free(at);
			return NULL;
		}
	}
}

void
ldap_objectclass_free(LDAP_OBJECT_CLASS * oc)
{
	LDAP_FREE(oc->oc_oid);
	LDAP_VFREE(oc->oc_names);
	LDAP_FREE(oc->oc_desc);
	LDAP_VFREE(oc->oc_sup_oids);
	LDAP_VFREE(oc->oc_at_oids_must);
	LDAP_VFREE(oc->oc_at_oids_may);
	free_extensions(oc->oc_extensions);
	LDAP_FREE(oc);
}

LDAP_OBJECT_CLASS *
ldap_str2objectclass( const char * s, int * code, const char ** errp )
{
	int kind;
	const char * ss = s;
	char * sval;
	int be_liberal = 1;	/* Future additional argument */
	int seen_name = 0;
	int seen_desc = 0;
	int seen_obsolete = 0;
	int seen_sup = 0;
	int seen_kind = 0;
	int seen_must = 0;
	int seen_may = 0;
	LDAP_OBJECT_CLASS * oc;
	char ** ssdummy;
	const char * savepos;

	if ( !s ) {
		*code = LDAP_SCHERR_EMPTY;
		*errp = "";
		return NULL;
	}

	*errp = s;
	oc = LDAP_CALLOC(1,sizeof(LDAP_OBJECT_CLASS));

	if ( !oc ) {
		*code = LDAP_SCHERR_OUTOFMEM;
		return NULL;
	}

	kind = get_token(&ss,&sval);
	if ( kind != TK_LEFTPAREN ) {
		*code = LDAP_SCHERR_NOLEFTPAREN;
		LDAP_FREE(sval);
		ldap_objectclass_free(oc);
		return NULL;
	}

	/*
	 * Definitions MUST begin with an OID in the numericoid format.
	 * However, this routine is used by clients to parse the response
	 * from servers and very well known servers will provide an OID
	 * in the wrong format or even no OID at all.  We do our best to
	 * extract info from those servers.
	 */
	parse_whsp(&ss);
	savepos = ss;
	oc->oc_oid = parse_numericoid(&ss,code,0);
	if ( !oc->oc_oid ) {
		if ( be_liberal ) {
			/* Backtracking */
			ss = savepos;
			kind = get_token(&ss,&sval);
			if ( kind == TK_BAREWORD ) {
				if ( !strcmp(sval, "NAME") ||
				     !strcmp(sval, "DESC") ||
				     !strcmp(sval, "OBSOLETE") ||
				     !strcmp(sval, "SUP") ||
				     !strcmp(sval, "ABSTRACT") ||
				     !strcmp(sval, "STRUCTURAL") ||
				     !strcmp(sval, "AUXILIARY") ||
				     !strcmp(sval, "MUST") ||
				     !strncmp(sval, "X-", 2) ) {
					/* Missing OID, backtrack */
					ss = savepos;
				} else {
					/* Non-numerical OID, ignore */
				}
			}
			LDAP_FREE(sval);
		} else {
			*errp = ss;
			ldap_objectclass_free(oc);
			return NULL;
		}
	}
	parse_whsp(&ss);

	/*
	 * Beyond this point we will be liberal an accept the items
	 * in any order.
	 */
	while (1) {
		kind = get_token(&ss,&sval);
		switch (kind) {
		case TK_EOS:
			*code = LDAP_SCHERR_NORIGHTPAREN;
			*errp = ss;
			ldap_objectclass_free(oc);
			return NULL;
		case TK_RIGHTPAREN:
			return oc;
		case TK_BAREWORD:
			if ( !strcmp(sval,"NAME") ) {
				LDAP_FREE(sval);
				if ( seen_name ) {
					*code = LDAP_SCHERR_DUPOPT;
					*errp = ss;
					ldap_objectclass_free(oc);
					return(NULL);
				}
				seen_name = 1;
				oc->oc_names = parse_qdescrs(&ss,code);
				if ( !oc->oc_names ) {
					if ( *code != LDAP_SCHERR_OUTOFMEM )
						*code = LDAP_SCHERR_BADNAME;
					*errp = ss;
					ldap_objectclass_free(oc);
					return NULL;
				}
			} else if ( !strcmp(sval,"DESC") ) {
				LDAP_FREE(sval);
				if ( seen_desc ) {
					*code = LDAP_SCHERR_DUPOPT;
					*errp = ss;
					ldap_objectclass_free(oc);
					return(NULL);
				}
				seen_desc = 1;
				parse_whsp(&ss);
				kind = get_token(&ss,&sval);
				if ( kind != TK_QDSTRING ) {
					*code = LDAP_SCHERR_UNEXPTOKEN;
					*errp = ss;
					LDAP_FREE(sval);
					ldap_objectclass_free(oc);
					return NULL;
				}
				oc->oc_desc = sval;
				parse_whsp(&ss);
			} else if ( !strcmp(sval,"OBSOLETE") ) {
				LDAP_FREE(sval);
				if ( seen_obsolete ) {
					*code = LDAP_SCHERR_DUPOPT;
					*errp = ss;
					ldap_objectclass_free(oc);
					return(NULL);
				}
				seen_obsolete = 1;
				oc->oc_obsolete = LDAP_SCHEMA_YES;
				parse_whsp(&ss);
			} else if ( !strcmp(sval,"SUP") ) {
				LDAP_FREE(sval);
				if ( seen_sup ) {
					*code = LDAP_SCHERR_DUPOPT;
					*errp = ss;
					ldap_objectclass_free(oc);
					return(NULL);
				}
				seen_sup = 1;
				oc->oc_sup_oids = parse_oids(&ss,
							     code,
							     be_liberal);
				if ( !oc->oc_sup_oids ) {
					*errp = ss;
					ldap_objectclass_free(oc);
					return NULL;
				}
			} else if ( !strcmp(sval,"ABSTRACT") ) {
				LDAP_FREE(sval);
				if ( seen_kind ) {
					*code = LDAP_SCHERR_DUPOPT;
					*errp = ss;
					ldap_objectclass_free(oc);
					return(NULL);
				}
				seen_kind = 1;
				oc->oc_kind = LDAP_SCHEMA_ABSTRACT;
				parse_whsp(&ss);
			} else if ( !strcmp(sval,"STRUCTURAL") ) {
				LDAP_FREE(sval);
				if ( seen_kind ) {
					*code = LDAP_SCHERR_DUPOPT;
					*errp = ss;
					ldap_objectclass_free(oc);
					return(NULL);
				}
				seen_kind = 1;
				oc->oc_kind = LDAP_SCHEMA_STRUCTURAL;
				parse_whsp(&ss);
			} else if ( !strcmp(sval,"AUXILIARY") ) {
				LDAP_FREE(sval);
				if ( seen_kind ) {
					*code = LDAP_SCHERR_DUPOPT;
					*errp = ss;
					ldap_objectclass_free(oc);
					return(NULL);
				}
				seen_kind = 1;
				oc->oc_kind = LDAP_SCHEMA_AUXILIARY;
				parse_whsp(&ss);
			} else if ( !strcmp(sval,"MUST") ) {
				LDAP_FREE(sval);
				if ( seen_must ) {
					*code = LDAP_SCHERR_DUPOPT;
					*errp = ss;
					ldap_objectclass_free(oc);
					return(NULL);
				}
				seen_must = 1;
				oc->oc_at_oids_must = parse_oids(&ss,code,0);
				if ( !oc->oc_at_oids_must ) {
					*errp = ss;
					ldap_objectclass_free(oc);
					return NULL;
				}
				parse_whsp(&ss);
			} else if ( !strcmp(sval,"MAY") ) {
				LDAP_FREE(sval);
				if ( seen_may ) {
					*code = LDAP_SCHERR_DUPOPT;
					*errp = ss;
					ldap_objectclass_free(oc);
					return(NULL);
				}
				seen_may = 1;
				oc->oc_at_oids_may = parse_oids(&ss,code,0);
				if ( !oc->oc_at_oids_may ) {
					*errp = ss;
					ldap_objectclass_free(oc);
					return NULL;
				}
				parse_whsp(&ss);
			} else if ( sval[0] == 'X' && sval[1] == '-' ) {
				LDAP_FREE(sval);
				/* Should be parse_qdstrings */
				ssdummy = parse_qdescrs(&ss, code);
				if ( !ssdummy ) {
					*errp = ss;
					ldap_objectclass_free(oc);
					return NULL;
				}
			} else {
				*code = LDAP_SCHERR_UNEXPTOKEN;
				*errp = ss;
				LDAP_FREE(sval);
				ldap_objectclass_free(oc);
				return NULL;
			}
			break;
		default:
			*code = LDAP_SCHERR_UNEXPTOKEN;
			*errp = ss;
			LDAP_FREE(sval);
			ldap_objectclass_free(oc);
			return NULL;
		}
	}
}

static char *const err2text[] = {
	"",
	"Out of memory",
	"Unexpected token",
	"Missing opening parenthesis",
	"Missing closing parenthesis",
	"Expecting digit",
	"Expecting a name",
	"Bad description",
	"Bad superiors",
	"Duplicate option",
	"Unexpected end of data"
};

char *
ldap_scherr2str(int code)
{
	if ( code < 1 || code >= (sizeof(err2text)/sizeof(char *)) ) {
		return "Unknown error";
	} else {
		return err2text[code];
	}
}

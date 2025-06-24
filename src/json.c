/*-------------------------------------------------------------------------
 *
 * json.c
 *		JSON data type support.
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2018, PipelineDB, Inc.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/transam.h"
#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "funcapi.h"
#include "lib/stringinfo.h"
#include "libpq/pqformat.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "parser/parse_coerce.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/datetime.h"
#include "utils/lsyscache.h"
#include "utils/json.h"
#include "utils/jsonfuncs.h"
#include "utils/typcache.h"
#include "utils/syscache.h"

/*
 * The context of the parser is maintained by the recursive descent
 * mechanism, but is passed explicitly to the error reporting routine
 * for better diagnostics.
 */
typedef enum					/* contexts of JSON parser */
{
	JSON_PARSE_VALUE,			/* expecting a value */
	JSON_PARSE_STRING,			/* expecting a string (for a field name) */
	JSON_PARSE_ARRAY_START,		/* saw '[', expecting value or ']' */
	JSON_PARSE_ARRAY_NEXT,		/* saw array element, expecting ',' or ']' */
	JSON_PARSE_OBJECT_START,	/* saw '{', expecting label or '}' */
	JSON_PARSE_OBJECT_LABEL,	/* saw object label, expecting ':' */
	JSON_PARSE_OBJECT_NEXT,		/* saw object value, expecting ',' or '}' */
	JSON_PARSE_OBJECT_COMMA,	/* saw object ',', expecting next label */
	JSON_PARSE_END				/* saw the end of a document, expect nothing */
} JsonParseContext;

typedef enum					/* type categories for datum_to_json */
{
	JSONTYPE_NULL,				/* null, so we didn't bother to identify */
	JSONTYPE_BOOL,				/* boolean (built-in types only) */
	JSONTYPE_NUMERIC,			/* numeric (ditto) */
	JSONTYPE_DATE,				/* we use special formatting for datetimes */
	JSONTYPE_TIMESTAMP,
	JSONTYPE_TIMESTAMPTZ,
	JSONTYPE_JSON,				/* JSON itself (and JSONB) */
	JSONTYPE_ARRAY,				/* array */
	JSONTYPE_COMPOSITE,			/* composite */
	JSONTYPE_CAST,				/* something with an explicit cast to JSON */
	JSONTYPE_OTHER				/* all else */
} JsonTypeCategory;

typedef struct JsonAggState
{
	StringInfo	str;
	JsonTypeCategory key_category;
	Oid			key_output_func;
	JsonTypeCategory val_category;
	Oid			val_output_func;
} JsonAggState;

static inline void json_lex_number(JsonLexContext *lex, char *s,
				bool *num_err, int *total_len);
static inline void parse_scalar(JsonLexContext *lex, JsonSemAction *sem);
static void parse_object_field(JsonLexContext *lex, JsonSemAction *sem);
static void parse_object(JsonLexContext *lex, JsonSemAction *sem);
static void parse_array_element(JsonLexContext *lex, JsonSemAction *sem);
static void parse_array(JsonLexContext *lex, JsonSemAction *sem);
static void report_parse_error(JsonParseContext ctx, JsonLexContext *lex);
static void report_invalid_token(JsonLexContext *lex);
static int	report_json_context(JsonLexContext *lex);
static void composite_to_json(Datum composite, StringInfo result,
				  bool use_line_feeds);
static void array_dim_to_json(StringInfo result, int dim, int ndims, int *dims,
				  Datum *vals, bool *nulls, int *valcount,
				  JsonTypeCategory tcategory, Oid outfuncoid,
				  bool use_line_feeds);
static void array_to_json_internal(Datum array, StringInfo result,
					   bool use_line_feeds);
static void json_categorize_type(Oid typoid,
					 JsonTypeCategory *tcategory,
					 Oid *outfuncoid);
static void datum_to_json(Datum val, bool is_null, StringInfo result,
			  JsonTypeCategory tcategory, Oid outfuncoid,
			  bool key_scalar);
static void add_json(Datum val, bool is_null, StringInfo result,
		 Oid val_type, bool key_scalar);
static text *catenate_stringinfo_string(StringInfo buffer, const char *addon);

/* Recursive Descent parser support routines */

/*
 * lex_peek
 *
 * what is the current look_ahead token?
*/
static inline JsonTokenType
lex_peek(JsonLexContext *lex)
{
	return lex->token_type;
}

/*
 * lex_accept
 *
 * accept the look_ahead token and move the lexer to the next token if the
 * look_ahead token matches the token parameter. In that case, and if required,
 * also hand back the de-escaped lexeme.
 *
 * returns true if the token matched, false otherwise.
 */
static inline bool
lex_accept(JsonLexContext *lex, JsonTokenType token, char **lexeme)
{
	if (lex->token_type == token)
	{
		if (lexeme != NULL)
		{
			if (lex->token_type == JSON_TOKEN_STRING)
			{
				if (lex->strval != NULL)
					*lexeme = pstrdup(lex->strval->data);
			}
			else
			{
				int			len = (lex->token_terminator - lex->token_start);
				char	   *tokstr = palloc(len + 1);

				memcpy(tokstr, lex->token_start, len);
				tokstr[len] = '\0';
				*lexeme = tokstr;
			}
		}
		json_lex(lex);
		return true;
	}
	return false;
}

/*
 * lex_accept
 *
 * move the lexer to the next token if the current look_ahead token matches
 * the parameter token. Otherwise, report an error.
 */
static inline void
lex_expect(JsonParseContext ctx, JsonLexContext *lex, JsonTokenType token)
{
	if (!lex_accept(lex, token, NULL))
		report_parse_error(ctx, lex);
}

/* chars to consider as part of an alphanumeric token */
#define JSON_ALPHANUMERIC_CHAR(c)  \
	(((c) >= 'a' && (c) <= 'z') || \
	 ((c) >= 'A' && (c) <= 'Z') || \
	 ((c) >= '0' && (c) <= '9') || \
	 (c) == '_' || \
	 IS_HIGHBIT_SET(c))

/*
 * Utility function to check if a string is a valid JSON number.
 *
 * str is of length len, and need not be null-terminated.
 */
bool
IsValidJsonNumber(const char *str, int len)
{
	bool		numeric_error;
	int			total_len;
	JsonLexContext dummy_lex;

	if (len <= 0)
		return false;

	/*
	 * json_lex_number expects a leading  '-' to have been eaten already.
	 *
	 * having to cast away the constness of str is ugly, but there's not much
	 * easy alternative.
	 */
	if (*str == '-')
	{
		dummy_lex.input = (char *) str + 1;
		dummy_lex.input_length = len - 1;
	}
	else
	{
		dummy_lex.input = (char *) str;
		dummy_lex.input_length = len;
	}

	json_lex_number(&dummy_lex, dummy_lex.input, &numeric_error, &total_len);

	return (!numeric_error) && (total_len == dummy_lex.input_length);
}

/*
 * Input.
 */
Datum
json_in(PG_FUNCTION_ARGS)
{
	char	   *json = PG_GETARG_CSTRING(0);
	text	   *result = cstring_to_text(json);
	JsonLexContext *lex;

	/* validate it */
	lex = makeJsonLexContext(result, false);
	pg_parse_json(lex, &nullSemAction);

	/* Internal representation is the same as text, for now */
	PG_RETURN_TEXT_P(result);
}

/*
 * Output.
 */
Datum
json_out(PG_FUNCTION_ARGS)
{
	/* we needn't detoast because text_to_cstring will handle that */
	Datum		txt = PG_GETARG_DATUM(0);

	PG_RETURN_CSTRING(TextDatumGetCString(txt));
}

/*
 * Binary send.
 */
Datum
json_send(PG_FUNCTION_ARGS)
{
	text	   *t = PG_GETARG_TEXT_PP(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendtext(&buf, VARDATA_ANY(t), VARSIZE_ANY_EXHDR(t));
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/*
 * Binary receive.
 */
Datum
json_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	char	   *str;
	int			nbytes;
	JsonLexContext *lex;

	str = pq_getmsgtext(buf, buf->len - buf->cursor, &nbytes);

	/* Validate it. */
	lex = makeJsonLexContextCstringLen(str, nbytes, GetDatabaseEncoding(), false);
	pg_parse_json(lex, &nullSemAction);

	PG_RETURN_TEXT_P(cstring_to_text_with_len(str, nbytes));
}

/*
 *	Recursive Descent parse routines. There is one for each structural
 *	element in a json document:
 *	  - scalar (string, number, true, false, null)
 *	  - array  ( [ ] )
 *	  - array element
 *	  - object ( { } )
 *	  - object field
 */
static inline void
parse_scalar(JsonLexContext *lex, JsonSemAction *sem)
{
	char	   *val = NULL;
	json_scalar_action sfunc = sem->scalar;
	char	  **valaddr;
	JsonTokenType tok = lex_peek(lex);

	valaddr = sfunc == NULL ? NULL : &val;

	/* a scalar must be a string, a number, true, false, or null */
	switch (tok)
	{
		case JSON_TOKEN_TRUE:
			lex_accept(lex, JSON_TOKEN_TRUE, valaddr);
			break;
		case JSON_TOKEN_FALSE:
			lex_accept(lex, JSON_TOKEN_FALSE, valaddr);
			break;
		case JSON_TOKEN_NULL:
			lex_accept(lex, JSON_TOKEN_NULL, valaddr);
			break;
		case JSON_TOKEN_NUMBER:
			lex_accept(lex, JSON_TOKEN_NUMBER, valaddr);
			break;
		case JSON_TOKEN_STRING:
			lex_accept(lex, JSON_TOKEN_STRING, valaddr);
			break;
		default:
			report_parse_error(JSON_PARSE_VALUE, lex);
	}

	if (sfunc != NULL)
		(*sfunc) (sem->semstate, val, tok);
}

static void
parse_object_field(JsonLexContext *lex, JsonSemAction *sem)
{
	/*
	 * An object field is "fieldname" : value where value can be a scalar,
	 * object or array.  Note: in user-facing docs and error messages, we
	 * generally call a field name a "key".
	 */

	char	   *fname = NULL;	/* keep compiler quiet */
	json_ofield_action ostart = sem->object_field_start;
	json_ofield_action oend = sem->object_field_end;
	bool		isnull;
	char	  **fnameaddr = NULL;
	JsonTokenType tok;

	if (ostart != NULL || oend != NULL)
		fnameaddr = &fname;

	if (!lex_accept(lex, JSON_TOKEN_STRING, fnameaddr))
		report_parse_error(JSON_PARSE_STRING, lex);

	lex_expect(JSON_PARSE_OBJECT_LABEL, lex, JSON_TOKEN_COLON);

	tok = lex_peek(lex);
	isnull = tok == JSON_TOKEN_NULL;

	if (ostart != NULL)
		(*ostart) (sem->semstate, fname, isnull);

	switch (tok)
	{
		case JSON_TOKEN_OBJECT_START:
			parse_object(lex, sem);
			break;
		case JSON_TOKEN_ARRAY_START:
			parse_array(lex, sem);
			break;
		default:
			parse_scalar(lex, sem);
	}

	if (oend != NULL)
		(*oend) (sem->semstate, fname, isnull);
}

static void
parse_object(JsonLexContext *lex, JsonSemAction *sem)
{
	/*
	 * an object is a possibly empty sequence of object fields, separated by
	 * commas and surrounded by curly braces.
	 */
	json_struct_action ostart = sem->object_start;
	json_struct_action oend = sem->object_end;
	JsonTokenType tok;

	check_stack_depth();

	if (ostart != NULL)
		(*ostart) (sem->semstate);

	/*
	 * Data inside an object is at a higher nesting level than the object
	 * itself. Note that we increment this after we call the semantic routine
	 * for the object start and restore it before we call the routine for the
	 * object end.
	 */
	lex->lex_level++;

	/* we know this will succeed, just clearing the token */
	lex_expect(JSON_PARSE_OBJECT_START, lex, JSON_TOKEN_OBJECT_START);

	tok = lex_peek(lex);
	switch (tok)
	{
		case JSON_TOKEN_STRING:
			parse_object_field(lex, sem);
			while (lex_accept(lex, JSON_TOKEN_COMMA, NULL))
				parse_object_field(lex, sem);
			break;
		case JSON_TOKEN_OBJECT_END:
			break;
		default:
			/* case of an invalid initial token inside the object */
			report_parse_error(JSON_PARSE_OBJECT_START, lex);
	}

	lex_expect(JSON_PARSE_OBJECT_NEXT, lex, JSON_TOKEN_OBJECT_END);

	lex->lex_level--;

	if (oend != NULL)
		(*oend) (sem->semstate);
}

static void
parse_array_element(JsonLexContext *lex, JsonSemAction *sem)
{
	json_aelem_action astart = sem->array_element_start;
	json_aelem_action aend = sem->array_element_end;
	JsonTokenType tok = lex_peek(lex);

	bool		isnull;

	isnull = tok == JSON_TOKEN_NULL;

	if (astart != NULL)
		(*astart) (sem->semstate, isnull);

	/* an array element is any object, array or scalar */
	switch (tok)
	{
		case JSON_TOKEN_OBJECT_START:
			parse_object(lex, sem);
			break;
		case JSON_TOKEN_ARRAY_START:
			parse_array(lex, sem);
			break;
		default:
			parse_scalar(lex, sem);
	}

	if (aend != NULL)
		(*aend) (sem->semstate, isnull);
}

static void
parse_array(JsonLexContext *lex, JsonSemAction *sem)
{
	/*
	 * an array is a possibly empty sequence of array elements, separated by
	 * commas and surrounded by square brackets.
	 */
	json_struct_action astart = sem->array_start;
	json_struct_action aend = sem->array_end;

	check_stack_depth();

	if (astart != NULL)
		(*astart) (sem->semstate);

	/*
	 * Data inside an array is at a higher nesting level than the array
	 * itself. Note that we increment this after we call the semantic routine
	 * for the array start and restore it before we call the routine for the
	 * array end.
	 */
	lex->lex_level++;

	lex_expect(JSON_PARSE_ARRAY_START, lex, JSON_TOKEN_ARRAY_START);
	if (lex_peek(lex) != JSON_TOKEN_ARRAY_END)
	{

		parse_array_element(lex, sem);

		while (lex_accept(lex, JSON_TOKEN_COMMA, NULL))
			parse_array_element(lex, sem);
	}

	lex_expect(JSON_PARSE_ARRAY_NEXT, lex, JSON_TOKEN_ARRAY_END);

	lex->lex_level--;

	if (aend != NULL)
		(*aend) (sem->semstate);
}

/*
 * The next token in the input stream is known to be a number; lex it.
 *
 * In JSON, a number consists of four parts:
 *
 * (1) An optional minus sign ('-').
 *
 * (2) Either a single '0', or a string of one or more digits that does not
 *	   begin with a '0'.
 *
 * (3) An optional decimal part, consisting of a period ('.') followed by
 *	   one or more digits.  (Note: While this part can be omitted
 *	   completely, it's not OK to have only the decimal point without
 *	   any digits afterwards.)
 *
 * (4) An optional exponent part, consisting of 'e' or 'E', optionally
 *	   followed by '+' or '-', followed by one or more digits.  (Note:
 *	   As with the decimal part, if 'e' or 'E' is present, it must be
 *	   followed by at least one digit.)
 *
 * The 's' argument to this function points to the ostensible beginning
 * of part 2 - i.e. the character after any optional minus sign, or the
 * first character of the string if there is none.
 *
 * If num_err is not NULL, we return an error flag to *num_err rather than
 * raising an error for a badly-formed number.  Also, if total_len is not NULL
 * the distance from lex->input to the token end+1 is returned to *total_len.
 */
static inline void
json_lex_number(JsonLexContext *lex, char *s,
				bool *num_err, int *total_len)
{
	bool		error = false;
	int			len = s - lex->input;

	/* Part (1): leading sign indicator. */
	/* Caller already did this for us; so do nothing. */

	/* Part (2): parse main digit string. */
	if (len < lex->input_length && *s == '0')
	{
		s++;
		len++;
	}
	else if (len < lex->input_length && *s >= '1' && *s <= '9')
	{
		do
		{
			s++;
			len++;
		} while (len < lex->input_length && *s >= '0' && *s <= '9');
	}
	else
		error = true;

	/* Part (3): parse optional decimal portion. */
	if (len < lex->input_length && *s == '.')
	{
		s++;
		len++;
		if (len == lex->input_length || *s < '0' || *s > '9')
			error = true;
		else
		{
			do
			{
				s++;
				len++;
			} while (len < lex->input_length && *s >= '0' && *s <= '9');
		}
	}

	/* Part (4): parse optional exponent. */
	if (len < lex->input_length && (*s == 'e' || *s == 'E'))
	{
		s++;
		len++;
		if (len < lex->input_length && (*s == '+' || *s == '-'))
		{
			s++;
			len++;
		}
		if (len == lex->input_length || *s < '0' || *s > '9')
			error = true;
		else
		{
			do
			{
				s++;
				len++;
			} while (len < lex->input_length && *s >= '0' && *s <= '9');
		}
	}

	/*
	 * Check for trailing garbage.  As in json_lex(), any alphanumeric stuff
	 * here should be considered part of the token for error-reporting
	 * purposes.
	 */
	for (; len < lex->input_length && JSON_ALPHANUMERIC_CHAR(*s); s++, len++)
		error = true;

	if (total_len != NULL)
		*total_len = len;

	if (num_err != NULL)
	{
		/* let the caller handle any error */
		*num_err = error;
	}
	else
	{
		/* return token endpoint */
		lex->prev_token_terminator = lex->token_terminator;
		lex->token_terminator = s;
		/* handle error if any */
		if (error)
			report_invalid_token(lex);
	}
}

/*
 * Report a parse error.
 *
 * lex->token_start and lex->token_terminator must identify the current token.
 */
static void
report_parse_error(JsonParseContext ctx, JsonLexContext *lex)
{
	char	   *token;
	int			toklen;

	/* Handle case where the input ended prematurely. */
	if (lex->token_start == NULL || lex->token_type == JSON_TOKEN_END)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type %s", "json"),
				 errdetail("The input string ended unexpectedly."),
				 report_json_context(lex)));

	/* Separate out the current token. */
	toklen = lex->token_terminator - lex->token_start;
	token = palloc(toklen + 1);
	memcpy(token, lex->token_start, toklen);
	token[toklen] = '\0';

	/* Complain, with the appropriate detail message. */
	if (ctx == JSON_PARSE_END)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type %s", "json"),
				 errdetail("Expected end of input, but found \"%s\".",
						   token),
				 report_json_context(lex)));
	else
	{
		switch (ctx)
		{
			case JSON_PARSE_VALUE:
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("invalid input syntax for type %s", "json"),
						 errdetail("Expected JSON value, but found \"%s\".",
								   token),
						 report_json_context(lex)));
				break;
			case JSON_PARSE_STRING:
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("invalid input syntax for type %s", "json"),
						 errdetail("Expected string, but found \"%s\".",
								   token),
						 report_json_context(lex)));
				break;
			case JSON_PARSE_ARRAY_START:
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("invalid input syntax for type %s", "json"),
						 errdetail("Expected array element or \"]\", but found \"%s\".",
								   token),
						 report_json_context(lex)));
				break;
			case JSON_PARSE_ARRAY_NEXT:
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("invalid input syntax for type %s", "json"),
						 errdetail("Expected \",\" or \"]\", but found \"%s\".",
								   token),
						 report_json_context(lex)));
				break;
			case JSON_PARSE_OBJECT_START:
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("invalid input syntax for type %s", "json"),
						 errdetail("Expected string or \"}\", but found \"%s\".",
								   token),
						 report_json_context(lex)));
				break;
			case JSON_PARSE_OBJECT_LABEL:
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("invalid input syntax for type %s", "json"),
						 errdetail("Expected \":\", but found \"%s\".",
								   token),
						 report_json_context(lex)));
				break;
			case JSON_PARSE_OBJECT_NEXT:
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("invalid input syntax for type %s", "json"),
						 errdetail("Expected \",\" or \"}\", but found \"%s\".",
								   token),
						 report_json_context(lex)));
				break;
			case JSON_PARSE_OBJECT_COMMA:
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("invalid input syntax for type %s", "json"),
						 errdetail("Expected string, but found \"%s\".",
								   token),
						 report_json_context(lex)));
				break;
			default:
				elog(ERROR, "unexpected json parse state: %d", ctx);
		}
	}
}

/*
 * Report an invalid input token.
 *
 * lex->token_start and lex->token_terminator must identify the token.
 */
static void
report_invalid_token(JsonLexContext *lex)
{
	char	   *token;
	int			toklen;

	/* Separate out the offending token. */
	toklen = lex->token_terminator - lex->token_start;
	token = palloc(toklen + 1);
	memcpy(token, lex->token_start, toklen);
	token[toklen] = '\0';

	ereport(ERROR,
			(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
			 errmsg("invalid input syntax for type %s", "json"),
			 errdetail("Token \"%s\" is invalid.", token),
			 report_json_context(lex)));
}

/*
 * Report a CONTEXT line for bogus JSON input.
 *
 * lex->token_terminator must be set to identify the spot where we detected
 * the error.  Note that lex->token_start might be NULL, in case we recognized
 * error at EOF.
 *
 * The return value isn't meaningful, but we make it non-void so that this
 * can be invoked inside ereport().
 */
static int
report_json_context(JsonLexContext *lex)
{
	const char *context_start;
	const char *context_end;
	const char *line_start;
	int			line_number;
	char	   *ctxt;
	int			ctxtlen;
	const char *prefix;
	const char *suffix;

	/* Choose boundaries for the part of the input we will display */
	context_start = lex->input;
	context_end = lex->token_terminator;
	line_start = context_start;
	line_number = 1;
	for (;;)
	{
		/* Always advance over newlines */
		if (context_start < context_end && *context_start == '\n')
		{
			context_start++;
			line_start = context_start;
			line_number++;
			continue;
		}
		/* Otherwise, done as soon as we are close enough to context_end */
		if (context_end - context_start < 50)
			break;
		/* Advance to next multibyte character */
		if (IS_HIGHBIT_SET(*context_start))
			context_start += pg_mblen(context_start);
		else
			context_start++;
	}

	/*
	 * We add "..." to indicate that the excerpt doesn't start at the
	 * beginning of the line ... but if we're within 3 characters of the
	 * beginning of the line, we might as well just show the whole line.
	 */
	if (context_start - line_start <= 3)
		context_start = line_start;

	/* Get a null-terminated copy of the data to present */
	ctxtlen = context_end - context_start;
	ctxt = palloc(ctxtlen + 1);
	memcpy(ctxt, context_start, ctxtlen);
	ctxt[ctxtlen] = '\0';

	/*
	 * Show the context, prefixing "..." if not starting at start of line, and
	 * suffixing "..." if not ending at end of line.
	 */
	prefix = (context_start > line_start) ? "..." : "";
	suffix = (lex->token_type != JSON_TOKEN_END && context_end - lex->input < lex->input_length && *context_end != '\n' && *context_end != '\r') ? "..." : "";

	return errcontext("JSON data, line %d: %s%s%s",
					  line_number, prefix, ctxt, suffix);
}

/*
 * Determine how we want to print values of a given type in datum_to_json.
 *
 * Given the datatype OID, return its JsonTypeCategory, as well as the type's
 * output function OID.  If the returned category is JSONTYPE_CAST, we
 * return the OID of the type->JSON cast function instead.
 */
static void
json_categorize_type(Oid typoid,
					 JsonTypeCategory *tcategory,
					 Oid *outfuncoid)
{
	bool		typisvarlena;

	/* Look through any domain */
	typoid = getBaseType(typoid);

	*outfuncoid = InvalidOid;

	/*
	 * We need to get the output function for everything except date and
	 * timestamp types, array and composite types, booleans, and non-builtin
	 * types where there's a cast to json.
	 */

	switch (typoid)
	{
		case BOOLOID:
			*tcategory = JSONTYPE_BOOL;
			break;

		case INT2OID:
		case INT4OID:
		case INT8OID:
		case FLOAT4OID:
		case FLOAT8OID:
		case NUMERICOID:
			getTypeOutputInfo(typoid, outfuncoid, &typisvarlena);
			*tcategory = JSONTYPE_NUMERIC;
			break;

		case DATEOID:
			*tcategory = JSONTYPE_DATE;
			break;

		case TIMESTAMPOID:
			*tcategory = JSONTYPE_TIMESTAMP;
			break;

		case TIMESTAMPTZOID:
			*tcategory = JSONTYPE_TIMESTAMPTZ;
			break;

		case JSONOID:
		case JSONBOID:
			getTypeOutputInfo(typoid, outfuncoid, &typisvarlena);
			*tcategory = JSONTYPE_JSON;
			break;

		default:
			/* Check for arrays and composites */
			if (OidIsValid(get_element_type(typoid)) || typoid == ANYARRAYOID
				|| typoid == RECORDARRAYOID)
				*tcategory = JSONTYPE_ARRAY;
			else if (type_is_rowtype(typoid))	/* includes RECORDOID */
				*tcategory = JSONTYPE_COMPOSITE;
			else
			{
				/* It's probably the general case ... */
				*tcategory = JSONTYPE_OTHER;
				/* but let's look for a cast to json, if it's not built-in */
				if (typoid >= FirstNormalObjectId)
				{
					Oid			castfunc;
					CoercionPathType ctype;

					ctype = find_coercion_pathway(JSONOID, typoid,
												  COERCION_EXPLICIT,
												  &castfunc);
					if (ctype == COERCION_PATH_FUNC && OidIsValid(castfunc))
					{
						*tcategory = JSONTYPE_CAST;
						*outfuncoid = castfunc;
					}
					else
					{
						/* non builtin type with no cast */
						getTypeOutputInfo(typoid, outfuncoid, &typisvarlena);
					}
				}
				else
				{
					/* any other builtin type */
					getTypeOutputInfo(typoid, outfuncoid, &typisvarlena);
				}
			}
			break;
	}
}

/*
 * Turn a Datum into JSON text, appending the string to "result".
 *
 * tcategory and outfuncoid are from a previous call to json_categorize_type,
 * except that if is_null is true then they can be invalid.
 *
 * If key_scalar is true, the value is being printed as a key, so insist
 * it's of an acceptable type, and force it to be quoted.
 */
static void
datum_to_json(Datum val, bool is_null, StringInfo result,
			  JsonTypeCategory tcategory, Oid outfuncoid,
			  bool key_scalar)
{
	char	   *outputstr;
	text	   *jsontext;

	check_stack_depth();

	/* callers are expected to ensure that null keys are not passed in */
	Assert(!(key_scalar && is_null));

	if (is_null)
	{
		appendStringInfoString(result, "null");
		return;
	}

	if (key_scalar &&
		(tcategory == JSONTYPE_ARRAY ||
		 tcategory == JSONTYPE_COMPOSITE ||
		 tcategory == JSONTYPE_JSON ||
		 tcategory == JSONTYPE_CAST))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("key value must be scalar, not array, composite, or json")));

	switch (tcategory)
	{
		case JSONTYPE_ARRAY:
			array_to_json_internal(val, result, false);
			break;
		case JSONTYPE_COMPOSITE:
			composite_to_json(val, result, false);
			break;
		case JSONTYPE_BOOL:
			outputstr = DatumGetBool(val) ? "true" : "false";
			if (key_scalar)
				escape_json(result, outputstr);
			else
				appendStringInfoString(result, outputstr);
			break;
		case JSONTYPE_NUMERIC:
			outputstr = OidOutputFunctionCall(outfuncoid, val);

			/*
			 * Don't call escape_json for a non-key if it's a valid JSON
			 * number.
			 */
			if (!key_scalar && IsValidJsonNumber(outputstr, strlen(outputstr)))
				appendStringInfoString(result, outputstr);
			else
				escape_json(result, outputstr);
			pfree(outputstr);
			break;
		case JSONTYPE_DATE:
			{
				DateADT		date;
				struct pg_tm tm;
				char		buf[MAXDATELEN + 1];

				date = DatumGetDateADT(val);
				/* Same as date_out(), but forcing DateStyle */
				if (DATE_NOT_FINITE(date))
					EncodeSpecialDate(date, buf);
				else
				{
					j2date(date + POSTGRES_EPOCH_JDATE,
						   &(tm.tm_year), &(tm.tm_mon), &(tm.tm_mday));
					EncodeDateOnly(&tm, USE_XSD_DATES, buf);
				}
				appendStringInfo(result, "\"%s\"", buf);
			}
			break;
		case JSONTYPE_TIMESTAMP:
			{
				Timestamp	timestamp;
				struct pg_tm tm;
				fsec_t		fsec;
				char		buf[MAXDATELEN + 1];

				timestamp = DatumGetTimestamp(val);
				/* Same as timestamp_out(), but forcing DateStyle */
				if (TIMESTAMP_NOT_FINITE(timestamp))
					EncodeSpecialTimestamp(timestamp, buf);
				else if (timestamp2tm(timestamp, NULL, &tm, &fsec, NULL, NULL) == 0)
					EncodeDateTime(&tm, fsec, false, 0, NULL, USE_XSD_DATES, buf);
				else
					ereport(ERROR,
							(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
							 errmsg("timestamp out of range")));
				appendStringInfo(result, "\"%s\"", buf);
			}
			break;
		case JSONTYPE_TIMESTAMPTZ:
			{
				TimestampTz timestamp;
				struct pg_tm tm;
				int			tz;
				fsec_t		fsec;
				const char *tzn = NULL;
				char		buf[MAXDATELEN + 1];

				timestamp = DatumGetTimestampTz(val);
				/* Same as timestamptz_out(), but forcing DateStyle */
				if (TIMESTAMP_NOT_FINITE(timestamp))
					EncodeSpecialTimestamp(timestamp, buf);
				else if (timestamp2tm(timestamp, &tz, &tm, &fsec, &tzn, NULL) == 0)
					EncodeDateTime(&tm, fsec, true, tz, tzn, USE_XSD_DATES, buf);
				else
					ereport(ERROR,
							(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
							 errmsg("timestamp out of range")));
				appendStringInfo(result, "\"%s\"", buf);
			}
			break;
		case JSONTYPE_JSON:
			/* JSON and JSONB output will already be escaped */
			outputstr = OidOutputFunctionCall(outfuncoid, val);
			appendStringInfoString(result, outputstr);
			pfree(outputstr);
			break;
		case JSONTYPE_CAST:
			/* outfuncoid refers to a cast function, not an output function */
			jsontext = DatumGetTextPP(OidFunctionCall1(outfuncoid, val));
			outputstr = text_to_cstring(jsontext);
			appendStringInfoString(result, outputstr);
			pfree(outputstr);
			pfree(jsontext);
			break;
		default:
			outputstr = OidOutputFunctionCall(outfuncoid, val);
			escape_json(result, outputstr);
			pfree(outputstr);
			break;
	}
}

/*
 * Process a single dimension of an array.
 * If it's the innermost dimension, output the values, otherwise call
 * ourselves recursively to process the next dimension.
 */
static void
array_dim_to_json(StringInfo result, int dim, int ndims, int *dims, Datum *vals,
				  bool *nulls, int *valcount, JsonTypeCategory tcategory,
				  Oid outfuncoid, bool use_line_feeds)
{
	int			i;
	const char *sep;

	Assert(dim < ndims);

	sep = use_line_feeds ? ",\n " : ",";

	appendStringInfoChar(result, '[');

	for (i = 1; i <= dims[dim]; i++)
	{
		if (i > 1)
			appendStringInfoString(result, sep);

		if (dim + 1 == ndims)
		{
			datum_to_json(vals[*valcount], nulls[*valcount], result, tcategory,
						  outfuncoid, false);
			(*valcount)++;
		}
		else
		{
			/*
			 * Do we want line feeds on inner dimensions of arrays? For now
			 * we'll say no.
			 */
			array_dim_to_json(result, dim + 1, ndims, dims, vals, nulls,
							  valcount, tcategory, outfuncoid, false);
		}
	}

	appendStringInfoChar(result, ']');
}

/*
 * Turn an array into JSON.
 */
static void
array_to_json_internal(Datum array, StringInfo result, bool use_line_feeds)
{
	ArrayType  *v = DatumGetArrayTypeP(array);
	Oid			element_type = ARR_ELEMTYPE(v);
	int		   *dim;
	int			ndim;
	int			nitems;
	int			count = 0;
	Datum	   *elements;
	bool	   *nulls;
	int16		typlen;
	bool		typbyval;
	char		typalign;
	JsonTypeCategory tcategory;
	Oid			outfuncoid;

	ndim = ARR_NDIM(v);
	dim = ARR_DIMS(v);
	nitems = ArrayGetNItems(ndim, dim);

	if (nitems <= 0)
	{
		appendStringInfoString(result, "[]");
		return;
	}

	get_typlenbyvalalign(element_type,
						 &typlen, &typbyval, &typalign);

	json_categorize_type(element_type,
						 &tcategory, &outfuncoid);

	deconstruct_array(v, element_type, typlen, typbyval,
					  typalign, &elements, &nulls,
					  &nitems);

	array_dim_to_json(result, 0, ndim, dim, elements, nulls, &count, tcategory,
					  outfuncoid, use_line_feeds);

	pfree(elements);
	pfree(nulls);
}

/*
 * Turn a composite / record into JSON.
 */
static void
composite_to_json(Datum composite, StringInfo result, bool use_line_feeds)
{
	HeapTupleHeader td;
	Oid			tupType;
	int32		tupTypmod;
	TupleDesc	tupdesc;
	HeapTupleData tmptup,
			   *tuple;
	int			i;
	bool		needsep = false;
	const char *sep;

	sep = use_line_feeds ? ",\n " : ",";

	td = DatumGetHeapTupleHeader(composite);

	/* Extract rowtype info and find a tupdesc */
	tupType = HeapTupleHeaderGetTypeId(td);
	tupTypmod = HeapTupleHeaderGetTypMod(td);
	tupdesc = lookup_rowtype_tupdesc(tupType, tupTypmod);

	/* Build a temporary HeapTuple control structure */
	tmptup.t_len = HeapTupleHeaderGetDatumLength(td);
	tmptup.t_data = td;
	tuple = &tmptup;

	appendStringInfoChar(result, '{');

	for (i = 0; i < tupdesc->natts; i++)
	{
		Datum		val;
		bool		isnull;
		char	   *attname;
		JsonTypeCategory tcategory;
		Oid			outfuncoid;

		if (TupleDescAttr(tupdesc, i)->attisdropped)
			continue;

		if (needsep)
			appendStringInfoString(result, sep);
		needsep = true;

		attname = NameStr(TupleDescAttr(tupdesc, i)->attname);
		escape_json(result, attname);
		appendStringInfoChar(result, ':');

		val = heap_getattr(tuple, i + 1, tupdesc, &isnull);

		if (isnull)
		{
			tcategory = JSONTYPE_NULL;
			outfuncoid = InvalidOid;
		}
		else
			json_categorize_type(TupleDescAttr(tupdesc, i)->atttypid,
								 &tcategory, &outfuncoid);

		datum_to_json(val, isnull, result, tcategory, outfuncoid, false);
	}

	appendStringInfoChar(result, '}');
	ReleaseTupleDesc(tupdesc);
}

/*
 * Append JSON text for "val" to "result".
 *
 * This is just a thin wrapper around datum_to_json.  If the same type will be
 * printed many times, avoid using this; better to do the json_categorize_type
 * lookups only once.
 */
static void
add_json(Datum val, bool is_null, StringInfo result,
		 Oid val_type, bool key_scalar)
{
	JsonTypeCategory tcategory;
	Oid			outfuncoid;

	if (val_type == InvalidOid)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("could not determine input data type")));

	if (is_null)
	{
		tcategory = JSONTYPE_NULL;
		outfuncoid = InvalidOid;
	}
	else
		json_categorize_type(val_type,
							 &tcategory, &outfuncoid);

	datum_to_json(val, is_null, result, tcategory, outfuncoid, key_scalar);
}

/*
 * SQL function array_to_json(row)
 */
extern Datum
array_to_json(PG_FUNCTION_ARGS)
{
	Datum		array = PG_GETARG_DATUM(0);
	StringInfo	result;

	result = makeStringInfo();

	array_to_json_internal(array, result, false);

	PG_RETURN_TEXT_P(cstring_to_text_with_len(result->data, result->len));
}

/*
 * SQL function array_to_json(row, prettybool)
 */
extern Datum
array_to_json_pretty(PG_FUNCTION_ARGS)
{
	Datum		array = PG_GETARG_DATUM(0);
	bool		use_line_feeds = PG_GETARG_BOOL(1);
	StringInfo	result;

	result = makeStringInfo();

	array_to_json_internal(array, result, use_line_feeds);

	PG_RETURN_TEXT_P(cstring_to_text_with_len(result->data, result->len));
}

/*
 * SQL function row_to_json(row)
 */
extern Datum
row_to_json(PG_FUNCTION_ARGS)
{
	Datum		array = PG_GETARG_DATUM(0);
	StringInfo	result;

	result = makeStringInfo();

	composite_to_json(array, result, false);

	PG_RETURN_TEXT_P(cstring_to_text_with_len(result->data, result->len));
}

/*
 * SQL function row_to_json(row, prettybool)
 */
extern Datum
row_to_json_pretty(PG_FUNCTION_ARGS)
{
	Datum		array = PG_GETARG_DATUM(0);
	bool		use_line_feeds = PG_GETARG_BOOL(1);
	StringInfo	result;

	result = makeStringInfo();

	composite_to_json(array, result, use_line_feeds);

	PG_RETURN_TEXT_P(cstring_to_text_with_len(result->data, result->len));
}

/*
 * SQL function to_json(anyvalue)
 */
Datum
to_json(PG_FUNCTION_ARGS)
{
	Datum		val = PG_GETARG_DATUM(0);
	Oid			val_type = get_fn_expr_argtype(fcinfo->flinfo, 0);
	StringInfo	result;
	JsonTypeCategory tcategory;
	Oid			outfuncoid;

	if (val_type == InvalidOid)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("could not determine input data type")));

	json_categorize_type(val_type,
						 &tcategory, &outfuncoid);

	result = makeStringInfo();

	datum_to_json(val, false, result, tcategory, outfuncoid, false);

	PG_RETURN_TEXT_P(cstring_to_text_with_len(result->data, result->len));
}

/*
 * combinable_json_agg_transfn function
 *
 * aggregate input column as a json array value.
 */
PG_FUNCTION_INFO_V1(combinable_json_agg_transfn);
Datum
combinable_json_agg_transfn(PG_FUNCTION_ARGS)
{
	MemoryContext aggcontext,
				oldcontext;
	JsonAggState *state;
	Datum		val;

	if (!AggCheckCallContext(fcinfo, &aggcontext))
	{
		/* cannot be called directly because of internal-type argument */
		elog(ERROR, "json_agg_transfn called in non-aggregate context");
	}

	if (PG_ARGISNULL(0))
	{
		Oid			arg_type = get_fn_expr_argtype(fcinfo->flinfo, 1);

		if (arg_type == InvalidOid)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("could not determine input data type")));

		/*
		 * Make this state object in a context where it will persist for the
		 * duration of the aggregate call.  MemoryContextSwitchTo is only
		 * needed the first time, as the StringInfo routines make sure they
		 * use the right context to enlarge the object if necessary.
		 */
		oldcontext = MemoryContextSwitchTo(aggcontext);
		state = (JsonAggState *) palloc(sizeof(JsonAggState));
		state->str = makeStringInfo();
		MemoryContextSwitchTo(oldcontext);

		appendStringInfoChar(state->str, '[');
		json_categorize_type(arg_type, &state->val_category,
							 &state->val_output_func);
	}
	else
	{
		state = (JsonAggState *) PG_GETARG_POINTER(0);
		appendStringInfoString(state->str, ", ");
	}

	/* fast path for NULLs */
	if (PG_ARGISNULL(1))
	{
		datum_to_json((Datum) 0, true, state->str, JSONTYPE_NULL,
					  InvalidOid, false);
		PG_RETURN_POINTER(state);
	}

	val = PG_GETARG_DATUM(1);

	/* add some whitespace if structured type and not first item */
	if (!PG_ARGISNULL(0) &&
		(state->val_category == JSONTYPE_ARRAY ||
		 state->val_category == JSONTYPE_COMPOSITE))
	{
		appendStringInfoString(state->str, "\n ");
	}

	datum_to_json(val, false, state->str, state->val_category,
				  state->val_output_func, false);

	/*
	 * The transition type for array_agg() is declared to be "internal", which
	 * is a pass-by-value type the same size as a pointer.  So we can safely
	 * pass the JsonAggState pointer through nodeAgg.c's machinations.
	 */
	PG_RETURN_POINTER(state);
}

/*
 * combinable_json_agg_finalfn
 */
PG_FUNCTION_INFO_V1(combinable_json_agg_finalfn);
Datum
combinable_json_agg_finalfn(PG_FUNCTION_ARGS)
{
	JsonAggState *state;

	state = PG_ARGISNULL(0) ?
		NULL :
		(JsonAggState *) PG_GETARG_POINTER(0);

	/* NULL result for no rows in, as is standard with aggregates */
	if (state == NULL)
		PG_RETURN_NULL();

	/* Else return state with appropriate array terminator added */
	PG_RETURN_TEXT_P(catenate_stringinfo_string(state->str, "]"));
}

/*
 * Helper function for aggregates: return given StringInfo's contents plus
 * specified trailing string, as a text datum.  We need this because aggregate
 * final functions are not allowed to modify the aggregate state.
 */
static text *
catenate_stringinfo_string(StringInfo buffer, const char *addon)
{
	/* custom version of cstring_to_text_with_len */
	int			buflen = buffer->len;
	int			addlen = strlen(addon);
	text	   *result = (text *) palloc(buflen + addlen + VARHDRSZ);

	SET_VARSIZE(result, buflen + addlen + VARHDRSZ);
	memcpy(VARDATA(result), buffer->data, buflen);
	memcpy(VARDATA(result) + buflen, addon, addlen);

	return result;
}

/*
 * SQL function json_build_object(variadic "any")
 */
Datum
json_build_object(PG_FUNCTION_ARGS)
{
	int			nargs = PG_NARGS();
	int			i;
	const char *sep = "";
	StringInfo	result;
	Datum	   *args;
	bool	   *nulls;
	Oid		   *types;

	/* fetch argument values to build the object */
	nargs = extract_variadic_args(fcinfo, 0, false, &args, &types, &nulls);

	if (nargs < 0)
		PG_RETURN_NULL();

	if (nargs % 2 != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("argument list must have even number of elements"),
				 errhint("The arguments of json_build_object() must consist of alternating keys and values.")));

	result = makeStringInfo();

	appendStringInfoChar(result, '{');

	for (i = 0; i < nargs; i += 2)
	{
		appendStringInfoString(result, sep);
		sep = ", ";

		/* process key */
		if (nulls[i])
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("argument %d cannot be null", i + 1),
					 errhint("Object keys should be text.")));

		add_json(args[i], false, result, types[i], true);

		appendStringInfoString(result, " : ");

		/* process value */
		add_json(args[i + 1], nulls[i + 1], result, types[i + 1], false);
	}

	appendStringInfoChar(result, '}');

	PG_RETURN_TEXT_P(cstring_to_text_with_len(result->data, result->len));
}

/*
 * degenerate case of json_build_object where it gets 0 arguments.
 */
Datum
json_build_object_noargs(PG_FUNCTION_ARGS)
{
	PG_RETURN_TEXT_P(cstring_to_text_with_len("{}", 2));
}

/*
 * SQL function json_build_array(variadic "any")
 */
Datum
json_build_array(PG_FUNCTION_ARGS)
{
	int			nargs;
	int			i;
	const char *sep = "";
	StringInfo	result;
	Datum	   *args;
	bool	   *nulls;
	Oid		   *types;

	/* fetch argument values to build the array */
	nargs = extract_variadic_args(fcinfo, 0, false, &args, &types, &nulls);

	if (nargs < 0)
		PG_RETURN_NULL();

	result = makeStringInfo();

	appendStringInfoChar(result, '[');

	for (i = 0; i < nargs; i++)
	{
		appendStringInfoString(result, sep);
		sep = ", ";
		add_json(args[i], nulls[i], result, types[i], false);
	}

	appendStringInfoChar(result, ']');

	PG_RETURN_TEXT_P(cstring_to_text_with_len(result->data, result->len));
}

/*
 * degenerate case of json_build_array where it gets 0 arguments.
 */
Datum
json_build_array_noargs(PG_FUNCTION_ARGS)
{
	PG_RETURN_TEXT_P(cstring_to_text_with_len("[]", 2));
}

/*
 * SQL function json_object(text[])
 *
 * take a one or two dimensional array of text as key/value pairs
 * for a json object.
 */
Datum
json_object(PG_FUNCTION_ARGS)
{
	ArrayType  *in_array = PG_GETARG_ARRAYTYPE_P(0);
	int			ndims = ARR_NDIM(in_array);
	StringInfoData result;
	Datum	   *in_datums;
	bool	   *in_nulls;
	int			in_count,
				count,
				i;
	text	   *rval;
	char	   *v;

	switch (ndims)
	{
		case 0:
			PG_RETURN_DATUM(CStringGetTextDatum("{}"));
			break;

		case 1:
			if ((ARR_DIMS(in_array)[0]) % 2)
				ereport(ERROR,
						(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
						 errmsg("array must have even number of elements")));
			break;

		case 2:
			if ((ARR_DIMS(in_array)[1]) != 2)
				ereport(ERROR,
						(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
						 errmsg("array must have two columns")));
			break;

		default:
			ereport(ERROR,
					(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
					 errmsg("wrong number of array subscripts")));
	}

	deconstruct_array(in_array,
					  TEXTOID, -1, false, 'i',
					  &in_datums, &in_nulls, &in_count);

	count = in_count / 2;

	initStringInfo(&result);

	appendStringInfoChar(&result, '{');

	for (i = 0; i < count; ++i)
	{
		if (in_nulls[i * 2])
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("null value not allowed for object key")));

		v = TextDatumGetCString(in_datums[i * 2]);
		if (i > 0)
			appendStringInfoString(&result, ", ");
		escape_json(&result, v);
		appendStringInfoString(&result, " : ");
		pfree(v);
		if (in_nulls[i * 2 + 1])
			appendStringInfoString(&result, "null");
		else
		{
			v = TextDatumGetCString(in_datums[i * 2 + 1]);
			escape_json(&result, v);
			pfree(v);
		}
	}

	appendStringInfoChar(&result, '}');

	pfree(in_datums);
	pfree(in_nulls);

	rval = cstring_to_text_with_len(result.data, result.len);
	pfree(result.data);

	PG_RETURN_TEXT_P(rval);

}

/*
 * SQL function json_object(text[], text[])
 *
 * take separate key and value arrays of text to construct a json object
 * pairwise.
 */
Datum
json_object_two_arg(PG_FUNCTION_ARGS)
{
	ArrayType  *key_array = PG_GETARG_ARRAYTYPE_P(0);
	ArrayType  *val_array = PG_GETARG_ARRAYTYPE_P(1);
	int			nkdims = ARR_NDIM(key_array);
	int			nvdims = ARR_NDIM(val_array);
	StringInfoData result;
	Datum	   *key_datums,
			   *val_datums;
	bool	   *key_nulls,
			   *val_nulls;
	int			key_count,
				val_count,
				i;
	text	   *rval;
	char	   *v;

	if (nkdims > 1 || nkdims != nvdims)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("wrong number of array subscripts")));

	if (nkdims == 0)
		PG_RETURN_DATUM(CStringGetTextDatum("{}"));

	deconstruct_array(key_array,
					  TEXTOID, -1, false, 'i',
					  &key_datums, &key_nulls, &key_count);

	deconstruct_array(val_array,
					  TEXTOID, -1, false, 'i',
					  &val_datums, &val_nulls, &val_count);

	if (key_count != val_count)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("mismatched array dimensions")));

	initStringInfo(&result);

	appendStringInfoChar(&result, '{');

	for (i = 0; i < key_count; ++i)
	{
		if (key_nulls[i])
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("null value not allowed for object key")));

		v = TextDatumGetCString(key_datums[i]);
		if (i > 0)
			appendStringInfoString(&result, ", ");
		escape_json(&result, v);
		appendStringInfoString(&result, " : ");
		pfree(v);
		if (val_nulls[i])
			appendStringInfoString(&result, "null");
		else
		{
			v = TextDatumGetCString(val_datums[i]);
			escape_json(&result, v);
			pfree(v);
		}
	}

	appendStringInfoChar(&result, '}');

	pfree(key_datums);
	pfree(key_nulls);
	pfree(val_datums);
	pfree(val_nulls);

	rval = cstring_to_text_with_len(result.data, result.len);
	pfree(result.data);

	PG_RETURN_TEXT_P(rval);
}


/*
 * Produce a JSON string literal, properly escaping characters in the text.
 */
void
escape_json(StringInfo buf, const char *str)
{
	const char *p;

	appendStringInfoCharMacro(buf, '"');
	for (p = str; *p; p++)
	{
		switch (*p)
		{
			case '\b':
				appendStringInfoString(buf, "\\b");
				break;
			case '\f':
				appendStringInfoString(buf, "\\f");
				break;
			case '\n':
				appendStringInfoString(buf, "\\n");
				break;
			case '\r':
				appendStringInfoString(buf, "\\r");
				break;
			case '\t':
				appendStringInfoString(buf, "\\t");
				break;
			case '"':
				appendStringInfoString(buf, "\\\"");
				break;
			case '\\':
				appendStringInfoString(buf, "\\\\");
				break;
			default:
				if ((unsigned char) *p < ' ')
					appendStringInfo(buf, "\\u%04x", (int) *p);
				else
					appendStringInfoCharMacro(buf, *p);
				break;
		}
	}
	appendStringInfoCharMacro(buf, '"');
}

/*
 * SQL function json_typeof(json) -> text
 *
 * Returns the type of the outermost JSON value as TEXT.  Possible types are
 * "object", "array", "string", "number", "boolean", and "null".
 *
 * Performs a single call to json_lex() to get the first token of the supplied
 * value.  This initial token uniquely determines the value's type.  As our
 * input must already have been validated by json_in() or json_recv(), the
 * initial token should never be JSON_TOKEN_OBJECT_END, JSON_TOKEN_ARRAY_END,
 * JSON_TOKEN_COLON, JSON_TOKEN_COMMA, or JSON_TOKEN_END.
 */
Datum
json_typeof(PG_FUNCTION_ARGS)
{
	text	   *json;

	JsonLexContext *lex;
	JsonTokenType tok;
	char	   *type;

	json = PG_GETARG_TEXT_PP(0);
	lex = makeJsonLexContext(json, false);

	/* Lex exactly one token from the input and check its type. */
	json_lex(lex);
	tok = lex_peek(lex);
	switch (tok)
	{
		case JSON_TOKEN_OBJECT_START:
			type = "object";
			break;
		case JSON_TOKEN_ARRAY_START:
			type = "array";
			break;
		case JSON_TOKEN_STRING:
			type = "string";
			break;
		case JSON_TOKEN_NUMBER:
			type = "number";
			break;
		case JSON_TOKEN_TRUE:
		case JSON_TOKEN_FALSE:
			type = "boolean";
			break;
		case JSON_TOKEN_NULL:
			type = "null";
			break;
		default:
			elog(ERROR, "unexpected json token: %d", tok);
	}

	PG_RETURN_TEXT_P(cstring_to_text(type));
}

PG_FUNCTION_INFO_V1(json_agg_serialize);
Datum
json_agg_serialize(PG_FUNCTION_ARGS)
{
	JsonAggState *state = (JsonAggState *) PG_GETARG_POINTER(0);
	bytea *result;

	if (state == NULL || PG_ARGISNULL(0))
		PG_RETURN_NULL();

	result = (bytea *) palloc0(state->str->len + VARHDRSZ);
	SET_VARSIZE(result, state->str->len + VARHDRSZ);
	memcpy(VARDATA(result), state->str->data, state->str->len);

	PG_RETURN_BYTEA_P(result);
}

PG_FUNCTION_INFO_V1(json_agg_deserialize);
Datum
json_agg_deserialize(PG_FUNCTION_ARGS)
{
	MemoryContext context;
	MemoryContext old;
	bytea *bytes;
	JsonAggState *result;

	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();

	if (!AggCheckCallContext(fcinfo, &context))
		context = CurrentMemoryContext;

	bytes = PG_GETARG_BYTEA_PP(0);

	old = MemoryContextSwitchTo(context);

	result = palloc0(sizeof(JsonAggState));
	result->str = makeStringInfo();
	appendBinaryStringInfo(result->str, VARDATA_ANY(bytes), VARSIZE_ANY_EXHDR(bytes));

	MemoryContextSwitchTo(old);

	PG_RETURN_POINTER(result);
}

/*
 * json_agg combine function
 */
PG_FUNCTION_INFO_V1(json_agg_combine);
Datum
json_agg_combine(PG_FUNCTION_ARGS)
{
	JsonAggState *state;
	JsonAggState *incoming;
	MemoryContext aggcontext;

	if (!AggCheckCallContext(fcinfo, &aggcontext))
	{
		/* cannot be called directly because of internal-type argument */
		elog(ERROR, "json_agg_combine called in non-aggregate context");
	}

	if (PG_ARGISNULL(0))
	{
		MemoryContext oldcontext;
		/*
		 * Make this StringInfo in a context where it will persist for the
		 * duration of the aggregate call.  MemoryContextSwitchTo is only
		 * needed the first time, as the StringInfo routines make sure they
		 * use the right context to enlarge the object if necessary.
		 */
		oldcontext = MemoryContextSwitchTo(aggcontext);
		state = (JsonAggState *) palloc(sizeof(JsonAggState));
		state->str = makeStringInfo();
		MemoryContextSwitchTo(oldcontext);

		appendStringInfoChar(state->str, '[');
	}
	else
	{
		state = (JsonAggState *) PG_GETARG_POINTER(0);
		if (state)
			appendStringInfoString(state->str, ", ");
	}

	if (!PG_ARGISNULL(1))
	{
		incoming = (JsonAggState *) PG_GETARG_POINTER(1);

		if (incoming)
		{
			/* skip the '[' in the beginning */
			Assert(incoming->str->data[0] == '[');
			appendBinaryStringInfo(state->str, incoming->str->data + 1, incoming->str->len - 1);
		}
	}

	PG_RETURN_POINTER(state);
}

/*
 * json_object_agg_combine
 */
PG_FUNCTION_INFO_V1(json_object_agg_combine);
Datum
json_object_agg_combine(PG_FUNCTION_ARGS)
{
	JsonAggState *state;
	JsonAggState *incoming;
	MemoryContext aggcontext;

	if (!AggCheckCallContext(fcinfo, &aggcontext))
	{
		/* cannot be called directly because of internal-type argument */
		elog(ERROR, "json_agg_combine called in non-aggregate context");
	}

	if (PG_ARGISNULL(0))
	{
		MemoryContext oldcontext;
		/*
		 * Make this StringInfo in a context where it will persist for the
		 * duration of the aggregate call.  MemoryContextSwitchTo is only
		 * needed the first time, as the StringInfo routines make sure they
		 * use the right context to enlarge the object if necessary.
		 */
		oldcontext = MemoryContextSwitchTo(aggcontext);
		state = (JsonAggState *) palloc(sizeof(JsonAggState));
		state->str = makeStringInfo();
		MemoryContextSwitchTo(oldcontext);

		appendStringInfoString(state->str, "{");
	}
	else
	{
		state = (JsonAggState *) PG_GETARG_POINTER(0);
		if (state)
			appendStringInfoString(state->str, ",");
	}

	if (!PG_ARGISNULL(1))
	{
		incoming = (JsonAggState *) PG_GETARG_POINTER(1);

		if (!incoming)
			PG_RETURN_POINTER(state);

		/* skip the '{' in the beginning */
		Assert(incoming->str->data[0] == '{');
		appendBinaryStringInfo(state->str, incoming->str->data + 1, incoming->str->len - 1);
	}

	PG_RETURN_POINTER(state);
}

/*
 * combinable_json_object_agg_transfn transition function.
 *
 * aggregate two input columns as a single json object value.
 */
PG_FUNCTION_INFO_V1(combinable_json_object_agg_transfn);
Datum
combinable_json_object_agg_transfn(PG_FUNCTION_ARGS)
{
	MemoryContext aggcontext,
				oldcontext;
	JsonAggState *state;
	Datum		arg;

	if (!AggCheckCallContext(fcinfo, &aggcontext))
	{
		/* cannot be called directly because of internal-type argument */
		elog(ERROR, "json_object_agg_transfn called in non-aggregate context");
	}

	if (PG_ARGISNULL(0))
	{
		Oid			arg_type;

		/*
		 * Make the StringInfo in a context where it will persist for the
		 * duration of the aggregate call. Switching context is only needed
		 * for this initial step, as the StringInfo routines make sure they
		 * use the right context to enlarge the object if necessary.
		 */
		oldcontext = MemoryContextSwitchTo(aggcontext);
		state = (JsonAggState *) palloc(sizeof(JsonAggState));
		state->str = makeStringInfo();
		MemoryContextSwitchTo(oldcontext);

		arg_type = get_fn_expr_argtype(fcinfo->flinfo, 1);

		if (arg_type == InvalidOid)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("could not determine data type for argument 1")));

		json_categorize_type(arg_type, &state->key_category,
							 &state->key_output_func);

		arg_type = get_fn_expr_argtype(fcinfo->flinfo, 2);

		if (arg_type == InvalidOid)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("could not determine data type for argument 2")));

		json_categorize_type(arg_type, &state->val_category,
							 &state->val_output_func);

		appendStringInfoString(state->str, "{ ");
	}
	else
	{
		state = (JsonAggState *) PG_GETARG_POINTER(0);
		appendStringInfoString(state->str, ", ");
	}

	/*
	 * Note: since json_object_agg() is declared as taking type "any", the
	 * parser will not do any type conversion on unknown-type literals (that
	 * is, undecorated strings or NULLs).  Such values will arrive here as
	 * type UNKNOWN, which fortunately does not matter to us, since
	 * unknownout() works fine.
	 */

	if (PG_ARGISNULL(1))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("field name must not be null")));

	arg = PG_GETARG_DATUM(1);

	datum_to_json(arg, false, state->str, state->key_category,
				  state->key_output_func, true);

	appendStringInfoString(state->str, " : ");

	if (PG_ARGISNULL(2))
		arg = (Datum) 0;
	else
		arg = PG_GETARG_DATUM(2);

	datum_to_json(arg, PG_ARGISNULL(2), state->str, state->val_category,
				  state->val_output_func, false);

	PG_RETURN_POINTER(state);
}

/*
 * combinable_json_object_agg_finalfn
 *
 */
PG_FUNCTION_INFO_V1(combinable_json_object_agg_finalfn);
Datum
combinable_json_object_agg_finalfn(PG_FUNCTION_ARGS)
{
	JsonAggState *state;

	state = PG_ARGISNULL(0) ? NULL : (JsonAggState *) PG_GETARG_POINTER(0);

	/* NULL result for no rows in, as is standard with aggregates */
	if (state == NULL)
		PG_RETURN_NULL();

	/* Else return state with appropriate object terminator added */
	PG_RETURN_TEXT_P(catenate_stringinfo_string(state->str, " }"));
}

#define JSON_OBJECT_INT_SUM_HEADER 0xFFA0

typedef struct JsonObjectIntSumState
{
	uint16 header;
	char *current_key;
	HTAB *kv;
} JsonObjectIntSumState;

typedef struct JsonObjectIntSumEntry
{
	char key[NAMEDATALEN];
	int64 value;
} JsonObjectIntSumEntry;

/*
 * handle_key_start
 */
static void
handle_key_start(void *_state, char *fname, bool isnull)
{
	JsonObjectIntSumState *state = (JsonObjectIntSumState *) _state;

	if (strlen(fname) > NAMEDATALEN)
		elog(ERROR, "json_object_sum requires keys to be no longer than %d characters", NAMEDATALEN);

	state->current_key = fname;
}

/*
 * handle_scalar
 */
static void
handle_scalar(void *_state, char *token, JsonTokenType tokentype)
{
	bool found;
	JsonObjectIntSumState *state = (JsonObjectIntSumState *) _state;
	JsonObjectIntSumEntry *entry = (JsonObjectIntSumEntry *) hash_search(state->kv, state->current_key, HASH_ENTER, &found);
	int64 result;

	if (!found)
		entry->value = 0;

	result = pg_strtoint64(token);
	entry->value += result;
}

/*
 * json_object_int_sum_startup
 */
static JsonObjectIntSumState *
json_object_int_sum_startup(FunctionCallInfo fcinfo)
{
	MemoryContext oldcontext;
	JsonSemAction *sem;
	JsonObjectIntSumState *state;
	HASHCTL ctl;

	oldcontext = MemoryContextSwitchTo(fcinfo->flinfo->fn_mcxt);
	state = palloc0(sizeof(JsonObjectIntSumState));
	state->header = JSON_OBJECT_INT_SUM_HEADER;

	MemSet(&ctl, 0, sizeof(HASHCTL));
	ctl.keysize = NAMEDATALEN;
	ctl.entrysize = sizeof(JsonObjectIntSumEntry);
	ctl.hcxt = CurrentMemoryContext;
	state->kv = hash_create("json_object_sum", 32, &ctl,
							HASH_ELEM | HASH_STRINGS | HASH_CONTEXT);

	sem = palloc0(sizeof(JsonSemAction));
	sem->semstate = (void *) state;
	sem->object_field_start = handle_key_start;
	sem->scalar = handle_scalar;
	fcinfo->flinfo->fn_extra = (void *) sem;

	MemoryContextSwitchTo(oldcontext);

	return state;
}

/*
 * json_object_int_sum_transfn
 */
PG_FUNCTION_INFO_V1(json_object_int_sum_transfn);
Datum
json_object_int_sum_transfn(PG_FUNCTION_ARGS)
{
	MemoryContext aggcontext;
	MemoryContext oldcontext;
	JsonObjectIntSumState *state;
	JsonLexContext *lex;
	JsonSemAction *sem;
	char *raw = TextDatumGetCString(PG_GETARG_TEXT_P(1));

	if (!AggCheckCallContext(fcinfo, &aggcontext))
	{
		/* cannot be called directly because of internal-type argument */
		elog(ERROR, "json_object_sum_transfn called in non-aggregate context");
	}

	if (PG_ARGISNULL(0))
	{
		oldcontext = MemoryContextSwitchTo(aggcontext);
		state = json_object_int_sum_startup(fcinfo);
		MemoryContextSwitchTo(oldcontext);
	}
	else
	{
		state = (JsonObjectIntSumState *) PG_GETARG_POINTER(0);
	}

	lex = makeJsonLexContextCstringLen(raw, strlen(raw), GetDatabaseEncoding(), true);
	sem = (JsonSemAction *) fcinfo->flinfo->fn_extra;
	sem->semstate = (void *) state;
	pg_parse_json(lex, sem);

	PG_RETURN_POINTER(state);
}

/*
 * json_object_int_sum_serialize
 */
PG_FUNCTION_INFO_V1(json_object_int_sum_serialize);
Datum
json_object_int_sum_serialize(PG_FUNCTION_ARGS)
{
	JsonObjectIntSumState *state;
	HASH_SEQ_STATUS seq;
	JsonObjectIntSumEntry *entry;
	StringInfoData buf;
	bool first = true;
	uint16 header;

	/*
	 * We can be receiving either a transition state from json_object_int_sum_transfn
	 * or a previously returned JSON result of this function. We detect which it is by
	 * peeking at the transition state's header.
	 *
	 * TODO(derekjn) Clean up the entire signature of the json_object_int_sum aggregate
	 * to work more cleanly.
	 */
	char *p = (char *) PG_GETARG_POINTER(0);
	memcpy(&header, p, sizeof(header));

	if (header != JSON_OBJECT_INT_SUM_HEADER && !AggCheckCallContext(fcinfo, NULL))
		PG_RETURN_TEXT_P(PG_GETARG_TEXT_P(0));

	state = (JsonObjectIntSumState *) PG_GETARG_POINTER(0);
	initStringInfo(&buf);
	appendStringInfoString(&buf, "{ ");

	hash_seq_init(&seq, state->kv);
	while ((entry = (JsonObjectIntSumEntry *) hash_seq_search(&seq)) != NULL)
	{
		if (!first)
			appendStringInfo(&buf, ", \"%s\": %ld", entry->key, entry->value);
		else
			appendStringInfo(&buf, "\"%s\": %ld", entry->key, entry->value);
		first = false;
	}

	appendStringInfoString(&buf, " }");

	PG_RETURN_TEXT_P(cstring_to_text_with_len(buf.data, buf.len));
}

/*
 * json_object_int_sum_deserialize
 */
PG_FUNCTION_INFO_V1(json_object_int_sum_deserialize);
Datum
json_object_int_sum_deserialize(PG_FUNCTION_ARGS)
{
	bytea *b = PG_GETARG_BYTEA_P_COPY(0);

	PG_RETURN_POINTER(b);
}

/*
 * json_object_int_sum_finalfn
 */
PG_FUNCTION_INFO_V1(json_object_int_sum_finalfn);
Datum
json_object_int_sum_finalfn(PG_FUNCTION_ARGS)
{
	JsonObjectIntSumState *state = PG_ARGISNULL(0) ? NULL : (JsonObjectIntSumState *) PG_GETARG_POINTER(0);
	Datum d;

	if (!state)
		PG_RETURN_NULL();

	d = DirectFunctionCall1(json_object_int_sum_serialize, (Datum) state);

	PG_RETURN_TEXT_P(d);
}

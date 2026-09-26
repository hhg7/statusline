/*Claude Code status line: context window + usage.

Reads the status-line JSON payload on stdin and prints one ANSI-coloured line.
Payload shape taken from the 2.1.276 binary (function q$o/xnt): keys used here
are model.display_name, workspace.current_dir, context_window.{total_input_tokens,
context_window_size}, cost.total_cost_usd, and rate_limits.{five_hour,seven_day,
spend_limit}.{used_percentage,resets_at}. In 2.1.281 (function jjn) resets_at is
the rate-limit window's own resets_at, a Unix time in seconds; an ISO-8601
string is still accepted.

The 7d segment is rationed, after ~/Scripts/C/token.rationing.c: the weekly
quota resets Saturday 07:00 America/Chicago, per the account's stated
weekly-limit reset, and an even burn would by now have spent the fraction of the
window already elapsed. The segment shows consumption and that on-pace budget as
two percentages to one decimal, "7d 12.3%/15.0%", green at or under pace and red
once over it.

A translation of statusline.py with no dependency beyond libc, kept as the
fallback for a machine without json-c; statusline.c is the maintained version
and links against json-c instead. STATUSLINE_NOW, a Unix time, stands in for
the clock, so that the test compares this and the Python at the same instant.
Build and install with:
	make nodeps && make install-nodeps
Re-check after a Claude Code upgrade with:
	make test-nodeps
and with `echo '{}' | ~/.claude/statusline` against /context and /usage
in-session. `make valgrind-nodeps` checks that every allocation is freed.

There is no JSON parser in the C library and none installed here, so one is
hand-rolled below -- it also keeps the binary free of shared-library deps.*/

#define _DEFAULT_SOURCE   /*for timegm() and tm_gmtoff, which are not in C11*/

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RESET "\033[0m"

/*The weekly reset is Chicago wall-clock, so a window across a DST change is 167
or 169 hours rather than 168; token.rationing.c has the measurements.*/
#define RESET_TZ "America/Chicago"
#define WEEK     (7 * 86400.0)

/*10000-01-01T00:00:00Z. Python's datetime stops at year 9999, so the reference
cannot place a reset past this and neither does the C.*/
#define YEAR_10000 253402300800.0

static void *xrealloc(void *p, size_t n)
{
	void *q = realloc(p, n);
	if (!q)
		exit(1);   /*nothing printed: the status line simply goes blank*/
	return q;
}

/*Growable string: the stdin slurp, and the output line as it is built.

Every piece of the line is appended to one buffer rather than returned as a
string of its own, so there is exactly one allocation to free at the end.*/
typedef struct {
	char *p;
	size_t len, cap;
} sbuf;

/*Room for n more bytes and the NUL.*/
static void sreserve(sbuf *b, size_t n)
{
	if (b->len + n + 1 > b->cap) {
		size_t cap = b->cap ? b->cap : 64;
		while (b->len + n + 1 > cap)
			cap *= 2;
		b->p = xrealloc(b->p, cap);
		b->cap = cap;
	}
}

/*s never points into b->p, which may move under sreserve(); restrict lets the
compiler keep b->len and b->p in registers across the memcpy.*/
static void sput(sbuf *restrict b, const char *restrict s, size_t n)
{
	sreserve(b, n);
	memcpy(b->p + b->len, s, n);
	b->len += n;
	b->p[b->len] = '\0';
}

static void sputs(sbuf *restrict b, const char *restrict s)
{
	sput(b, s, strlen(s));
}

/*printf onto the end of b. No argument may point into b itself: the buffer can
move when it grows.*/
static void sappf(sbuf *restrict b, const char *restrict fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	if (n < 0)
		exit(1);
	sreserve(b, (size_t)n);
	va_start(ap, fmt);
	vsnprintf(b->p + b->len, (size_t)n + 1, fmt, ap);
	va_end(ap);
	b->len += (size_t)n;
}

static void sfree(sbuf *b)
{
	free(b->p);
	b->p = NULL;
	b->len = b->cap = 0;
}

/*JSON value tree*/

typedef enum {
	JNULL,   // also what a failed parse and a missing key yield
	JBOOL,
	JNUM,
	JSTR,
	JARR,
	JOBJ
} jtype;

typedef struct jval jval;
struct jval {
	jtype type;
	double num;      // JNUM
	int truth;       // JBOOL
	char *str;       // JSTR, decoded and NUL-terminated
	char **keys;     // JOBJ
	jval **items;    // JARR and JOBJ
	size_t n;
	size_t cap;      // allocated slots in keys/items; see jpush()
	int is_int;      // JNUM: the literal had no '.', 'e' or 'E', so Python made an int
};

static jval jnull = { JNULL, 0, 0, NULL, NULL, NULL, 0, 0, 0 };

static jval *jnew(jtype t)
{
	jval *v = xrealloc(NULL, sizeof *v);
	memset(v, 0, sizeof *v);
	v->type = t;
	return v;
}

/*Free a tree and everything it owns. The shared jnull is static, so it is
skipped, and anything jparse() or jget() returns can be passed. Recursion is
bounded by MAX_DEPTH, as the parse that built the tree was.*/
static void jfree(jval *v)
{
	if (!v || v == &jnull)
		return;
	for (size_t i = 0; i < v->n; i++) {
		if (v->keys)
			free(v->keys[i]);
		jfree(v->items[i]);
	}
	free(v->keys);
	free(v->items);
	free(v->str);
	free(v);
}

static void sputc(sbuf *b, char ch)
{
	sput(b, &ch, 1);
}

/*Append one code point as UTF-8.*/
static void sputcp(sbuf *b, unsigned long cp)
{
	char t[4];
	if (cp < 0x80) {
		t[0] = (char)cp;
		sput(b, t, 1);
	} else if (cp < 0x800) {
		t[0] = (char)(0xC0 | (cp >> 6));
		t[1] = (char)(0x80 | (cp & 0x3F));
		sput(b, t, 2);
	} else if (cp < 0x10000) {
		t[0] = (char)(0xE0 | (cp >> 12));
		t[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		t[2] = (char)(0x80 | (cp & 0x3F));
		sput(b, t, 3);
	} else {
		t[0] = (char)(0xF0 | (cp >> 18));
		t[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
		t[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
		t[3] = (char)(0x80 | (cp & 0x3F));
		sput(b, t, 4);
	}
}

/*Nesting past this is rejected rather than parsed.

jparse_value() recurses per level, so an unbounded document is a stack overflow
and not merely a slow parse: 100000 open brackets segfaulted before this. The
limit is json-c's JSON_TOKENER_DEFAULT_DEPTH (json_tokener.h:92), so that this
build and the json-c one agree on what is too deep. The payload itself nests
three levels.*/
#define MAX_DEPTH 32

typedef struct {
	const char *p;
	int ok;     // cleared on the first syntax error; the whole parse is then discarded
	int depth;  // containers currently open
} jparser;

/*Returns a value the caller owns, even when it clears js->ok: a container is
then handed back half-built, or the static jnull, and either goes to jfree().*/
static jval *jparse_value(jparser *js);

static void jskip_ws(jparser *js)
{
	while (*js->p == ' ' || *js->p == '\t' || *js->p == '\n' || *js->p == '\r')
		js->p++;
}

static int jhex4(jparser *js, unsigned long *out)
{
	unsigned long v = 0;
	for (int i = 0; i < 4; i++) {
		int ch = (unsigned char)js->p[i];
		int d;
		if (ch >= '0' && ch <= '9')
			d = ch - '0';
		else if (ch >= 'a' && ch <= 'f')
			d = ch - 'a' + 10;
		else if (ch >= 'A' && ch <= 'F')
			d = ch - 'A' + 10;
		else
			return 0;
		v = v * 16 + (unsigned long)d;
	}
	js->p += 4;
	*out = v;
	return 1;
}

/*Parse a string body, cursor sitting on the opening quote.*/
static char *jparse_string(jparser *js)
{
	sbuf b = { NULL, 0, 0 };
	sput(&b, "", 0);   /*so an empty string is "" rather than NULL*/
	js->p++;
	for (;;) {
		unsigned char ch = (unsigned char)*js->p;
		if (ch == '\0') {
			js->ok = 0;
			return b.p;
		}
		if (ch == '"') {
			js->p++;
			return b.p;
		}
		if (ch != '\\') {
			sputc(&b, (char)ch);
			js->p++;
			continue;
		}
		js->p++;
		switch (*js->p) {
		case '"':  sputc(&b, '"');  js->p++; break;
		case '\\': sputc(&b, '\\'); js->p++; break;
		case '/':  sputc(&b, '/');  js->p++; break;
		case 'b':  sputc(&b, '\b'); js->p++; break;
		case 'f':  sputc(&b, '\f'); js->p++; break;
		case 'n':  sputc(&b, '\n'); js->p++; break;
		case 'r':  sputc(&b, '\r'); js->p++; break;
		case 't':  sputc(&b, '\t'); js->p++; break;
		case 'u': {
			js->p++;
			unsigned long cp;
			if (!jhex4(js, &cp)) {
				js->ok = 0;
				return b.p;
			}
			if (cp >= 0xD800 && cp <= 0xDBFF && js->p[0] == '\\' && js->p[1] == 'u') {
				const char *save = js->p;
				js->p += 2;
				unsigned long lo;
				if (jhex4(js, &lo) && lo >= 0xDC00 && lo <= 0xDFFF)
					cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
				else
					js->p = save;   /*lone high surrogate: emit it as-is, as Python does*/
			}
			sputcp(&b, cp);
			break;
		}
		default:
			js->ok = 0;
			return b.p;
		}
	}
}

/*Append one member, doubling the capacity when it runs out. v takes ownership
of key and item.

Growing by one and copying each time is quadratic, which a 200k-key document
turns into a hang rather than a parse -- measured at over 20s before this, well
under a second after.*/
static void jpush(jval *v, char *key, jval *item)
{
	if (v->n == v->cap) {
		size_t cap = v->cap ? v->cap * 2 : 8;
		v->items = xrealloc(v->items, cap * sizeof *v->items);
		if (v->type == JOBJ)
			v->keys = xrealloc(v->keys, cap * sizeof *v->keys);
		v->cap = cap;
	}
	v->items[v->n] = item;
	if (v->type == JOBJ)
		v->keys[v->n] = key;
	v->n++;
}

static jval *jparse_value(jparser *js)
{
	jskip_ws(js);
	if (*js->p == '{' || *js->p == '[') {
		if (js->depth >= MAX_DEPTH) {
			js->ok = 0;
			return &jnull;
		}
		js->depth++;
	}
	switch (*js->p) {
	case '"': {
		jval *v = jnew(JSTR);
		v->str = jparse_string(js);
		return v;
	}
	case '{': {
		jval *v = jnew(JOBJ);
		js->p++;
		jskip_ws(js);
		if (*js->p == '}') {
			js->p++;
			js->depth--;
			return v;
		}
		for (;;) {
			jskip_ws(js);
			if (*js->p != '"') {
				js->ok = 0;
				return v;
			}
			char *key = jparse_string(js);
			jskip_ws(js);
			if (!js->ok || *js->p != ':') {
				js->ok = 0;
				free(key);
				return v;
			}
			js->p++;
			jval *item = jparse_value(js);
			if (!js->ok) {
				free(key);
				jfree(item);
				return v;
			}
			jpush(v, key, item);
			jskip_ws(js);
			if (*js->p == ',') {
				js->p++;
				continue;
			}
			if (*js->p == '}') {
				js->p++;
				js->depth--;
				return v;
			}
			js->ok = 0;
			return v;
		}
	}
	case '[': {
		jval *v = jnew(JARR);
		js->p++;
		jskip_ws(js);
		if (*js->p == ']') {
			js->p++;
			js->depth--;
			return v;
		}
		for (;;) {
			jval *item = jparse_value(js);
			if (!js->ok) {
				jfree(item);
				return v;
			}
			jpush(v, NULL, item);
			jskip_ws(js);
			if (*js->p == ',') {
				js->p++;
				continue;
			}
			if (*js->p == ']') {
				js->p++;
				js->depth--;
				return v;
			}
			js->ok = 0;
			return v;
		}
	}
	case 't':
		if (strncmp(js->p, "true", 4) == 0) {
			js->p += 4;
			jval *v = jnew(JBOOL);
			v->truth = 1;
			return v;
		}
		break;
	case 'f':
		if (strncmp(js->p, "false", 5) == 0) {
			js->p += 5;
			return jnew(JBOOL);
		}
		break;
	case 'n':
		if (strncmp(js->p, "null", 4) == 0) {
			js->p += 4;
			return jnew(JNULL);
		}
		break;
	default:
		break;
	}
	{
		char *end;
		double d = strtod(js->p, &end);
		if (end != js->p) {
			/*Python's json builds an int from a token with no fraction and no
			exponent and a float otherwise, and str() shows the difference:
			str(3) is "3" where str(3.0) is "3.0". The distinction is only
			recoverable from the literal, since both land in this double.*/
			int is_int = 1;
			for (const char *q = js->p; q < end; q++)
				if (*q == '.' || *q == 'e' || *q == 'E')
					is_int = 0;
			js->p = end;
			jval *v = jnew(JNUM);
			v->num = d;
			v->is_int = is_int;
			return v;
		}
	}
	js->ok = 0;
	return &jnull;
}

/*json.load(): a whole document, or JNULL if it does not parse. The caller
owns the result and releases it with jfree().*/
static jval *jparse(const char *text)
{
	jparser js = { text, 1, 0 };
	jval *v = jparse_value(&js);
	jskip_ws(&js);
	if (!js.ok || *js.p != '\0') {
		jfree(v);
		return &jnull;
	}
	return v;
}

/*dict.get(): never NULL, so chains need no guards.*/
static jval *jget(jval *v, const char *key)
{
	if (!v || v->type != JOBJ)
		return &jnull;
	for (size_t i = 0; i < v->n; i++)
		if (strcmp(v->keys[i], key) == 0)
			return v->items[i];
	return &jnull;
}

/*Python truthiness: null, false, 0, "", [] and {} are falsy.*/
static int jtruthy(const jval *v)
{
	switch (v->type) {
	case JNULL: return 0;
	case JBOOL: return v->truth;
	case JNUM:  return v->num != 0;
	case JSTR:  return v->str[0] != '\0';
	case JARR:
	case JOBJ:  return v->n > 0;
	}
	return 0;
}

/*isinstance(x, (int, float)) -- not numeric strings.

A JSON true/false counts, because Python's bool is a subclass of int: the
isinstance() guards in the Python accept True and go on to divide by it.*/
static int jnum(const jval *v, double *out)
{
	if (v->type == JBOOL) {
		*out = v->truth ? 1.0 : 0.0;
		return 1;
	}
	if (v->type != JNUM)
		return 0;
	*out = v->num;
	return 1;
}

static const char *jstr(const jval *v)
{
	return v->type == JSTR ? v->str : NULL;
}

/*Append str(float), matching CPython: shortest form that round-trips, and a
trailing ".0" on anything integral, so 3.0 prints as "3.0" rather than "3".*/
static void sput_pyfloat(sbuf *b, double d)
{
	if (isnan(d)) {
		sputs(b, "nan");
		return;
	}
	if (isinf(d)) {
		sputs(b, d < 0 ? "-inf" : "inf");
		return;
	}

	/*Fewest significant digits that still read back as the same double, which
	is what CPython's repr produces.*/
	char sci[64];
	int sig = 17;
	for (int p = 1; p <= 17; p++) {
		snprintf(sci, sizeof sci, "%.*e", p - 1, d);
		if (strtod(sci, NULL) == d) {
			sig = p;
			break;
		}
	}

	/*CPython shows a float in fixed notation while its decimal exponent is in
	[-4, 16) and switches to scientific outside that, so str(1e15) is
	"1000000000000000.0" but str(1e16) is "1e+16". Plain "%g" turns instead on
	the precision it was given, which made str(1e3) come out as "1e+03".*/
	const char *e = strchr(sci, 'e');
	int exp = e ? atoi(e + 1) : 0;
	if (exp < -4 || exp >= 16) {
		sputs(b, sci);
		return;
	}

	int decimals = sig - 1 - exp;
	if (decimals < 0)
		decimals = 0;
	char out[64];
	snprintf(out, sizeof out, "%.*f", decimals, d);
	sputs(b, out);
	if (!strchr(out, '.'))
		sputs(b, ".0");   /*str() keeps an integral float's ".0"*/
}

/*Append str() of a scalar; 0, appending nothing, for a container or null.
Matching Python's repr() for a list or dict is out of proportion for a field the
payload sends as a string.*/
static int sput_pystr(sbuf *b, const jval *v)
{
	switch (v->type) {
	case JSTR:
		sputs(b, v->str);
		return 1;
	case JNUM:
		if (v->is_int)
			sappf(b, "%lld", (long long)v->num);
		else
			sput_pyfloat(b, v->num);
		return 1;
	case JBOOL:
		sputs(b, v->truth ? "True" : "False");
		return 1;
	default:
		return 0;
	}
}

//Status line

static void sopen(sbuf *b, const char *code)
{
	sappf(b, "\033[%sm", code);
}

static void scol(sbuf *b, const char *code, const char *s)
{
	sopen(b, code);
	sputs(b, s);
	sputs(b, RESET);
}

/*Separator before every segment but the first; np counts segments so far.*/
static void part(sbuf *b, int *np)
{
	if ((*np)++)
		scol(b, "90", " │ ");   // │
}

/*float() of a percentage field; returns 0 if there is no number in it.

rate_limits.*.used_percentage already arrives as a percentage, so this only
coerces the type. An earlier version guessed, scaling anything at or below 1.0
on the theory it might be a utilisation fraction; that turned a genuine 1% into
a displayed 100%, seen 2026-09-18 with the bar reading "5h 100%" while /usage
reported 2%. A fraction-encoded 2% would have arrived as 0.02 and displayed
correctly, so the fraction case does not occur -- the guess could only ever
misfire, never help.*/
static int pct(const jval *o, double *out)
{
	double d;
	if (!jnum(o, &d)) {
		/*float("45") succeeds in Python, so a numeric string is accepted too.*/
		const char *s = jstr(o);
		if (!s)
			return 0;
		char *end;
		d = strtod(s, &end);
		if (end == s)
			return 0;
		while (isspace((unsigned char)*end))
			end++;
		if (*end != '\0')
			return 0;
	}
	*out = d;
	return 1;
}

//Green under 60%, yellow from 60%, red from 80%
static const char *colour_for(double p)
{
	return p < 60 ? "32" : (p < 80 ? "33" : "31");
}

#define BAR_FULL  "▰"   // ▰
#define BAR_EMPTY "▱"   // ▱
#define BAR_WIDTH 10

/*Cells a percentage fills, 0..BAR_WIDTH.

rint() rounds half to even under the default mode, as Python's round() does.
The clamp comes before the cast because a double past INT_MAX does not convert,
and a NaN, which Python would raise on, pins to empty.*/
static int cells(double p)
{
	double c = rint(p / 100 * BAR_WIDTH);
	if (!(c > 0))
		return 0;
	return c > BAR_WIDTH ? BAR_WIDTH : (int)c;
}

/*A coloured run of n copies of cell, closed even when n is 0, as the Python's
c(code, "") is.*/
static void srun(sbuf *b, const char *code, const char *cell, int n)
{
	sopen(b, code);
	for (int i = 0; i < n; i++)
		sputs(b, cell);
	sputs(b, RESET);
}

static void bar(sbuf *b, double p)
{
	int filled = cells(p);
	srun(b, colour_for(p), BAR_FULL, filled);
	srun(b, "90", BAR_EMPTY, BAR_WIDTH - filled);
}

static void human(sbuf *b, long long n)
{
	if (n >= 1000000)
		sappf(b, "%.1fM", (double)n / 1000000.0);
	else if (n >= 1000)
		sappf(b, "%.0fk", (double)n / 1000.0);
	else
		sappf(b, "%lld", n);
}

/*Read exactly `width` ASCII digits, advancing the cursor; 0 if they are not there.

sscanf("%2d") is no good here: it skips leading whitespace, accepts a sign, and
settles for one digit where two are required. ISO-8601 fields are fixed-width
and unsigned, and so is datetime.fromisoformat(), which this has to match.*/
static int read_digits(const char **p, int width, int *out)
{
	int v = 0;
	for (int i = 0; i < width; i++) {
		if (!isdigit((unsigned char)(*p)[i]))
			return 0;
		v = v * 10 + ((*p)[i] - '0');
	}
	*p += width;
	*out = v;
	return 1;
}

/*Parse an ISO-8601 timestamp to a Unix time, 1 on success.

Accepts what datetime.fromisoformat() does on CPython 3.12, since the Python
this replaces hands it whatever the payload carries and shows nothing when it
raises. Each rule below was checked against 3.12.3 rather than read off the
grammar, because several are quirks rather than ISO:

  date      YYYY-MM-DD or YYYYMMDD, never half-separated ("2027-0101" raises)
  time      HH, HH:MM, HH:MM:SS, HHMM, HHMMSS -- colons all-or-nothing within
            the time, but independent of the date's dashes ("20270101T05:30:15"
            parses)
  fraction  [.,] then digits, of which the first six are kept as microseconds
            and the rest dropped, as CPython 3.14.2 was seen to do. A bare "."
            at end of string raises, yet "15.+02:00" parses, so the digits are
            only required when nothing follows
  offset    +/-HH[:MM[:SS]], basic or extended, and rejected when the total
            reaches 24h -- the fields themselves are unchecked, so "+02:60"
            parses as +03:00
  naive     read as UTC, matching the tz-less branch in the Python

"Z" is not handled here: the caller's str.replace("Z", "+00:00") runs first, as
in the Python, which is also why a lowercase "z" is rejected.

Not accepted, and not emitted by the status-line payload: ordinal and week
dates (2026-W01-1).*/
static int parse_iso8601(const char *s, double *out)
{
	const char *p = s;
	int y, mo, d, h = 0, mi = 0, se = 0;
	long us = 0;   // microseconds

	/*Date. The dashes are all-or-nothing.*/
	if (!read_digits(&p, 4, &y))
		return 0;
	int dash = (*p == '-');
	if (dash)
		p++;
	if (!read_digits(&p, 2, &mo))
		return 0;
	if (dash && *p++ != '-')
		return 0;
	if (!read_digits(&p, 2, &d))
		return 0;

	long off = 0;
	if (*p == 'T' || *p == 't' || *p == ' ') {
		p++;
		if (!read_digits(&p, 2, &h))
			return 0;
		int colon = (*p == ':');
		if (colon)
			p++;
		if (isdigit((unsigned char)*p)) {
			if (!read_digits(&p, 2, &mi))
				return 0;
			if (colon) {
				if (*p == ':') {
					p++;
					if (!read_digits(&p, 2, &se))
						return 0;
				}
			} else if (isdigit((unsigned char)*p)) {
				if (!read_digits(&p, 2, &se))
					return 0;
			}
		} else if (colon) {
			return 0;   /*"05:" with no minutes*/
		}

		if (*p == '.' || *p == ',') {
			p++;
			int digits = 0;
			long scale = 100000;   // place value of the next digit, in microseconds
			while (isdigit((unsigned char)*p)) {
				if (digits < 6) {
					us += (*p - '0') * scale;
					scale /= 10;
				}
				p++;
				digits++;
			}
			if (!digits && *p == '\0')
				return 0;
		}

		if (*p == '+' || *p == '-') {
			int sign = (*p == '-') ? -1 : 1;
			int oh = 0, om = 0, os = 0;
			p++;
			if (!read_digits(&p, 2, &oh))
				return 0;
			int ocolon = (*p == ':');
			if (ocolon)
				p++;
			if (isdigit((unsigned char)*p)) {
				if (!read_digits(&p, 2, &om))
					return 0;
				if (ocolon) {
					if (*p == ':') {
						p++;
						if (!read_digits(&p, 2, &os))
							return 0;
					}
				} else if (isdigit((unsigned char)*p)) {
					if (!read_digits(&p, 2, &os))
						return 0;
				}
			} else if (ocolon) {
				return 0;
			}
			long total = oh * 3600L + om * 60L + os;
			if (total >= 24 * 3600L)
				return 0;
			off = sign * total;
		}
	}
	if (*p != '\0')
		return 0;
	if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || se > 59)
		return 0;

	struct tm tm;
	memset(&tm, 0, sizeof tm);
	tm.tm_year = y - 1900;
	tm.tm_mon = mo - 1;
	tm.tm_mday = d;
	tm.tm_hour = h;
	tm.tm_min = mi;
	tm.tm_sec = se;
	time_t t = timegm(&tm);
	if (t == (time_t)-1)
		return 0;

	/*timegm() normalises out-of-range input instead of rejecting it, so a bad
	day is only caught by converting back: 2026-02-30 would otherwise pass, and
	"2026-13-99T99:99:99" would land 200 days out rather than raising.*/
	struct tm back;
	if (!gmtime_r(&t, &back))
		return 0;
	if (back.tm_year != y - 1900 || back.tm_mon != mo - 1 || back.tm_mday != d
	    || back.tm_hour != h || back.tm_min != mi || back.tm_sec != se)
		return 0;

	/*Whole microseconds first, as timedelta.total_seconds() counts them, so the
	one rounding is the final division.*/
	*out = (double)(((int64_t)t - off) * 1000000 + us) / 1e6;
	return 1;
}

/*Unix time of a resets_at field, 1 on success.

A number is a Unix time already, which is what the binary sends. A string is
ISO-8601: str(ts).replace("Z", "+00:00"), as in the Python -- every "Z", not
just a trailing one, which is why parse_iso8601() need not know about "Z".*/
static int reset_time(const jval *o, double *out)
{
	double n;
	if (jnum(o, &n)) {
		if (!isfinite(n))
			return 0;
		*out = n;
		return 1;
	}
	const char *ts = jstr(o);
	if (!ts || !*ts)
		return 0;

	sbuf z = { NULL, 0, 0 };
	for (const char *q = ts; *q; q++) {
		if (*q == 'Z')
			sput(&z, "+00:00", 6);
		else
			sput(&z, q, 1);
	}
	int ok = parse_iso8601(z.p, out);
	sfree(&z);
	return ok;
}

/*Offset from UTC in seconds of RESET_TZ at Unix time t, 1 on success. The
floor matches datetime.fromtimestamp(), which looks up the whole second.*/
static int utcoffset(double t, long *out)
{
	time_t tt = (time_t)floor(t);
	struct tm tm;
	if (!localtime_r(&tt, &tm))
		return 0;
	*out = tm.tm_gmtoff;
	return 1;
}

/*Percent of the weekly window ending at `end` that has elapsed by `now`, 1 on
success; 0 once the reset has passed.

The window opens at the same Chicago wall-clock time a week earlier: a week of
seconds back, then corrected by however far the zone's offset moved in between.
That is exact for a 07:00 anchor, which is never within an hour of a Chicago
transition; token.rationing.c has the sweep of which anchors are safe.*/
static int week_elapsed(double end, double now, double *out)
{
	long o_end, o_start;
	if (end <= now || !(end < YEAR_10000))
		return 0;
	if (!utcoffset(end, &o_end) || !utcoffset(end - WEEK, &o_start))
		return 0;
	double start = end - WEEK + (double)o_end - (double)o_start;
	double p = (now - start) / (end - start) * 100;
	*out = p < 0 ? 0 : (p > 100 ? 100 : p);
	return 1;
}

/*time.time(), or STATUSLINE_NOW when the test sets it.*/
static double now_unix(void)
{
	const char *s = getenv("STATUSLINE_NOW");
	if (s && *s)
		return strtod(s, NULL);
	struct timespec ts;
	if (!timespec_get(&ts, TIME_UTC))
		return (double)time(NULL);
	return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(void)
{
	sbuf in = { NULL, 0, 0 };
	sput(&in, "", 0);
	char chunk[4096];
	size_t got;
	while ((got = fread(chunk, 1, sizeof chunk, stdin)) > 0)
		sput(&in, chunk, got);

	jval *d = jparse(in.p);
	sfree(&in);

	double now = now_unix();
	/*Pin the zone for utcoffset(): the reset is Chicago wall-clock, whatever TZ
	the caller has. Nothing else here reads local time. If it cannot be set the
	ration bar is left out rather than drawn against the wrong zone.*/
	int tz_ok = setenv("TZ", RESET_TZ, 1) == 0;
	tzset();

	sbuf out = { NULL, 0, 0 };
	sput(&out, "", 0);
	int np = 0;

	/*A display_name that is not a string yields no segment here. The Python
	raises TypeError on it and prints a traceback instead of a status line; a
	blank segment is the better failure for something drawn every render.*/
	const char *model = jstr(jget(jget(d, "model"), "display_name"));
	if (model && *model) {
		sbuf label = { NULL, 0, 0 };
		int nt = 0;
		sputs(&label, model);
		if (jtruthy(jget(d, "fast_mode"))) {
			sputs(&label, " fast");
			nt++;
		}
		jval *lvl = jget(jget(d, "effort"), "level");
		if (jtruthy(lvl)) {
			sbuf s = { NULL, 0, 0 };
			sput(&s, "", 0);
			if (sput_pystr(&s, lvl)) {
				sputs(&label, nt ? "/" : " ");
				sputs(&label, s.p);
			}
			sfree(&s);
		}
		part(&out, &np);
		scol(&out, "36", label.p);
		sfree(&label);
	}

	jval *cw = jget(d, "context_window");
	double used, size;
	if (jnum(jget(cw, "total_input_tokens"), &used)
	    && jnum(jget(cw, "context_window_size"), &size) && size != 0) {
		double p = used / size * 100;
		part(&out, &np);
		bar(&out, p);
		sputs(&out, " ");
		sopen(&out, colour_for(p));
		sappf(&out, "%.0f%%" RESET " ", p);
		sopen(&out, "90");
		human(&out, (long long)used);
		sputs(&out, "/");
		human(&out, (long long)size);
		sputs(&out, RESET);
	}

	jval *rl = jget(d, "rate_limits");
	static const char *keys[] = { "five_hour", "seven_day", "spend_limit" };
	static const char *shorts[] = { "5h", "7d", "spend" };
	for (int i = 0; i < 3; i++) {
		jval *v = jget(rl, keys[i]);
		if (!jtruthy(v))
			continue;
		double p;
		if (!pct(jget(v, "used_percentage"), &p))
			continue;
		const char *hue = colour_for(p);
		double end = 0, sched = 0;   /*set only on success; zeroed for -Wmaybe-uninitialized*/
		int have_end = reset_time(jget(v, "resets_at"), &end);

		part(&out, &np);
		/*Only the weekly window is rationed, and a NaN or infinite percentage
		cannot be measured against a pace.*/
		if (tz_ok && have_end && strcmp(keys[i], "seven_day") == 0 && isfinite(p)
		    && week_elapsed(end, now, &sched)) {
			/*Exactly on pace counts as under, so an untouched quota is never
			red. The colour is decided before rounding, so 15.04 against 14.96
			is red although both print as 15.0.*/
			sopen(&out, p > sched ? "31" : "32");
			sappf(&out, "%s %.1f%%/%.1f%%" RESET, shorts[i], p, sched);
		} else {
			sopen(&out, hue);
			sappf(&out, "%s %.0f%%" RESET, shorts[i], p);
		}

		double hours = have_end ? (end - now) / 3600 : 0;
		if (hours > 0) {
			sopen(&out, "90");
			if (hours >= 1)
				sappf(&out, "↺%.0fh" RESET, hours);
			else
				sappf(&out, "↺%.0fm" RESET, hours * 60);
		}
	}

	double cost;
	if (jnum(jget(jget(d, "cost"), "total_cost_usd"), &cost) && cost > 0) {
		part(&out, &np);
		sopen(&out, "90");
		sappf(&out, "$%.2f" RESET, cost);
	}

	const char *cwd = jstr(jget(jget(d, "workspace"), "current_dir"));
	if (!cwd || !*cwd)
		cwd = jstr(jget(d, "cwd"));
	if (cwd && *cwd) {
		/*str.rstrip("/") then the segment after the last "/".*/
		size_t len = strlen(cwd);
		while (len > 0 && cwd[len - 1] == '/')
			len--;
		size_t start = len;
		while (start > 0 && cwd[start - 1] != '/')
			start--;
		part(&out, &np);
		sopen(&out, "35");
		sput(&out, cwd + start, len - start);
		sputs(&out, RESET);
	}

	sputs(&out, "\n");
	fputs(out.p, stdout);
	sfree(&out);
	jfree(d);
	return 0;
}

/*Claude Code status line: context window + usage.

Reads the status-line JSON payload on stdin and prints one ANSI-coloured line.
Payload shape taken from the 2.1.276 binary (function q$o/xnt): keys used here
are model.display_name, workspace.current_dir, context_window.{total_input_tokens,
context_window_size}, cost.total_cost_usd, and rate_limits.{five_hour,seven_day,
spend_limit}.{used_percentage,resets_at}.

A translation of statusline.py with no dependency beyond libc, kept as the
fallback for a machine without json-c; statusline.c is the maintained version
and links against json-c instead. Build and install with:
	make nodeps && make install-nodeps
Re-check after a Claude Code upgrade with:
	make test-nodeps
and with `echo '{}' | ~/.claude/statusline` against /context and /usage
in-session.

There is no JSON parser in the C library and none installed here, so one is
hand-rolled below -- it also keeps the binary free of shared-library deps. The
parser allocates and never frees: the process reads one small payload, prints a
line and exits, so the arena is the process itself.*/

#define _DEFAULT_SOURCE   /*for timegm(), which is not in C11*/

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RESET "\033[0m"

static void *xmalloc(size_t n)
{
	void *p = malloc(n);
	if (!p)
		exit(1);   /*nothing printed: the status line simply goes blank*/
	return p;
}

/*printf into a freshly allocated string.*/
static char *ssprintf(const char *fmt, ...)
{
	va_list ap, ap2;
	va_start(ap, fmt);
	va_copy(ap2, ap);
	int n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	if (n < 0)
		exit(1);
	char *s = xmalloc((size_t)n + 1);
	vsnprintf(s, (size_t)n + 1, fmt, ap2);
	va_end(ap2);
	return s;
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
	jval *v = xmalloc(sizeof *v);
	memset(v, 0, sizeof *v);
	v->type = t;
	return v;
}

/*Growable string, used for decoded JSON strings.*/
typedef struct {
	char *p;
	size_t len, cap;
} sbuf;

static void sput(sbuf *b, const char *s, size_t n)
{
	if (b->len + n + 1 > b->cap) {
		size_t cap = b->cap ? b->cap : 32;
		while (b->len + n + 1 > cap)
			cap *= 2;
		char *q = xmalloc(cap);
		if (b->p)
			memcpy(q, b->p, b->len);
		b->p = q;
		b->cap = cap;
	}
	memcpy(b->p + b->len, s, n);
	b->len += n;
	b->p[b->len] = '\0';
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

/*Append one member, doubling the capacity when it runs out.

Growing by one and copying each time is quadratic, which a 200k-key document
turns into a hang rather than a parse -- measured at over 20s before this, well
under a second after.*/
static void jpush(jval *v, char *key, jval *item)
{
	if (v->n == v->cap) {
		size_t cap = v->cap ? v->cap * 2 : 8;
		jval **items = xmalloc(cap * sizeof *items);
		if (v->items)
			memcpy(items, v->items, v->n * sizeof *items);
		v->items = items;
		if (v->type == JOBJ) {
			char **keys = xmalloc(cap * sizeof *keys);
			if (v->keys)
				memcpy(keys, v->keys, v->n * sizeof *keys);
			v->keys = keys;
		}
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
			if (*js->p != ':') {
				js->ok = 0;
				return v;
			}
			js->p++;
			jval *item = jparse_value(js);
			if (!js->ok)
				return v;
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
			if (!js->ok)
				return v;
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

/*json.load(): a whole document, or JNULL if it does not parse.*/
static jval *jparse(const char *text)
{
	jparser js = { text, 1, 0 };
	jval *v = jparse_value(&js);
	jskip_ws(&js);
	if (!js.ok || *js.p != '\0')
		return &jnull;
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

/*str(float), matching CPython: shortest form that round-trips, and a trailing
".0" on anything integral, so 3.0 prints as "3.0" rather than "3".*/
static char *pystr_double(double d)
{
	if (isnan(d))
		return ssprintf("nan");
	if (isinf(d))
		return ssprintf(d < 0 ? "-inf" : "inf");

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
	if (exp < -4 || exp >= 16)
		return ssprintf("%s", sci);

	int decimals = sig - 1 - exp;
	if (decimals < 0)
		decimals = 0;
	char out[64];
	snprintf(out, sizeof out, "%.*f", decimals, d);
	if (!strchr(out, '.'))
		return ssprintf("%s.0", out);   /*str() keeps an integral float's ".0"*/
	return ssprintf("%s", out);
}

/*Status line*/

static char *col(int code, const char *s)
{
	return ssprintf("\033[%dm%s" RESET, code, s);
}

/*float() of a percentage field; returns 0 if there is no number in it.

rate_limits.*.used_percentage already arrives as a percentage, so this only
coerces the type. An earlier version guessed, scaling anything at or below 1.0
on the theory it might be a utilisation fraction; that turned a genuine 1% into
a displayed 100%, seen 2026-09-18 with the bar reading "5h 100%" while /usage
reported 2%. A fraction-encoded 2% would have arrived as 0.02 and displayed
correctly, so the fraction case does not occur -- the guess could only ever
misfire, never help.*/
static int pct(const jval *v, double *out)
{
	double d;
	if (!jnum(v, &d)) {
		if (v->type != JSTR)
			return 0;
		/*float("45") succeeds in Python, so a numeric string is accepted here too.*/
		char *end;
		d = strtod(v->str, &end);
		if (end == v->str)
			return 0;
		while (isspace((unsigned char)*end))
			end++;
		if (*end != '\0')
			return 0;
	}
	*out = d;
	return 1;
}

/*Green under 60%, yellow from 60%, red from 80%.*/
static int colour_for(double p)
{
	return p < 60 ? 32 : (p < 80 ? 33 : 31);
}

#define BAR_FULL  "▰"   // ▰
#define BAR_EMPTY "▱"   // ▱

static char *bar(double p)
{
	const int width = 10;
	/*rint() rounds half to even under the default mode, as Python's round() does.*/
	int filled = (int)rint(p / 100 * width);
	if (filled < 0)
		filled = 0;
	if (filled > width)
		filled = width;

	sbuf f = { NULL, 0, 0 }, e = { NULL, 0, 0 };
	sput(&f, "", 0);
	sput(&e, "", 0);
	for (int i = 0; i < filled; i++)
		sput(&f, BAR_FULL, strlen(BAR_FULL));
	for (int i = 0; i < width - filled; i++)
		sput(&e, BAR_EMPTY, strlen(BAR_EMPTY));

	char *a = col(colour_for(p), f.p);
	char *b = col(90, e.p);
	return ssprintf("%s%s", a, b);
}

static char *human(long long n)
{
	if (n >= 1000000)
		return ssprintf("%.1fM", (double)n / 1000000.0);
	if (n >= 1000)
		return ssprintf("%.0fk", (double)n / 1000.0);
	return ssprintf("%lld", n);
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
  fraction  [.,] then digits; dropped here, as the output is whole minutes. A
            bare "." at end of string raises, yet "15.+02:00" parses, so the
            digits are only required when nothing follows
  offset    +/-HH[:MM[:SS]], basic or extended, and rejected when the total
            reaches 24h -- the fields themselves are unchecked, so "+02:60"
            parses as +03:00
  naive     read as UTC, matching the tz-less branch in the Python

"Z" is not handled here: the caller's str.replace("Z", "+00:00") runs first, as
in the Python, which is also why a lowercase "z" is rejected.

Not accepted, and not emitted by the status-line payload: ordinal and week
dates (2026-W01-1).*/
static int parse_iso8601(const char *s, time_t *out)
{
	const char *p = s;
	int y, mo, d, h = 0, mi = 0, se = 0;

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
			while (isdigit((unsigned char)*p)) {
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

	*out = t - off;
	return 1;
}

/*Time until an ISO-8601 reset timestamp, or NULL if past or unparseable.*/
static char *resets_in(const jval *v)
{
	const char *ts = jstr(v);
	if (!ts || !*ts)
		return NULL;
	/*str(ts).replace("Z", "+00:00"), as in the Python -- every "Z", not just a
	trailing one, which is why parse_iso8601() need not know about "Z" at all.*/
	sbuf z = { NULL, 0, 0 };
	sput(&z, "", 0);
	for (const char *q = ts; *q; q++) {
		if (*q == 'Z')
			sput(&z, "+00:00", 6);
		else
			sput(&z, q, 1);
	}

	time_t t;
	if (!parse_iso8601(z.p, &t))
		return NULL;
	double hours = difftime(t, time(NULL)) / 3600.0;
	if (hours <= 0)
		return NULL;
	return hours >= 1 ? ssprintf("%.0fh", hours) : ssprintf("%.0fm", hours * 60);
}

int main(void)
{
	/*Slurp stdin.*/
	sbuf in = { NULL, 0, 0 };
	sput(&in, "", 0);
	char chunk[4096];
	size_t got;
	while ((got = fread(chunk, 1, sizeof chunk, stdin)) > 0)
		sput(&in, chunk, got);

	jval *d = jparse(in.p);

	char *parts[8];
	int np = 0;

	/*A display_name that is not a string yields no segment here. The Python
	raises TypeError on it and prints a traceback instead of a status line; a
	blank segment is the better failure for something drawn every render.*/
	const char *model = jstr(jget(jget(d, "model"), "display_name"));
	if (model && *model) {
		const char *tags[2];
		int nt = 0;
		if (jtruthy(jget(d, "fast_mode")))
			tags[nt++] = "fast";
		/*str(level). A container would need Python's repr() to match, which is
		out of proportion for a field the payload sends as a string, so only
		scalars are rendered and a list or dict shows no tag.*/
		jval *lvl = jget(jget(d, "effort"), "level");
		const char *lvls = NULL;   /*one branch assigns a string literal*/
		if (lvl->type == JSTR && lvl->str[0])
			lvls = lvl->str;
		else if (lvl->type == JNUM)
			lvls = lvl->is_int ? ssprintf("%lld", (long long)lvl->num)
			                   : pystr_double(lvl->num);
		else if (lvl->type == JBOOL && lvl->truth)
			lvls = "True";   /*str(True); false is falsy and tags nothing*/
		if (lvls)
			tags[nt++] = lvls;

		char *label;
		if (nt == 2)
			label = ssprintf("%s %s/%s", model, tags[0], tags[1]);
		else if (nt == 1)
			label = ssprintf("%s %s", model, tags[0]);
		else
			label = ssprintf("%s", model);
		parts[np++] = col(36, label);
	}

	jval *cw = jget(d, "context_window");
	double used, size;
	if (jnum(jget(cw, "total_input_tokens"), &used)
	    && jnum(jget(cw, "context_window_size"), &size) && size != 0) {
		double p = used / size * 100;
		char *b = bar(p);
		char *ppct = col(colour_for(p), ssprintf("%.0f%%", p));
		char *counts = col(90, ssprintf("%s/%s", human((long long)used), human((long long)size)));
		parts[np++] = ssprintf("%s %s %s", b, ppct, counts);
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
		char *left = resets_in(jget(v, "resets_at"));
		char *head = col(colour_for(p), ssprintf("%s %.0f%%", shorts[i], p));
		parts[np++] = left ? ssprintf("%s%s", head, col(90, ssprintf("↺%s", left)))
		                   : head;
	}

	double cost;
	if (jnum(jget(jget(d, "cost"), "total_cost_usd"), &cost) && cost > 0)
		parts[np++] = col(90, ssprintf("$%.2f", cost));

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
		parts[np++] = col(35, ssprintf("%.*s", (int)(len - start), cwd + start));
	}

	char *sep = col(90, " │ ");   // │
	for (int i = 0; i < np; i++) {
		if (i)
			fputs(sep, stdout);
		fputs(parts[i], stdout);
	}
	putchar('\n');
	return 0;
}

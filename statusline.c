/*Claude Code status line: context window + usage.

Reads the status-line JSON payload on stdin and prints one ANSI-coloured line.
Payload shape taken from the 2.1.276 binary (function q$o/xnt): keys used here
are model.display_name, workspace.current_dir, context_window.{total_input_tokens,
context_window_size}, cost.total_cost_usd, and rate_limits.{five_hour,seven_day,
spend_limit}.{used_percentage,resets_at}.

A translation of statusline.py, which stays the readable reference; behaviour is
meant to match it field for field, and statusline-test.sh checks that it does.
Build and install with:
	make && make install
Re-check after a Claude Code upgrade with:
	make test
and with `echo '{}' | ~/.claude/statusline` against /context and /usage
in-session.

statusline-nodeps.c is the same program with a hand-rolled parser and no
dependency beyond libc, for a machine without json-c.

Parsing is json-c 0.17 in JSON_TOKENER_STRICT mode, which is what makes it
reject the trailing garbage and leading zeros that Python's json module also
rejects; the default mode accepts both. Parsed objects are never freed: the
process reads one small payload, prints a line and exits.

Four inputs render differently from the Python, all of them malformed and none
of them emitted by the binary. statusline-test.sh leaves them out because it
compares for equality; they were measured, not assumed:

  non-string display_name, and any non-object document ("[]", "42") -- the
    Python raises and prints a traceback where its status line should be. A
    blank segment is the better failure for something drawn every render, so
    that is deliberate rather than a gap.
  a lone UTF-16 surrogate in a string ("\ud83d") -- json-c substitutes U+FFFD;
    Python builds the surrogate and then dies encoding it to stdout.
  single-quoted object keys ("{'cost':{...}}") -- json-c accepts them even
    under STRICT, where Python's json and statusline-nodeps.c both reject. This
    one is a laxity the library brings in, not a choice.*/

#define _DEFAULT_SOURCE   /*for timegm(), which is not in C11*/

#include <ctype.h>
#include <inttypes.h>
#include <json-c/json.h>
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

/*Growable string, for the stdin slurp and for building runs of bar cells.*/
typedef struct {
	char *p;
	size_t len, cap;
} sbuf;

static void sput(sbuf *b, const char *s, size_t n)
{
	if (b->len + n + 1 > b->cap) {
		size_t cap = b->cap ? b->cap : 64;
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

/*Python accessors over json-c*/

/*dict.get(): NULL for a missing key or a non-dict, so chains need no guards.*/
static struct json_object *jget(struct json_object *o, const char *key)
{
	struct json_object *v = NULL;
	if (!o || json_object_get_type(o) != json_type_object)
		return NULL;
	json_object_object_get_ex(o, key, &v);
	return v;
}

/*Python truthiness: None, False, 0, "", [] and {} are falsy.*/
static int jtruthy(struct json_object *o)
{
	switch (json_object_get_type(o)) {
	case json_type_null:    return 0;
	case json_type_boolean: return json_object_get_boolean(o);
	case json_type_int:
	case json_type_double:  return json_object_get_double(o) != 0;
	case json_type_string:  return json_object_get_string_len(o) > 0;
	case json_type_array:   return json_object_array_length(o) > 0;
	case json_type_object:  return json_object_object_length(o) > 0;
	}
	return 0;
}

/*isinstance(x, (int, float)).

A JSON true/false counts, because Python's bool is a subclass of int: the
isinstance() guards in the Python accept True and go on to divide by it.*/
static int jnum(struct json_object *o, double *out)
{
	switch (json_object_get_type(o)) {
	case json_type_int:
	case json_type_double:
		*out = json_object_get_double(o);
		return 1;
	case json_type_boolean:
		*out = json_object_get_boolean(o) ? 1.0 : 0.0;
		return 1;
	default:
		return 0;
	}
}

/*A JSON string, or NULL for any other type.

json_object_get_string() stringifies whatever it is given, which would make a
number satisfy the `if model:` test the Python applies to a str; the type check
keeps that from happening.*/
static const char *jstr(struct json_object *o)
{
	if (json_object_get_type(o) != json_type_string)
		return NULL;
	return json_object_get_string(o);
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

/*str() of a scalar. Containers return NULL: matching Python's repr() for a list
or dict is out of proportion for a field the payload sends as a string.*/
static char *pystr(struct json_object *o)
{
	switch (json_object_get_type(o)) {
	case json_type_string:  return ssprintf("%s", json_object_get_string(o));
	case json_type_int:     return ssprintf("%" PRId64, json_object_get_int64(o));
	case json_type_double:  return pystr_double(json_object_get_double(o));
	case json_type_boolean: return json_object_get_boolean(o) ? ssprintf("True")
	                                                          : ssprintf("False");
	default:                return NULL;
	}
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
static int pct(struct json_object *o, double *out)
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
	/*rint() rounds half to even under the default mode, as Python's round() does.
	A NaN percentage casts to an unspecified int, so it is pinned to empty here.*/
	int filled = isnan(p) ? 0 : (int)rint(p / 100 * width);
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

	return ssprintf("%s%s", col(colour_for(p), f.p), col(90, e.p));
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
static char *resets_in(struct json_object *o)
{
	const char *ts = jstr(o);
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
	sbuf in = { NULL, 0, 0 };
	sput(&in, "", 0);
	char chunk[4096];
	size_t got;
	while ((got = fread(chunk, 1, sizeof chunk, stdin)) > 0)
		sput(&in, chunk, got);

	struct json_tokener *tok = json_tokener_new();
	if (!tok)
		exit(1);
	json_tokener_set_flags(tok, JSON_TOKENER_STRICT);
	struct json_object *d = json_tokener_parse_ex(tok, in.p, (int)in.len);
	/*A truncated document leaves the tokener asking for more; the Python's
	except JSONDecodeError treats that as an empty payload, so NULL is right.*/
	if (json_tokener_get_error(tok) != json_tokener_success)
		d = NULL;

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
		struct json_object *lvl = jget(jget(d, "effort"), "level");
		if (jtruthy(lvl)) {
			char *s = pystr(lvl);
			if (s)
				tags[nt++] = s;
		}

		char *label;
		if (nt == 2)
			label = ssprintf("%s %s/%s", model, tags[0], tags[1]);
		else if (nt == 1)
			label = ssprintf("%s %s", model, tags[0]);
		else
			label = ssprintf("%s", model);
		parts[np++] = col(36, label);
	}

	struct json_object *cw = jget(d, "context_window");
	double used, size;
	if (jnum(jget(cw, "total_input_tokens"), &used)
	    && jnum(jget(cw, "context_window_size"), &size) && size != 0) {
		double p = used / size * 100;
		char *ppct = col(colour_for(p), ssprintf("%.0f%%", p));
		char *counts = col(90, ssprintf("%s/%s", human((long long)used),
		                                         human((long long)size)));
		parts[np++] = ssprintf("%s %s %s", bar(p), ppct, counts);
	}

	struct json_object *rl = jget(d, "rate_limits");
	static const char *keys[] = { "five_hour", "seven_day", "spend_limit" };
	static const char *shorts[] = { "5h", "7d", "spend" };
	for (int i = 0; i < 3; i++) {
		struct json_object *v = jget(rl, keys[i]);
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

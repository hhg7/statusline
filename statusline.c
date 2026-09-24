/*Claude Code status line: context window + usage.

Reads the status-line JSON payload on stdin and prints one ANSI-coloured line.
Payload shape taken from the 2.1.276 binary (function q$o/xnt): keys used here
are model.display_name, workspace.current_dir, context_window.{total_input_tokens,
context_window_size}, cost.total_cost_usd, and rate_limits.{five_hour,seven_day,
spend_limit}.{used_percentage,resets_at}. In 2.1.281 (function jjn) resets_at is
the rate-limit window's own resets_at, a Unix time in seconds; an ISO-8601
string is still accepted.

The 7d segment carries a ration bar, after ~/Scripts/C/token.rationing.c: the
weekly quota resets Saturday 07:00 America/Chicago, per the account's stated
weekly-limit reset, and an even burn would by now have spent the fraction of the
window already elapsed. The bar shows consumption against that on-pace budget,
green with ration to spare and red once it is over-used.

A translation of statusline.py, which stays the readable reference; behaviour is
meant to match it field for field, and statusline-test.sh checks that it does.
STATUSLINE_NOW, a Unix time, stands in for the clock in both, so that the test
compares the two at the same instant.
Build and install with:
	make && make install
Re-check after a Claude Code upgrade with:
	make test
and with `echo '{}' | ~/.claude/statusline` against /context and /usage
in-session. `make valgrind` checks that every allocation is freed.

statusline-nodeps.c is the same program with a hand-rolled parser and no
dependency beyond libc, for a machine without json-c.

Parsing is json-c 0.17 in JSON_TOKENER_STRICT mode, which is what makes it
reject the trailing garbage and leading zeros that Python's json module also
rejects; the default mode accepts both.

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

#define _DEFAULT_SOURCE   /*for timegm() and tm_gmtoff, which are not in C11*/

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

static void sput(sbuf *b, const char *s, size_t n)
{
	sreserve(b, n);
	memcpy(b->p + b->len, s, n);
	b->len += n;
	b->p[b->len] = '\0';
}

static void sputs(sbuf *b, const char *s)
{
	sput(b, s, strlen(s));
}

/*printf onto the end of b. No argument may point into b itself: the buffer can
move when it grows.*/
static void sappf(sbuf *b, const char *fmt, ...)
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

/*Append str() of a scalar; 0, appending nothing, for a container. Matching
Python's repr() for a list or dict is out of proportion for a field the payload
sends as a string.*/
static int sput_pystr(sbuf *b, struct json_object *o)
{
	switch (json_object_get_type(o)) {
	case json_type_string:  sputs(b, json_object_get_string(o)); return 1;
	case json_type_int:     sappf(b, "%" PRId64, json_object_get_int64(o)); return 1;
	case json_type_double:  sput_pyfloat(b, json_object_get_double(o)); return 1;
	case json_type_boolean: sputs(b, json_object_get_boolean(o) ? "True" : "False"); return 1;
	default:                return 0;
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

/*Consumption against the on-pace budget, both as percentages.

Under or exactly on pace, the consumed cells are green and the cells up to the
budget are bold green: ration to spare. Over it, the cells up to the budget are
red and the ones past it bold red: ration over-used. Exactly on pace counts as
under, so an untouched quota is never red. cells() is monotonic, so u >= s
exactly when used > sched and neither run below goes negative.*/
static void pace_bar(sbuf *b, double used, double sched)
{
	int u = cells(used), s = cells(sched);
	if (used > sched) {
		srun(b, "31", BAR_FULL, s);
		srun(b, "1;31", BAR_FULL, u - s);
		srun(b, "90", BAR_EMPTY, BAR_WIDTH - u);
	} else {
		srun(b, "32", BAR_FULL, u);
		srun(b, "1;32", BAR_EMPTY, s - u);
		srun(b, "90", BAR_EMPTY, BAR_WIDTH - s);
	}
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
static int reset_time(struct json_object *o, double *out)
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

	struct json_tokener *tok = json_tokener_new();
	if (!tok)
		exit(1);
	json_tokener_set_flags(tok, JSON_TOKENER_STRICT);
	struct json_object *d = json_tokener_parse_ex(tok, in.p, (int)in.len);
	/*A truncated document leaves the tokener asking for more; the Python's
	except JSONDecodeError treats that as an empty payload, so NULL is right.
	json_object_put() is a no-op on NULL.*/
	if (json_tokener_get_error(tok) != json_tokener_success) {
		json_object_put(d);
		d = NULL;
	}
	json_tokener_free(tok);
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
		struct json_object *lvl = jget(jget(d, "effort"), "level");
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

	struct json_object *cw = jget(d, "context_window");
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
		const char *hue = colour_for(p);
		double end = 0, sched = 0;   /*set only on success; zeroed for -Wmaybe-uninitialized*/
		int have_end = reset_time(jget(v, "resets_at"), &end);

		part(&out, &np);
		/*Only the weekly window is rationed; a NaN or infinite percentage has
		no place on a bar, and the Python's round() raises on one.*/
		if (tz_ok && have_end && strcmp(keys[i], "seven_day") == 0 && isfinite(p)
		    && week_elapsed(end, now, &sched)) {
			scol(&out, hue, shorts[i]);
			sputs(&out, " ");
			pace_bar(&out, p, sched);
			sputs(&out, " ");
			sopen(&out, hue);
			sappf(&out, "%.0f%%" RESET, p);
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
	json_object_put(d);
	return 0;
}

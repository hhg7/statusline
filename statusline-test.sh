#!/bin/bash
# Differential test: statusline (C) against statusline.py, which is the reference.
#
# Compares stdout byte for byte over curated payloads, malformed input, every
# ISO-8601 form datetime.fromisoformat() was observed to accept or reject on
# CPython 3.14.2, randomised payloads, and (via statusline-test-floats.py) the
# str(float) rendering across the exponent range where CPython switches between
# fixed and scientific notation. Python's stderr is dropped: on a few
# inputs it raises where the C prints a blank segment, and those divergences are
# deliberate and recorded in statusline.c.
#
# Usage: ./statusline-test.sh [fuzz-count]        (default 3000)
#        STATUSLINE_BIN=./other ./statusline-test.sh
#
# STATUSLINE_BIN picks which build to check; it defaults to ./statusline, the
# json-c one. Point it at a build of statusline-nodeps.c to check that fallback,
# which is not otherwise covered.
set -u
cd "$(dirname "$0")" || exit 1
PY=./statusline.py
C=${STATUSLINE_BIN:-./statusline}
FUZZ=${1:-3000}
tot=0; bad=0

check() {   # check <json>
	local a b
	a=$(printf '%s' "$1" | $PY 2>/dev/null)
	b=$(printf '%s' "$1" | $C 2>/dev/null)
	tot=$((tot + 1))
	if [ "$a" != "$b" ]; then
		bad=$((bad + 1))
		printf 'MISMATCH %s\n   py: %s\n   c : %s\n' "$1" "$(printf '%s' "$a" | cat -v)" "$(printf '%s' "$b" | cat -v)"
	fi
}

while IFS= read -r line; do
	[ -n "$line" ] && check "$line"
done < <(python3 statusline-test-cases.py)

# Resets a whole number of seconds away, landing on a half-hour or half-minute,
# stamped immediately before each run. The true interval is then a few
# milliseconds under x.5, which Python rounds down; a C that read the clock in
# whole seconds saw exactly x.5 whenever it ran in the stamp's second and
# rounded half to even, so "11h" came out "12h". Generated here rather than in
# statusline-test-cases.py because those are all stamped up front, and are a
# second stale by the time they run.
for i in 1 2 3 4 5; do
	for off in 41400 9000 1770; do   # 11.5h, 2.5h, 29.5m
		ts=$(date -u -d "@$(( $(date +%s) + off ))" +%Y-%m-%dT%H:%M:%SZ)
		check '{"rate_limits":{"five_hour":{"used_percentage":5,"resets_at":"'"$ts"'"}}}'
	done
done

STATUSLINE_BIN=$C python3 statusline-test-fuzz.py "$FUZZ" || bad=$((bad + 1))
STATUSLINE_BIN=$C python3 statusline-test-floats.py || bad=$((bad + 1))

printf '%s: %d curated cases, %d mismatches (plus the %d-payload fuzz above)\n' "$C" "$tot" "$bad" "$FUZZ"
[ "$bad" -eq 0 ]

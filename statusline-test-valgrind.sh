#!/bin/bash
# Memory check: every curated payload and malformed document through valgrind.
#
# Fails on any leak of any kind -- definite, indirect, possible, and still
# reachable -- and on any invalid read, write or use of uninitialised memory.
# "Still reachable" counts too because both builds free everything before
# exiting: 0 bytes in use at exit was measured under valgrind 3.22 with glibc's
# setenv("TZ") and tzset() in the path, so anything left over is this program's.
#
# Usage: ./statusline-test-valgrind.sh              (checks ./statusline)
#        STATUSLINE_BIN=./statusline-nodeps ./statusline-test-valgrind.sh
#
# The curated payloads run with the rate-limit cache off, so they do not touch
# ~/.cache; the last three runs write, re-read and borrow from a temporary one.
set -u
cd "$(dirname "$0")" || exit 1
C=${STATUSLINE_BIN:-./statusline}
export STATUSLINE_CACHE=
export STATUSLINE_NOW=${STATUSLINE_NOW:-$(python3 -c 'import time; print(repr(time.time()))')}
command -v valgrind >/dev/null || { echo "valgrind not installed" >&2; exit 1; }
tot=0; bad=0

while IFS= read -r line; do
	[ -n "$line" ] || continue
	tot=$((tot + 1))
	if ! log=$(printf '%s' "$line" | valgrind -q --leak-check=full --show-leak-kinds=all \
	           --errors-for-leak-kinds=all --error-exitcode=99 "$C" 2>&1 >/dev/null); then
		bad=$((bad + 1))
		[ "$bad" -le 5 ] && printf 'VALGRIND %s\n%s\n' "$line" "$log"
	fi
done < <(python3 statusline-test-cases.py; printf '%s\n' '{"a":[1,{"b":"x' '{"k":"\q"}' '{"a"' '{"a":}')

tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT
live='{"rate_limits":{"seven_day":{"used_percentage":10,"resets_at":'$((${STATUSLINE_NOW%.*} + 86400))'}}}'   # a day out
for line in "$live" "$live" '{}'; do   # write, find unchanged, borrow
	tot=$((tot + 1))
	if ! log=$(printf '%s' "$line" | STATUSLINE_CACHE=$tmp/c.json valgrind -q --leak-check=full \
	           --show-leak-kinds=all --errors-for-leak-kinds=all --error-exitcode=99 "$C" 2>&1 >/dev/null); then
		bad=$((bad + 1))
		printf 'VALGRIND (cache) %s\n%s\n' "$line" "$log"
	fi
done

printf '%s: %d payloads under valgrind, %d with errors or leaks\n' "$C" "$tot" "$bad"
[ "$bad" -eq 0 ]

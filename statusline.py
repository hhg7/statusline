#!/usr/bin/env python3
"""Claude Code status line: context window + usage.

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

A fresh session's payload carries no rate_limits until the first API reply
comes back, so the usage segments would be missing at startup. Every payload
that does carry them is saved whole to a cache file, and a payload without them
borrows the cached rate_limits instead. A cached window whose reset has passed,
or that has no reset time to age it by, is left out: its percentage no longer
describes anything. The file is $STATUSLINE_CACHE, else
$XDG_CACHE_HOME/claude-statusline.json, else ~/.cache/claude-statusline.json;
STATUSLINE_CACHE set but empty turns the cache off, which the tests rely on.

STATUSLINE_NOW, a Unix time, stands in for the clock; statusline-test.sh sets it
so that this and the C read the same instant.

Re-check after a Claude Code upgrade with:
    echo '{}' | ~/.claude/statusline.py
and against /context and /usage in-session."""

import json
import math
import os
import sys
import time
from datetime import datetime, timezone
from zoneinfo import ZoneInfo

R = "\033[0m"

# The reset is Chicago wall-clock, so a window across a DST change is 167 or 169
# hours rather than 168; see token.rationing.c for the measurements.
RESET_TZ = ZoneInfo("America/Chicago")
WEEK = 7 * 86400

NOW = float(os.environ["STATUSLINE_NOW"]) if "STATUSLINE_NOW" in os.environ else time.time()


def c(code, s):
	return f"\033[{code}m{s}{R}"


def pct(v):
	"""Normalise a percentage field to 0-100.

	rate_limits.*.used_percentage already arrives as a percentage, so this only
	coerces the type. An earlier version guessed, scaling anything at or below
	1.0 on the theory it might be a utilisation fraction; that turned a genuine
	1% into a displayed 100%, seen 2026-09-18 with the bar reading "5h 100%"
	while /usage reported 2%. A fraction-encoded 2% would have arrived as 0.02
	and displayed correctly, so the fraction case does not occur -- the guess
	could only ever misfire, never help."""
	try:
		return float(v)
	except (TypeError, ValueError):
		return None


def colour_for(p):
	"""Green under 60%, yellow from 60%, red from 80%."""
	return 32 if p < 60 else (33 if p < 80 else 31)


def cells(p, width):
	return max(0, min(width, int(round(p / 100 * width))))


def bar(p, width=10):
	filled = cells(p, width)
	return c(colour_for(p), "▰" * filled) + c(90, "▱" * (width - filled))


def human(n):
	if n >= 1_000_000:
		return f"{n / 1_000_000:.1f}M"
	if n >= 1_000:
		return f"{n / 1_000:.0f}k"
	return str(n)


def reset_time(ts):
	"""Unix time of a resets_at field, or None if it holds no usable time.

	A number is a Unix time already. A string is ISO-8601, and a naive one is
	read as UTC."""
	if isinstance(ts, (int, float)):
		try:
			t = float(ts)
		except OverflowError:   # an int past the double range
			return None
		return t if math.isfinite(t) else None
	if not ts:
		return None
	try:
		t = datetime.fromisoformat(str(ts).replace("Z", "+00:00"))
	except ValueError:
		return None
	if t.tzinfo is None:
		t = t.replace(tzinfo=timezone.utc)
	return t.timestamp()


def resets_in(end):
	"""Hours or minutes until a reset, or None once it has passed."""
	h = (end - NOW) / 3600
	if h <= 0:
		return None
	return f"{h:.0f}h" if h >= 1 else f"{h * 60:.0f}m"


def utcoffset(t):
	return datetime.fromtimestamp(t, RESET_TZ).utcoffset().total_seconds()


def week_elapsed(end):
	"""Percent of the weekly window ending at `end` that has elapsed, or None.

	The window opens at the same Chicago wall-clock time a week earlier: a week
	of seconds back, then corrected by however far the zone's offset moved in
	between. That is exact for a 07:00 anchor, which is never within an hour of
	a Chicago transition."""
	if end <= NOW:
		return None
	try:
		start = end - WEEK + utcoffset(end) - utcoffset(end - WEEK)
	except (OverflowError, ValueError, OSError):
		return None
	return max(0.0, min(100.0, (NOW - start) / (end - start) * 100))


def cache_path():
	"""Where the last payload with rate_limits is kept, or None for no cache."""
	if "STATUSLINE_CACHE" in os.environ:
		return os.environ["STATUSLINE_CACHE"] or None
	base = os.environ.get("XDG_CACHE_HOME", "")
	if not base.startswith("/"):   # the XDG spec says a relative value is ignored
		home = os.environ.get("HOME")
		if not home:
			return None
		base = home + "/.cache"
	return base + "/claude-statusline.json"


def cache_read(path):
	"""The rate_limits of the cached payload, or {} if there is none."""
	try:
		with open(path, encoding="utf-8") as f:
			d = json.load(f)
	except (OSError, ValueError):
		return {}
	rl = d.get("rate_limits") if isinstance(d, dict) else None
	return rl if isinstance(rl, dict) else {}


def cache_write(path, text):
	"""Replace the cache with text, through a rename so that a concurrent
	session never reads half a file. Unchanged contents are not rewritten, since
	this runs on every render. Failure is silent: the cache is a convenience."""
	try:
		with open(path, encoding="utf-8") as f:
			if f.read() == text:
				return
	except (OSError, ValueError):
		pass
	tmp = f"{path}.{os.getpid()}.tmp"
	try:
		fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
		with os.fdopen(fd, "w", encoding="utf-8") as f:
			f.write(text)
		os.rename(tmp, path)
	except OSError:
		try:
			os.unlink(tmp)
		except OSError:
			pass


def main():
	text = None
	try:
		text = sys.stdin.read()
		d = json.loads(text)
	except (json.JSONDecodeError, ValueError):
		d = {}

	parts = []

	model = (d.get("model") or {}).get("display_name")
	if model:
		tags = []
		if d.get("fast_mode"):
			tags.append("fast")
		lvl = (d.get("effort") or {}).get("level")
		if lvl:
			tags.append(str(lvl))
		label = model + (" " + "/".join(tags) if tags else "")
		parts.append(c(36, label))

	cw = d.get("context_window") or {}
	used = cw.get("total_input_tokens")
	size = cw.get("context_window_size")
	if isinstance(used, (int, float)) and isinstance(size, (int, float)) and size:
		p = used / size * 100
		parts.append(f"{bar(p)} {c(colour_for(p), f'{p:.0f}%')} {c(90, f'{human(int(used))}/{human(int(size))}')}")

	rl = d.get("rate_limits") or {}
	cached = False
	path = cache_path()
	if path:
		if isinstance(rl, dict) and rl:
			cache_write(path, text)
		elif not rl:
			rl = cache_read(path)
			cached = True
	for key, short in (("five_hour", "5h"), ("seven_day", "7d"), ("spend_limit", "spend")):
		v = rl.get(key)
		if not v:
			continue
		p = pct(v.get("used_percentage"))
		if p is None:
			continue
		end = reset_time(v.get("resets_at"))
		if cached and (end is None or end <= NOW):
			continue
		left = resets_in(end) if end is not None else None
		sched = week_elapsed(end) if key == "seven_day" and end is not None else None
		tail = c(90, f"↺{left}") if left else ""
		if sched is not None and math.isfinite(p):
			# Exactly on pace counts as under, so an untouched quota is never red.
			# The colour is decided before rounding, so 15.04 against 14.96 is red
			# although both print as 15.0.
			parts.append(c(31 if p > sched else 32, f"{short} {p:.1f}%/{sched:.1f}%") + tail)
		else:
			parts.append(c(colour_for(p), f"{short} {p:.0f}%") + tail)

	cost = (d.get("cost") or {}).get("total_cost_usd")
	if isinstance(cost, (int, float)) and cost > 0:
		parts.append(c(90, f"${cost:.2f}"))

	cwd = (d.get("workspace") or {}).get("current_dir") or d.get("cwd")
	if cwd:
		parts.append(c(35, cwd.rstrip("/").rsplit("/", 1)[-1]))

	print(c(90, " │ ").join(parts) if parts else "")


main()

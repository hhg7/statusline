#!/usr/bin/env python3
"""Claude Code status line: context window + usage.

Reads the status-line JSON payload on stdin and prints one ANSI-coloured line.
Payload shape taken from the 2.1.276 binary (function q$o/xnt): keys used here
are model.display_name, workspace.current_dir, context_window.{total_input_tokens,
context_window_size}, cost.total_cost_usd, and rate_limits.{five_hour,seven_day,
spend_limit}.{used_percentage,resets_at}.

Re-check after a Claude Code upgrade with:
    echo '{}' | ~/.claude/statusline.py
and against /context and /usage in-session."""

import json
import sys
from datetime import datetime, timezone

R = "\033[0m"


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


def bar(p, width=10):
	filled = int(round(p / 100 * width))
	filled = max(0, min(width, filled))
	return c(colour_for(p), "▰" * filled) + c(90, "▱" * (width - filled))


def human(n):
	if n >= 1_000_000:
		return f"{n / 1_000_000:.1f}M"
	if n >= 1_000:
		return f"{n / 1_000:.0f}k"
	return str(n)


def resets_in(ts):
	"""Hours until an ISO-8601 reset timestamp, or None if unparseable."""
	if not ts:
		return None
	try:
		t = datetime.fromisoformat(str(ts).replace("Z", "+00:00"))
	except ValueError:
		return None
	if t.tzinfo is None:
		t = t.replace(tzinfo=timezone.utc)
	h = (t - datetime.now(timezone.utc)).total_seconds() / 3600
	if h <= 0:
		return None
	return f"{h:.0f}h" if h >= 1 else f"{h * 60:.0f}m"


def main():
	try:
		d = json.load(sys.stdin)
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
	for key, short in (("five_hour", "5h"), ("seven_day", "7d"), ("spend_limit", "spend")):
		v = rl.get(key)
		if not v:
			continue
		p = pct(v.get("used_percentage"))
		if p is None:
			continue
		left = resets_in(v.get("resets_at"))
		parts.append(c(colour_for(p), f"{short} {p:.0f}%") + (c(90, f"↺{left}") if left else ""))

	cost = (d.get("cost") or {}).get("total_cost_usd")
	if isinstance(cost, (int, float)) and cost > 0:
		parts.append(c(90, f"${cost:.2f}"))

	cwd = (d.get("workspace") or {}).get("current_dir") or d.get("cwd")
	if cwd:
		parts.append(c(35, cwd.rstrip("/").rsplit("/", 1)[-1]))

	print(c(90, " │ ").join(parts) if parts else "")


main()

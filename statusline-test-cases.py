#!/usr/bin/env python3
"""Curated payloads for statusline-test.sh, one JSON document per line.

Covers every field the status line reads, the empty/zero/overflow edges of
each, the rounding boundaries in bar()/human()/pct(), malformed and non-object
documents, and the ISO-8601 forms CPython 3.12.3 was observed to accept or
reject. Timestamps are generated relative to now -- STATUSLINE_NOW when
statusline-test.sh sets it, which is also the clock both builds then read -- so
cases stay meaningful."""

import json
import os
import time
from datetime import datetime, timezone, timedelta

NOW = float(os.environ.get("STATUSLINE_NOW", time.time()))
now = datetime.fromtimestamp(NOW, timezone.utc)


def iso(h):
	return (now + timedelta(hours=h)).strftime("%Y-%m-%dT%H:%M:%SZ")


def rl(**kw):
	return {"rate_limits": {"five_hour": kw}}


def wk(used, h):
	"""A seven_day window resetting h hours from now, as a Unix time."""
	return {"rate_limits": {"seven_day": {"used_percentage": used, "resets_at": NOW + h * 3600}}}


cases = [
	{},
	# model label and its tags
	{"model": {"display_name": "Opus 5"}},
	{"model": {"display_name": "Opus 5"}, "fast_mode": True, "effort": {"level": "high"}},
	{"model": {"display_name": "Opus 5"}, "effort": {"level": "high"}},
	{"model": {"display_name": "Opus 5"}, "fast_mode": True},
	{"model": {"display_name": "Opus 5"}, "fast_mode": False, "effort": {"level": 3}},
	# bool is a subclass of int in Python, so str(True) tags and True/n divides
	{"model": {"display_name": "X"}, "effort": {"level": True}},
	{"model": {"display_name": "X"}, "effort": {"level": False}},
	{"model": {"display_name": "X"}, "effort": {"level": 3.0}},
	{"model": {"display_name": "X"}, "effort": {"level": 3.5}},
	{"model": {"display_name": "X"}, "effort": {"level": 0.1}},
	{"model": {"display_name": "X"}, "effort": {"level": None}},
	{"model": {"display_name": "X"}, "effort": {"level": ""}},
	{"model": {"display_name": ""}},
	{"model": {"display_name": None}},
	{"model": {}},
	# context window, including the bar() half-cell rounding boundaries
	{"context_window": {"total_input_tokens": 0, "context_window_size": 200000}},
	{"context_window": {"total_input_tokens": 45123, "context_window_size": 200000}},
	{"context_window": {"total_input_tokens": 125000, "context_window_size": 200000}},
	{"context_window": {"total_input_tokens": 175000, "context_window_size": 200000}},
	{"context_window": {"total_input_tokens": 200000, "context_window_size": 200000}},
	{"context_window": {"total_input_tokens": 1500000, "context_window_size": 1000000}},
	{"context_window": {"total_input_tokens": 999, "context_window_size": 1000000}},
	{"context_window": {"total_input_tokens": 1000, "context_window_size": 1000000}},
	{"context_window": {"total_input_tokens": 999999, "context_window_size": 1000000}},
	{"context_window": {"total_input_tokens": 1000000, "context_window_size": 1000000}},
	{"context_window": {"total_input_tokens": 950, "context_window_size": 0}},
	{"context_window": {"total_input_tokens": "x", "context_window_size": 200000}},
	{"context_window": {"total_input_tokens": True, "context_window_size": 100}},
	{"context_window": {"total_input_tokens": 5, "context_window_size": 100}},
	{"context_window": {"total_input_tokens": 15, "context_window_size": 100}},
	{"context_window": {"total_input_tokens": 25, "context_window_size": 100}},
	{"context_window": None},
	# rate limits: fraction vs percentage, colour thresholds, reset formatting
	rl(used_percentage=0.42, resets_at=iso(3.4)),
	rl(used_percentage=42, resets_at=iso(0.7)),
	rl(used_percentage="61.5", resets_at=iso(-2)),
	rl(used_percentage=True),
	rl(used_percentage=None),
	rl(used_percentage=59.4), rl(used_percentage=59.6),
	rl(used_percentage=79.4), rl(used_percentage=79.6),
	{"rate_limits": {"five_hour": {"used_percentage": 85},
	                 "seven_day": {"used_percentage": 12.6, "resets_at": iso(70)},
	                 "spend_limit": {"used_percentage": 1.0}}},
	{"rate_limits": {"five_hour": {}, "seven_day": None,
	                 "spend_limit": {"used_percentage": None}}},
	# resets_at as the binary sends it, a Unix time in seconds
	rl(used_percentage=42, resets_at=int(NOW) + 7200),
	rl(used_percentage=42, resets_at=NOW + 1800.5),
	rl(used_percentage=42, resets_at=int(NOW) - 60),
	rl(used_percentage=42, resets_at=0),
	rl(used_percentage=42, resets_at=True),
	rl(used_percentage=42, resets_at=1e300),
	rl(used_percentage=42, resets_at=[1]),
	# the 7d ration bar: under, on and over pace, early and late in the week
	wk(0, 167.9), wk(0, 84), wk(0, 0.1), wk(50, 84), wk(49, 84), wk(51, 84),
	wk(5, 150), wk(30, 150), wk(90, 150), wk(5, 50), wk(30, 50), wk(90, 50),
	wk(100, 1), wk(120, 1), wk(-5, 100), wk(35.7, 100), wk(45.9, 100),
	wk("61.5", 60), wk("nan", 60), wk("inf", 60), wk(True, 60), wk(1e300, 60),
	# a reset more than a week out, so the window has not opened yet
	wk(10, 200), wk(10, 10000),
	# past the year 9999, where Python's datetime gives up
	{"rate_limits": {"seven_day": {"used_percentage": 10, "resets_at": 253402300800}}},
	{"rate_limits": {"seven_day": {"used_percentage": 10, "resets_at": 253402300799}}},
	{"rate_limits": {"seven_day": {"used_percentage": 10, "resets_at": 1e12}}},
	{"rate_limits": {"seven_day": {"used_percentage": 10, "resets_at": iso(70)}}},
	{"rate_limits": {"seven_day": {"used_percentage": 10, "resets_at": iso(-1)}}},
	{"rate_limits": {"seven_day": {"used_percentage": 10}}},
	# cost and cwd
	{"cost": {"total_cost_usd": 0}},
	{"cost": {"total_cost_usd": 12.3456}},
	{"cost": {"total_cost_usd": -1}},
	{"cost": {"total_cost_usd": True}},
	{"workspace": {"current_dir": "/home/con/.claude/"}},
	{"workspace": {"current_dir": "/"}},
	{"workspace": {"current_dir": "relative"}},
	{"workspace": {"current_dir": "/p/q//"}},
	{"cwd": "/home/con/Scripts/stats"},
	{"workspace": {}, "cwd": "/tmp/fallback"},
	{"workspace": {"current_dir": "/home/con/café – dir/"}},
	# the full shape, as the binary sends it
	{"model": {"display_name": "Opus 5 (1M context)"}, "fast_mode": True,
	 "effort": {"level": "high"},
	 "context_window": {"total_input_tokens": 312455, "context_window_size": 1000000},
	 "rate_limits": {"five_hour": {"used_percentage": 0.631, "resets_at": iso(2.2)},
	                 "seven_day": {"used_percentage": 0.08, "resets_at": iso(96)},
	                 "spend_limit": {"used_percentage": 0.955, "resets_at": iso(400)}},
	 "cost": {"total_cost_usd": 4.7}, "workspace": {"current_dir": "/home/con/.claude"}},
	{"model": {"display_name": "Opus 5.5"}, "effort": {"level": "high"},
	 "context_window": {"total_input_tokens": 62000, "context_window_size": 220000},
	 "rate_limits": {"five_hour": {"used_percentage": 41.0, "resets_at": int(NOW) + 10800},
	                 "seven_day": {"used_percentage": 38.2, "resets_at": int(NOW) + 200000}},
	 "cost": {"total_cost_usd": 1.84}, "workspace": {"current_dir": "/home/con/Scripts/C/statusline"}},
]

# ISO-8601 acceptance, checked against CPython 3.12.3 rather than the grammar
stamps = [
	"2026-13-99T99:99:99Z", "2026-02-30T00:00:00Z", "2026-02-29T00:00:00Z",
	"2028-02-29T00:00:00Z", "2026-00-10T00:00:00Z", "2026-01-00T00:00:00Z",
	"2026-01-32T00:00:00Z", "2026-01-10T24:00:00Z", "2026-01-10T23:60:00Z",
	"2026-01-10T23:59:60Z", "2027-1-1T00:00:00Z", " 2027-01-01T00:00:00Z",
	"+2027-01-01T00:00:00Z", "-2027-01-01", "2027-01-01", "2027-01-01T05:30",
	"2027-01-01T05", "2027-01-01T05:30:15", "2027-01-01T05:30:15.5Z",
	"2027-01-01T05:30:15.123456Z", "2027-01-01T05:30:15,123Z",
	"2027-01-01T05:30:15.Z", "2027-01-01T05:30:15.", "2027-01-01T05:30:15.+02:00",
	"2027-01-01 05:30:15Z", "2027-01-01t05:30:15Z", "2027-01-01t05:30:15z",
	"2027-01-01T05:30:15z", "2027-01-01T05:30:15+02:00", "2027-01-01T05:30:15-08:00",
	"2027-01-01T05:30:15+0200", "2027-01-01T05:30:15+02", "2027-01-01T05:30:15+02:00:30",
	"2027-01-01T05:30:15+24:00", "2027-01-01T05:30:15+23:59", "2027-01-01T05:30:15+02:60",
	"2027-01-01T05:30:15+23:60", "2027-01-01T05:30:15Zjunk", "2027-01-01T0530:15",
	"2027-01-01T05:3015", "20270101T053015", "20270101T05:30:15", "20270101",
	"20270101T0530", "2027-01-01T0530", "2027-0101", "202701-01", "2026-W01-1",
	"", "bogus", "9999-12-31T23:59:59Z", "1970-01-01T00:00:00Z", None,
] + [(now + timedelta(hours=x)).strftime("%Y-%m-%dT%H:%M:%SZ")
     for x in (0.4, 0.9, 1.0, 1.5, 2.5, 23.5, 168, 999)] \
  + [(now + timedelta(hours=x)).isoformat() for x in (0.51, 5, 100)] \
  + [(now + timedelta(hours=2)).strftime("%Y-%m-%dT%H:%M:%S") + f for f in
     (".1234567", ".9999999", ",25", ".000001", ".5+01:00")]

for s in stamps:
	cases.append(rl(used_percentage=50, resets_at=s))

for x in cases:
	print(json.dumps(x))

# Malformed and non-object documents, emitted raw rather than via json.dumps
for raw in ['   ', '{"model":{"display_name":"X"}', 'not json at all', '[]', '[1,2,3]',
            '"a string"', 'null', '42', '{"model":"notadict"}', '{"a":1,"a":2}',
            '{"a":01}', '{} trailing', '{}{}', '{"a":1e400}', '{"a":NaN}',
            '{"workspace":{"current_dir":"/a/b\\u00e9/c\\ud83d\\ude00d"}}',
            '{"model":{"display_name":"A\\"B\\\\C\\tD"}}', '{"model":{"display_name":5}}']:
	print(raw)

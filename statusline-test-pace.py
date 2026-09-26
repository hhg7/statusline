#!/usr/bin/env python3
"""Check the 7d ration against hand-worked windows, on fixed clocks.

The differential test shows the C and the Python agree; this shows they are
right, on the two weeks a year where a fixed 168-hour window would not be. Each
case pins STATUSLINE_NOW and a Saturday 07:00 America/Chicago reset, and the
cell counts below were worked by hand from the 2026-27 US transitions (first
Sunday of November, second Sunday of March), not taken from either program.
used is chosen to fall between the true on-pace budget and the one a 168-hour
window would give, so a window of the wrong length flips the colour as well as
printing the wrong budget.

  autumn  Sat 2026-10-31 07:00 CDT -> Sat 2026-11-07 07:00 CST, 169 h
          now Wed 2026-11-04 12:00Z, 96 h in: budget 56.80% (168 h: 56.55%)
          used 56.7 is under: green "56.7%/56.8%"
  spring  Sat 2027-03-13 07:00 CST -> Sat 2027-03-20 07:00 CDT, 167 h
          now Wed 2027-03-17 13:00Z, 96 h in: budget 57.49% (168 h: 57.74%)
          used 57.6 is over: red "57.6%/57.5%"
  plain   Sat 2026-09-19 07:00 CDT -> Sat 2026-09-26 07:00 CDT, 168 h
          now Thu 2026-09-24 12:00Z, 120 h in: budget 71.43%, printed 71.4%
          used 71.42 and 71.43 both print as 71.4% but fall either side of
          71.4286, so they are green and red: the colour is decided unrounded

Usage: statusline-test-pace.py            (checks statusline.py and STATUSLINE_BIN)"""
import json, os, subprocess, sys
from datetime import datetime, timezone

HERE = os.path.dirname(os.path.abspath(__file__))
C = os.environ.get("STATUSLINE_BIN") or os.path.join(HERE, "statusline")
if not os.path.isabs(C):
	C = os.path.join(HERE, C) if os.sep in C else C
builds = [("statusline.py", os.path.join(HERE, "statusline.py")), (os.path.basename(C), C)]

R = "\033[0m"


def utc(s):
	return datetime.fromisoformat(s).replace(tzinfo=timezone.utc).timestamp()


def seg(hue, used, sched, left):
	return f"\033[{hue}m7d {used}%/{sched}%{R}\033[90m↺{left}{R}\n"


cases = [   # now, resets_at, used, expected line
	("2026-11-04T12:00:00", "2026-11-07T13:00:00", 56.7, seg(32, "56.7", "56.8", "73h")),
	("2027-03-17T13:00:00", "2027-03-20T12:00:00", 57.6, seg(31, "57.6", "57.5", "71h")),
	("2026-09-24T12:00:00", "2026-09-26T12:00:00", 30, seg(32, "30.0", "71.4", "48h")),
	("2026-09-24T12:00:00", "2026-09-26T12:00:00", 90, seg(31, "90.0", "71.4", "48h")),
	("2026-09-24T12:00:00", "2026-09-26T12:00:00", 71.42, seg(32, "71.4", "71.4", "48h")),
	("2026-09-24T12:00:00", "2026-09-26T12:00:00", 71.43, seg(31, "71.4", "71.4", "48h")),
	("2026-09-19T12:00:00", "2026-09-26T12:00:00", 0, seg(32, "0.0", "0.0", "168h")),
]

bad = tested = 0
for label, exe in builds:
	for now, end, used, want in cases:
		p = json.dumps({"rate_limits": {"seven_day": {"used_percentage": used,
		                                              "resets_at": int(utc(end))}}})
		env = dict(os.environ, STATUSLINE_NOW=repr(utc(now)))
		got = subprocess.run([exe], input=p, capture_output=True, text=True, env=env).stdout
		tested += 1
		if got != want:
			bad += 1
			print(f"MISMATCH {label} now={now} end={end} used={used}\n"
			      f"   want: {want!r}\n   got : {got!r}")
print(f"{tested} fixed-clock rations across {len(builds)} builds, {bad} mismatches")
sys.exit(1 if bad else 0)

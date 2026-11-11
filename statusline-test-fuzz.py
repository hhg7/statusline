#!/usr/bin/env python3
"""Randomised differential fuzz for statusline-test.sh.

Feeds the same payload to statusline.py and the C build named by
STATUSLINE_BIN (default ./statusline) and compares stdout. Seeded, so a failure
reproduces. Usage: statusline-test-fuzz.py [count]"""

import json
import os
import random
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PY = os.path.join(HERE, "statusline.py")
C = os.environ.get("STATUSLINE_BIN") or os.path.join(HERE, "statusline")
if not os.path.isabs(C):
	C = os.path.join(HERE, C) if os.sep in C else C

random.seed(7)
import time
from datetime import datetime, timezone, timedelta
# The clock both builds read; statusline-test.sh exports it. Standalone, pin it
# here so the two children cannot straddle a rounding boundary between them.
os.environ.setdefault("STATUSLINE_NOW", repr(time.time()))
NOW = float(os.environ["STATUSLINE_NOW"])
now = datetime.fromtimestamp(NOW, timezone.utc)


def rnum():
	return random.choice([
		random.randint(0, 2_000_000),
		random.uniform(0, 1),
		random.uniform(0, 100),
		round(random.uniform(0, 100), 1),
		random.choice([0, 1, 0.5, 1.0, True, False, 999, 1000, 1001,
		               999_999, 1_000_000, 1_500_000]),
	])


def rts():
	return random.choice([
		(now + timedelta(minutes=random.randint(-100, 10000))).strftime("%Y-%m-%dT%H:%M:%SZ"),
		(now + timedelta(seconds=random.randint(0, 400000))).isoformat(),
		int(NOW) + random.randint(-3600, 700000),
		NOW + random.uniform(-3600, 700000),
		None, "", "bogus", "2026-13-99T99:99:99Z", "2026-02-30T12:00:00Z",
	])


def payload():
	d = {}
	if random.random() < .8:
		d["model"] = {"display_name": random.choice(["Opus 5", "Sonnet 5", "Haiku 4.5", ""])}
	if random.random() < .4:
		d["fast_mode"] = random.choice([True, False, None])
	if random.random() < .4:
		d["effort"] = {"level": random.choice(["high", "low", 3, 3.0, 2.5, True, None])}
	if random.random() < .8:
		d["context_window"] = {"total_input_tokens": rnum(),
		                       "context_window_size": random.choice([200000, 1000000, 100, 0, rnum()])}
	if random.random() < .8:
		d["rate_limits"] = {k: {"used_percentage": rnum(), "resets_at": rts()}
		                    for k in ("five_hour", "seven_day", "spend_limit")
		                    if random.random() < .7}
	if random.random() < .6:
		d["cost"] = {"total_cost_usd": round(random.uniform(-1, 500), 4)}
	if random.random() < .8:
		d["workspace"] = {"current_dir": random.choice(
			["/home/con/.claude", "/a/b/", "/", "x", "/p/q//"])}
	return json.dumps(d)


n = int(sys.argv[1]) if len(sys.argv) > 1 else 3000
bad = 0
for _ in range(n):
	s = payload()
	a = subprocess.run([PY], input=s, capture_output=True, text=True).stdout
	b = subprocess.run([C], input=s, capture_output=True, text=True).stdout
	if a != b:
		bad += 1
		if bad <= 5:
			print(f"MISMATCH {s}\n   py: {a!r}\n   c : {b!r}")
print(f"{n} random payloads, {bad} mismatches")
sys.exit(1 if bad else 0)

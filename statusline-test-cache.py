#!/usr/bin/env python3
"""Check the rate-limit cache that fills the usage segments at startup.

A fresh session's payload has no rate_limits until the first API reply, so each
build saves the last payload that had them and borrows its rate_limits when the
current one has none. Each case runs a build against a cache file in a fresh
temporary directory and checks the line against the same build rendering the
equivalent live payload with the cache off. Each build must therefore agree with
itself, the differential test has already shown the builds agree with each
other, and the last case swaps a cache written by one build into the other.

Usage: statusline-test-cache.py            (checks statusline.py and STATUSLINE_BIN)"""
import json, os, stat, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
C = os.environ.get("STATUSLINE_BIN") or os.path.join(HERE, "statusline")
if not os.path.isabs(C):
	C = os.path.join(HERE, C) if os.sep in C else C
builds = [("statusline.py", os.path.join(HERE, "statusline.py")), (os.path.basename(C), C)]

NOW = float(os.environ.get("STATUSLINE_NOW") or time.time())
MODEL = {"model": {"display_name": "Opus 5"}, "workspace": {"current_dir": "/x/proj"}}
FIVE = {"used_percentage": 41, "resets_at": NOW + 3 * 3600}
WEEK = {"used_percentage": 38.5, "resets_at": NOW + 56 * 3600}
LIVE = dict(MODEL, rate_limits={"five_hour": FIVE, "seven_day": WEEK})

bad = tested = 0


def run(exe, payload, **env):
	"""stdout of exe on payload; env values of None are removed."""
	e = dict(os.environ, STATUSLINE_NOW=repr(NOW))
	for k, v in env.items():
		if v is None:
			e.pop(k, None)
		else:
			e[k] = v
	text = payload if isinstance(payload, str) else json.dumps(payload)
	return subprocess.run([exe], input=text, capture_output=True, text=True, env=e).stdout


def check(label, got, want):
	global bad, tested
	tested += 1
	if got != want:
		bad += 1
		print(f"MISMATCH {label}\n   want: {want!r}\n   got : {got!r}")


def plain(exe, payload):
	return run(exe, payload, STATUSLINE_CACHE="")


for name, exe in builds:
	with tempfile.TemporaryDirectory() as tmp:
		cache = os.path.join(tmp, "c.json")
		use = lambda p: run(exe, p, STATUSLINE_CACHE=cache)

		check(f"{name} no cache file yet", use(MODEL), plain(exe, MODEL))
		check(f"{name} live payload unchanged by caching", use(LIVE), plain(exe, LIVE))
		with open(cache) as f:
			check(f"{name} cache holds the payload verbatim", f.read(), json.dumps(LIVE))
		check(f"{name} cache is private", oct(stat.S_IMODE(os.stat(cache).st_mode)), "0o600")
		check(f"{name} no temporary left behind", sorted(os.listdir(tmp)), ["c.json"])

		check(f"{name} startup payload borrows rate_limits", use(MODEL), plain(exe, LIVE))
		for empty in ({}, None):
			check(f"{name} rate_limits={empty!r} borrows too",
			      use(dict(MODEL, rate_limits=empty)), plain(exe, LIVE))

		# The live rate_limits wins over the cache, and replaces it.
		newer = dict(MODEL, rate_limits={"seven_day": dict(WEEK, used_percentage=50)})
		check(f"{name} live beats cache", use(newer), plain(exe, newer))
		check(f"{name} cache now the newer payload", use(MODEL), plain(exe, newer))

		# A window past its reset, or with no reset to age it by, is dropped.
		with open(cache, "w") as f:
			json.dump(dict(MODEL, rate_limits={
				"five_hour": dict(FIVE, resets_at=NOW - 60),
				"seven_day": WEEK,
				"spend_limit": {"used_percentage": 12}}), f)
		check(f"{name} expired and undated windows dropped",
		      use(MODEL), plain(exe, dict(MODEL, rate_limits={"seven_day": WEEK})))

		for junk in ("{not json", "", "[1,2]", '{"rate_limits": 5}'):
			with open(cache, "w") as f:
				f.write(junk)
			check(f"{name} unusable cache {junk!r} ignored", use(MODEL), plain(exe, MODEL))

		# The default locations: XDG_CACHE_HOME if absolute, else ~/.cache.
		os.mkdir(os.path.join(tmp, "xdg"))
		os.mkdir(os.path.join(tmp, ".cache"))
		run(exe, LIVE, STATUSLINE_CACHE=None, XDG_CACHE_HOME=os.path.join(tmp, "xdg"), HOME=tmp)
		run(exe, LIVE, STATUSLINE_CACHE=None, XDG_CACHE_HOME="rel", HOME=tmp)
		check(f"{name} default paths",
		      [os.path.exists(os.path.join(tmp, d, "claude-statusline.json")) for d in ("xdg", ".cache")],
		      [True, True])

		before = sorted(os.listdir(tmp))
		run(exe, LIVE, STATUSLINE_CACHE="", HOME=tmp)
		check(f"{name} empty STATUSLINE_CACHE writes nothing", sorted(os.listdir(tmp)), before)

# A cache written by one build is read by the other.
with tempfile.TemporaryDirectory() as tmp:
	cache = os.path.join(tmp, "c.json")
	for (wname, wexe), (rname, rexe) in ((builds[0], builds[1]), (builds[1], builds[0])):
		os.path.exists(cache) and os.unlink(cache)
		run(wexe, LIVE, STATUSLINE_CACHE=cache)
		check(f"{wname} writes, {rname} reads", run(rexe, MODEL, STATUSLINE_CACHE=cache), plain(rexe, LIVE))

print(f"{tested} cache checks across {len(builds)} builds, {bad} mismatches")
sys.exit(1 if bad else 0)

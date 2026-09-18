"""Check the C str(float) against CPython's, through effort.level.

Only the C binary is spawned per value; the expected line is built in-process
from CPython's own str(), which is the thing under comparison. Full-pipeline
parity is covered by statusline-test.sh -- this isolates the float formatter so
it can sweep far more values.

Sweeps the decimal exponents where CPython switches between fixed and
scientific notation, plus subnormals, boundaries and random bit patterns."""
import json, os, random, struct, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
C = os.environ.get("STATUSLINE_BIN") or os.path.join(HERE, "statusline")
builds = [(os.path.basename(C), C)]

vals = []
for e in range(-330, 309):
	for m in ("1", "1.5", "9.99", "1.234567890123456"):
		try:
			v = float(f"{m}e{e}")
		except (ValueError, OverflowError):
			continue
		if v not in (float("inf"), float("-inf")):
			vals.append(v)
vals += [0.1, 0.5, 1/3, 2/3, 1e15, 1e16, 1e17, 1e-4, 1e-5, 5e-324, 1e308,
         1.7976931348623157e308, 2.2250738585072014e-308, 123456789.0,
         1234567890123456.0, 0.30000000000000004, 2**53, 2.0**53 + 2]
random.seed(3)
while len(vals) < 12000:
	v = struct.unpack("<d", struct.pack("<Q", random.getrandbits(64)))[0]
	if v == v and v not in (float("inf"), float("-inf")):
		vals.append(v)

bad = tested = 0
for label, exe in builds:
	for v in vals:
		if v == 0:              # falsy: no tag, nothing to compare
			continue
		p = json.dumps({"model": {"display_name": "M"}, "effort": {"level": v}})
		if json.loads(p)["effort"]["level"] != v:
			continue            # json round-trip lost it; not this program's business
		want = f"\033[36mM {str(v)}\033[0m\n"
		got = subprocess.run([exe], input=p, capture_output=True, text=True).stdout
		tested += 1
		if got != want:
			bad += 1
			if bad <= 8:
				print(f"MISMATCH {label} {v!r}\n   want: {want!r}\n   got : {got!r}")
print(f"{tested} float renderings across {len(builds)} builds, {bad} mismatches")
sys.exit(1 if bad else 0)

# statusline

A status line for [Claude Code](https://claude.com/claude-code): one ANSI-coloured
line showing the model, a context-window meter, the 5-hour / 7-day / spend rate
limits with their reset times, the weekly quota against its ration, session cost,
and the current directory.

```
Opus 5 │ ▰▰▰▱▱▱▱▱▱▱ 28% 62k/220k │ 5h 41%↺3h │ 7d 38.0%/66.7%↺56h │ $1.84 │ statusline
```

Claude Code runs the configured status-line command on every render, passing a
JSON payload on stdin; this program reads that payload and prints the line.

## Build and install

Needs a C11 compiler and [json-c](https://github.com/json-c/json-c)
(`libjson-c-dev` on Debian/Ubuntu, `json-c-devel` on Fedora, `brew install
json-c` on macOS).

```sh
make
make install          # installs to ~/.claude/statusline
```

`make install` writes to `$(CLAUDE_DIR)`, which defaults to `~/.claude`; set it
if your Claude Code config lives elsewhere:

```sh
make install CLAUDE_DIR=/somewhere/else/.claude
```

Then point Claude Code at it, in `~/.claude/settings.json`:

```json
{
  "statusLine": {
    "type": "command",
    "command": "/home/you/.claude/statusline",
    "padding": 0
  }
}
```

The command needs an absolute path — Claude Code does not expand `~`.

### Without json-c

`statusline-nodeps.c` is the same program with a hand-rolled JSON parser and no
dependency beyond libc, for a machine where json-c is not available. It installs
under the same name:

```sh
make nodeps
make install-nodeps
```

`statusline.c` is the maintained version; the two are kept in step by the same
test suite.

## What it shows

Each segment is dropped when the payload does not carry its field. The one
exception is the rate limits, which a fresh session would otherwise lack; see
below.

| Segment | Source field | Notes |
| --- | --- | --- |
| model | `model.display_name` | plus `fast` and the effort level when set |
| context meter | `context_window.{total_input_tokens,context_window_size}` | 10-cell bar, percentage, used/total |
| `5h` / `7d` / `spend` | `rate_limits.*.used_percentage`, `.resets_at` | `↺` is the time until reset |
| ration | `rate_limits.seven_day.{used_percentage,resets_at}` | on the `7d` segment; see below |
| cost | `cost.total_cost_usd` | shown only when above zero |
| directory | `workspace.current_dir`, else `cwd` | last path segment |

The context bar and the percentages are green under 60%, yellow from 60%, red
from 80%.

### The ration

The weekly quota resets Saturday 07:00 America/Chicago, and an even burn would
by now have used the fraction of the week that has elapsed — the same schedule
as `~/Scripts/C/token.rationing.c`. The `7d` segment shows both, to one decimal:
`7d 38.0%/66.7%` is 38.0% of the quota consumed against an on-pace budget of
66.7%. The whole segment is green when consumption is at or under the budget
and red when it is over. The comparison is made before rounding, so the two
numbers can print the same while the segment is red.

The window's start is found in Chicago wall-clock, so the two weeks a year that
cross a DST change are 167 and 169 hours rather than 168. `resets_at` arrives as
a Unix time; without it the segment shows only the consumed percentage,
coloured by the 60%/80% thresholds, and no `↺`.

### At startup

Claude Code sends no `rate_limits` until the first API reply of a session, so
on its own the line would start without `5h`, `7d` or the ration. Every payload
that does carry `rate_limits` is therefore saved whole, and a payload without
them borrows the saved ones. A saved window whose reset has passed is left out,
as is one with no `resets_at` to age it by, so what shows at startup is the
last reading of each window that is still current. It is replaced by live
figures as soon as the first reply arrives, and usage elsewhere in the
meantime (another machine, claude.ai) is not reflected until then.

The file is `$XDG_CACHE_HOME/claude-statusline.json`, or
`~/.cache/claude-statusline.json` when that is unset, written mode 0600 and
replaced by rename so that concurrent sessions never see half a file.
`STATUSLINE_CACHE` names another file; set to the empty string it turns the
cache off.

## Tests

`statusline.py` is the reference implementation: readable, and the thing the C
is checked against. `statusline-test.sh` runs both over curated payloads,
malformed input, every ISO-8601 form CPython 3.12 was observed to accept or
reject, and several thousand randomised payloads, and compares stdout byte for
byte.

```sh
make test                   # checks ./statusline
make test-nodeps            # checks ./statusline-nodeps
make test FUZZ=20000        # more randomised payloads (default 3000)
make valgrind               # every curated payload under valgrind
make valgrind-nodeps
```

`statusline-test.sh` pins one clock for the whole run through `STATUSLINE_NOW`,
which both implementations read in place of the time, so the two are compared
at the same instant. `statusline-test-pace.py` checks the ration against
hand-worked windows on fixed clocks, including both DST weeks. The differential
run turns the cache off; `statusline-test-cache.py` checks it in temporary
directories, including a cache written by one build and read by the other. The valgrind run
fails on any leak of any kind, "still reachable" included: both builds free
everything before exiting.

Needs `python3`; the memory check also needs `valgrind`. The handful of inputs where the C deliberately diverges from
the Python — all of them malformed, none emitted by Claude Code — are listed in
the header comment of `statusline.c`.

## Keeping it current

The payload shape was read off the Claude Code 2.1.276 binary, and the numeric
`resets_at` off 2.1.281. After an upgrade, run `make test`, then check the line
against `/context` and `/usage` in a live session; `echo '{}' |
~/.claude/statusline` should print an empty line rather than an error.

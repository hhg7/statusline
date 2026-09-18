# statusline

A status line for [Claude Code](https://claude.com/claude-code): one ANSI-coloured
line showing the model, a context-window meter, the 5-hour / 7-day / spend rate
limits with their reset times, session cost, and the current directory.

```
Opus 5 │ ▰▰▰▱▱▱▱▱▱▱ 28% 62k/220k │ 5h 41%↺3h │ 7d 12%↺5d │ $1.84 │ statusline
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

Each segment is dropped when the payload does not carry its field, so a fresh
session with no rate-limit data simply shows fewer segments.

| Segment | Source field | Notes |
| --- | --- | --- |
| model | `model.display_name` | plus `fast` and the effort level when set |
| context meter | `context_window.{total_input_tokens,context_window_size}` | 10-cell bar, percentage, used/total |
| `5h` / `7d` / `spend` | `rate_limits.*.used_percentage`, `.resets_at` | `↺` is the time until reset |
| cost | `cost.total_cost_usd` | shown only when above zero |
| directory | `workspace.current_dir`, else `cwd` | last path segment |

The bar and the percentages are green under 60%, yellow from 60%, red from 80%.

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
```

Needs `python3`. The handful of inputs where the C deliberately diverges from
the Python — all of them malformed, none emitted by Claude Code — are listed in
the header comment of `statusline.c`.

## Keeping it current

The payload shape was read off the Claude Code 2.1.276 binary. After an upgrade,
run `make test`, then check the line against `/context` and `/usage` in a live
session; `echo '{}' | ~/.claude/statusline` should print an empty line rather
than an error.

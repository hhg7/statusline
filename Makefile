# Claude Code status line.
#
# `make` builds ./statusline from statusline.c against json-c; `make nodeps`
# builds ./statusline-nodeps from the libc-only translation, for a machine with
# no json-c. `make install` copies the built binary into $(CLAUDE_DIR), where
# the statusLine entry in settings.json points at it. See README.md.

CC         ?= cc
CFLAGS     ?= -std=c11 -O2 -Wall -Wextra
CLAUDE_DIR ?= $(HOME)/.claude
FUZZ       ?= 3000

JSONC_CFLAGS := $(shell pkg-config --cflags json-c 2>/dev/null)
JSONC_LIBS   := $(shell pkg-config --libs json-c 2>/dev/null)
# json-c has shipped a .pc file since 0.12, but a hand-built copy may not have
# installed one; fall back to the plain link flag rather than fail the build.
ifeq ($(strip $(JSONC_LIBS)),)
JSONC_LIBS := -ljson-c
endif

# -lm is for rint(), floor() and isnan() in cells(), utcoffset() and sput_pyfloat().
LIBM := -lm

.PHONY: all nodeps test test-nodeps valgrind valgrind-nodeps install install-nodeps uninstall clean

all: statusline

statusline: statusline.c
	$(CC) $(CFLAGS) $(JSONC_CFLAGS) -o $@ $< $(JSONC_LIBS) $(LIBM)

statusline-nodeps: statusline-nodeps.c
	$(CC) $(CFLAGS) -o $@ $< $(LIBM)

nodeps: statusline-nodeps

# Differential test against statusline.py, the reference implementation.
test: statusline
	./statusline-test.sh $(FUZZ)

test-nodeps: statusline-nodeps
	STATUSLINE_BIN=./statusline-nodeps ./statusline-test.sh $(FUZZ)

# Every curated payload under valgrind; fails on any leak or memory error.
valgrind: statusline
	./statusline-test-valgrind.sh

valgrind-nodeps: statusline-nodeps
	STATUSLINE_BIN=./statusline-nodeps ./statusline-test-valgrind.sh

install: statusline
	mkdir -p $(CLAUDE_DIR)
	install -m 755 statusline $(CLAUDE_DIR)/statusline

install-nodeps: statusline-nodeps
	mkdir -p $(CLAUDE_DIR)
	install -m 755 statusline-nodeps $(CLAUDE_DIR)/statusline

uninstall:
	rm -f $(CLAUDE_DIR)/statusline

clean:
	rm -f statusline statusline-nodeps

# Makefile — build Hemma natively on Linux / a Raspberry Pi (gcc or clang) or a Mac
# (Xcode's clang: xcode-select --install). `make CC="zig cc"` works too.
# On Windows use build.ps1 (it can also cross-compile the Linux, Pi and Mac binaries).
#
#   make                 build/hemma
#   make test            build and run every *_test.c
#   make run             run on 127.0.0.1:8080 with web/ served live from disk
#   make SKIP="tv"       leave module folders out (media needs av and tv needs media: they go too)
#   sudo make install    Linux with systemd: /opt/hemma + the service (see deploy/, docs/linux.md)
#                        (a Mac uses deploy/local.hemma.plist instead: docs/macos.md)

CC       ?= cc
CFLAGS   ?= -O2
# Always on, whatever CFLAGS says. LEAN leaves unused code and unwind tables out of the binary.
WARN     := -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
LEAN     := -ffunction-sections -fdata-sections -fno-asynchronous-unwind-tables
ifeq ($(shell uname -s),Darwin)
LEAN_LD  := -Wl,-dead_strip
else
LEAN_LD  := -Wl,--gc-sections
endif
# 64-bit file offsets and time_t also on 32-bit glibc (Raspberry Pi OS armhf): files > 4 GB, dates after 2038
CPPFLAGS += -Icore -Ithird_party/sqlite -D_FILE_OFFSET_BITS=64 -D_TIME_BITS=64
# The commit a binary is built from, for `hemma --version` ("-dirty": with uncommitted changes).
# Nothing outside a git checkout.
HEMMA_COMMIT := $(shell git rev-parse --short=12 HEAD 2>/dev/null)$(shell git status --porcelain --untracked-files=no 2>/dev/null | grep -q . && echo -dirty)
CPPFLAGS += $(if $(HEMMA_COMMIT),-DHEMMA_COMMIT=$(HEMMA_COMMIT))
LDLIBS   += -lpthread -lm
# SQLite for size: its data is read into memory once, so -Os costs nothing you'd notice
SQLITE_FLAGS = -Os -w -DSQLITE_DQS=0 -DSQLITE_THREADSAFE=1 -DSQLITE_DEFAULT_MEMSTATUS=0 \
  -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_MAX_EXPR_DEPTH=0 \
  -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_USE_ALLOCA \
  -DSQLITE_ENABLE_FTS5 -DSQLITE_ENABLE_MATH_FUNCTIONS -DSQLITE_OMIT_JSON -DSQLITE_ENABLE_DBPAGE_VTAB \
  -ffunction-sections -fdata-sections
SQLITE_OBJ = build/obj/sqlite3.o build/obj/recover.o

SKIP        ?=
SKIPPED     := $(SKIP) $(if $(filter av,$(SKIP)),media) $(if $(filter av media,$(SKIP)),tv)
MODULE_DIRS := $(filter-out $(addprefix modules/,$(SKIPPED)),$(wildcard modules/*))
SRC_ALL     := $(wildcard core/*.c) $(foreach d,$(MODULE_DIRS),$(wildcard $(d)/*.c))
SRC         := $(filter-out %_test.c,$(SRC_ALL))
TEST_SRC    := $(filter-out core/main.c,$(SRC)) $(filter %_test.c,$(SRC_ALL)) tests/runner.c tests/harness.c
HEADERS     := $(wildcard core/*.h) $(foreach d,$(MODULE_DIRS),$(wildcard $(d)/*.h))
CSS         := web/app.css $(sort $(foreach d,$(MODULE_DIRS),$(wildcard $(d)/*.css)))
WEB         := $(filter-out web/app.css,$(shell find web -type f ! -name '.*'))

all: build/hemma

build/.dir:
	mkdir -p build/obj && touch $@

build/embed: tools/embed.c build/.dir
	$(CC) -O1 -o $@ tools/embed.c

build/app.css: $(CSS) build/.dir
	for f in $(CSS); do printf '/* ---- %s ---- */\n' "$$f"; cat "$$f"; printf '\n'; done > $@

# The converter for browsers (modules/av/wasm/avconv.c), served as /static/av.wasm: uploads
# convert on the sending device. Needs a C compiler that targets WebAssembly - zig (found by
# itself) or e.g. WASM_CC="clang --target=wasm32-wasi --sysroot=/path/to/wasi-sysroot".
# Without one Hemma is built without it, and uploads send originals.
ifeq ($(origin WASM_CC),undefined)
WASM_CC  := $(shell command -v zig >/dev/null 2>&1 && echo "zig cc -target wasm32-wasi")
endif
WASM_SRC := modules/av/wasm/avconv.c $(addprefix modules/av/,conv.c mkv.c mpeg2.c deint.c h264enc.c ac3.c aac.c mp4.c)
ifneq ($(strip $(WASM_CC)),)
ifeq ($(filter av,$(SKIPPED)),)
WASM := build/av.wasm
endif
endif

build/av.wasm: $(WASM_SRC) $(wildcard modules/av/*.h) build/.dir
	$(WASM_CC) $(WARN) -mexec-model=reactor -msimd128 -O2 -DNDEBUG -s -Wl,-z,stack-size=8388608 \
	  -Imodules/av -o $@ $(WASM_SRC)

# Each text file and av.wasm also goes in gzipped ("/app.css.gz"), for browsers that accept it.
GZ_SRC := $(filter %.css %.js %.svg %.webmanifest,$(WEB)) build/app.css $(WASM)

build/assets.c: build/embed build/app.css $(WEB) $(WASM)
	mkdir -p build/gz
	twins=""; if command -v gzip >/dev/null 2>&1; then for f in $(GZ_SRC); do n=$$(basename "$$f"); \
	  gzip -9nc "$$f" > "build/gz/$$n.gz" && twins="$$twins /$$n.gz=build/gz/$$n.gz"; done; fi; \
	build/embed $@ web $(WEB) /app.css=build/app.css $(if $(WASM),/av.wasm=$(WASM)) $$twins

build/obj/sqlite3.o: third_party/sqlite/sqlite3.c build/.dir
	$(CC) -c $(SQLITE_FLAGS) -o $@ third_party/sqlite/sqlite3.c

# SQLite's recovery extension (third_party/sqlite/recover.c = dbdata.c + sqlite3recover.c)
build/obj/recover.o: third_party/sqlite/recover.c third_party/sqlite/dbdata.c third_party/sqlite/sqlite3recover.c \
  third_party/sqlite/sqlite3recover.h build/.dir
	$(CC) -c $(SQLITE_FLAGS) -o $@ third_party/sqlite/recover.c

build/hemma: $(SRC) $(HEADERS) build/assets.c $(SQLITE_OBJ)
	$(CC) $(CPPFLAGS) $(WARN) $(LEAN) $(CFLAGS) -o $@ $(SRC) build/assets.c $(SQLITE_OBJ) $(LEAN_LD) $(LDFLAGS) $(LDLIBS)

build/test: $(TEST_SRC) $(HEADERS) build/assets.c $(SQLITE_OBJ)
	$(CC) $(CPPFLAGS) $(WARN) $(CFLAGS) -Itests -o $@ $(TEST_SRC) build/assets.c $(SQLITE_OBJ) $(LDFLAGS) $(LDLIBS)

test: build/test
	build/test

run: build/hemma
	build/hemma --host 127.0.0.1 --web web --data data

# Not built here: `sudo make install` would build as root and leave root-owned files in build/.
install:
	@test -x build/hemma || { echo "run make first (as yourself), then sudo make install"; exit 1; }
	sh deploy/install-pi.sh build/hemma

clean:
	rm -rf build

.PHONY: all test run install clean

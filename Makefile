# RUNBOOK — build.
#
#   make            build build/runbook
#   make check      the gates: --health, then determinism
#   make health     the health gate alone
#   make windows    cross-build the Windows binary (needs mingw-w64)
#   make clean
#
# ONE C COMPILER, ONE LINK, NO THIRD-PARTY LIBRARIES. A single self-contained
# binary per platform is a locked target (handoff decision 3), and every
# dependency added here is a dependency the player's machine has to satisfy.
#
# The floating point flags are load-bearing, not decoration: contraction and
# fast-math let the compiler reassociate arithmetic, which breaks the
# byte-identical replay guarantee that decision 16 rests on. Nothing in the
# model uses a double yet — the flags are here now so that the day one lands,
# it lands already pinned, instead of being retrofitted after the Windows
# build starts disagreeing with the Linux one.

CC      ?= cc
CSTD     = -std=c11
WARN     = -Wall -Wextra -Wno-unused-parameter
FPFLAGS  = -ffp-contract=off -fno-fast-math
OPT     ?= -O2
# -MMD -MP: without header dependencies, editing world.h rebuilds only world.o
# and every other object keeps the old struct layout. That fails as a segfault,
# not as a compile error. Do not remove.
CFLAGS  += $(CSTD) $(WARN) $(FPFLAGS) $(OPT) -Icore -MMD -MP
LDFLAGS +=

# THE CORE IS A LIBRARY. main.c is the only file below that knows a command
# line exists; everything else is linked, unchanged, into the GDExtension when
# the client arrives at M3. If a source file here ever needs to know whether it
# is running under Godot, the model/view rule has already been broken.
CORE_SRC = core/util.c core/world.c core/proto.c core/serve.c core/health.c
CORE_OBJ = $(CORE_SRC:core/%.c=build/%.o)
MAIN_OBJ = build/main.o

BIN = build/runbook

# The default goal is pinned: a rule defined above `all:` would otherwise
# become the default and silently build the wrong thing.
.DEFAULT_GOAL := all

.PHONY: all check health determinism clean windows

all: $(BIN)

$(BIN): $(CORE_OBJ) $(MAIN_OBJ) | build
	$(CC) -o $@ $(CORE_OBJ) $(MAIN_OBJ) $(LDFLAGS)

build:
	@mkdir -p build

build/%.o: core/%.c | build
	$(CC) $(CFLAGS) -c -o $@ $<

# ---------------------------------------------------------------- the gates
# M0's whole deliverable. Nothing else starts until these are green
# (handoff §15).
check: health determinism

health: $(BIN)
	@./$(BIN) --health

determinism: $(BIN)
	@./tools/check_determinism.sh

# ------------------------------------------------------------------ windows
# Windows is a target, not a port (handoff §0). It is cross-built here so the
# determinism gate can compare the two binaries' output; if this rule rots,
# the cross-platform half of decision 16 rots with it and nothing says so.
WINCC    ?= x86_64-w64-mingw32-gcc
WINFLAGS  = $(CSTD) $(WARN) $(FPFLAGS) $(OPT) -Icore -static
WIN_BIN   = build/win/runbook.exe

windows: $(WIN_BIN)

$(WIN_BIN): $(CORE_SRC) core/main.c
	@mkdir -p build/win
	$(WINCC) $(WINFLAGS) $(CORE_SRC) core/main.c -o $@ -lws2_32

clean:
	rm -rf build

-include $(CORE_OBJ:.o=.d) $(MAIN_OBJ:.o=.d)

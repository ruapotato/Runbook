#!/bin/sh
# Determinism gate. Handoff decision 16: failure is deterministic per seed.
#
# That decision is not about fairness to the player, or not only. The balance
# harness (--play, --naive, --vacation) grades numbers, and an agent cannot
# tune a curve that moves under it between runs. Every number in §5 and §10 of
# the handoff is meant to be settled by running the game, which is only
# possible if running it twice means the same thing.
#
# Four checks, in increasing order of how much they hurt to fix later:
#   1. same binary, same seed, twice   -> byte-identical world
#   2. different seeds                 -> different world (the seed is real)
#   3. -O0 build vs -O2 build          -> byte-identical world
#   4. Linux build vs Windows build    -> byte-identical world (skipped when
#                                         mingw-w64 or wine is not installed)
#
# Checks 3 and 4 are the ones that catch floating-point contraction and
# reassociation. The model is integer-only today and they are therefore
# trivially green — which is the point of having them now. They will start
# doing real work the first time a double enters the world model, and they
# will do it on that commit rather than three months after it.
set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK" 2>/dev/null; true' EXIT

BIN=build/runbook
SEED=${SEED:-424242}
# LONG ENOUGH TO COMPOUND. A one-day run reproduces by accident; growth,
# attrition and the weekly waves only have room to drift apart over months of
# simulated time, and drift is what this gate is looking for.
#
# 45 days is a whole game: the reference agent plays from forty users through
# the Act I wall and the whole of Act II, closing about a thousand tickets. It
# used to be 120 days of an UNPLAYED world, which was both less meaningful and
# far slower -- nobody works the queue, so every ticket goes past its SLA and
# gets chased forever, and the gate spent minutes hashing a queue that exists
# only because there is no player.
DAYS=${DAYS:-45}

[ -x "$BIN" ] || { echo "determinism: $BIN not built"; exit 1; }

# A PLAYED RUN, NOT A GROWN ONE. The reference agent works the queue for
# $DAYS simulated days -- thousands of appliance calls, every failure mode
# rolled, every retry taken -- and the world that comes out the other end is
# what gets compared. Hashing a world nobody touched would prove that the
# growth model is deterministic and nothing else.
run() { "$BIN" --play --seed "$1" --days "$DAYS" --out "$2" >/dev/null 2>&1; }

fail=0

# ---- 1. same binary, same inputs, twice
run "$SEED" "$WORK/a.json"
run "$SEED" "$WORK/b.json"
if cmp -s "$WORK/a.json" "$WORK/b.json"; then
    echo "determinism: PASS  same seed reproduces byte-identically ($DAYS days, $(wc -c <"$WORK/a.json") bytes)"
else
    echo "determinism: FAIL  same seed produced two different worlds"
    diff "$WORK/a.json" "$WORK/b.json" | head -10
    fail=1
fi

# ---- 2. the seed actually reaches the world
run $((SEED + 1)) "$WORK/c.json"
if cmp -s "$WORK/a.json" "$WORK/c.json"; then
    echo "determinism: FAIL  a different seed produced an identical world"
    fail=1
else
    echo "determinism: PASS  a different seed produces a different world"
fi

# ---- 3. optimisation level must not change the numbers
if [ "${SKIP_O0:-0}" != "1" ]; then
    make -s clean >/dev/null 2>&1 || true
    make -s OPT=-O0 >/dev/null 2>&1
    run "$SEED" "$WORK/o0.json"
    make -s clean >/dev/null 2>&1 || true
    make -s >/dev/null 2>&1
    if cmp -s "$WORK/a.json" "$WORK/o0.json"; then
        echo "determinism: PASS  -O0 and -O2 agree byte-for-byte"
    else
        echo "determinism: FAIL  -O0 and -O2 disagree (arithmetic is not pinned)"
        diff "$WORK/a.json" "$WORK/o0.json" | head -10
        fail=1
    fi
fi

# ---- 4. Linux and Windows must agree. Both platforms ship (handoff §0), and
#         "deterministic on Linux" is not the claim being made.
#
# The wine prefix is PERSISTENT and 64-bit on purpose. A fresh prefix per run
# costs half a gigabyte and a couple of minutes, which is how a check ends up
# commented out; and the default ~/.wine on this machine is a 32-bit
# installation, under which a PE32+ binary fails with "Bad EXE format" — a
# FAIL that means nothing about the code. Override with RUNBOOK_WINEPREFIX.
WINEPFX=${RUNBOOK_WINEPREFIX:-$HOME/.runbook-wine64}
WINE64=""
for cand in /usr/lib/wine/wine64 "$(command -v wine64 2>/dev/null)"; do
    [ -n "$cand" ] && [ -x "$cand" ] && WINE64="$cand" && break
done

if ! command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then
    echo "determinism: SKIP  cross-platform check (needs mingw-w64)"
elif [ -z "$WINE64" ]; then
    echo "determinism: SKIP  cross-platform check (needs a 64-bit wine)"
elif [ ! -d "$WINEPFX" ]; then
    # SAID OUT LOUD, WITH THE COMMAND TO FIX IT. A skip whose cause is a
    # mystery gets skipped forever.
    echo "determinism: SKIP  cross-platform check — no 64-bit wine prefix at $WINEPFX"
    echo "determinism:       create it once with:"
    echo "determinism:         WINEARCH=win64 WINEPREFIX=$WINEPFX $WINE64 wineboot -u"
elif ! make -s windows >/dev/null 2>&1; then
    echo "determinism: FAIL  the Windows cross-build broke"
    fail=1
else
    WINEDEBUG=-all WINEPREFIX="$WINEPFX" "$WINE64" build/win/runbook.exe --play \
        --seed "$SEED" --days "$DAYS" --out "$WORK/win.json" >/dev/null 2>&1 || true
    if [ -f "$WORK/win.json" ] && cmp -s "$WORK/a.json" "$WORK/win.json"; then
        echo "determinism: PASS  Linux and Windows agree byte-for-byte"
    else
        echo "determinism: FAIL  the Windows build disagrees with the Linux build"
        fail=1
    fi
fi

exit $fail

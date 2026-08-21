#!/bin/sh
# The client gate, under a ceiling.
#
# UNDER A CEILING BECAUSE NOMINAL'S SUITE ONCE ATE 18.5 GB and the desktop had
# to be rescued. This box is somebody's workstation; a headless run with no
# bound on it can take the machine down while you are reading the output of
# the last one. `ulimit -v` is the limit that actually bites, because it fails
# the allocation rather than waiting for the OOM killer to pick a victim --
# and the victim is not always the process you would have chosen.
#
# It is a SKIP, not a failure, when there is no Godot: the C gates are the
# ones that must be green everywhere, and a contributor without an engine
# should still be able to run `make check`.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)

if [ -z "${GODOT:-}" ]; then
    for cand in "$ROOT"/Godot_v4.*-stable_linux.x86_64 \
                "$HOME/NOMINAL"/Godot_v4.*-stable_linux.x86_64 \
                "$(command -v godot 2>/dev/null)"; do
        [ -n "$cand" ] && [ -x "$cand" ] && GODOT="$cand" && break
    done
fi
GODOT="${GODOT:-}"
if [ ! -x "$GODOT" ]; then
    echo "client: SKIP  no Godot found (set GODOT=/path/to/godot)"
    exit 0
fi

if [ ! -f "$ROOT/game/bin/librunbook.linux.x86_64.so" ]; then
    echo "client: FAIL  the extension is not built -- run: make gdext"
    exit 1
fi

# THE EXTENSION MUST BE NEWER THAN THE CORE IT BINDS.
#
# A stale .so does not crash and does not warn. It answers "unknown verb" to
# everything added since it was built, and the gate reports a client that
# cannot talk to its own API. That happened within an hour of the binding
# existing, so: check the timestamps and say so plainly.
for src in "$ROOT"/core/*.c "$ROOT"/core/*.h "$ROOT"/gdext/*.c; do
    if [ "$src" -nt "$ROOT/game/bin/librunbook.linux.x86_64.so" ]; then
        echo "client: FAIL  $(basename "$src") is newer than the extension -- run: make gdext"
        exit 1
    fi
done

out=$( ulimit -v 8000000; timeout 180 "$GODOT" --headless --path "$ROOT/game" \
       -s tests/client_test.gd 2>&1 )
rc=$?
printf '%s\n' "$out" | grep -E "PASS|FAIL|checks," | sed 's/^/client: /'

if [ $rc -eq 124 ]; then
    echo "client: FAIL  timed out (a GDScript parse error hangs a -s run rather than failing it)"
    printf '%s\n' "$out" | grep -E "Parse Error" | head -3
    exit 1
fi

# ENGINE ERRORS TOO. A GDScript error abandons the function and the run
# carries on, so a gate whose first line dies reports nothing and stays green.
# "rp_child is null" is the headless renderer's dummy stub complaining about
# a Control it was asked to draw with no display attached. It says nothing
# about the game and it is unavoidable if the gate is going to instantiate the
# real desktop -- which it must, because a client gate that never opens a
# window is testing the API twice.
engine=$(printf '%s\n' "$out" | grep -E "^ERROR:|^SCRIPT ERROR:" \
         | grep -vE "resources still in use at exit|rp_child|were leaked|RID allocations" \
         | head -3)
if [ -n "$engine" ]; then
    echo "client: FAIL  the engine complained:"
    printf 'client:   %s\n' "$engine"
    exit 1
fi

printf '%s\n' "$out" | grep -q "0 failures" || exit 1
exit 0

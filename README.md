# RUNBOOK

Factorio's ratchet, applied to IT operations. You start clicking. You end up
with an orchestrator. You win by going on vacation.

The design is in [`handoff.md`](handoff.md) and it is the authority. Anything
in this file that disagrees with it is stale, and the handoff wins.

## Where this is

**M0.** The harness, and nothing else. There is no game here yet: no
appliances, no tickets, no client. What exists is an org that boots from a
seed, grows, and can be driven entirely over an API — plus the two gates that
say so.

That order is deliberate (handoff §15). The gates exist before the gameplay
because they are the reason this project can be built by agents at all, and
because a balance harness bolted on afterwards is measuring a game that has
already been tuned by argument.

## Build and check

```sh
make          # build/runbook — one binary, no runtime dependencies
make check    # both gates: --health, then determinism
```

`make check` is the M0 deliverable. It must be green before anything from M1
starts.

## Running it

Every mode drives the same API; there is no back door into the model.

```sh
./build/runbook                          # commands on stdin
./build/runbook --serve                  # 127.0.0.1:7711, then telnet in
./build/runbook --exec 'world.info'
./build/runbook --seed 7 --days 90 --out world.json
```

Type `help` for the verbs. Every response ends with a lone `.` on its own
line, so a dumb client can find the end without parsing the body.

## The gates

| Gate | What it asserts | State |
|---|---|---|
| `--health` | A pristine org boots; identifiers are sound; a departed user's login is never reissued; offboarding is idempotent; every verb `help` advertises actually dispatches | live |
| determinism | Same seed reproduces byte-for-byte over 120 simulated days; a different seed diverges; `-O0` and `-O2` agree; the Linux and Windows builds agree | live |
| `--mancheck` | Every command example in every in-game document executes | M1 |
| `--naive` | The naive bot's failure rate stays inside the handoff §8 band | M5 |
| `--play` | A reference agent plays all three acts over the API | M2 |
| `--vacation N` | N days, zero input, against §12 | M7 |

`--health` prints `PENDING` lines for the parts of its handoff definition that
have nothing to check yet. That is on purpose: a check with nothing to check
reports green, and a suite that is green for that reason is worse than no
suite.

The Windows half of the determinism gate needs `mingw-w64` and a 64-bit wine
prefix. Without them it prints `SKIP` and the command to fix it.

## Layout

```
core/rb.h       limits, buffers, the RNG, the hash
core/util.c     lifted from NOMINAL, trimmed
core/world.h/.c the org: people, clock, growth, provenance
core/proto.h/.c the API — the only way anything talks to the world
core/serve.c    the local socket; lifted from NOMINAL's net.c
core/health.c   the health gate
core/main.c     the command line; the only file that knows one exists
```

The core is a library. `main.c` is the only file that knows about a command
line, so everything else links unchanged into the GDExtension when the client
arrives at M3. The world lives in the model; Godot will be a view of it.

## What M1 does next

Appliances from spec files, the API surface generated from those specs, and
one directory appliance you can onboard a user through over the socket. Two
seams are already cut for it: `world_day_advance()` computes the day's hires
and applies them in a separate loop, which is where the ticket generator
lands at M2; and `Session.prov` already distinguishes work done by hand from
work done by script, which is the debt mechanic (§11) that M6 reads back.

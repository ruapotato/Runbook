# RUNBOOK

Factorio's ratchet, applied to IT operations. You start clicking. You end up
with an orchestrator. You win by going on vacation.

The design is in [`handoff.md`](handoff.md) and it is the authority. Anything
in this file that disagrees with it is stale, and the handoff wins.

## Where this is

**M1.** An org, one appliance, and the API. No tickets and no client yet.

The org has forty people and a directory of record with an account, a
department group and a membership for each of them — all of it there before
the player arrives, and all of it attributed to nobody. You can onboard a
forty-first over the socket, by hand through a form or by script through the
API, and the directory will argue with you about it: logins collide,
memberships need their account to exist first, a retried create is not a
duplicate, and one write in two hundred commits and then times out.

A **person and their account are different objects**, and that is the game. A
User is what HR knows. Their login, groups and (later) mailbox and home folder
live in appliances, and putting them there is the work.

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
| `--health` | Specs load and validate; a pristine org boots with its directory intact and following its own naming convention; every endpoint in every spec responds; idempotency, ordering and spent logins behave; every verb `help` advertises dispatches | live |
| determinism | Same seed reproduces byte-for-byte over 120 simulated days; a different seed diverges; `-O0` and `-O2` agree; the Linux and Windows builds agree | live |
| `--mancheck` | Every example in every generated manual executes against a live world; every endpoint is documented | live |
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
core/rb.h        limits, buffers, the RNG, the hash, provenance
core/util.c      lifted from NOMINAL, trimmed
core/yaml.h/.c   the subset of YAML the specs are written in
core/spec.h/.c   appliance specs: load, and above all validate
core/appl.h/.c   instances, records, and calling an endpoint
core/world.h/.c  the org: people, clock, growth, appliances
core/proto.h/.c  the API — the only way anything talks to the world
core/serve.c     the local socket; lifted from NOMINAL's net.c
core/health.c    the health gate
core/mancheck.c  every documented example, executed
core/main.c      the command line; the only file that knows one exists
specs/*.yaml     the appliance library; embedded by tools/mkspecs.sh
```

The core is a library. `main.c` is the only file that knows about a command
line, so everything else links unchanged into the GDExtension when the client
arrives at M3. The world lives in the model; Godot will be a view of it.

## Writing an appliance

One YAML file. It drives the endpoints, the web forms, the manual and the
examples the gate executes — one source, so they cannot disagree.

```sh
$EDITOR specs/my_appliance.yaml
./build/runbook --specs specs/ --mancheck     # iterate without a rebuild
./tools/mkspecs.sh && make check              # embed it and check it in
```

Endpoints declare an operation (`list`, `get`, `create`, `update`, `delete`)
over a named collection, plus the things that make scripting them a game:
`idempotent_on`, `references`, `failure_modes`, `latency_ms`. `--health`
refuses a spec that documents an endpoint it cannot dispatch, a form that
offers a field its endpoint will not take, or a model claiming an API from a
vendor that does not sell one.

## What M2 does next

Tickets: typed objects with `acceptance` checks the game evaluates against
world state. The player never marks anything done. The seam is already cut —
`world_day_advance()` computes the day's hires and applies them in a separate
loop, and that loop is where the ticket generator lands.

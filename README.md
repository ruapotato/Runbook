# RUNBOOK

Factorio's ratchet, applied to IT operations. You start clicking. You end up
with an orchestrator. You win by going on vacation.

The design is in [`handoff.md`](handoff.md) and it is the authority. Anything
in this file that disagrees with it is stale, and the handoff wins.

## Where this is

**M0–M2, M5, M6, M7.** The whole game, headless: an org that grows, tickets
that close only by state verification, three appliances driven from specs,
exceptions that ramp, capacity and provisioning, and the vacation.

`make check` runs six gates in about eight seconds, and the last of them is
the win condition:

```
vacation: 7 days, 4197 users, nobody watching. Day 77.
vacation: PASS  100% of tickets resolved within SLA (want 99%)
vacation: PASS  queue 0 deep at the start, 0 at the end
vacation: PASS  no service went over nominal capacity
vacation: the company ran itself for 7 days. Go on holiday.
```

Still missing: the Godot client (M3) and scripting on the emulated machine
with the macro recorder (M4). Those are the two milestones that need a human
at a keyboard — they answer *is Act I pleasant* and *does the relief land*,
and no gate can answer either.

The org has forty people, fully provisioned across a directory, a mail server
and a file server — all of it there before the player arrives, all of it
attributed to nobody. It grows about 6% a day. Every hire raises an onboarding
ticket with seven acceptance checks, and **the only way to close one is to
make the checks true**. There is no "mark as done" verb; `--health` asserts
by name that there never will be.

A **person and their account are different objects**, and that is the game. A
`User` is what HR knows: hired, into a department, on a day. Their login,
groups, mailbox, home folder and share access live in appliances, and putting
them there is the work.

```
$ ./build/runbook --exec 'ticket.check TCK-00007'
+OK TCK-00007 does not pass yet
PASS account_exists            They have an account in the directory of record.
PASS login_follows_convention  The login is first initial plus family name...
     account_active            The account is active, not left in whatever
                               state it was created in.  -- status is
                               contractor, not active
PASS in_department_group       They are in their department's group.
...
```

That one is a returning contractor. Their account already existed, so a script
that relies on `create` being idempotent gets a 200 and moves on, leaving
somebody whose account exists and does not work. Idempotency is not enough;
you have to reconcile. There is nothing to *diagnose* — the ticket said
`"rehire":"yes"` from the moment it opened — there is something to *handle*,
and that is the difference the whole design rests on (§2).

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
| `--play` | A reference agent plays through the acts over the API, and must keep up: ≤2% of tickets failed, ≥95% within SLA. Reports per-act wall time and where it stalled | live |
| `--naive-gate` | §8's degeneracy band, over three seeds: a bot that does not branch must fail ≤5% of tickets at the Act I wall and ≥35% by the end of Act II | live |
| `--vacation N` | The win condition: N simulated days at 4,000 users with nobody watching — ≥99% within SLA, queue no deeper at the end, no service over nominal for more than 30 in-game minutes. Failing is diagnostic: it names the ticket types your systems did not handle and the service that fell over | live |

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

## The balance harness

Nothing in this game is tuned by argument. The growth model, the exception
ramp, the failure rates and the size of the surname table were all set by
running `--play` and `--naive-gate` and reading the numbers:

```sh
make play     # a competent script must keep up
make naive    # an incompetent one must not
```

Between them they found a runaway follow-up loop that grew the queue 1.4× a
day regardless of play, a use-after-free that only appeared once the queue got
big, and a collision space that was three times smaller than it looked. Every
constant they settled carries a comment saying so.

## Writing a ticket type

One YAML file, like an appliance. Acceptance checks are declarative predicates
over appliance collections, validated at load against the appliance specs — a
ticket whose acceptance names a collection no appliance has could never close,
and that is caught at startup rather than in a playtest.

```yaml
  - id: in_second_group
    check: exists
    when: also_dept          # only for tickets carrying that field
    appliance: directory     # any instance of the kind, not a named one
    collection: memberships
    where: { login: "{account.login}", group: "dept-{also_dept}" }
    doc: "They are also in the second department they were hired into."
```

`{account.login}` is bound by an earlier check, which is how acceptance can
verify work whose exact shape was the player's decision. A check that does not
apply reports `n/a`, never `PASS`.

## Act III

Past about 600 users per appliance an instance goes over nominal and every
call to it gets slower in proportion — and slow calls eat the day budget,
which is the only currency in the game. The world raises a `service.capacity`
ticket; `appl.install` racks another box, and it costs forty in-game minutes
whether a person or a script asks for it.

**Placement and replica count are the decisions, and there is no cabling.**
Nothing else scales: acceptance checks search the whole service rather than a
named appliance, so where you put things is yours to get right. Reference data
(groups, shares) replicates to every instance, because making the player
configure each new box is config management and that is the sequel. Identity
does not shard — `scope: service` on a collection means its key must be free
across every instance, since you can shard storage but never names.

## What M3 does next

The Godot client: a window-manager shell, a generic form renderer driven by
`appl.forms`, per-vendor themes. Everything it needs is already served over
the API, because the client is a client (decision 7). M3 and M4 are the two
milestones that need a human at a keyboard — the questions they answer are
*is Act I pleasant* and *does the relief land*, and no gate can answer those.

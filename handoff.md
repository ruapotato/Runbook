# RUNBOOK — Handoff

**Status:** design locked, no code written.
**Base repo:** new repo. Lifts the machine layer from `~/NOMINAL/`. Does **not** fork it.
**Targets:** Windows and Linux. Single self-contained binary per platform.
**One-line pitch:** Factorio's ratchet, applied to IT operations. You start clicking. You end up with an orchestrator. You win by going on vacation.

---

## 1. Locked decisions

These are settled. Do not relitigate them in code review. If one has to change, change it here first and say why.

| # | Decision | Rationale |
|---|---|---|
| 1 | Name is **RUNBOOK** | The artifact the game produces. Legible to helpdesk and platform engineers alike. Not a joke that ages. |
| 2 | New repo; lift NOMINAL's machine core as a library | NOMINAL's hull, decks, walking, and design docs are actively wrong here. Fresh repo makes carrying them impossible rather than merely discouraged. |
| 3 | Godot client, retained | gdext already links the C machine; Windows build already fought and won; one binary, no runtime deps. Rewriting the client is a yak-shave. |
| 4 | **No network simulation.** Everything is networked by default. | Learned at cost in NOMINAL: correct packets, terrible game. Links are `true`. Depth lives in state, not routes. |
| 5 | Tickets are structured objects, not prose | Everything must be automatable. Prose parsing is not the game. |
| 6 | Appliances are declarative specs, not scenes | Content scales. Agents can author appliances. UI and API generate from one source. |
| 7 | The UI is a client of the API, from commit one | Makes "find the API" a discovery of something always present. Halves the implementation. |
| 8 | Invented vendor and product names throughout | Legal, and it makes interface quality a design lever. |
| 9 | Tickets close by **state verification**, never by player assertion | This is the oracle. Everything else depends on it. |
| 10 | Win condition is a 7-day unattended run ("vacation") | The actual sysadmin fantasy, and machine-checkable. |
| 11 | Ship three tiers only: manual → scripted → scaled | Config management, load balancing, orchestration are sequel scope. |
| 12 | Compounding pressure, never instant game-over | TNI's one-complaint-and-you're-fired is why it feels brittle. |
| 13 | Player scripts run on the emulated machine | The moat. No other game in this space has a real interpreter on a real machine. |
| 14 | Scripting language: **Python subset** (MicroPython-class), not Lua | Audience knows Python. Fallback to Lua only if emulated perf fails the M3 gate. |
| 15 | Macro recorder is the on-ramp to scripting | The UI→script transition is a cliff for most players. This is the single most important accessibility feature in the game. |
| 16 | Failure is deterministic per seed | Agents cannot grade balance otherwise. Players cannot compare runs otherwise. |

---

## 2. Anti-goals

Written down because each one has already been tried and rejected, in this project's lineage or its neighbours.

- **No packet, frame, ARP, VLAN, DHCP, or routing simulation.** Not "later." Not "lightweight." Cabling is not a mechanic here.
- **No diagnosis-as-content.** Fault-finding collapses into a checklist within a few hours. Every prior attempt in this lineage died here. Pressure comes from *volume and growth*, never from hidden faults to discover.
- **No walking, no 3D, no spatial traversal.**
- **No random prose tickets.** If a human has to read free text to know what to do, a script can't do it either.
- **No fail screen for falling behind.** Falling behind generates more work; that is the punishment.
- **No accuracy for its own sake.** The rule from NOMINAL still holds — every technical claim in the game must be true of the game's machine — but *fidelity to real-world protocols is not a goal.* Believable beats accurate.

---

## 3. The fantasy

You are the entire IT department of a company that is growing faster than you are. Day one you can handle everything by hand and it feels fine. By day sixty the company has thirty times the headcount and you have the same two hands, and the only reason the place still runs is that you have spent those sixty days making yourself progressively unnecessary.

The emotional beat the whole game is built around: **the first time a script clears in four seconds a queue that would have taken you forty minutes.** Everything before that exists to make that moment land. Everything after exists because that feeling is repeatable at larger scales.

---

## 4. Core loop

1. Tickets arrive in a queue.
2. You resolve them — by hand, by script, or by a system you built that resolves them without you.
3. Resolved tickets add users and devices to the org.
4. Users and devices generate load and generate more tickets.
5. Volume outgrows your current technique. Go to 2, one tier up.

Growth is **endogenous**. There is no external difficulty curve. Success is the difficulty curve — the users you provision are the load that breaks you.

---

## 5. Progression and escalating counts

Three acts. All numbers below are **starting values for the balance harness**, not sacred. The harness tunes them; the shape is what's locked.

### Act I — By Hand
| | |
|---|---|
| Users | 40 → 150 |
| Tickets/day | 4 → 40 |
| Duration | 15–30 minutes of real play |
| Tools | Web UIs only |
| Ends when | The queue no longer fits in a day |

The UI is **good**. It never becomes bad. It becomes insufficient. A player who finishes Act I should feel competent, not punished — if playtesters describe Act I as tedious, it is too long, not too slow.

Manual user onboarding: 6 form submissions across 3 appliances. ~12 in-game minutes. The day budget is 480 in-game minutes, so the wall lands at roughly 40 tickets/day.

### Act II — Scripted
| | |
|---|---|
| Users | 150 → 800 |
| Tickets/day | 40 → 220 |
| Exception rate | 2% → 18% |
| Tools | API, macro recorder, script editor, scheduler |
| Ends when | Service load, not ticket volume, becomes the constraint |

Volume stops being the problem within an hour. **Exceptions become the problem.** The naive script handles the happy path; the act is about the tail. See §8.

### Act III — Scaled
| | |
|---|---|
| Users | 800 → 4,000 |
| Tickets/day | 220 → 600 |
| Capacity | ~600 users per appliance instance at nominal |
| Tools | Provisioning templates, instance groups, health checks |
| Ends when | The vacation test passes at 4,000 users |

Past nominal capacity, appliance latency climbs and slow-performance tickets appear — the load is the users you created. Provisioning a new instance by hand costs 40 in-game minutes, which does not fit in a day at this volume, so it must be scripted. Placement and replica count are the decisions; there is no cabling.

### Growth model
- Baseline headcount growth: ~6%/day compounding.
- Hiring waves: 15–40 users on seeded days, roughly weekly.
- Attrition: ~1%/day (offboarding tickets — and offboarding is where stale records hurt).
- Unresolved tickets roll over and spawn follow-ups at 0.4× per day unresolved. This is the compounding pressure.

---

## 6. Task domain

The whole game is **object lifecycle**: users, devices, access. Chosen because correctness is a verifiable state, not a judgement call.

**Primary task — user lifecycle.** Create account, apply naming convention, assign department groups, allocate a license, create mailbox, create home folder, grant department shares, record in the directory of record. Offboarding runs it in reverse and is unforgiving of records that were never written.

**Secondary tasks**, in roughly the order they should unlock:

- **Scan-to-folder setup.** Ticket: user X can't scan to folder on printer PWD. Resolution requires branching on real state: does X have a home folder, is the printer's file service configured, has the service credential expired, does X's department use a non-default share path.
- **Bulk scan-to-folder.** A list of users wanting the same thing. Trivial if you scripted onboarding, miserable if you didn't.
- **New printer install.** Order, receive, configure, add users. Vendor choice matters (§9).
- **Printer replacement / migration.** Read config off the dead unit, map users, write to the new one, verify. *The naive script gets everyone provisioned by script and silently misses everyone you set up by hand.* This is the debt mechanic (§11) and it is the most important single ticket type in the game.
- **Capacity expansion.** Things are slow; stand up another instance of a service, or a second directory server.

---

## 7. Ticket schema

Every ticket is a typed object retrievable by API. Ticket types are the difficulty dial: Act I ships one type with three fields; Act III ships polymorphic types with optional fields, bundles, and tickets whose stated problem is a symptom rather than the cause.

```json
{
  "id": "TCK-10432",
  "type": "access.scan_to_folder",
  "opened": "day 34, 09:12",
  "sla_minutes": 480,
  "subject": { "kind": "user", "ref": "u_8823" },
  "target":  { "kind": "device", "ref": "prn_pwd_02" },
  "fields":  { "requested_path": "default" },
  "description": "User cannot scan to folder on PWD printer.",
  "acceptance": ["scan_target_exists", "credential_valid", "test_scan_ok"]
}
```

- `description` is human prose and is **always** redundant with the structured fields. It is flavour and onboarding, never the only source of a required fact.
- `acceptance` names the state checks the game will run. It is visible to the player. Closing is the game evaluating those checks against world state — the player never marks anything done.
- Bundle tickets carry an array of subjects and are otherwise identical, so a correct script handles one and forty with the same code.

---

## 8. Where the difficulty actually lives

The ticket is clean. The **world** is messy. A script that parses a field and calls one endpoint is a form, not a game. The script must inspect state and branch.

Four sources of exception, ramping across Act II:

1. **Collisions and history.** Name collisions. A new hire already in the system as a contractor. Someone in two departments. A department whose group scheme changed last quarter.
2. **Ordering.** The mailbox must exist before the group add succeeds. Reversed, you get a partial object that looks correct and isn't. Verification catches it; the player has to work out why.
3. **Idempotency.** A batch half-completes. Re-running either double-applies or doesn't, depending on how you wrote it. This is the soul of ops scripting and has never been a game mechanic.
4. **Partial failure.** A call times out mid-batch after the write landed. Retrying creates a duplicate. Not retrying leaves a gap. The player learns to make operations idempotent because the game punishes both alternatives.

**Degeneracy gate:** a deliberately naive bot — parse ticket, call the obvious endpoint, no branching, no retry — must fail a rising fraction of tickets: ≤5% at the start of Act II, ≥35% by its end. If the naive bot clears the queue, the exception design is degenerate and the act is dead content. This is a CI gate, not a vibe check.

---

## 9. Vendors and appliances

Vendors are characterised **through interface quality**. This makes purchasing a real decision with a real trade, and it teaches a true thing.

| Vendor archetype | Interface | Cost | Effect on play |
|---|---|---|---|
| The good one | Clean documented API, sane errors, honest status codes | Expensive | Scriptable immediately |
| The cheap one | Web UI only, no API | Cheap | Every unit is permanent manual labour |
| The legacy one | API exists, is XML, lies about status codes, returns 200 on failure | Mid | Scriptable but you must verify everything |
| The flaky one | Good API, poor uptime, rate limits aggressively | Cheap | Forces queuing and backoff |

A player ten hours in will pay double to avoid the cheap one. *Feeling* that preference is the game teaching them something real about procurement.

All names invented. Generic technology nouns (domain controller, print queue, share, mailbox) are fine and should be used — they're the vocabulary. Vendor and product names must not resemble real ones.

**Appliance spec format** — one file per appliance model, drives UI, API, and in-game docs together:

```yaml
model: Caldera 4400 MFP
vendor: caldera
interfaces: [web, api]          # omit api for the cheap vendor
theme: caldera_blue             # per-vendor Godot theme; see §14
state:
  scan_targets: { type: list, item: scan_target }
  credential:   { type: credential, expires_days: 90 }
endpoints:
  - id: list_scan_targets
    method: GET
    latency_ms: 180
    returns: scan_targets
  - id: add_scan_target
    method: POST
    latency_ms: 400
    idempotent_on: [user_ref, path]
    requires: [credential_valid]
    failure_modes: [timeout_after_commit, rate_limited]
forms:                           # the web UI, generated
  - id: scan_target_form
    calls: add_scan_target
    fields: [user_ref, path, format]
```

---

## 10. Time, latency, and failure

- **Act I latency is real seconds.** 3s per form submit. It is a teacher, and 3s is bearable at 5 tickets/day and lethal at 40. That gap *is* the Act I→II pressure.
- **From Act II, latency costs in-game time, not real time.** Scripts must not make the player watch spinners. API calls consume the day budget; the day advances at the speed of the fiction, not the wall clock.
- **API baseline:** 150–500ms per call, per the appliance spec.
- **Rate limits:** 60 requests/minute per appliance instance, per vendor. Exceeding returns a retryable rejection. This is the throughput dial — the belt speed of this game.
- **Transient failure:** 3% baseline, rising to 8% under load, seeded per run.
- **Nasty failure:** 0.5% of writes time out *after committing*. This is the idempotency teacher. It must exist from the first hour of Act II.
- **Stalls:** occasional 8s API stalls to make naive sequential scripts feel slow and reward concurrency.

---

## 11. Provenance and debt

Every object in the world records **how it was created**: by hand, by script, or by a system.

- Work done by hand leaves no record in the player's own data — only in the appliance.
- Work done by script leaves a record the script can read back.
- Migrations, offboarding, and audits read those records.

Consequence: sloppiness in Act I bites in Act III, and past automation is what makes future automation possible. That is the ratchet — the same role belts feeding belts play in Factorio, sourced from provenance instead of throughput. It also gives the player a reason to go back and automate tasks they already "finished," which is where "just one more optimization" comes from.

Do **not** surface this as a stat or a debt meter. It should be discovered when a migration silently misses six people.

---

## 12. Win condition

**The vacation.** Seven simulated days, zero player input, at 4,000 users.

Pass criteria:
- ≥99% of tickets resolved within SLA.
- Queue depth at end ≤ queue depth at start.
- No service below capacity threshold for >30 consecutive in-game minutes.

The player triggers it voluntarily and can abort. Failing it is diagnostic, not fatal — the run report says which ticket types went unhandled and which service fell over, and the player goes back and fixes their systems. This is the endgame loop.

---

## 13. Verification gates

Mirrors NOMINAL's discipline. **Build these before gameplay.** They are the reason this project can be built by agents at all.

| Gate | Asserts |
|---|---|
| `--health` | Pristine org boots; every appliance reachable; every endpoint in every spec responds |
| `--mancheck` | Every command and endpoint example in every in-game doc, man page, and vendor manual actually executes against a live world |
| `--naive` | The naive bot's failure rate stays inside the §8 band per act |
| `--play` | A reference agent plays through all three acts via the API; reports per-act wall time and where it stalled |
| `--vacation N` | N days, zero input, against §12 criteria |
| `--determinism` | Same seed, same run, identical world state hash |

`--play` and `--naive` together are the balance harness. Every number in §5 and §10 is tuned by running them, not by argument.

**Project rule, inherited and non-negotiable:** every technical claim anywhere in this game — vendor manual, in-game doc, ticket description, source comment — must be true of this world, verified by running it. A vendor manual documenting an endpoint that doesn't exist teaches the player to distrust everything, and the trust is the product.

**Model/view rule, inherited:** the world lives in the model. Godot is a view. Everything must be drivable headless over a socket. Anything that cannot be driven that way rots.

---

## 14. Stack

**Lift from `~/NOMINAL/` as a library:**
- RV64IM emulator
- VFS, package database, init and service supervision
- The Makefile gate structure and the socket-driven harness

**Leave behind:** hull generation, deck topology, walking, power/conduit model, combat, crew, `docs/`.

**New:**
- World model in C: org, users, devices, appliances, tickets, load, provenance.
- Appliance spec loader → generates API surface, web forms, and docs from one file.
- Godot client: window manager metaphor, per-vendor themes, generic form renderer driven by appliance specs, terminal, script editor, macro recorder.
- Python-subset interpreter on the emulated machine, with an HTTP-shaped client module.

**Protocol note:** the API *looks* like HTTP. Do not implement HTTP. Request in, response out; latency, rate limits, and failures are numbers. This is the direct lesson of the packet simulation.

**Theming caution:** Godot's default Controls all look like Godot. If a dozen vendor appliances share one look, the vendor-quality mechanic dies quietly. Theme variation per vendor comes from the spec file and is a launch requirement, not polish.

---

## 15. Build order

Each milestone ends with a gate that passes in CI.

- **M0 — Harness first.** Repo, Makefile, `--health`, `--determinism`. No gameplay. Nothing else starts until these are green.
- **M1 — World and one appliance.** Org model, users, one directory appliance from a spec, API only, no client. Onboard a user over the socket.
- **M2 — Tickets and verification.** Ticket generator, `acceptance` evaluation, state-check closing, `--play` with a scripted reference agent. Still no client. **The game should be provably playable headless before it is visible.**
- **M3 — Client and Act I.** Godot window shell, generic form renderer, three appliances, 3s latency, 40→150 users. First playtest. *Question answered: is Act I pleasant?*
- **M4 — The transition.** API discovery, script editor, Python interpreter on the machine, macro recorder. *Question answered: does the relief land?* This is the hypothesis. If it fails here, stop and reconsider — do not build Act III on an unvalidated Act II.
- **M5 — Exceptions.** The four exception classes, `--naive` gate, exception ramp. Act II proper.
- **M6 — Scale.** Capacity model, instance provisioning, printer migration, Act III.
- **M7 — Vacation.** Endgame, run report, `--vacation` gate.
- **M8 — Vendors and content.** Full vendor roster, appliance library, theming, docs. Agent-authored, spec-validated.

M0–M2 and M5–M8 are largely taste-free and can run unattended. M3 and M4 need a human at a keyboard.

---

## 16. Open questions

Flagged honestly; none block M0–M2.

1. **Does the emulated interpreter perform?** Thousands of API calls per simulated day through an RV64IM emulator is the long pole. Measure at M4. Fallback is Lua; second fallback is a native interpreter with the emulator kept for the terminal only, which costs some of the moat.
2. **What exactly does the macro recorder emit?** It has to produce code a non-programmer will read and edit. Probably a flat sequence of named calls with literal arguments, then teach loops by showing the player the repetition. Needs prototyping at M4.
3. **How much does the player see of the org?** A roster view is convenient and might kill the motivation to generate reports from the systems. Lean toward showing less.
4. **Act I length.** 15–30 min is a guess. `--play` gives wall time; playtesters give the verdict.
5. **Is the spreadsheet an explicit object?** The inversion — from maintaining a sheet by hand to *generating* it from the systems — is a strong Act II beat. Unclear whether it's a mechanic or just what a player's script naturally produces.

---

## 17. What kills this project

Named so it can be watched for.

- **Act II never lands.** If the jump from clicking to scripting is a wall rather than a ramp, there is no game. This is the whole bet. M4 exists to answer it early and cheaply.
- **Exceptions are decoration.** If the naive bot clears the queue, Act II is a form-filling simulator with extra steps. `--naive` is the tripwire.
- **Scope creep back into networking or diagnosis.** Both have killed a version of this already.
- **Building the world before validating the loop.** M2 before M3 is not negotiable for this reason.

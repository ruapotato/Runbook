# KESTREL — the design

One ship, one fight, and a console that prints every button you press.

This document is the authority. Anything anywhere else that disagrees with it
is stale.

---

## 0. What happened to the last design

The previous game — a ticket queue where you automate yourself out of an IT
job — is in [`docs/handoff-runbook-superseded.md`](docs/handoff-runbook-superseded.md).
It was built, it passed nine gates, every number in it was set by running a
harness rather than by argument, and it was not fun.

That is worth writing down precisely, because it is the only expensive thing
this project has learned:

- **No decisions in the first hour.** Filling in a form correctly is not a
  choice. There was nothing to be wrong about, so there was nothing to get
  better at.
- **Nothing to watch.** The world advanced when you spent minutes on it. A
  screen that only changes because you changed it has no second thing
  happening in it, and no second thing means no tension.
- **Automation with no spectacle.** The reward for writing a script was that a
  form you never enjoyed filling in got filled in without you. The relief was
  real and it was invisible.

Passing gates measured whether the thing was built correctly and said nothing
about whether it was worth building. That is not an argument against gates —
every gate below is still here — it is an argument for having a gate on the
one question that matters, which is §5.

---

## 1. Locked decisions

These came from the person the game is for, and they are not open.

1. **Real time.** It starts running, not paused -- a keystroke between the
   player and the game makes interface trivia the first thing they learn.
   Pause is still there and still free, because FTL was right that a
   real-time game can be about thinking; it is a thing you reach for, not a
   door you open to begin.
2. **One ship, one fight.** No campaign, no map, no meta-progression of the
   usual kind.
3. **The filesystem survives death.** You lose the ship. You do not lose your
   scripts. That is the run-to-run progression and it is the only one.
4. **Scripting is a thing you can do**, not a thing you must do. A player who
   never opens the terminal should be able to win.
5. **Every UI action shows up in the console as a command.** This is the
   premise. See §3.

## 2. Anti-goals

- Not a programming tutorial. Nobody is taught syntax; they watch it.
- Not a roguelike. There is no run structure to balance.
- No diagnosis-as-content. The game never withholds a reason. Every refusal
  says why, in the model's own words: "only 1 bar spare — take it from
  somewhere first", not "invalid".
- No second channel. The interface cannot do anything a script cannot.

## 3. The premise

Every action on the bridge prints the command that performed it, in the strip
under the ship, in the syntax a script would use.

Give the shields a bar and it says `power shields 3`. Send somebody to the
fire and it says `send Vane 2`. Those are not descriptions of what you did.
They are what you did — the button has no private channel, it calls
`proto_exec` with that line of text, and so does the terminal, and so does a
script.

There is no scripting mode to graduate into and no tutorial that teaches the
syntax. By the time somebody wants a script they have watched the vocabulary
go past a hundred times, and the first one they write is a line they have
already read.

**The goal is to make programmers out of regular people, not to attract
programmers to the game.** The macro recorder is the other half of that ramp:
it watches what you did, finds the repeat, and writes it out as a Python
script with the changing parts pulled into a list — a template to edit, not a
program to admire.

## 4. The economy

Eight reactor bars. Shields, engines, weapons, oxygen, medbay and the
computer all want them, and you cannot have four of those things.

**Scripts run on the ship's computer, and the computer runs on reactor bars.**
With no power in the computer, your automation does nothing at all — the game
says so, in a sentence, when you start a script into a dark computer.

That is the whole design in one rule. Automation is a trade, not a cheat. A
script that watches one thing costs you a shield layer, and whether that is
worth it is the question the fight is asking.

Concretely: `ship_compute_slices()` returns the computer's working bars times
two, and `world_tick` hands that many scheduling slices to the emulated
machine. Nothing else about the guest is throttled.

## 5. The gate that decides whether this is a game

`--fight` flies forty identical fights with two bots. One only shoots. The
other also puts fires out and moves people out of vacuum.

```
fight:   only shooting      32% won, 96s average
fight:   actually playing  100% won, 129s average
```

A hundred percent says the ceiling is reachable, not that the game is easy:
`bot_playing` is a reference, not a person -- it re-evaluates every tick,
counts doorways exactly, and never gets distracted by the terminal it has
open. The number that says something about difficulty is the other one.

`--fight` also asserts the RULES, separately from the balance: that crew are
not where you sent them the instant you ask, that a shut door on the route
costs longer, that the teleporter refuses for each of its three reasons, and
that shooting their engine room damages their engine room and nothing else.
Those are rules a player builds a strategy on, and one that silently stops
holding is worse than one that was never there.

If shooting alone won, the ship would be decoration. If playing well could not
win, the fight would be unfair. **Every constant in `core/ship.c` was set by
running this**, and the band is asserted, so a change that flattens the
decisions fails the build.

This is the gate the last design did not have.

## 6. The model

Eight rooms. Systems with power bars and damage. Crew whose job is decided by
where they stand — a burning room gets fought, a broken one gets repaired, a
working one gets manned, and so `send <who> <room>` is the only crew command
there is. Fire spreads through open doors. Breaches drain air. The raider
fires volleys, which is what makes shields feel fine and then suddenly not.

The dials are all in `core/ship.c` with the harness result next to them.

## 6b. The ship is a directory

Plan 9's idea, and the reason this game has a real machine under it. Every
value about the ship is a file, one value per file, so nothing needs parsing:

```
cat /dev/ship/ready                     -> yes
cat /dev/ship/rooms/weapons/fire        -> 43
echo 3 > /dev/ship/rooms/shields/power
echo open > /dev/ship/rooms/medbay/vent
echo medbay > /dev/crew/Vane/room
```

There is no second implementation behind any of it: every write ends up in
`proto_exec`, the same function the buttons and the terminal and `do()` all
call. So a file accepts exactly what the button accepts, refuses what the
button refuses, and says the same sentence when it does.

`ls /dev/ship` is the documentation. A ship you can `cat` is a ship you can
script without being taught an API, and the shell that is already on the disk
is enough -- which is why `if`, `while` and `/bin/test` were added to it. A
shell with none of those can only write macros, and a macro cannot look before
it acts.

## 7. The machine

Under the bridge is a real RV64IM computer: a disk, an init, a package
database, `/bin/sh` with pipes and loops and shell scripts in files, and a
Python subset at `/bin/py`. It is lifted from NOMINAL and compiled unchanged
against a shim.

The ship is reachable from it through `/bin/rb`, which is a program on that
disk like any other. `run <script>` starts a script running *inside* the
fight, as a daemon, scheduled out of the computer's power.

`do()` is the one native that matters. It takes the envelope off the protocol
response, because the first script anybody writes is
`for line in lines(do("rooms"))` and it should not have to skip a status line
to work.

## 8. Determinism

Every number is integer or `-ffp-contract=off` double, the RNG is splitmix64,
and the desktop advances the fight in fixed steps of a fiftieth of a second
with the remainder carried — so the fight on a 144Hz monitor is the fight the
harness ran.

`make determinism` asserts four things: same seed twice, different seeds
differ, `-O0` and `-O2` agree, and **Linux and Windows agree byte-for-byte**.

## 9. Stack

C11, no third-party dependencies, one self-contained binary per platform.
The client is Godot 4.7 talking to a GDExtension written in plain C against
`gdextension_interface.h`, and it is a client of the same protocol a telnet
session speaks.

## 10. The gates

`make check`:

| gate | what it asserts |
|---|---|
| `fight` | the decisions are worth something (§5) |
| `machine` | the computer boots, the shell works, the language passes its own suite, and a script can fly the ship in real time |
| `client` | the bridge, sensors and a terminal are open at boot; the windows float, **and every click prints its command** -- including that clicking the raider's weapon room sends `fire weapons`, and that the map catches shots actually in flight |
| `games` | the ten mini-games still run |
| `determinism` | §8 |

## 11. What kills this project

- Tuning a number by argument instead of by running the harness.
- Letting the interface do something a script cannot. The moment there are two
  channels, the console is a highlight reel and the premise is dead.
- Making the console a log window off to one side. It is under the ship and it
  scrolls while you play, or nobody reads it.
- Adding a second fight before the first one is fun.

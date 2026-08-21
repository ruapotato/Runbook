# KESTREL

One ship, one fight, and a console that prints every button you press.

A raider has caught you. You have eight reactor bars, six systems that all
want them, three people whose job is decided by where they stand, and a fire
in the engine room.

Under the bridge is a real computer. Every click you make arrives in it as a
line of text — so by the time you want to write a script, you have already
read it a hundred times.

![the bridge](docs/desk.png)

The design is in [`DESIGN.md`](DESIGN.md) and it is the authority. Anything in
this file that disagrees with it is stale.

## The premise

Click the third power pip in the shields room and the console says:

```
power shields 3
```

That is not a description of what you did. It is what you did — the button has
no private channel. The same line works in the terminal:

```
$ rb power shields 3
shields at 3, 0 spare
```

and in a script:

```python
while True:
    s = json(ship())
    if s["weapon"] == "100":
        do("fire")
```

Four lines, and it fires your gun for the rest of the fight while you deal
with the engine room. It is on the disk as `/root/examples/gunner.py`.

**And it costs you.** Scripts run on the ship's computer, and the computer
runs on reactor bars — the same bars the shields want. With no power in the
computer, your automation does nothing at all. Automation is a trade, not a
cheat, and that is the whole game.

## The recorder

There is a record button on the panel. It watches what you do, notices the
repeat, and writes it out as a script with the changing parts pulled into a
list:

```python
work = [
    ["3", "Vane"],
    ["2", "Ash"],
]

for row in work:
    bars = row[0]
    who = row[1]

    do("power shields " + bars)
    do("send " + who + " 4")
```

Add a row and the loop does another one. The goal is to make programmers out
of regular people, not to attract programmers to the game, and a template you
edit is a much shorter walk than a blank file.

## The computer

It is not a themed command box. It is an emulated RV64IM machine with a disk,
an init, a package database, and a shell with pipes, loops, globbing, `$?` and
scripts in files.

```
$ ls /root/examples
README  firewatch.py  gunner.py  selftest.py  selftest.sh  watch.sh
$ rb rooms | grep true
$ py /root/examples/selftest.py
selftest: OK
```

`py` is a Python subset — `if`/`elif`/`else`, `while`, `for`/`in`, `def`,
lists, dicts, and about thirty builtins. It passes its own test suite on the
guest, and the machine gate runs it, because a player debugging their own
logic against an interpreter that is quietly wrong will conclude they cannot
program. Which is the exact opposite of the point.

## Building it

No dependencies. C11 and a compiler.

```
make            # build/runbook
make check      # every gate
make run        # play it, if Godot 4.7 is about
```

Godot is only needed for the client. Everything else — the ship, the machine,
the language, the balance harness — is one self-contained binary, and
`./build/runbook` drops you at a prompt with the whole game behind it.

```
$ ./build/runbook
Kestrel -- a raider is closing.
type 'help'; every response ends with a lone '.'
```

## The gates

```
$ make check
fight:   only shooting      45% won, 86s average
fight:   actually playing   97% won, 106s average
fight: PASS  doing nothing but shooting is not enough
fight: PASS  putting fires out and moving people wins it
fight: PASS  the decisions are worth 52 points
machine: 12 checks, 0 failed
client: client_test: 42 checks, 0 failures
games: games_test: 10 games, 0 failures
determinism: PASS  same seed reproduces byte-identically
determinism: PASS  a different seed produces a different world
determinism: PASS  -O0 and -O2 agree byte-for-byte
determinism: PASS  Linux and Windows agree byte-for-byte
```

`fight` is the one that decides whether this is a game. Two bots fly the same
forty fights — one that only shoots, one that also puts fires out and moves
people out of vacuum — and the gate asserts a band between them. If shooting
alone won, the ship would be decoration. Every constant in `core/ship.c` was
set by running it.

`client` asserts the premise, because it is the one thing that would be
silently easy to lose:

```
PASS  clicking the third power pip gives the shields three bars
PASS  and the console shows `power shields 3` -- the command the click sent
PASS  and the same line showed up in the open terminal, as text
```

## Where the code is

```
core/ship.c        the model: rooms, power, crew, fire, air, the raider
core/proto.c       the protocol. every command, and there is only one door
core/recorder.c    watches you play and writes a script
core/fight.c       the balance harness
core/box.c         the ship's computer, and the bridge into it
machine/           a lifted RV64IM machine: cpu, kernel, vfs, disk image
guest/             what runs ON it, including the Python subset
game/              the Godot client. bridge.gd is the ship
```

`machine/` is borrowed from NOMINAL and compiles unchanged against a shim, so
it stays visibly separate from what this project wrote.

## A note on the name

The repository is called Runbook because the first design was about IT
operations. That design is in `docs/handoff-runbook-superseded.md`, along with
an honest account of why it was thrown away — it passed nine gates and was not
fun, which turned out to be the most useful thing this project has learned.

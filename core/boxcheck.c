/* boxcheck.c — the machine gate.
 *
 * Handoff decision 13 calls the emulated machine the moat, and §16's first
 * open question is whether the interpreter on it PERFORMS -- "thousands of
 * API calls per simulated day through an RV64IM emulator is the long pole.
 * Measure at M4."
 *
 * This is that measurement, and the checks that keep the machine honest. It
 * runs a real /bin/sh on a real disk on an emulated CPU, so every number
 * below is instructions actually executed rather than an estimate.
 */
#include "proto.h"
#include "box.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static int fails, checks;

static void ck(bool cond, const char *what)
{
    checks++;
    if (cond) printf("machine: PASS  %s\n", what);
    else    { printf("machine: FAIL  %s\n", what); fails++; }
}

static void sh(Box *b, const char *line, Buf *out)
{
    buf_clear(out);
    box_sh(b, line, out);
}

static bool has(const Buf *b, const char *s)
{
    return b->p && strstr(b->p, s) != NULL;
}

static double now_ms(void)
{
#ifdef _WIN32
    return 0.0;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
#endif
}

int boxcheck_run(uint64_t seed)
{
    fails = checks = 0;
    World *w = world_new(seed);
    double t0 = now_ms();
    Box *b = world_box(w);
    double boot_ms = now_ms() - t0;
    ck(box_up(b), "the workstation installs and boots");
    printf("machine:       %.0f ms to install and boot an RV64IM machine\n", boot_ms);

    Buf out;
    buf_init(&out);

    /* ---- it is a real machine, not a command box */
    sh(b, "uname -a", &out);
    ck(has(&out, "rv64"), "uname says what it is running on");
    sh(b, "ls /bin", &out);
    ck(has(&out, "sh") && has(&out, "rb"), "there is a /bin with a shell and rb in it");
    sh(b, "cat /etc/hostname", &out);
    ck(out.len > 0 && !has(&out, "not found"), "there is a real filesystem with real files in it");

    /* ---- a shell, with the things that make a shell useful */
    sh(b, "echo one two three | wc", &out);
    ck(out.len > 0 && !has(&out, "not found"), "pipelines carry bytes");
    sh(b, "mkdir /root/gate", &out);
    sh(b, "echo hello > /root/gate/f", &out);
    sh(b, "cat /root/gate/f", &out);
    ck(has(&out, "hello"), "redirection writes a file and cat reads it back");

    /* ---- THE MOAT: a program on the machine reaching the ship.
     *
     * This is the thing that makes the premise work. The computer under the
     * console is a real RV64IM machine, and /bin/rb on it speaks the same
     * commands the buttons send -- so a script written here can fly the ship
     * while the ship is being shot at. */
    sh(b, "rb ship", &out);
    ck(has(&out, "hull"), "a program on the machine can see the ship");
    sh(b, "rb power shields 3", &out);
    ck(has(&out, "+OK"), "and change it");

    /* ---- DOES THE LANGUAGE WORK, in full. */
    /* ------------------------------------------------- /dev/ship
     *
     * THE SHIP IS A DIRECTORY. This is the check that says so, and it drives
     * the game entirely through the filesystem: read a value, write a value,
     * read it back, and confirm the model actually moved. If this passes, the
     * shell that is already on this disk is a complete scripting language for
     * the game and nobody has to be taught an API to use it. */
    sh(b, "cat /dev/ship/hull", &out);
    ck(has(&out, "16"), "the ship's hull is a file you can cat");

    sh(b, "cat /dev/ship/ready", &out);
    ck(has(&out, "no"), "and whether the gun is ready is a word, not a percentage");

    sh(b, "echo 3 > /dev/ship/rooms/shields/power", &out);
    sh(b, "cat /dev/ship/rooms/shields/power", &out);
    ck(has(&out, "3"), "writing to a room's power file routes power to it");

    /* AND THE MODEL REALLY MOVED, asked through the other door. A device file
     * that only agrees with itself proves nothing. */
    sh(b, "rb rooms | grep shields", &out);
    ck(has(&out, "\"bars\":3"), "and the ship agrees, asked the other way");

    sh(b, "echo open > /dev/ship/rooms/medbay/vent", &out);
    sh(b, "cat /dev/ship/rooms/medbay/vent", &out);
    ck(has(&out, "open"), "an airlock is a file too");

    sh(b, "echo medbay > /dev/crew/Vane/room", &out);
    sh(b, "cat /dev/crew/Vane/doing", &out);
    ck(has(&out, "walking"), "and sending somebody is a write to /dev/crew");

    /* A REFUSAL HAS TO BE AUDIBLE. Silence here is the failure mode that
     * teaches a player these files do not work. */
    sh(b, "echo 9 > /dev/ship/rooms/shields/power", &out);
    ck(has(&out, "refused"), "a value the ship will not take says so");

    /* THE SHELL CAN PLAY THE GAME, which is the point of all of the above.
     * `if` and `while` were added for exactly this: a shell with neither can
     * only write macros, and a macro cannot look before it acts. */
    sh(b, "if [ a = a ]; then echo YES; else echo NO; fi", &out);
    ck(has(&out, "YES") && !has(&out, "NO"), "the shell has if/then/else");

    sh(b, "echo 3 > /tmp/n; while [ $(cat /tmp/n) != 0 ]; do echo turn; echo 0 > /tmp/n; done", &out);
    ck(has(&out, "turn"), "and while, with the condition re-read every turn");

    sh(b, "if [ a = a ]; then if [ b = b ]; then echo IN; fi; echo OUT; fi", &out);
    ck(has(&out, "IN") && has(&out, "OUT"), "and blocks nest without eating each other");

    sh(b, "py /root/examples/selftest.py", &out);
    ck(has(&out, "selftest: OK"), "the language passes its own suite");
    if (!has(&out, "selftest: OK") && out.p) printf("machine:       %s\n", out.p);

    sh(b, "/root/examples/selftest.sh", &out);
    ck(has(&out, "selftest.sh: done") && has(&out, "status 0"),
       "and the shell has pipes, loops, redirection and a working $?");

    /* ---- and a script can fly her */
    sh(b, "py -c 'print(json(ship())[\"hull\"])'", &out);
    ck(out.len > 0 && !has(&out, "error"), "a script can read the ship as a dict");

    /* ---- THE PERFORMANCE QUESTION (§16.1), measured rather than guessed.
     *
     * The number that matters is API calls per simulated day. Act III runs at
     * about 600 tickets a day and a careful script makes roughly ten calls
     * per ticket, so six thousand calls a day is the target to beat. */
    const int N = 60;
    double t1 = now_ms();
    for (int i = 0; i < N; i++) sh(b, "rb world.info", &out);
    double per = (now_ms() - t1) / (double)N;

    /* AND THE WAY AUTOMATION ACTUALLY RUNS: many calls inside ONE shell,
     * which is what a script is. Each interactive line pays for a fresh
     * /bin/sh and a fresh /bin/rb -- two ELF loads and two process
     * teardowns -- and a loop pays that once. The difference between the two
     * numbers is the cost of the prompt, and it is worth knowing which of
     * them the vacation test is actually paying. */
    sh(b, "echo 'for i in a b c d e f g h i j; do rb world.info; done' > /root/gate/loop", &out);
    double t2 = now_ms();
    for (int i = 0; i < 6; i++) sh(b, "/root/gate/loop", &out);
    double per_script = (now_ms() - t2) / 60.0;

    printf("machine:       %.2f ms per `rb` call typed at a prompt\n", per);
    printf("machine:       %.2f ms per `rb` call inside a script loop\n", per_script);
    /* THE NUMBER THAT MATTERS IS PER TICK, not per fight. The game runs at
     * ten ticks a second and a control loop asks the ship once a tick, so a
     * script needs to answer in well under a hundred milliseconds or the
     * fight stutters while somebody's automation thinks. */
    printf("machine:       ~%.0f calls/second scripted, so a 100-second fight costs ~%.1f s\n",
           1000.0 / (per_script > 0 ? per_script : 1), 1000.0 * per_script / 1000.0);

    /* AND THE SAME QUESTION FOR THE LANGUAGE, which is the one §16 actually
     * asked: the interpreter is a bytecode VM running on an emulated CPU, so
     * every one of its instructions costs several of the machine's. */
    double t3 = now_ms();
    sh(b, "py -c 'i = 0\nwhile i < 200:\n    ship()\n    i = i + 1'", &out);
    double per_py = (now_ms() - t3) / 200.0;
    printf("machine:       %.2f ms per API call from a py script (%.0f/second)\n",
           per_py, 1000.0 / (per_py > 0 ? per_py : 1));
    /* Ten milliseconds a call, ten calls a tick, ten ticks a second: at the
     * limit a script can just keep up with real time. Anything slower and the
     * player watches their own automation lag the fight. */
    ck(per_script < 10.0, "and it is fast enough to fly the ship in real time");

    buf_free(&out);
    world_free(w);
    printf("machine: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}

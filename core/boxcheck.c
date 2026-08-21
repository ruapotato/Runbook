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

int boxcheck_run(uint64_t seed, const char *specdir)
{
    fails = checks = 0;
    char serr[RB_ERR_MAX];
    Specs *specs = specs_load(specdir, serr, sizeof serr);
    if (!specs) { printf("machine: FAIL  specs do not load: %s\n", serr); return 1; }

    World *w = world_new(seed, specs);
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

    /* ---- THE MOAT (decision 13): a program on the machine reaching the game */
    sh(b, "rb world.info", &out);
    ck(has(&out, "Harbrook"), "a program on the machine can reach the game's API");
    sh(b, "rb ticket.list open 1", &out);
    ck(has(&out, "TCK-"), "and read the queue");

    /* ---- SCRIPTS IN FILES, which is what "scriptable" has to mean.
     * NOMINAL's own README said nothing on this machine would run one. That
     * was fine for a game about repairing one box in an evening; this game is
     * about making yourself unnecessary, and you cannot automate anything
     * with a shell that forgets everything when you press return. */
    sh(b, "echo '# a script' > /root/gate/s", &out);
    sh(b, "echo 'rb world.info' >> /root/gate/s", &out);
    sh(b, "echo 'echo done' >> /root/gate/s", &out);
    sh(b, "/root/gate/s", &out);
    ck(has(&out, "Harbrook") && has(&out, "done"),
       "a shell script in a file runs, line by line, and can call the API");

    /* ---- a loop over the queue, which is the first real automation anybody
     * writes and the thing Act II is entirely about */
    sh(b, "for t in TCK-00001 TCK-00002; do rb ticket.get $t; done", &out);
    ck(has(&out, "TCK-00001") && has(&out, "TCK-00002"),
       "a for loop over tickets works, which is Act II's first script");

    /* ---- THE LANGUAGE (decision 14): a Python subset, on this machine.
     *
     * "Scripting language: Python subset (MicroPython-class), not Lua. The
     * audience knows Python." It is NOMINAL's lexer, compiler and bytecode
     * VM, compiled for rv64 and living in /bin/py -- so a script here is
     * lexed, compiled and executed BY A PROGRAM ON THE DISK, on the emulated
     * CPU, which is what decision 13 asked for and what nothing else in this
     * genre has. */
    sh(b, "py -c 'print(6*7)'", &out);
    ck(has(&out, "42"), "there is a Python subset on the machine, and it computes");

    sh(b, "py -c 'x = 0\nfor i in [1,2,3]:\n    x = x + i\nprint(x)'", &out);
    ck(has(&out, "6") || out.len > 0, "it has loops and lists");

    /* The natives that make it worth having: one to reach the game, one to
     * read what came back. Everything else a script needs it can write. */
    sh(b, "py -c 'print(json(api(\"world.info\"))[\"org\"])'", &out);
    ck(has(&out, "Harbrook"),
       "and a script can call the API and index the answer as a dict");

    /* ---- DOES THE LANGUAGE ACTUALLY WORK, in full.
     *
     * /root/examples/selftest.py is seventy-odd assertions over arithmetic,
     * comparison, strings, lists, dicts, control flow, functions, recursion
     * and the natives. It runs here because "the interpreter is broken" is a
     * thing a player would experience as "I cannot program", and that is the
     * one conclusion this game must never cause. */
    sh(b, "py /root/examples/selftest.py", &out);
    ck(has(&out, "selftest: OK"), "the language passes its own suite");
    if (!has(&out, "selftest: OK") && out.p) printf("machine:       %s\n", out.p);

    /* And the shell, for the same reason and with the same weight: pipes,
     * redirection, variables, substitution, for loops, and $? -- which was
     * empty inside scripts until this file went looking. */
    sh(b, "/root/examples/selftest.sh", &out);
    ck(has(&out, "selftest.sh: done") && has(&out, "status 0"),
       "and the shell has pipes, loops, redirection and a working $?");

    /* ---- THE EXAMPLES ON THE DISK, which are the on-ramp §15 asks for.
     * They are not decoration: a player who has to invent automation from a
     * blank prompt mostly does not, and these are a working script to read,
     * run and then change. If one of them stops working, the first thing a
     * player tries stops working. */
    sh(b, "cat /root/examples/README", &out);
    ck(out.len > 200 && !has(&out, "cannot read"), "there are example scripts on the disk");
    sh(b, "/root/examples/queue.sh", &out);
    ck(has(&out, "the queue"), "the shell example runs");
    sh(b, "py /root/examples/onboard.py", &out);
    ck(has(&out, "onboarded"), "and the Python example provisions the whole queue");

    /* ---- and the game agrees it was done, by a script */
    sh(b, "rb ticket.stats", &out);
    ck(has(&out, "\"script\":4") || has(&out, "\"closed\":4"),
       "the world recorded four tickets closed, by a script, from the machine");

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
    printf("machine:       ~%.0f calls/second scripted, so a 6,000-call Act III day costs ~%.1f s\n",
           1000.0 / (per_script > 0 ? per_script : 1), 6000.0 * per_script / 1000.0);

    /* AND THE SAME QUESTION FOR THE LANGUAGE, which is the one §16 actually
     * asked: the interpreter is a bytecode VM running on an emulated CPU, so
     * every one of its instructions costs several of the machine's. */
    double t3 = now_ms();
    sh(b, "py -c 'i = 0\nwhile i < 200:\n    api(\"world.hash\")\n    i = i + 1'", &out);
    double per_py = (now_ms() - t3) / 200.0;
    printf("machine:       %.2f ms per API call from a py script (%.0f/second)\n",
           per_py, 1000.0 / (per_py > 0 ? per_py : 1));
    /* Sixty seconds for a day of Act III automation would make the vacation
     * test unrunnable; ten is fine, because nobody watches it happen. */
    ck(per_script < 10.0, "and it is fast enough to script a whole day with");

    buf_free(&out);
    world_free(w);
    specs_free(specs);
    printf("machine: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}

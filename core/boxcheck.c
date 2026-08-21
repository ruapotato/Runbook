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
    /* Sixty seconds for a day of Act III automation would make the vacation
     * test unrunnable; ten is fine, because nobody watches it happen. */
    ck(per_script < 10.0, "and it is fast enough to script a whole day with");

    buf_free(&out);
    world_free(w);
    specs_free(specs);
    printf("machine: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}

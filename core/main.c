/* main.c — the command line.
 *
 * One binary, no runtime dependencies, Windows and Linux. It runs the gates,
 * it drives a fight from stdin, and it serves the socket. The Godot client
 * links the same core and gets exactly these capabilities and no others.
 */
#include "proto.h"
#include "serve.h"
#include "box.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int boxcheck_run(uint64_t seed);                      /* boxcheck.c */
int fight_run(uint64_t seed, int runs, bool verbose); /* fight.c    */

#define DEFAULT_SEED 424242ULL
#define DEFAULT_PORT 7711

static void usage(void)
{
    puts("runbook — one ship, one fight, and a computer to fly her from.\n"
         "\n"
         "  runbook                     play at a prompt (try: help)\n"
         "  runbook --serve [--port P]  listen on 127.0.0.1 (default 7711)\n"
         "  runbook --exec 'command'    run one command and exit\n"
         "\n"
         "  runbook --fight [--runs N]  the balance gate: does doing nothing lose,\n"
         "                              and does playing well win?\n"
         "  runbook --fight --verbose   ...and print one fight, so you can read it\n"
         "  runbook --machine           the ship's computer: boots, shells, scripts\n"
         "  runbook --seed N --out F    play a fixed fight and dump the world\n"
         "\n"
         "Every mode drives the same commands. There is no back door into the\n"
         "model, on purpose: anything not reachable this way cannot be tested\n"
         "and rots.");
}

/* Drive a fight from stdin, one command per line. How a script, a pipe or a
 * gate plays without opening a socket. */
static int run_stdin(World *w, bool banner)
{
    Session s;
    proto_open(&s, w);
    if (banner) {
        Buf hello;
        buf_init(&hello);
        proto_hello(&s, &hello);
        fwrite(hello.p, 1, hello.len, stdout);
        buf_free(&hello);
    }
    char line[RB_LINE_MAX];
    while (fgets(line, sizeof line, stdin)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        Buf out;
        buf_init(&out);
        bool keep = proto_exec(&s, line, &out);
        fwrite(out.p, 1, out.len, stdout);
        fflush(stdout);
        buf_free(&out);
        if (!keep) break;
    }
    return 0;
}

int main(int argc, char **argv)
{
    uint64_t seed = DEFAULT_SEED;
    int  port = DEFAULT_PORT, runs = 0, ticks = 0;
    bool serve = false, verbose = false, machine = false, fight = false;
    const char *out_path = NULL, *exec_line = NULL;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if      (!strcmp(a, "--serve"))   serve = true;
        else if (!strcmp(a, "--verbose")) verbose = true;
        else if (!strcmp(a, "--machine")) machine = true;
        else if (!strcmp(a, "--fight"))   fight = true;
        else if (!strcmp(a, "--seed")  && i + 1 < argc) seed = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(a, "--runs")  && i + 1 < argc) runs = atoi(argv[++i]);
        else if (!strcmp(a, "--ticks") && i + 1 < argc) ticks = atoi(argv[++i]);
        else if (!strcmp(a, "--port")  && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(a, "--out")   && i + 1 < argc) out_path = argv[++i];
        else if (!strcmp(a, "--exec")  && i + 1 < argc) exec_line = argv[++i];
        else if (!strcmp(a, "--help") || !strcmp(a, "-h")) { usage(); return 0; }
        else {
            /* An unrecognised flag is an error, not a shrug. A typo in a gate
             * invocation that silently runs the default is how a suite ends up
             * measuring something nobody asked for. */
            fprintf(stderr, "runbook: unknown option: %s\n", a);
            usage();
            return 2;
        }
    }

    if (machine) return boxcheck_run(seed);
    if (fight)   return fight_run(seed, runs, verbose);

    World *w = world_new(seed);

    /* A fixed number of seconds, for the determinism gate: the same fight,
     * played by nobody, has to come out identical. */
    if (ticks > 0) {
        ship_pause(&w->ship, false);
        for (int i = 0; i < ticks && !w->ship.over; i++) world_tick(w, 0.1);
    }

    int rc = 0;
    if (exec_line) {
        Session s;
        proto_open(&s, w);
        Buf out;
        buf_init(&out);
        proto_exec(&s, exec_line, &out);
        fwrite(out.p, 1, out.len, stdout);
        buf_free(&out);
    } else if (serve) {
        rc = serve_run(w, port, verbose);
    } else if (!out_path) {
        rc = run_stdin(w, true);
    }

    if (out_path) {
        Buf dump;
        buf_init(&dump);
        world_dump(w, &dump);
        FILE *f = fopen(out_path, "wb");
        if (!f) { fprintf(stderr, "runbook: cannot write %s\n", out_path); rc = 1; }
        else { fwrite(dump.p, 1, dump.len, f); fclose(f); }
        buf_free(&dump);
    }

    world_free(w);
    return rc;
}

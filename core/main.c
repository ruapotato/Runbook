/* main.c — the command line.
 *
 * One binary, no runtime dependencies, Windows and Linux (handoff decision
 * 3). It runs the gates, it drives a world from stdin, and it serves the
 * socket. The Godot client links the same core as a library and gets exactly
 * these capabilities and no others.
 */
#include "proto.h"
#include "serve.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int health_run(uint64_t seed, const char *specdir);   /* health.c */
int mancheck_run(uint64_t seed, const char *specdir); /* mancheck.c */

#define DEFAULT_SEED 424242ULL
#define DEFAULT_PORT 7711

static void usage(void)
{
    puts("runbook — Factorio's ratchet, applied to IT operations.\n"
         "\n"
         "  runbook --health [--seed N]        the health gate\n"
         "  runbook --mancheck                 every documented example, executed\n"
         "  runbook --seed N --days D --out F  run D days, write the world to F\n"
         "  runbook --serve [--port P]         listen on 127.0.0.1 (default 7711)\n"
         "  runbook --exec 'verb args'         run one command and exit\n"
         "  runbook                            read commands from stdin\n"
         "\n"
         "Every mode drives the same API. There is no back door into the model,\n"
         "on purpose: anything not reachable this way cannot be tested and rots.");
}

/* Drive a world from stdin, one command per line. This is how a script, a
 * pipe or a reference agent plays without opening a socket — and how the
 * determinism gate will drive a scripted run once there is something to
 * script (M2). */
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
    int  days = 0, port = DEFAULT_PORT;
    bool health = false, serve = false, verbose = false, mancheck = false;
    const char *out_path = NULL, *exec_line = NULL, *specdir = NULL;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if      (!strcmp(a, "--health"))  health = true;
        else if (!strcmp(a, "--mancheck")) mancheck = true;
        else if (!strcmp(a, "--specs") && i + 1 < argc) specdir = argv[++i];
        else if (!strcmp(a, "--serve"))   serve = true;
        else if (!strcmp(a, "--verbose")) verbose = true;
        else if (!strcmp(a, "--seed")  && i + 1 < argc) seed = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(a, "--days")  && i + 1 < argc) days = atoi(argv[++i]);
        else if (!strcmp(a, "--port")  && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(a, "--out")   && i + 1 < argc) out_path = argv[++i];
        else if (!strcmp(a, "--exec")  && i + 1 < argc) exec_line = argv[++i];
        else if (!strcmp(a, "--help") || !strcmp(a, "-h")) { usage(); return 0; }
        else {
            /* An unrecognised flag is an error, not a shrug. A typo in a gate
             * invocation that silently runs the default world is how a suite
             * ends up measuring something nobody asked for. */
            fprintf(stderr, "runbook: unknown option: %s\n", a);
            usage();
            return 2;
        }
    }

    if (health)   return health_run(seed, specdir);
    if (mancheck) return mancheck_run(seed, specdir);

    /* Specs load once and the world borrows them. A spec that fails to load
     * stops the program here rather than producing a world with two thirds of
     * an appliance in it. */
    char serr[RB_ERR_MAX];
    Specs *specs = specs_load(specdir, serr, sizeof serr);
    if (!specs) { fprintf(stderr, "runbook: %s\n", serr); return 1; }

    World *w = world_new(seed, specs);
    for (int i = 0; i < days; i++) world_day_advance(w);

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
    specs_free(specs);
    return rc;
}

/* box.c — the workstation, wired to the game.
 *
 * Small on purpose. Everything hard here was done in NOMINAL: this file
 * installs one machine, boots it, and points the machine's one outward
 * syscall at proto_exec().
 */
#include "box.h"
#include "proto.h"
#include "nom.h"
#include "machine.h"
#include "kernel.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

struct Box {
    Machine m;
    Session s;
    Buf     bootlog;
    bool    installed;
};

/* THE BRIDGE, AND WHY IT IS A GLOBAL.
 *
 * kernel.c takes a plain function pointer, because it is lifted code and the
 * smallest possible edit was the point. That means one hook for the process,
 * which is fine and worth saying out loud: there is one player, at one
 * workstation, in one world. If a second world ever runs in the same process
 * -- a gate comparing two seeds, say -- this is the thing that has to become
 * a per-machine pointer, and the assert below is what will notice.
 */
static Box *g_box = NULL;

static void box_api(Machine *m, const char *line, Buf *out)
{
    (void)m;
    if (!g_box) { buf_puts(out, "-ERR no world attached\n.\n"); return; }
    proto_exec(&g_box->s, line, out);
}

Box *box_new(World *w, uint64_t seed)
{
    Box *b = rb_alloc(sizeof *b);
    memset(b, 0, sizeof *b);
    buf_init(&b->bootlog);

    machine_install(&b->m, seed);
    b->installed = true;
    machine_boot(&b->m);

    proto_open(&b->s, w);
    /* WORK DONE FROM THE MACHINE IS WORK DONE BY A SCRIPT (handoff §11).
     *
     * Not by hand: a person clicking a form is PROV_HAND, and that
     * distinction is the whole debt mechanic. Typing `rb` at a prompt is a
     * grey area -- it is a person, doing it one line at a time -- but it is
     * also the exact thing they are about to put in a file and run nightly,
     * and calling it script from the first line means the provenance does not
     * change under them the day they automate it. */
    b->s.prov = PROV_SCRIPT;

    g_box = b;
    rb_api_hook = box_api;
    return b;
}

void box_free(Box *b)
{
    if (!b) return;
    if (g_box == b) { g_box = NULL; rb_api_hook = NULL; }
    if (b->installed) machine_free(&b->m);
    buf_free(&b->bootlog);
    rb_free(b);
}

/* The machine's daemons get their slice. `kernel_tick` is NOMINAL's, and it
 * is exactly the right shape: cooperative, budgeted in instructions, and
 * deterministic -- a script that runs off the end of its budget resumes
 * where it left off next tick rather than being killed. */
void box_run_slices(Box *b, int slices)
{
    if (!b || slices <= 0) return;
    Buf console;
    buf_init(&console);
    kernel_tick(&b->m, slices, &console);
    buf_free(&console);
}

bool box_start(Box *b, const char *path, char *err, size_t errcap)
{
    if (!b) { snprintf(err, errcap, "no machine"); return false; }
    Buf console;
    buf_init(&console);
    int64_t rc = kernel_start_daemon(&b->m, "/bin/py", path, "script", 0, &console, NULL);
    /* THE MACHINE'S OWN WORDS, not ours. "could not start" is what a wrapper
     * says; the kernel knows whether the file was missing, not executable or
     * not an ELF, and that sentence is the difference between a player fixing
     * it in ten seconds and giving up. */
    if (rc < 0) {
        snprintf(err, errcap, "%s", console.len ? console.p : "the computer refused it");
        for (char *p = err; *p; p++) if (*p == '\n') { *p = 0; break; }
        buf_free(&console);
        return false;
    }
    buf_free(&console);
    return true;
}

void box_sh(Box *b, const char *line, Buf *out)
{
    if (!b) { buf_puts(out, "no machine\n"); return; }
    kernel_run(&b->m, line, out);
}

bool box_up(const Box *b) { return b && b->installed; }

void box_boot_log(const Box *b, Buf *out)
{
    if (b && b->bootlog.p) buf_put(out, b->bootlog.p, b->bootlog.len);
}

/* ------------------------------------------------------------ the disk
 * A file browser reads the machine's OWN filesystem through these, not a
 * copy of it kept in the client. Two opinions about what is on a disk is one
 * opinion too many, and it is exactly the mistake NOMINAL's model/view rule
 * exists to stop. */
void box_list(Box *b, const char *path, Buf *out)
{
    if (!b) return;
    vfs_list(&b->m.disk, path, out);
}

bool box_read(Box *b, const char *path, Buf *out)
{
    if (!b) return false;
    return vfs_read(&b->m.disk, path, out) == IO_OK;
}

bool box_write(Box *b, const char *path, const char *data, size_t len)
{
    if (!b) return false;
    return vfs_write(&b->m.disk, path, data, len) == IO_OK;
}

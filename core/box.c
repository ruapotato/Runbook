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
#include <stdarg.h>

struct Box {
    Machine m;
    Session s;
    Buf     bootlog;
    bool    installed;
    /* WHAT A RUNNING SCRIPT PRINTS.
     *
     * The daemon's console used to be a local Buf inside box_start, freed the
     * moment the process was launched -- so a script that started fine and
     * then went wrong printed its error into a buffer nobody owned. Debugging
     * one meant guessing. It lives here now, and everything new in it is
     * copied into the ship's log, which is what the bridge console shows and
     * what `rb log` prints.
     *
     * Which makes `print()` in a script, and `echo` in a shell script, a
     * working way to see what your own automation is thinking. */
    Buf     script_out;
    size_t  script_seen;
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


/* ------------------------------------------------------ /dev/ship
 *
 * PLAN 9'S IDEA, AND THE REASON THIS GAME HAS A REAL MACHINE UNDER IT.
 *
 * Everything about the ship is a file. Not a status blob you parse -- one
 * value per file, so the shell that is already on this disk is a complete
 * scripting language for the game:
 *
 *     cat /dev/ship/ready            -> yes
 *     echo 3 > /dev/ship/rooms/shields/power
 *     echo open > /dev/ship/rooms/weapons/vent
 *     echo medbay > /dev/crew/Vane/room
 *
 *     while true; do
 *         if [ $(cat /dev/ship/ready) = yes ]; then echo shields > /dev/ship/fire; fi
 *     done
 *
 * There is no second implementation behind any of this: every write ends up
 * in proto_exec, the same function the buttons and the terminal and `do()`
 * all call. So a file that accepts a value accepts exactly what the button
 * accepts, refuses what the button refuses, and says the same sentence when
 * it does -- and the recorder sees it, because the recorder watches
 * proto_exec.
 *
 * A ship you can `cat` is a ship you can script without being taught an API.
 */
enum {
    D_HULL = 1, D_HULLMAX, D_SHIELDS, D_SHIELDSMAX, D_WEAPON, D_READY,
    D_POWER, D_CLOCK, D_EVADE, D_PAUSED, D_OVER, D_WON, D_LOG, D_CTL, D_FIRE,
    D_EHULL, D_ECHARGE, D_ESHIELDS, D_EEVADE, D_ENAME,
    /* per-room and per-crew fields carry their index in the high digits */
    D_R_POWER = 100, D_R_CAP, D_R_DAMAGE, D_R_OXYGEN, D_R_FIRE,
    D_R_BREACH, D_R_DOOR, D_R_VENT, D_R_CREW, D_R_SYSTEM,
    D_C_ROOM = 200, D_C_HEALTH, D_C_WALKING, D_C_DOING,
    D_E_DAMAGE = 300, D_E_WORKING,
};
#define DEV_ID(field, idx)  ((field) * 1000 + (idx))
#define DEV_FIELD(id)       ((id) / 1000)
#define DEV_INDEX(id)       ((id) % 1000)

static void dev_num(Buf *out, int v)      { buf_printf(out, "%d\n", v); }
static void dev_word(Buf *out, const char *s) { buf_printf(out, "%s\n", s); }
/* yes/no, not true/false -- because `[ $(cat ...) = yes ]` is what somebody
 * writes in a shell, and the shell on this machine has no booleans. */
static void dev_yn(Buf *out, bool b)      { buf_puts(out, b ? "yes\n" : "no\n"); }

static IoStatus dev_read(VNode *n, Buf *out, void *ctx)
{
    (void)ctx;
    if (!g_box || !g_box->s.w) return IO_ERR;
    Ship *s = &g_box->s.w->ship;
    int f = DEV_FIELD(n->id), i = DEV_INDEX(n->id);

    if (f >= DEV_FIELD(DEV_ID(D_E_DAMAGE, 0))) { /* never taken: keeps the
                                                  * compiler honest below */ }
    switch (n->id >= 1000 ? f * 1000 : n->id) {
    case D_HULL:       dev_num(out, (int)s->hull); return IO_OK;
    case D_HULLMAX:    dev_num(out, (int)s->hull_max); return IO_OK;
    case D_SHIELDS:    dev_num(out, s->shields); return IO_OK;
    case D_WEAPON:     dev_num(out, (int)(s->weapon_charge * 100)); return IO_OK;
    /* THE FILE THE WHOLE IDEA IS NAMED AFTER. A script that wants to know
     * whether it can shoot reads one file and compares it to one word. */
    case D_READY:      dev_yn(out, s->weapon_charge >= 1.0); return IO_OK;
    case D_POWER:      dev_num(out, ship_power_free(s)); return IO_OK;
    case D_CLOCK:      dev_num(out, (int)s->clock); return IO_OK;
    case D_EVADE:      dev_num(out, (int)(ship_evade(s) * 100)); return IO_OK;
    case D_PAUSED:     dev_yn(out, s->paused); return IO_OK;
    case D_OVER:       dev_yn(out, s->over); return IO_OK;
    case D_WON:        dev_yn(out, s->won); return IO_OK;
    case D_ENAME:      dev_word(out, s->enemy.name); return IO_OK;
    case D_EHULL:      dev_num(out, (int)s->enemy.hull); return IO_OK;
    case D_ECHARGE:    dev_num(out, (int)(s->enemy.charge * 100)); return IO_OK;
    case D_ESHIELDS:   dev_num(out, s->enemy.shields); return IO_OK;
    case D_EEVADE:     dev_num(out, (int)(ship_enemy_evade(s) * 100)); return IO_OK;
    case D_LOG:
        for (int k = 0; k < s->nlog; k++) buf_printf(out, "%s\n", s->log[k]);
        return IO_OK;
    default: break;
    }

    if (f >= D_C_ROOM && f < D_E_DAMAGE) {
        if (i < 0 || i >= s->ncrew) return IO_ERR;
        const Crew *c = &s->crew[i];
        switch (f) {
        case D_C_ROOM:    dev_word(out, s->room[c->room].name); return IO_OK;
        case D_C_HEALTH:  dev_num(out, (int)(c->health * 100)); return IO_OK;
        case D_C_WALKING: dev_yn(out, c->step >= 0); return IO_OK;
        case D_C_DOING: {
            /* WHAT THEY ARE ACTUALLY DOING, in one word, worked out the same
             * way the model works it out -- from where they are standing. */
            const Room *r = &s->room[c->room];
            if (!c->alive)            dev_word(out, "dead");
            else if (c->step >= 0)    dev_word(out, "walking");
            else if (r->fire > 0.0)   dev_word(out, "firefighting");
            else if (r->sys.damage>0) dev_word(out, "repairing");
            else if (r->sys.kind != SYS_NONE) dev_word(out, "manning");
            else                      dev_word(out, "idle");
            return IO_OK;
        }
        default: return IO_ERR;
        }
    }

    if (f >= D_E_DAMAGE) {
        if (i < 0 || i >= s->enemy.nroom) return IO_ERR;
        const EnemyRoom *r = &s->enemy.room[i];
        switch (f) {
        case D_E_DAMAGE:  dev_num(out, r->damage); return IO_OK;
        case D_E_WORKING: dev_num(out, r->cap - r->damage < 0 ? 0 : r->cap - r->damage);
                          return IO_OK;
        default: return IO_ERR;
        }
    }

    if (f >= D_R_POWER) {
        if (i < 0 || i >= s->nroom) return IO_ERR;
        const Room *r = &s->room[i];
        switch (f) {
        case D_R_POWER:  dev_num(out, r->sys.bars); return IO_OK;
        case D_R_CAP:    dev_num(out, r->sys.cap); return IO_OK;
        case D_R_DAMAGE: dev_num(out, r->sys.damage); return IO_OK;
        case D_R_OXYGEN: dev_num(out, (int)(r->oxygen * 100)); return IO_OK;
        case D_R_FIRE:   dev_num(out, (int)(r->fire * 100)); return IO_OK;
        case D_R_BREACH: dev_yn(out, r->breach); return IO_OK;
        case D_R_DOOR:   dev_word(out, r->door_open ? "open" : "shut"); return IO_OK;
        case D_R_VENT:   dev_word(out, r->vent_open ? "open" : "shut"); return IO_OK;
        case D_R_SYSTEM: dev_word(out, sys_name(r->sys.kind)); return IO_OK;
        case D_R_CREW: {
            for (int k = 0; k < s->ncrew; k++)
                if (s->crew[k].alive && s->crew[k].room == i && s->crew[k].step < 0)
                    buf_printf(out, "%s\n", s->crew[k].name);
            return IO_OK;
        }
        default: return IO_ERR;
        }
    }
    return IO_ERR;
}

/* Every write becomes a command. There is no second path into the model, so
 * a file cannot drift away from the button that does the same thing. */
static IoStatus dev_run(const char *fmt, ...)
{
    char line[RB_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);

    Buf out;
    buf_init(&out);
    proto_exec(&g_box->s, line, &out);
    bool bad = out.len > 4 && out.p[0] == '-';
    if (bad) {
        /* THE MODEL'S OWN SENTENCE, onto the Vfs error, so `echo 9 >
         * .../power` prints "shields takes 4 bars at most" rather than
         * "write error". A device file that cannot explain itself is worse
         * than the command it replaced. */
        char why[NOM_ERR_MAX];
        size_t k = 0;
        for (size_t j = 5; j < out.len && out.p[j] != '\n' && k < sizeof why - 1; j++)
            why[k++] = out.p[j];
        why[k] = 0;
        snprintf(g_box->m.disk.err, sizeof g_box->m.disk.err, "%s", why);
        /* AND INTO THE SHIP'S LOG, which is the one place the player is
         * already looking -- the bridge console shows it, `rb log` prints it,
         * and /dev/ship/log is a file. The Vfs error string never leaves the
         * host; without this the refusal is invisible from inside the
         * machine, which is where the person who typed it is standing. */
        ship_log(&g_box->s.w->ship, "refused: %s", why);
    }
    buf_free(&out);
    return bad ? IO_ERR : IO_OK;
}

static IoStatus dev_write(VNode *n, const char *data, size_t len, void *ctx)
{
    (void)ctx;
    if (!g_box || !g_box->s.w) return IO_ERR;
    Ship *s = &g_box->s.w->ship;

    /* Trailing newline from `echo` is not part of the value. */
    char val[128];
    size_t k = 0;
    for (size_t i = 0; i < len && k < sizeof val - 1; i++) {
        if (data[i] == '\n' || data[i] == '\r') break;
        val[k++] = data[i];
    }
    val[k] = 0;

    int f = DEV_FIELD(n->id), i = DEV_INDEX(n->id);
    if (n->id < 1000) {
        switch (n->id) {
        case D_CTL:   return dev_run("%s", val);
        case D_FIRE:  return dev_run("fire %s", val);
        case D_PAUSED:
            return dev_run("%s", (!strcmp(val, "yes") || !strcmp(val, "1")) ? "pause" : "resume");
        default: return IO_ERR;
        }
    }
    if (f >= D_C_ROOM && f < D_E_DAMAGE) {
        if (i < 0 || i >= s->ncrew) return IO_ERR;
        if (f != D_C_ROOM) return IO_ERR;
        return dev_run("send %s %s", s->crew[i].name, val);
    }
    if (f >= D_R_POWER && f < D_C_ROOM) {
        if (i < 0 || i >= s->nroom) return IO_ERR;
        switch (f) {
        case D_R_POWER: return dev_run("power %s %s", sys_name(s->room[i].sys.kind), val);
        case D_R_DOOR:  return dev_run("door %s %s", s->room[i].name, val);
        case D_R_VENT:  return dev_run("vent %s %s", s->room[i].name, val);
        default: return IO_ERR;
        }
    }
    return IO_ERR;
}

/* MODE BITS, SET EXPLICITLY. vfs_mkdev leaves them at zero, and a device
 * with mode 0 is listed by `ls` and refused by `cat` -- which is exactly the
 * shape of bug that makes a player think the feature is broken rather than
 * that a permission is wrong. It is also the second time this project has
 * hit it, the first being files under implicitly created directories.
 *
 * A read-only value is 0444 and a settable one is 0644, so `ls -l` tells you
 * which files you can write before you try. That is the whole reason a mode
 * column exists. */
static void dev_mode(Vfs *fs, const char *path, unsigned mode)
{
    VNode *n = vfs_lookup(fs, path);
    if (n) n->mode = mode;
}
static void dev_ro(Vfs *fs, const char *path, int field, int idx)
{
    vfs_mkdev(fs, path, dev_read, NULL, DEV_ID(field, idx));
    dev_mode(fs, path, 0444);
}
static void dev_rw(Vfs *fs, const char *path, int field, int idx)
{
    vfs_mkdev(fs, path, dev_read, dev_write, DEV_ID(field, idx));
    dev_mode(fs, path, 0644);
}
static void dev_plain(Vfs *fs, const char *path, int id, bool writable)
{
    vfs_mkdev(fs, path, dev_read, writable ? dev_write : NULL, id);
    dev_mode(fs, path, writable ? 0644 : 0444);
}

static void box_mount_ship(Box *b)
{
    Vfs *fs = &b->m.disk;
    Ship *s = &b->s.w->ship;
    char p[NOM_PATH_MAX];

    vfs_mkdir(fs, "/dev/ship");
    dev_plain(fs, "/dev/ship/hull",     D_HULL,     false);
    dev_plain(fs, "/dev/ship/hull_max", D_HULLMAX,  false);
    dev_plain(fs, "/dev/ship/shields",  D_SHIELDS,  false);
    dev_plain(fs, "/dev/ship/weapon",   D_WEAPON,   false);
    dev_plain(fs, "/dev/ship/ready",    D_READY,    false);
    dev_plain(fs, "/dev/ship/power",    D_POWER,    false);
    dev_plain(fs, "/dev/ship/clock",    D_CLOCK,    false);
    dev_plain(fs, "/dev/ship/evade",    D_EVADE,    false);
    dev_plain(fs, "/dev/ship/over",     D_OVER,     false);
    dev_plain(fs, "/dev/ship/won",      D_WON,      false);
    dev_plain(fs, "/dev/ship/log",      D_LOG,      false);
    dev_plain(fs, "/dev/ship/paused",   D_PAUSED,   true);
    /* Write-mostly: `echo weapons > /dev/ship/fire` and `echo "power shields
     * 3" > /dev/ship/ctl`. ctl is the escape hatch -- anything `help` lists. */
    dev_plain(fs, "/dev/ship/fire",     D_FIRE,     true);
    dev_plain(fs, "/dev/ship/ctl",      D_CTL,      true);

    vfs_mkdir(fs, "/dev/ship/enemy");
    dev_plain(fs, "/dev/ship/enemy/name",    D_ENAME,    false);
    dev_plain(fs, "/dev/ship/enemy/hull",    D_EHULL,    false);
    dev_plain(fs, "/dev/ship/enemy/shields", D_ESHIELDS, false);
    dev_plain(fs, "/dev/ship/enemy/charge",  D_ECHARGE,  false);
    dev_plain(fs, "/dev/ship/enemy/evade",   D_EEVADE,   false);

    vfs_mkdir(fs, "/dev/ship/enemy/rooms");
    for (int i = 0; i < s->enemy.nroom; i++) {
        const char *nm = s->enemy.room[i].name;
        snprintf(p, sizeof p, "/dev/ship/enemy/rooms/%s", nm);
        vfs_mkdir(fs, p);
        snprintf(p, sizeof p, "/dev/ship/enemy/rooms/%s/damage", nm);
        dev_ro(fs, p, D_E_DAMAGE, i);
        snprintf(p, sizeof p, "/dev/ship/enemy/rooms/%s/working", nm);
        dev_ro(fs, p, D_E_WORKING, i);
    }

    /* ROOMS BY NAME, NOT BY NUMBER. `/dev/ship/rooms/weapons/power` is a
     * path somebody can guess, and guessing correctly is the whole feeling
     * this layout is for. */
    vfs_mkdir(fs, "/dev/ship/rooms");
    for (int i = 0; i < s->nroom; i++) {
        const char *nm = s->room[i].name;
        snprintf(p, sizeof p, "/dev/ship/rooms/%s", nm);
        vfs_mkdir(fs, p);
        struct { const char *leaf; int field; bool rw; } fields[] = {
            { "power",  D_R_POWER,  true  },
            { "cap",    D_R_CAP,    false },
            { "damage", D_R_DAMAGE, false },
            { "oxygen", D_R_OXYGEN, false },
            { "fire",   D_R_FIRE,   false },
            { "breach", D_R_BREACH, false },
            { "door",   D_R_DOOR,   true  },
            { "vent",   D_R_VENT,   true  },
            { "crew",   D_R_CREW,   false },
            { "system", D_R_SYSTEM, false },
        };
        for (size_t k = 0; k < sizeof fields / sizeof fields[0]; k++) {
            snprintf(p, sizeof p, "/dev/ship/rooms/%s/%s", nm, fields[k].leaf);
            if (fields[k].rw) dev_rw(fs, p, fields[k].field, i);
            else              dev_ro(fs, p, fields[k].field, i);
        }
    }

    /* CREW BY NAME, and `echo medbay > /dev/crew/Vane/room` sends them. */
    vfs_mkdir(fs, "/dev/crew");
    for (int i = 0; i < s->ncrew; i++) {
        const char *nm = s->crew[i].name;
        snprintf(p, sizeof p, "/dev/crew/%s", nm);
        vfs_mkdir(fs, p);
        snprintf(p, sizeof p, "/dev/crew/%s/room", nm);
        dev_rw(fs, p, D_C_ROOM, i);
        snprintf(p, sizeof p, "/dev/crew/%s/health", nm);
        dev_ro(fs, p, D_C_HEALTH, i);
        snprintf(p, sizeof p, "/dev/crew/%s/walking", nm);
        dev_ro(fs, p, D_C_WALKING, i);
        snprintf(p, sizeof p, "/dev/crew/%s/doing", nm);
        dev_ro(fs, p, D_C_DOING, i);
    }
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
    /* AFTER the boot, because the boot builds the disk this grafts onto, and
     * after g_box, because every callback reads through it. */
    box_mount_ship(b);
    return b;
}

void box_free(Box *b)
{
    if (!b) return;
    if (g_box == b) { g_box = NULL; rb_api_hook = NULL; }
    if (b->installed) machine_free(&b->m);
    buf_free(&b->bootlog);
    buf_free(&b->script_out);
    rb_free(b);
}

static void box_drain(Box *b);

/* The machine's daemons get their slice. `kernel_tick` is NOMINAL's, and it
 * is exactly the right shape: cooperative, budgeted in instructions, and
 * deterministic -- a script that runs off the end of its budget resumes
 * where it left off next tick rather than being killed. */
void box_run_slices(Box *b, int slices)
{
    if (!b || slices <= 0) return;
    /* THE SAME BUFFER THE DAEMONS WERE STARTED WITH, so a script's output
     * accumulates in one place rather than into a fresh buffer that is thrown
     * away every tick -- which is how a script could fail every single tick
     * and never say a word. */
    kernel_tick(&b->m, slices, &b->script_out);
    box_drain(b);
}

bool box_start(Box *b, const char *path, char *err, size_t errcap)
{
    if (!b) { snprintf(err, errcap, "no machine"); return false; }
    /* WHICH INTERPRETER, decided by the name -- because there are two ways to
     * script this ship and neither one is the special case. `.sh` is the
     * shell, and the shell can play the whole game now that /dev/ship exists;
     * everything else is the Python subset. A player who writes a shell
     * script and is told "py: syntax error" learns that their language is not
     * welcome, which is the opposite of the point. */
    size_t plen = strlen(path);
    const char *interp = (plen > 3 && !strcmp(path + plen - 3, ".sh")) ? "/bin/sh" : "/bin/py";
    int64_t rc = kernel_start_daemon(&b->m, interp, path, "script", 0, &b->script_out, NULL);
    /* THE MACHINE'S OWN WORDS, not ours. "could not start" is what a wrapper
     * says; the kernel knows whether the file was missing, not executable or
     * not an ELF, and that sentence is the difference between a player fixing
     * it in ten seconds and giving up. */
    if (rc < 0) {
        const char *why = b->script_out.len > b->script_seen
                        ? b->script_out.p + b->script_seen : NULL;
        snprintf(err, errcap, "%s", why && *why ? why : "the computer refused it");
        for (char *p = err; *p; p++) if (*p == '\n') { *p = 0; break; }
        b->script_seen = b->script_out.len;
        return false;
    }
    b->script_seen = b->script_out.len;
    return true;
}

/* Everything the running scripts have printed since last time, one log line
 * per output line. Called after every slice, so a script that says something
 * is heard in the same second it says it. */
static void box_drain(Box *b)
{
    if (!b || !g_box || !g_box->s.w) return;
    while (b->script_seen < b->script_out.len) {
        size_t start = b->script_seen;
        size_t e = start;
        while (e < b->script_out.len && b->script_out.p[e] != '\n') e++;
        char line[96];
        size_t n = e - start;
        if (n >= sizeof line) n = sizeof line - 1;
        memcpy(line, b->script_out.p + start, n);
        line[n] = 0;
        if (line[0]) ship_log(&g_box->s.w->ship, "script: %s", line);
        b->script_seen = (e < b->script_out.len) ? e + 1 : e;
    }
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

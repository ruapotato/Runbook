/* recorder.c — turning what you did into a script you can read.
 *
 * Decision 15 called this the single most important accessibility feature in
 * the game, and the goal it serves is making programmers out of regular
 * people rather than attracting programmers. It survived the pivot unchanged
 * in spirit and fits the new game better: what it records now is a fight.
 *
 * The whole file is one idea: the player has already written this program by
 * playing, and all that is missing is somebody typing it out.
 */
#include "world.h"
#include "recorder.h"
#include <string.h>
#include <stdio.h>

void recorder_start(World *w, const char *name)
{
    Recorder *r = &w->rec;
    memset(r, 0, sizeof *r);
    r->on = true;
    snprintf(r->name, sizeof r->name, "%s", (name && name[0]) ? name : "recorded");
}

void recorder_stop(World *w)  { w->rec.on = false; }
void recorder_clear(World *w) { memset(&w->rec, 0, sizeof w->rec); }

void recorder_step(World *w, const char *line)
{
    Recorder *r = &w->rec;
    if (!r->on) return;
    if (r->nstep >= REC_MAX_STEPS) { r->overflowed = true; return; }
    RecStep *s = &r->step[r->nstep];
    memset(s, 0, sizeof *s);
    s->at = w->ship.clock;

    const char *p = line;
    while (*p && s->nwords < REC_MAX_WORDS) {
        while (*p == ' ') p++;
        if (!*p) break;
        int n = 0;
        while (*p && *p != ' ' && n < RB_VAL_MAX - 1) s->word[s->nwords][n++] = *p++;
        s->word[s->nwords][n] = 0;
        s->nwords++;
    }
    if (s->nwords) r->nstep++;
}

/* ------------------------------------------------------------ the repeat
 *
 * FINDING THE LOOP is the trick, and it is simpler than it sounds: somebody
 * who did the same thing to three rooms did the same sequence of verbs three
 * times. Look for the shortest period the recording repeats at.
 *
 * Shortest, not longest, because three steps done twice should become a loop
 * of three -- not a loop of six that runs once, which is the same flat script
 * wearing a hat.
 */
static bool same_shape(const RecStep *a, const RecStep *b)
{
    if (a->nwords != b->nwords) return false;
    /* The VERB has to match. The arguments are what we expect to differ --
     * that is what makes them variables. */
    return strcmp(a->word[0], b->word[0]) == 0;
}

static int find_period(const Recorder *r)
{
    for (int len = 1; len <= r->nstep / 2; len++) {
        if (r->nstep % len) continue;
        bool ok = true;
        for (int i = len; i < r->nstep && ok; i++)
            if (!same_shape(&r->step[i], &r->step[i % len])) ok = false;
        if (ok) return len;
    }
    return 0;
}

/* ------------------------------------------------------- the variables */
#define REC_MAX_VARS 6

typedef struct {
    char name[RB_NAME_MAX];
    char value[REC_MAX_STEPS][RB_VAL_MAX];
} RecVar;

typedef struct {
    RecVar var[REC_MAX_VARS];
    int    nvar;
    int    reps;
    int    slot[REC_MAX_STEPS][REC_MAX_WORDS];
} RecPlan;

/* A NAME A PERSON WOULD HAVE CHOSEN. The verb says what the argument is: the
 * second word of `send` is somebody's name, the second word of `power` is a
 * system. Naming it `who` or `system` rather than `arg1` is the difference
 * between a script somebody edits and a script somebody looks at once. */
static const char *arg_name(const char *verb, int pos)
{
    if (!strcmp(verb, "power")) return pos == 1 ? "system" : "bars";
    if (!strcmp(verb, "send"))  return pos == 1 ? "who" : "room";
    if (!strcmp(verb, "door"))  return pos == 1 ? "room" : "state";
    if (!strcmp(verb, "fire"))  return "target";
    return "value";
}

static int var_for(RecPlan *p, const char *name, int reps, char v[][RB_VAL_MAX])
{
    for (int i = 0; i < p->nvar; i++) {
        bool same = true;
        for (int k = 0; k < reps && same; k++)
            if (strcmp(p->var[i].value[k], v[k])) same = false;
        if (same) return i;
    }
    if (p->nvar >= REC_MAX_VARS) return -1;
    RecVar *nv = &p->var[p->nvar];
    snprintf(nv->name, sizeof nv->name, "%s", name);
    for (int i = 0; i < p->nvar; i++)
        if (!strcmp(p->var[i].name, nv->name))
            snprintf(nv->name, sizeof nv->name, "%s%d", name, p->nvar + 1);
    for (int k = 0; k < reps; k++) snprintf(nv->value[k], RB_VAL_MAX, "%s", v[k]);
    return p->nvar++;
}

static void plan_build(const Recorder *r, int period, RecPlan *p)
{
    memset(p, 0, sizeof *p);
    p->reps = r->nstep / period;
    for (int s = 0; s < period; s++) {
        for (int a = 1; a < r->step[s].nwords; a++) {
            p->slot[s][a] = -1;
            char vals[REC_MAX_STEPS][RB_VAL_MAX];
            bool varies = false;
            for (int k = 0; k < p->reps; k++) {
                const RecStep *st = &r->step[k * period + s];
                snprintf(vals[k], RB_VAL_MAX, "%s", a < st->nwords ? st->word[a] : "");
                if (k && strcmp(vals[k], vals[0])) varies = true;
            }
            if (varies) p->slot[s][a] = var_for(p, arg_name(r->step[s].word[0], a), p->reps, vals);
        }
    }
}

/* ---------------------------------------------------------------- output */
static void emit(Buf *out, const RecStep *s, const RecPlan *p, int idx, const char *indent)
{
    buf_printf(out, "%sdo(\"%s", indent, s->word[0]);
    bool open_string = true;
    for (int a = 1; a < s->nwords; a++) {
        int slot = p ? p->slot[idx][a] : -1;
        if (slot < 0) {
            if (!open_string) { buf_puts(out, " + \" "); open_string = true; }
            else buf_putc(out, ' ');
            buf_puts(out, s->word[a]);
        } else {
            if (open_string) { buf_puts(out, " \" + "); open_string = false; }
            else             { buf_puts(out, " + \" \" + "); }
            buf_puts(out, p->var[slot].name);
        }
    }
    if (open_string) buf_puts(out, "\")\n");
    else             buf_puts(out, ")\n");
}

void recorder_script(const World *w, Buf *out)
{
    const Recorder *r = &w->rec;
    if (!r->nstep) {
        buf_puts(out,
            "# Nothing was recorded.\n"
            "#\n"
            "# Press record, then play -- route some power, send somebody to a\n"
            "# fire, take a shot. Stop, and what you did will be here as a\n"
            "# script you can run again.\n");
        return;
    }

    int period = find_period(r);
    RecPlan plan;
    if (period) plan_build(r, period, &plan);

    buf_printf(out, "# %s.py -- recorded during a fight.\n#\n", r->name);

    if (!period) {
        buf_printf(out,
            "# %d thing%s you did, in order.\n"
            "#\n"
            "# do(...) sends one command to the ship -- the same command the\n"
            "# button sent. That is all a button ever was.\n"
            "#\n"
            "# Start it in a fight with:  run /root/scripts/%s.py\n"
            "#\n"
            "# DO THE SAME THING TWICE BEFORE YOU RECORD and this comes out as a\n"
            "# loop instead, with the parts that changed pulled into a list. It\n"
            "# is worth doing once just to see it.\n\n",
            r->nstep, r->nstep == 1 ? "" : "s", r->name);
        for (int i = 0; i < r->nstep; i++) emit(out, &r->step[i], NULL, 0, "");
    } else {
        buf_printf(out, "# You did the same %d thing%s %d times.\n#\n",
                   period, period == 1 ? "" : "s", plan.reps);
        if (plan.nvar) {
            buf_printf(out,
                "# The %d thing%s that changed each time %s in the list below.\n"
                "# Add a row and the loop does another one; that is the whole idea.\n"
                "#\n"
                "# Start it with:  run /root/scripts/%s.py\n\n",
                plan.nvar, plan.nvar == 1 ? "" : "s",
                plan.nvar == 1 ? "is" : "are", r->name);
            buf_puts(out, "work = [\n");
            for (int k = 0; k < plan.reps; k++) {
                buf_puts(out, "    [");
                for (int i = 0; i < plan.nvar; i++)
                    buf_printf(out, "%s\"%s\"", i ? ", " : "", plan.var[i].value[k]);
                buf_puts(out, "],\n");
            }
            buf_puts(out, "]\n\nfor row in work:\n");
            for (int i = 0; i < plan.nvar; i++)
                buf_printf(out, "    %s = row[%d]\n", plan.var[i].name, i);
            buf_putc(out, '\n');
        } else {
            buf_printf(out, "# Nothing changed between them.\n#\n"
                            "# Start it with:  run /root/scripts/%s.py\n\n"
                            "for row in [1, 2]:\n", r->name);
        }
        for (int s = 0; s < period; s++) emit(out, &r->step[s], &plan, s, "    ");
    }

    buf_puts(out,
        "\n# WHAT THIS DOES NOT DO YET.\n"
        "#\n"
        "# It does these things once, in order, the moment you start it. It does\n"
        "# not WATCH anything. A script that is worth leaving running looks at\n"
        "# the ship and decides -- something like:\n"
        "#\n"
        "#   while True:\n"
        "#       s = json(ship())\n"
        "#       if s[\"weapon\"] == \"100\":\n"
        "#           do(\"fire\")\n"
        "#\n"
        "# That one is four lines and it fires your gun for the rest of the\n"
        "# fight. It is also the last thing anybody writes by hand, because\n"
        "# after that they start writing the interesting ones.\n");
}

void recorder_status(const World *w, Buf *out)
{
    const Recorder *r = &w->rec;
    int period = r->nstep ? find_period(r) : 0;
    buf_printf(out, "{\"recording\":%s,\"name\":\"%s\",\"steps\":%d,\"repeats\":%d,\"loop_body\":%d}",
               r->on ? "true" : "false", r->name, r->nstep,
               period ? r->nstep / period : 0, period);
}

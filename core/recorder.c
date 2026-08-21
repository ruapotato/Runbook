/* recorder.c — turning what you did into a script you can read.
 *
 * The whole file is one idea: the player has already written this program by
 * doing it, and all that is missing is somebody typing it out.
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
    r->started_day = w->day;
    snprintf(r->name, sizeof r->name, "%s", (name && name[0]) ? name : "recorded");
}

void recorder_stop(World *w)  { w->rec.on = false; }
void recorder_clear(World *w) { memset(&w->rec, 0, sizeof w->rec); }

void recorder_step(World *w, const char *target, const char *action,
                   const Field *args, int nargs, bool by_hand)
{
    Recorder *r = &w->rec;
    if (!r->on) return;
    if (r->nstep >= REC_MAX_STEPS) { r->overflowed = true; return; }
    RecStep *s = &r->step[r->nstep++];
    memset(s, 0, sizeof *s);
    snprintf(s->target, sizeof s->target, "%s", target);
    snprintf(s->action, sizeof s->action, "%s", action);
    s->by_hand = by_hand;
    for (int i = 0; i < nargs && i < SPEC_MAX_FIELDS; i++) s->arg[s->nargs++] = args[i];
}

/* ------------------------------------------------------------ the repeat
 *
 * FINDING THE LOOP is the whole trick, and it is simpler than it sounds: a
 * player onboarding two people did the same sequence of (appliance, endpoint)
 * pairs twice. So look for the shortest period the whole recording repeats
 * at, and if there is one, the recording is a loop with that body.
 *
 * Shortest, not longest, because six steps done twice should become a loop of
 * six -- not a loop of twelve that runs once, which is the same flat script
 * wearing a hat.
 */
static bool same_shape(const RecStep *a, const RecStep *b)
{
    return strcmp(a->target, b->target) == 0 && strcmp(a->action, b->action) == 0;
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

/* ------------------------------------------------------- the variables
 *
 * Within one position of the loop body, an argument whose value is the same
 * every time round is a constant and stays written out. One that changes is
 * the thing the player was really doing, and it becomes a variable.
 *
 * AND VALUES ARE UNIFIED ACROSS THE BODY, which is what makes the output read
 * like something a person would write. A login appears in five of the six
 * calls; noticing that they are the same string means the script says `login`
 * five times instead of inventing login, login_2 and mailbox_login. That one
 * rule is the difference between a script somebody edits and a script
 * somebody looks at once.
 */
#define REC_MAX_VARS 8

typedef struct {
    char name[RB_NAME_MAX];
    /* value[i] is what this variable held on iteration i */
    char value[REC_MAX_STEPS][RB_VAL_MAX];
    bool quoted;              /* any value has a space in it */
    /* DERIVED VALUES, which is the part that makes the output look like
     * something a person wrote.
     *
     * An address is a login with "@harbrook.example" on the end. A group is
     * "dept-" and a department. A player who types those out twice has, in
     * their head, a rule -- and the recorder can see it, because the same
     * substring is sitting in both columns of its own table.
     *
     * So it writes the rule instead of the data:
     *
     *     address = login + "@harbrook.example"
     *
     * which is not just shorter. It is the first thing in the file that is
     * about the JOB rather than about the six things that happened, and it is
     * the line a player edits when the mail domain changes. */
    int  from;                /* index of the variable it is built from, -1 */
    char prefix[RB_VAL_MAX];
    char suffix[RB_VAL_MAX];
} RecVar;

typedef struct {
    RecVar var[REC_MAX_VARS];
    int    nvar;
    int    reps;
    /* which variable each (step, arg) uses, or -1 for a literal */
    int    slot[REC_MAX_STEPS][SPEC_MAX_FIELDS];
} RecPlan;

static bool ident_ok(const char *s)
{
    if (!s[0] || (s[0] >= '0' && s[0] <= '9')) return false;
    for (const char *p = s; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_')) return false;
    return true;
}

static int var_for(RecPlan *p, const char *name, int reps, char v[][RB_VAL_MAX])
{
    /* Already have a variable holding exactly these values? Then it is the
     * same thing under another field's name. */
    for (int i = 0; i < p->nvar; i++) {
        bool same = true;
        for (int k = 0; k < reps && same; k++)
            if (strcmp(p->var[i].value[k], v[k])) same = false;
        if (same) return i;
    }
    if (p->nvar >= REC_MAX_VARS) return -1;
    RecVar *nv = &p->var[p->nvar];
    /* A field called `user_ref` makes a fine variable name; one called
     * `quota_mb` that happens to vary does too. Anything that is not a legal
     * name gets a numbered one rather than producing a script that will not
     * parse. */
    nv->from = -1;
    nv->quoted = false;
    for (int k = 0; k < reps; k++)
        for (const char *c = v[k]; *c; c++)
            if (*c == ' ' || *c == '\t') nv->quoted = true;
    if (ident_ok(name)) snprintf(nv->name, sizeof nv->name, "%s", name);
    else                snprintf(nv->name, sizeof nv->name, "value%d", p->nvar + 1);
    for (int i = 0; i < p->nvar; i++)
        if (!strcmp(p->var[i].name, nv->name))
            snprintf(nv->name, sizeof nv->name, "%s%d", name, p->nvar + 1);
    for (int k = 0; k < reps; k++) snprintf(nv->value[k], RB_VAL_MAX, "%s", v[k]);
    return p->nvar++;
}

/* Is `a` always `prefix + b + suffix`? Worked out from the first iteration
 * and then CHECKED against every other one, because a coincidence in one row
 * is not a rule -- and a recorder that guesses wrong produces a script that
 * quietly does the wrong thing, which is worse than one that produces six
 * boring lines. */
static bool derive(RecVar *a, const RecVar *b, int reps)
{
    if (!b->value[0][0]) return false;
    const char *hit = strstr(a->value[0], b->value[0]);
    if (!hit) return false;
    size_t plen = (size_t)(hit - a->value[0]);
    size_t blen = strlen(b->value[0]);

    char prefix[RB_VAL_MAX], suffix[RB_VAL_MAX];
    snprintf(prefix, sizeof prefix, "%.*s", (int)plen, a->value[0]);
    snprintf(suffix, sizeof suffix, "%s", a->value[0] + plen + blen);
    /* A rule that is the whole value is not a rule, it is the same variable
     * twice -- and those were already unified. */
    if (!prefix[0] && !suffix[0]) return false;

    for (int k = 1; k < reps; k++) {
        char want[RB_VAL_MAX * 3];
        snprintf(want, sizeof want, "%s%s%s", prefix, b->value[k], suffix);
        if (strcmp(want, a->value[k])) return false;
    }
    snprintf(a->prefix, sizeof a->prefix, "%s", prefix);
    snprintf(a->suffix, sizeof a->suffix, "%s", suffix);
    return true;
}

static void plan_derive(RecPlan *p)
{
    for (int i = 1; i < p->nvar; i++) {
        for (int j = 0; j < i; j++) {
            /* Only from a variable that is itself a column of the table, so
             * the assignments can be emitted in order and there is no chain
             * to resolve. */
            if (p->var[j].from >= 0) continue;
            if (derive(&p->var[i], &p->var[j], p->reps)) { p->var[i].from = j; break; }
        }
    }
}

static void plan_build(const Recorder *r, int period, RecPlan *p)
{
    memset(p, 0, sizeof *p);
    p->reps = r->nstep / period;
    for (int s = 0; s < period; s++) {
        for (int a = 0; a < r->step[s].nargs; a++) {
            p->slot[s][a] = -1;
            char vals[REC_MAX_STEPS][RB_VAL_MAX];
            bool varies = false;
            for (int k = 0; k < p->reps; k++) {
                const RecStep *st = &r->step[k * period + s];
                const char *v = "";
                /* The same argument by NAME, because a form and a typed call
                 * may not order them identically. */
                for (int b = 0; b < st->nargs; b++)
                    if (!strcmp(st->arg[b].k, r->step[s].arg[a].k)) v = st->arg[b].v;
                snprintf(vals[k], RB_VAL_MAX, "%s", v);
                if (k && strcmp(vals[k], vals[0])) varies = true;
            }
            if (varies) p->slot[s][a] = var_for(p, r->step[s].arg[a].k, p->reps, vals);
        }
    }
    plan_derive(p);
}

/* ---------------------------------------------------------------- output */
static bool needs_quotes(const char *v)
{
    for (const char *p = v; *p; p++) if (*p == ' ' || *p == '\t') return true;
    return !v[0];
}

/* One api() line. The literal parts are inside the string; the variables are
 * concatenated in, which is the shape a person writes by hand and therefore
 * the shape they will be able to edit. */
static void emit_call(Buf *out, const RecStep *s, const RecPlan *p, int idx, const char *indent)
{
    buf_printf(out, "%sapi(\"api.call %s %s", indent, s->target, s->action);
    bool open_string = true;
    for (int a = 0; a < s->nargs; a++) {
        int slot = p ? p->slot[idx][a] : -1;
        if (slot < 0) {
            if (!open_string) { buf_puts(out, " + \""); open_string = true; }
            if (needs_quotes(s->arg[a].v))
                buf_printf(out, " %s=\\\"%s\\\"", s->arg[a].k, s->arg[a].v);
            else
                buf_printf(out, " %s=%s", s->arg[a].k, s->arg[a].v);
        } else {
            /* A VALUE WITH A SPACE IN IT HAS TO CARRY ITS QUOTES. Display
             * names are two words, and a generated script that sent
             * `display_name=Alma Barrow` unquoted would split at the space
             * and fail -- on the recorder's own output, which is the worst
             * possible place for a player to meet their first bug. */
            const char *q = p->var[slot].quoted ? "\\\"" : "";
            if (open_string) { buf_printf(out, " %s=%s\" + ", s->arg[a].k, q); open_string = false; }
            else             { buf_printf(out, " + \" %s=%s\" + ", s->arg[a].k, q); }
            buf_puts(out, p->var[slot].name);
            if (p->var[slot].quoted) buf_puts(out, " + \"\\\"\"");
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
            "# Press record, then do a job the way you normally would -- fill in\n"
            "# the forms, click the buttons. Stop, and what you did will be here\n"
            "# as a script.\n");
        return;
    }

    int period = find_period(r);
    RecPlan plan;
    if (period) plan_build(r, period, &plan);

    buf_printf(out, "# %s.py -- recorded on day %d.\n#\n", r->name, r->started_day);

    if (!period) {
        buf_printf(out,
            "# %d step%s, exactly as you did them.\n"
            "#\n"
            "# Every line below is one thing you did. The buttons were sending\n"
            "# these all along -- this is not a translation of your clicking, it\n"
            "# IS your clicking, written down.\n"
            "#\n"
            "# Run it with:  py %s.py\n"
            "#\n"
            "# DO THE SAME JOB TWICE BEFORE YOU RECORD and this file will come\n"
            "# out as a loop instead, with the parts that changed pulled out\n"
            "# into a list you can add to. That is worth doing once, just to\n"
            "# see it.\n\n",
            r->nstep, r->nstep == 1 ? "" : "s", r->name);
        for (int i = 0; i < r->nstep; i++) emit_call(out, &r->step[i], NULL, 0, "");
        if (r->overflowed)
            buf_printf(out, "\n# (the recorder stopped at %d steps)\n", REC_MAX_STEPS);
        return;
    }

    /* THE TEACHING CASE. */
    buf_printf(out,
        "# You did the same %d step%s %d times.\n"
        "#\n", period, period == 1 ? "" : "s", plan.reps);

    if (plan.nvar) {
        int cols = 0, derived = 0;
        for (int i = 0; i < plan.nvar; i++) (plan.var[i].from < 0) ? cols++ : derived++;
        buf_printf(out,
            "# The %d thing%s that changed each time %s in the list below. Add a\n"
            "# row and the loop does another one; that is the whole idea.\n",
            cols, cols == 1 ? "" : "s", cols == 1 ? "is" : "are");
        if (derived)
            buf_printf(out,
                "#\n"
                "# %d other%s worked out from those, just under the loop -- you were\n"
                "# following a rule, and that is the rule written down. Change it\n"
                "# there and every row follows.\n",
                derived, derived == 1 ? "" : "s");
        buf_printf(out, "#\n# Run it with:  py /root/scripts/%s.py\n\n", r->name);

        buf_puts(out, "work = [\n");
        for (int k = 0; k < plan.reps; k++) {
            buf_puts(out, "    [");
            int col = 0;
            for (int i = 0; i < plan.nvar; i++) {
                if (plan.var[i].from >= 0) continue;
                buf_printf(out, "%s\"%s\"", col++ ? ", " : "", plan.var[i].value[k]);
            }
            buf_puts(out, "],\n");
        }
        buf_puts(out, "]\n\nfor row in work:\n");
        int col = 0;
        for (int i = 0; i < plan.nvar; i++)
            if (plan.var[i].from < 0)
                buf_printf(out, "    %s = row[%d]\n", plan.var[i].name, col++);
        for (int i = 0; i < plan.nvar; i++) {
            if (plan.var[i].from < 0) continue;
            buf_printf(out, "    %s = ", plan.var[i].name);
            if (plan.var[i].prefix[0]) buf_printf(out, "\"%s\" + ", plan.var[i].prefix);
            buf_puts(out, plan.var[plan.var[i].from].name);
            if (plan.var[i].suffix[0]) buf_printf(out, " + \"%s\"", plan.var[i].suffix);
            buf_putc(out, '\n');
        }
        buf_putc(out, '\n');
    } else {
        buf_printf(out,
            "# Nothing changed between them, so this is the same work done over\n"
            "# and over. Run it with:  py %s.py\n\n"
            "for row in [1, 2]:\n", r->name);
    }

    for (int s = 0; s < period; s++) emit_call(out, &r->step[s], &plan, s, "    ");

    buf_puts(out,
        "\n# What this does NOT do yet, and what will bite you:\n"
        "#\n"
        "#   * it does not check what came back. One of your appliances answers\n"
        "#     200 to everything, including its failures.\n"
        "#   * it does not retry. Calls fail sometimes.\n"
        "#   * it assumes the login it was given is free. One day it will not be.\n"
        "#\n"
        "# /root/examples/onboard.py handles the first of those. The rest is\n"
        "# yours.\n");
}

void recorder_status(const World *w, Buf *out)
{
    const Recorder *r = &w->rec;
    int period = r->nstep ? find_period(r) : 0;
    buf_printf(out, "{\"recording\":%s,\"name\":\"%s\",\"steps\":%d,\"repeats\":%d,\"loop_body\":%d}",
               r->on ? "true" : "false", r->name, r->nstep,
               period ? r->nstep / period : 0, period);
}

/* ticket.c — evaluating acceptance, and the queue.
 *
 * The whole of the game's honesty is in ticket_evaluate(). Everything else —
 * the generator, the follow-ups, the SLA arithmetic — is bookkeeping around
 * it.
 */
#include "world.h"
#include "ticket.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* --------------------------------------------------------- substitution */
/* Values in a check's `where` may contain placeholders, resolved against the
 * ticket's subject and against records bound by earlier checks:
 *
 *   {subject}         the subject user's id
 *   {dept}            their department
 *   {given} {family}  their name
 *   {name.field}      a field of the record bound under `name`
 *
 * Bindings are what let a check depend on a login the player chose rather
 * than one the game dictated. `where: { login: "{account.login}" }` says "the
 * mailbox belongs to whatever account they made", which is the only way to
 * verify work whose exact shape was the player's decision. */
typedef struct {
    char  name[RB_NAME_MAX];
    Rec  *rec;
} Binding;

typedef struct {
    Binding b[TK_MAX_CHECKS];
    int     n;
} Bindings;

static Rec *binding_get(const Bindings *bs, const char *name)
{
    for (int i = 0; i < bs->n; i++) if (!strcmp(bs->b[i].name, name)) return bs->b[i].rec;
    return NULL;
}

static bool expand(const World *w, const Ticket *t, const Bindings *bs,
                   const char *in, char *out, size_t cap)
{
    const User *u = world_user_find((World *)w, t->subject);
    size_t o = 0;
    for (const char *p = in; *p; ) {
        if (*p != '{') {
            if (o + 1 >= cap) return false;
            out[o++] = *p++;
            continue;
        }
        const char *end = strchr(p, '}');
        if (!end) return false;
        char name[RB_NAME_MAX * 2];
        size_t n = (size_t)(end - p - 1);
        if (n >= sizeof name) return false;
        memcpy(name, p + 1, n); name[n] = 0;
        p = end + 1;

        char val[RB_VAL_MAX] = "";
        char *dot = strchr(name, '.');
        if (dot) {
            *dot = 0;
            Rec *r = binding_get(bs, name);
            if (!r) return false;                 /* an earlier check did not bind */
            const char *v = rec_get(r, dot + 1);
            if (!v) return false;
            snprintf(val, sizeof val, "%s", v);
        } else if (!strcmp(name, "subject")) {
            snprintf(val, sizeof val, "%s", t->subject);
        } else if (u && !strcmp(name, "dept")) {
            snprintf(val, sizeof val, "%s", rb_dept_name[u->dept]);
        } else if (u && !strcmp(name, "given")) {
            snprintf(val, sizeof val, "%s", u->given);
        } else if (u && !strcmp(name, "family")) {
            snprintf(val, sizeof val, "%s", u->family);
        } else {
            /* A field of the ticket itself, then. Ticket fields are the
             * structured payload a script reads (handoff §7). */
            bool got = false;
            for (int i = 0; i < t->nfields; i++)
                if (!strcmp(t->fields[i].k, name)) { snprintf(val, sizeof val, "%s", t->fields[i].v); got = true; }
            if (!got) return false;
        }
        size_t vl = strlen(val);
        if (o + vl >= cap) return false;
        memcpy(out + o, val, vl); o += vl;
    }
    out[o] = 0;
    return true;
}

/* ------------------------------------------------------------ the search */
/* ACROSS EVERY INSTANCE OF THE KIND, not a named one.
 *
 * A check says "directory", not "directory_01". In Act III there are seven
 * directory servers and the player decides what goes where; a check that
 * named an instance would either dictate that decision or start failing the
 * moment they scaled out. This is also why appliance ids are not in the
 * ticket: the placement is the player's, and the acceptance is the game's. */
static Rec *find_record(World *w, const char *kind, const char *coll,
                        const Field *where, int nwhere, Inst **which)
{
    for (size_t i = 0; i < w->ninst; i++) {
        Inst *in = w->inst[i];
        if (strcmp(in->m->kind, kind)) continue;
        Coll *c = inst_coll(in, coll);
        if (!c) continue;

        /* Use the index when the where-clause is exactly the collection's
         * key, which is the common case and the difference between a
         * vacation test that finishes and one that does not. */
        if (nwhere == c->cs->nkey) {
            const char *kv[SPEC_MAX_KEYS];
            bool aligned = true;
            for (int k = 0; k < c->cs->nkey && aligned; k++) {
                const char *v = NULL;
                for (int f = 0; f < nwhere; f++) if (!strcmp(where[f].k, c->cs->key[k])) v = where[f].v;
                if (!v) aligned = false; else kv[k] = v;
            }
            if (aligned) {
                Rec *r = coll_find(c, kv, c->cs->nkey);
                if (r && !r->dead) { if (which) *which = in; return r; }
                continue;
            }
        }
        /* The one extra indexed field, when the where-clause is exactly it. */
        if (nwhere == 1) {
            Rec *r = coll_find_by(c, where[0].k, where[0].v);
            if (r) { if (which) *which = in; return r; }
            if (c->cs->index_field[0] && !strcmp(c->cs->index_field, where[0].k)) continue;
        }
        for (size_t ri = 0; ri < c->nr; ri++) {
            Rec *r = &c->r[ri];
            if (r->dead) continue;
            bool match = true;
            for (int f = 0; f < nwhere && match; f++) {
                const char *have = rec_get(r, where[f].k);
                if (!have || strcmp(have, where[f].v)) match = false;
            }
            if (match) { if (which) *which = in; return r; }
        }
    }
    return NULL;
}

/* ---------------------------------------------------------- the evaluator */
void ticket_evaluate(World *w, Ticket *t, Verdict *v)
{
    memset(v, 0, sizeof *v);
    const TicketType *tt = t->type;
    v->n = tt->ncheck;
    v->all = true;

    Bindings bs;
    bs.n = 0;
    uint8_t prov = PROV_SEED;
    bool prov_set = false;

    for (int i = 0; i < tt->ncheck; i++) {
        const Check *ck = &tt->check[i];
        v->passed[i] = false;

        /* Does this check apply to this ticket at all? */
        bool has_when = false, has_unless = false;
        for (int f = 0; f < t->nfields; f++) {
            if (ck->when[0]   && !strcmp(t->fields[f].k, ck->when))   has_when = true;
            if (ck->unless[0] && !strcmp(t->fields[f].k, ck->unless)) has_unless = true;
        }
        if ((ck->when[0] && !has_when) || (ck->unless[0] && has_unless)) {
            v->skipped[i] = true;
            v->passed[i]  = true;      /* not a pass to the player; see below */
            snprintf(v->why[i], sizeof v->why[i], "does not apply to this ticket");
            continue;
        }

        switch (ck->kind) {
        case CHK_EXISTS:
        case CHK_ABSENT: {
            Field where[TK_MAX_WHERE];
            int nw = 0;
            bool ok = true;
            for (int f = 0; f < ck->nwhere; f++) {
                snprintf(where[nw].k, sizeof where[nw].k, "%s", ck->where[f].k);
                if (!expand(w, t, &bs, ck->where[f].v, where[nw].v, sizeof where[nw].v)) { ok = false; break; }
                nw++;
            }
            if (!ok) {
                /* A placeholder that will not resolve almost always means an
                 * earlier check has not passed yet — the account does not
                 * exist, so there is no {account.login}. Say that, rather
                 * than "internal error". */
                snprintf(v->why[i], sizeof v->why[i], "not yet: this depends on a check above it");
                break;
            }
            Inst *found_in = NULL;
            Rec *r = find_record(w, ck->appliance, ck->coll, where, nw, &found_in);
            if (ck->kind == CHK_EXISTS) {
                v->passed[i] = (r != NULL);
                if (r) {
                    if (ck->bind[0] && bs.n < TK_MAX_CHECKS) {
                        snprintf(bs.b[bs.n].name, sizeof bs.b[bs.n].name, "%s", ck->bind);
                        bs.b[bs.n].rec = r;
                        bs.n++;
                    }
                    /* WHO DID THE WORK. Taken from the first record a check
                     * matched, because that is the object the ticket is
                     * really about. It is what the run report means by "these
                     * forty were closed by a script and those six by hand",
                     * and what the migration reads back at M6. */
                    if (!prov_set) { prov = r->prov; prov_set = true; }
                } else {
                    /* SAY WHAT TO DO, not just what is absent.
                     *
                     * "no memberships in the directory matching login=abarrow"
                     * is true, and a player who does not already know the
                     * answer learns nothing from it -- it names a table they
                     * have never seen and a field they did not choose. What
                     * they need is the appliance and the form.
                     *
                     * It is worked out from the specs rather than written in
                     * the ticket, so it cannot drift: find an installed
                     * appliance of this kind, find the form on it whose
                     * endpoint creates this collection, and name it. A player
                     * reading this can go and click the thing. */
                    char fix[SPEC_DOC_MAX] = "";
                    for (size_t k = 0; k < w->ninst && !fix[0]; k++) {
                        const Inst *in2 = w->inst[k];
                        if (strcmp(in2->m->kind, ck->appliance)) continue;
                        for (int fi = 0; fi < in2->m->nform && !fix[0]; fi++) {
                            const Endpoint *ep = model_endpoint(in2->m, in2->m->form[fi].calls);
                            if (!ep || ep->op != OP_CREATE || strcmp(ep->coll, ck->coll)) continue;
                            snprintf(fix, sizeof fix, " -- %s, \"%s\"",
                                     in2->id, in2->m->form[fi].title);
                        }
                    }
                    /* And what it would have to say, which is the part that
                     * turns a diagnosis into an instruction. */
                    char want[SPEC_DOC_MAX] = "";
                    size_t o = 0;
                    for (int f = 0; f < nw && o < sizeof want - 1; f++)
                        o += (size_t)snprintf(want + o, sizeof want - o, "%s%s=%s",
                                              f ? ", " : "", where[f].k, where[f].v);
                    snprintf(v->why[i], sizeof v->why[i], "nothing has %s yet%s", want, fix);
                }
            } else {
                v->passed[i] = (r == NULL);
                if (r) snprintf(v->why[i], sizeof v->why[i], "a %s record still exists in %s",
                                ck->coll, ck->appliance);
            }
            break;
        }
        case CHK_EQUALS: {
            Rec *r = binding_get(&bs, ck->bind);
            if (!r) { snprintf(v->why[i], sizeof v->why[i], "not yet: this depends on a check above it"); break; }
            const char *have = rec_get(r, ck->field);
            v->passed[i] = have && !strcmp(have, ck->value);
            if (!v->passed[i])
                snprintf(v->why[i], sizeof v->why[i], "%s is %s, not %s",
                         ck->field, have && *have ? have : "unset", ck->value);
            break;
        }
        case CHK_CONVENTION: {
            Rec *r = binding_get(&bs, ck->bind);
            User *u = world_user_find(w, t->subject);
            if (!r || !u) { snprintf(v->why[i], sizeof v->why[i], "not yet: this depends on a check above it"); break; }
            const char *have = rec_get(r, ck->field);
            char expect[RB_NAME_MAX];
            world_login_for(w, u, expect, sizeof expect);
            v->passed[i] = false;
            if (have) {
                size_t n = strlen(expect);
                if (!strncmp(have, expect, n)) {
                    v->passed[i] = true;
                    for (const char *p = have + n; *p; p++)
                        if (*p < '0' || *p > '9') v->passed[i] = false;
                }
            }
            if (!v->passed[i])
                snprintf(v->why[i], sizeof v->why[i], "%s is '%s'; the convention gives '%s'",
                         ck->field, have ? have : "", expect);
            break;
        }
        case CHK_CAPACITY: {
            char kind[RB_NAME_MAX];
            if (!expand(w, t, &bs, ck->appliance, kind, sizeof kind)) {
                snprintf(v->why[i], sizeof v->why[i], "this ticket does not name an appliance kind");
                break;
            }
            int64_t live = 0, cap = 0;
            int instances = 0;
            for (size_t k = 0; k < w->ninst; k++) {
                Inst *in = w->inst[k];
                if (strcmp(in->m->kind, kind)) continue;
                instances++;
                cap += in->m->capacity;
                if (in->ncoll) for (size_t r = 0; r < in->coll[0].nr; r++)
                    if (!in->coll[0].r[r].dead) live++;
            }
            int pct = cap ? (int)((live * 100) / cap) : 999;
            v->passed[i] = pct <= ck->max_pct;
            if (!v->passed[i])
                snprintf(v->why[i], sizeof v->why[i],
                         "%d %s appliance%s hold %lld records against %lld of capacity (%d%%, want %d%%)",
                         instances, kind, instances == 1 ? "" : "s",
                         (long long)live, (long long)cap, pct, ck->max_pct);
            break;
        }
        default:
            snprintf(v->why[i], sizeof v->why[i], "unimplemented check");
            break;
        }
        if (!v->passed[i]) v->all = false;
    }
    if (v->all) t->closed_prov = prov;
}

/* THE BOOKKEEPING LIVES HERE, not in the sweep.
 *
 * It used to live in world_ticket_sweep(), which was fine for exactly as long
 * as the sweep was the only thing that closed tickets. The moment reading one
 * ticket stopped sweeping the whole queue -- a performance fix -- the queue
 * counters stopped being updated at all, and the run report cheerfully said
 * "7,347 open, 0 closed" about a run that had closed 6,997 of them. Counters
 * that are maintained by the caller are counters that go wrong. */
bool ticket_settle(World *w, Ticket *t)
{
    if (t->closed_day >= 0) return false;
    Verdict v;
    ticket_evaluate(w, t, &v);
    if (!v.all) return false;
    t->closed_day = w->day;
    /* Breached is sticky: a ticket closed late was still closed late, and the
     * vacation criteria (§12) count tickets resolved WITHIN SLA. Marking it
     * only at close time would let a queue that ran a week behind report a
     * perfect score the moment it caught up. */
    if (w->day > t->due_day || (w->day == t->due_day && w->ms > t->due_ms)) t->breached = true;
    w->open_count--;
    w->closed_total++;
    if (t->breached) w->breached_total++;
    return true;
}

/* ------------------------------------------------------------- rendering */
void ticket_describe(const World *w, const Ticket *t, char *out, size_t cap)
{
    Bindings bs; bs.n = 0;
    if (!expand(w, t, &bs, t->type->description, out, cap))
        snprintf(out, cap, "%s", t->type->description);
}

void ticket_render(const World *w, const Ticket *t, Buf *out)
{
    char desc[SPEC_DOC_MAX * 2];
    ticket_describe(w, t, desc, sizeof desc);
    buf_printf(out, "{\"id\":\"%s\",\"type\":\"%s\",\"opened\":\"day %d, %02d:%02d\","
                    "\"sla_minutes\":%d,\"due\":\"day %d, %02d:%02d\",\"state\":\"%s\","
                    "\"breached\":%s,\"subject\":{\"kind\":\"%s\",\"ref\":\"%s\"},\"fields\":{",
               t->id, t->type->id, t->opened_day,
               9 + (t->opened_ms / 60000) / 60, (t->opened_ms / 60000) % 60,
               t->type->sla_minutes, t->due_day,
               9 + (t->due_ms / 60000) / 60, (t->due_ms / 60000) % 60,
               t->closed_day >= 0 ? "closed" : "open",
               t->breached ? "true" : "false",
               t->type->subject_kind, t->subject);
    for (int i = 0; i < t->nfields; i++)
        buf_printf(out, "%s\"%s\":\"%s\"", i ? "," : "", t->fields[i].k, t->fields[i].v);
    if (t->parent[0]) buf_printf(out, "},\"chasing\":\"%s\",\"description\":\"%s\",\"acceptance\":[",
                                 t->parent, desc);
    else              buf_printf(out, "},\"description\":\"%s\",\"acceptance\":[", desc);
    for (int i = 0; i < t->type->ncheck; i++)
        buf_printf(out, "%s\"%s\"", i ? "," : "", t->type->check[i].id);
    buf_puts(out, "]");
    if (t->closed_day >= 0)
        buf_printf(out, ",\"closed_day\":%d,\"closed_by\":\"%s\"",
                   t->closed_day, prov_name((Prov)t->closed_prov));
    buf_puts(out, "}");
    (void)w;
}

/* -------------------------------------------------------- THE EXCEPTIONS
 *
 * Handoff §8: the ticket is clean, the world is messy. A script that parses a
 * field and calls one endpoint is a form, not a game. These are the reasons
 * it has to look at what came back.
 *
 * All three are exception class 1 -- collisions and history -- expressed as
 * facts about the person rather than as faults to diagnose. NONE OF THEM IS
 * HIDDEN. Each is a structured field on the ticket, visible from the moment
 * it opens, and each turns on an acceptance check the player can read. There
 * is nothing here to discover; there is something here to HANDLE, which is a
 * different thing and the only one that scales (§2, no diagnosis-as-content).
 *
 * THE RAMP IS THE MECHANIC. At forty users almost nothing is exceptional and
 * Act I is pleasant. At eight hundred, a fifth of every intake is, and the
 * naive script that cleared the queue in Act I now leaves a quarter of the
 * company half-provisioned. Nothing about the exceptions changes; the company
 * got bigger, and the company got bigger because the player onboarded it.
 *
 * The numbers below are what the --naive gate settled on, not what looked
 * right. See tools/ and the gate itself for the band they have to hit. */
#define EXC_USERS_LO   150     /* the Act I wall */
#define EXC_USERS_HI   800     /* the end of Act II */
#define EXC_RATE_LO_BP 50      /* 0.5% of intake is exceptional at the wall */
#define EXC_RATE_HI_BP 3400    /* 34% by the end of Act II */
/* §5 SAYS 2% TO 18% AND THIS IS 0.5% TO 34%. Said out loud rather than left for
 * someone to notice: the handoff's own numbers are starting values for the
 * harness ("the harness tunes them; the shape is locked"), and §8's
 * degeneracy band is the thing that has to hold. With collisions supplying
 * about 10 points at the end of Act II, 18% of exceptions lands a naive
 * script at roughly 24% failure -- comfortably inside the band it is supposed
 * to be outside, which would mean Act II is dead content.
 *
 * The exceptions have to carry the ramp because nothing else can: see the
 * note on the surname tables in world.c. --naive-gate is what decides these
 * two numbers, and it decides them across several seeds because a single
 * run samples about 140 tickets and a 42% rate on 140 samples is 42% give or
 * take eight. */

static int exception_bp(int users)
{
    if (users <= EXC_USERS_LO) return EXC_RATE_LO_BP;
    if (users >= EXC_USERS_HI) return EXC_RATE_HI_BP;
    /* Linear in headcount. Integer arithmetic on purpose: this feeds the rng
     * roll, so it is world state and must be identical on every platform. */
    int span = EXC_USERS_HI - EXC_USERS_LO;
    return EXC_RATE_LO_BP + (int)(((int64_t)(EXC_RATE_HI_BP - EXC_RATE_LO_BP) *
                                   (users - EXC_USERS_LO)) / span);
}

static void ticket_field(Ticket *t, const char *k, const char *v)
{
    if (t->nfields >= TK_MAX_FIELDS) return;
    snprintf(t->fields[t->nfields].k, RB_NAME_MAX, "%s", k);
    snprintf(t->fields[t->nfields].v, RB_VAL_MAX, "%s", v);
    t->nfields++;
}

/* Give this ticket a history, if the dice say so. Called only for onboarding;
 * other types will want their own, which is why it is not in the generator. */
void world_ticket_exception(World *w, Ticket *t)
{
    if (rng_range(&w->rng, 0, 9999) >= exception_bp(w->active)) return;

    User *u = world_user_find(w, t->subject);
    if (!u) return;

    switch (rng_range(&w->rng, 0, 2)) {
    case 0: {
        /* THEY WORKED HERE BEFORE. A contractor account is still in the
         * directory, disabled, under the login the convention would give
         * them. Creating an account is idempotent, so a naive script's
         * create returns the old record, reports 200, and moves on -- leaving
         * a person whose account exists and is not active.
         *
         * Idempotency is not enough. You have to reconcile. That is the
         * lesson, and it is the one lesson of this game that no other game in
         * this space teaches at all. */
        Inst *dir = NULL;
        for (size_t i = 0; i < w->ninst; i++) if (!strcmp(w->inst[i]->m->kind, "directory")) dir = w->inst[i];
        if (!dir) return;
        Coll *ac = inst_coll(dir, "accounts");
        char login[RB_NAME_MAX];
        world_login_for(w, u, login, sizeof login);
        const char *kv[1] = { login };
        /* Across the whole directory service, not this one box: the world
         * must not create a duplicate identity while inventing a history. */
        if (service_find(w, dir, "accounts", kv, 1, NULL)) return;
        Rec *old = coll_insert(ac, PROV_SEED, w->day);
        rec_set(old, "login", login);
        rec_set(old, "user_ref", u->id);
        rec_setf(old, "display_name", "%s_%s", u->given, u->family);
        rec_set(old, "dept", rb_dept_name[u->dept]);
        rec_set(old, "status", "contractor");
        coll_index_rec(ac, old);
        ticket_field(t, "rehire", "yes");
        break;
    }
    case 1: {
        /* SOMEBODY IN TWO DEPARTMENTS. The ticket says so, in a field. A
         * script that reads `dept` and stops gets six of seven checks. */
        int d = rng_range(&w->rng, 0, RB_DEPT__N - 1);
        if (d == u->dept) d = (d + 1) % RB_DEPT__N;
        ticket_field(t, "also_dept", rb_dept_name[d]);
        break;
    }
    default: {
        /* A DEPARTMENT WITH A NON-DEFAULT SHARE. Handoff §6 names this one
         * exactly. The share exists; it is simply not the one the pattern
         * would have guessed, and the ticket says which. */
        char name[RB_VAL_MAX];
        snprintf(name, sizeof name, "proj-%s", rb_dept_name[u->dept]);
        bool exists = false;
        for (size_t i = 0; i < w->ninst && !exists; i++) {
            if (strcmp(w->inst[i]->m->kind, "fileserver")) continue;
            Coll *sc = inst_coll(w->inst[i], "shares");
            const char *kv[1] = { name };
            if (sc && coll_find(sc, kv, 1)) exists = true;
        }
        if (!exists) {
            /* On every file server, not the first one. A share the player can
             * only reach from one of three appliances is a puzzle about our
             * data model, not about theirs. */
            for (size_t i = 0; i < w->ninst; i++) {
                if (strcmp(w->inst[i]->m->kind, "fileserver")) continue;
                Coll *sc = inst_coll(w->inst[i], "shares");
                if (!sc) continue;
                Rec *sh = coll_insert(sc, PROV_SEED, w->day);
                rec_set(sh, "name", name);
                rec_setf(sh, "path", "/srv/projects/%s", rb_dept_name[u->dept]);
                rec_set(sh, "dept", rb_dept_name[u->dept]);
                coll_index_rec(sc, sh);
            }
        }
        ticket_field(t, "share_override", name);
        break;
    }
    }
}

/* ------------------------------------------------------------- the queue */
Ticket *world_ticket_find(World *w, const char *id)
{
    for (size_t i = 0; i < w->ntick; i++) if (!strcmp(w->tick[i].id, id)) return &w->tick[i];
    return NULL;
}

/* SLA IS IN WORKING MINUTES, not wall clock. 480 of them is one day, so a
 * ticket raised at four in the afternoon with an eight-hour SLA is due at
 * four the next afternoon and not at midnight tonight. Anything else would
 * make the tickets raised late in the day unwinnable, which is a difficulty
 * curve made of arithmetic rather than of design. */
Ticket *world_ticket_new(World *w, const TicketType *tt, const char *subject)
{
    if (w->ntick == w->tcap) {
        w->tcap = w->tcap ? w->tcap * 2 : 128;
        w->tick = rb_realloc(w->tick, w->tcap * sizeof *w->tick);
    }
    Ticket *t = &w->tick[w->ntick++];
    memset(t, 0, sizeof *t);
    snprintf(t->id, sizeof t->id, "TCK-%05d", ++w->next_tid);
    t->type = tt;
    t->opened_day = w->day;
    /* Arrivals are spread across the working day rather than all landing at
     * nine. A queue that appears in one lump is a queue you plan around once;
     * a queue that arrives while you are working is the actual job. */
    t->opened_ms = rng_range(&w->rng, 0, RB_DAY_MS - 1);
    t->closed_day = -1;
    snprintf(t->subject, sizeof t->subject, "%s", subject);

    int64_t opened_abs = (int64_t)t->opened_day * RB_DAY_MINUTES + t->opened_ms / 60000;
    int64_t due_abs = opened_abs + tt->sla_minutes;
    t->due_day = (int32_t)(due_abs / RB_DAY_MINUTES);
    t->due_ms  = (int32_t)((due_abs % RB_DAY_MINUTES) * 60000);
    w->open_count++;
    return t;
}

int world_ticket_sweep(World *w)
{
    int closed = 0;
    for (size_t i = 0; i < w->ntick; i++) {
        if (w->tick[i].closed_day >= 0) continue;
        if (ticket_settle(w, &w->tick[i])) closed++;
    }
    return closed;
}

/* COMPOUNDING PRESSURE, NEVER INSTANT GAME-OVER (handoff decision 12).
 *
 * TNI's one-complaint-and-you-are-fired is why that game feels brittle. Here,
 * falling behind generates more work and that is the whole punishment: a
 * ticket past its SLA gets chased, at 0.4 chases per day, and the chase is
 * another ticket in the queue with the same subject. It closes when the
 * original work is done -- it is not extra work, it is extra QUEUE, which is
 * exactly what being behind feels like and exactly what makes the numbers in
 * the run report get worse while you catch up.
 *
 * There is no fail screen here and there must not be one. */
void world_ticket_day(World *w)
{
    size_t n = w->ntick;      /* the follow-ups we add must not chase themselves today */
    for (size_t i = 0; i < n; i++) {
        Ticket *t = &w->tick[i];
        if (t->closed_day >= 0) continue;
        /* A CHASE IS NEVER CHASED, and this line is the difference between
         * compounding pressure and a runaway.
         *
         * Follow-ups are born already past their SLA, so letting them spawn
         * their own makes the open queue grow by 1.4x PER DAY whatever the
         * player does -- 27,000 tickets by day 45 in the run that found this,
         * against an intake of about fifty. That is not pressure, it is a
         * divergent series with a user interface.
         *
         * One piece of late work gets asked about repeatedly. It does not
         * breed. Decision 12: compounding, never instant, and never
         * unsurvivable. */
        if (t->parent[0]) continue;
        if (w->day < t->due_day) continue;
        t->breached = true;
        t->followup_milli += RB_FOLLOWUP_MILLI;
        while (t->followup_milli >= 1000) {
            t->followup_milli -= 1000;
            t->followups++;
            w->followups_total++;
            /* THE SUBJECT IS COPIED FIRST, and that is not tidiness.
             * world_ticket_new() may grow the queue, and growing it reallocs
             * the array this ticket lives in -- so passing t->subject
             * straight through hands the callee a pointer into freed memory.
             * It survived every short run and segfaulted on day 27 of a naive
             * agent's run, which is the sort of bug that only ever shows up
             * once the queue is big enough to be interesting. */
            char subject[RB_ID_MAX];
            snprintf(subject, sizeof subject, "%s", t->subject);
            const TicketType *tt = t->type;
            int32_t due_day = t->due_day, due_ms = t->due_ms;
            Ticket *f = world_ticket_new(w, tt, subject);
            /* The chase inherits the original's clock, so a follow-up is born
             * already late. It is not a fresh eight hours to do work that was
             * due yesterday. */
            snprintf(f->parent, sizeof f->parent, "%s", w->tick[i].id);
            f->due_day = due_day;
            f->due_ms  = due_ms;
            f->breached = true;
            /* and the queue may have moved under us */
            t = &w->tick[i];
        }
    }
}

/* THE WORLD NOTICING IT IS FULL.
 *
 * Raised once per kind, when the appliances of that kind are past the
 * threshold and there is not already a ticket open about it. Once, because a
 * service that is full is one problem however many days it stays full -- and
 * because a ticket type that re-raises itself daily would make the queue
 * depth criterion of the vacation test (§12) unwinnable for reasons that have
 * nothing to do with the player. */
void world_ticket_capacity(World *w)
{
    const TicketType *tt = w->specs ? spec_ticket(w->specs, "service.capacity") : NULL;
    if (!tt) return;

    for (size_t i = 0; i < w->ninst; i++) {
        const char *kind = w->inst[i]->m->kind;
        bool first = true;
        for (size_t j = 0; j < i; j++) if (!strcmp(w->inst[j]->m->kind, kind)) first = false;
        if (!first) continue;

        int64_t live = 0, cap = 0;
        for (size_t k = 0; k < w->ninst; k++) {
            Inst *in = w->inst[k];
            if (strcmp(in->m->kind, kind)) continue;
            cap += in->m->capacity;
            if (in->ncoll) for (size_t r = 0; r < in->coll[0].nr; r++)
                if (!in->coll[0].r[r].dead) live++;
        }
        if (!cap || (live * 100) / cap <= RB_CAPACITY_WARN_PCT) continue;

        bool already = false;
        for (size_t q = 0; q < w->ntick && !already; q++)
            if (w->tick[q].closed_day < 0 && w->tick[q].type == tt &&
                !strcmp(w->tick[q].subject, kind)) already = true;
        if (already) continue;

        world_ticket_new(w, tt, kind);
    }
}

void world_ticket_stats(const World *w, Buf *out)
{
    int open_breached = 0, by_hand = 0, by_script = 0, by_system = 0, by_seed = 0;
    int32_t oldest = -1;
    for (size_t i = 0; i < w->ntick; i++) {
        const Ticket *t = &w->tick[i];
        if (t->closed_day < 0) {
            if (t->breached) open_breached++;
            if (oldest < 0 || t->opened_day < oldest) oldest = t->opened_day;
        } else {
            switch (t->closed_prov) {
            case PROV_HAND:   by_hand++;   break;
            case PROV_SCRIPT: by_script++; break;
            case PROV_SYSTEM: by_system++; break;
            default:          by_seed++;   break;
            }
        }
    }
    int within = w->closed_total - w->breached_total;
    buf_printf(out, "{\"day\":%d,\"raised\":%d,\"open\":%d,\"closed\":%d,\"breached\":%d,"
                    "\"open_breached\":%d,\"followups\":%d,\"within_sla_pct\":%d,"
                    "\"oldest_open_day\":%d,"
                    "\"closed_by\":{\"hand\":%d,\"script\":%d,\"system\":%d,\"seed\":%d}}",
               w->day, w->next_tid, w->open_count, w->closed_total, w->breached_total,
               open_breached, w->followups_total,
               w->closed_total ? (within * 100) / w->closed_total : 100,
               oldest, by_hand, by_script, by_system, by_seed);
}

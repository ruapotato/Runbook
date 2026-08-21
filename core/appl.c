/* appl.c — the appliance runtime.
 *
 * Five operations over declared collections, plus the four things that make
 * scripting them a game instead of a form: rate limits, transient failure,
 * writes that commit and then time out, and vendors that lie.
 *
 * THE RULE THIS FILE EXISTS TO ENFORCE (handoff §8): the ticket is clean, the
 * world is messy. A script that parses a field and calls one endpoint is a
 * form with extra steps. Everything below is a reason the player's script has
 * to look at what came back.
 */
#include "world.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

/* ------------------------------------------------------------ collections */
static uint64_t key_hash(const char *const *vals, int n)
{
    uint64_t h = rb_hash_init();
    for (int i = 0; i < n; i++) {
        h = rb_hash_str(h, vals[i]);
        h = rb_hash_bytes(h, "\x1f", 1);   /* a separator, so a|bc != ab|c */
    }
    return h;
}

#define IDX_EMPTY 0u
#define IDX_DEAD  0xFFFFFFFFu

static void coll_reindex(Coll *c, size_t cap)
{
    uint32_t *tab = rb_alloc(cap * sizeof *tab);
    size_t m = cap - 1;
    for (size_t i = 0; i < c->nr; i++) {
        const char *kv[SPEC_MAX_KEYS];
        for (int k = 0; k < c->cs->nkey; k++) kv[k] = rec_get(&c->r[i], c->cs->key[k]);
        for (int k = 0; k < c->cs->nkey; k++) if (!kv[k]) kv[k] = "";
        /* A record whose key is spent but whose row is gone still occupies
         * its slot; that is what makes a login stay taken. */
        if (c->r[i].dead && c->reuse) continue;
        size_t s = (size_t)(key_hash(kv, c->cs->nkey) & m);
        while (tab[s] != IDX_EMPTY) s = (s + 1) & m;
        tab[s] = (uint32_t)(i + 1);
    }
    rb_free(c->idx);
    c->idx = tab;
    c->icap = cap;
}

static void coll_reindex2(Coll *c, size_t cap)
{
    uint32_t *tab = rb_alloc(cap * sizeof *tab);
    size_t m = cap - 1;
    for (size_t i = 0; i < c->nr; i++) {
        if (c->r[i].dead) continue;
        const char *v = rec_get(&c->r[i], c->cs->index_field);
        if (!v) continue;
        const char *one[1] = { v };
        size_t s = (size_t)(key_hash(one, 1) & m);
        while (tab[s] != IDX_EMPTY) s = (s + 1) & m;
        tab[s] = (uint32_t)(i + 1);
    }
    rb_free(c->idx2);
    c->idx2 = tab;
    c->icap2 = cap;
}

static void coll_reserve(Coll *c, size_t want)
{
    if (want > c->cap) {
        size_t cap = c->cap ? c->cap : 32;
        while (cap < want) cap *= 2;
        c->r = rb_realloc(c->r, cap * sizeof *c->r);
        memset(c->r + c->cap, 0, (cap - c->cap) * sizeof *c->r);
        c->cap = cap;
    }
    if (!c->icap || want * 10 >= c->icap * 7) {
        size_t cap = c->icap ? c->icap : 64;
        while (want * 10 >= cap * 7) cap *= 2;
        coll_reindex(c, cap);
    }
    if (c->cs->index_field[0] && (!c->icap2 || want * 10 >= c->icap2 * 7)) {
        size_t cap = c->icap2 ? c->icap2 : 64;
        while (want * 10 >= cap * 7) cap *= 2;
        coll_reindex2(c, cap);
    }
}

const char *rec_get(const Rec *r, const char *k)
{
    for (int i = 0; i < r->nf; i++) if (!strcmp(r->f[i].k, k)) return r->f[i].v;
    return NULL;
}

void rec_set(Rec *r, const char *k, const char *v)
{
    for (int i = 0; i < r->nf; i++)
        if (!strcmp(r->f[i].k, k)) { snprintf(r->f[i].v, RB_VAL_MAX, "%s", v); return; }
    if (r->nf >= SPEC_MAX_FIELDS) return;
    snprintf(r->f[r->nf].k, RB_NAME_MAX, "%s", k);
    snprintf(r->f[r->nf].v, RB_VAL_MAX, "%s", v);
    r->nf++;
}

void rec_setf(Rec *r, const char *k, const char *fmt, ...)
{
    char tmp[RB_VAL_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    rec_set(r, k, tmp);
}

Rec *coll_find(Coll *c, const char *const *keyvals, int nkey)
{
    if (!c->icap || nkey != c->cs->nkey) return NULL;
    size_t m = c->icap - 1;
    size_t s = (size_t)(key_hash(keyvals, nkey) & m);
    while (c->idx[s] != IDX_EMPTY) {
        if (c->idx[s] != IDX_DEAD) {
            Rec *r = &c->r[c->idx[s] - 1];
            bool same = true;
            for (int k = 0; k < nkey && same; k++) {
                const char *have = rec_get(r, c->cs->key[k]);
                if (!have || strcmp(have, keyvals[k])) same = false;
            }
            if (same) return r;
        }
        s = (s + 1) & m;
    }
    return NULL;
}

Rec *coll_insert(Coll *c, Prov prov, int32_t day)
{
    coll_reserve(c, c->nr + 1);
    Rec *r = &c->r[c->nr++];
    memset(r, 0, sizeof *r);
    r->prov = (uint8_t)prov;
    r->created_day = day;
    return r;
}

/* Called after the key fields are filled in. Kept separate from insert
 * because the key is not known until the caller has set it. */
Rec *coll_find_by(Coll *c, const char *field, const char *val)
{
    if (!c->cs->index_field[0] || strcmp(c->cs->index_field, field) || !c->icap2) return NULL;
    size_t m = c->icap2 - 1;
    const char *one[1] = { val };
    size_t s = (size_t)(key_hash(one, 1) & m);
    while (c->idx2[s] != IDX_EMPTY) {
        if (c->idx2[s] != IDX_DEAD) {
            Rec *r = &c->r[c->idx2[s] - 1];
            const char *have = rec_get(r, field);
            if (have && !strcmp(have, val) && !r->dead) return r;
        }
        s = (s + 1) & m;
    }
    return NULL;
}

void coll_index_rec(Coll *c, Rec *r)
{
    const char *kv[SPEC_MAX_KEYS];
    for (int k = 0; k < c->cs->nkey; k++) {
        const char *v = rec_get(r, c->cs->key[k]);
        kv[k] = v ? v : "";
    }
    size_t m = c->icap - 1;
    size_t s = (size_t)(key_hash(kv, c->cs->nkey) & m);
    while (c->idx[s] != IDX_EMPTY && c->idx[s] != IDX_DEAD) s = (s + 1) & m;
    c->idx[s] = (uint32_t)((r - c->r) + 1);

    if (c->cs->index_field[0] && c->icap2) {
        const char *v = rec_get(r, c->cs->index_field);
        if (v) {
            const char *one[1] = { v };
            size_t m2 = c->icap2 - 1;
            size_t s2 = (size_t)(key_hash(one, 1) & m2);
            while (c->idx2[s2] != IDX_EMPTY && c->idx2[s2] != IDX_DEAD) s2 = (s2 + 1) & m2;
            c->idx2[s2] = (uint32_t)((r - c->r) + 1);
        }
    }
}

static void coll_unindex(Coll *c, Rec *r)
{
    const char *kv[SPEC_MAX_KEYS];
    for (int k = 0; k < c->cs->nkey; k++) {
        const char *v = rec_get(r, c->cs->key[k]);
        kv[k] = v ? v : "";
    }
    size_t m = c->icap - 1;
    size_t s = (size_t)(key_hash(kv, c->cs->nkey) & m);
    uint32_t want = (uint32_t)((r - c->r) + 1);
    while (c->idx[s] != IDX_EMPTY) {
        if (c->idx[s] == want) { c->idx[s] = IDX_DEAD; return; }
        s = (s + 1) & m;
    }
}

Rec *service_find(World *w, const Inst *like, const char *coll,
                  const char *const *keyvals, int nkey, Inst **which)
{
    for (size_t i = 0; i < w->ninst; i++) {
        Inst *in = w->inst[i];
        if (strcmp(in->m->kind, like->m->kind)) continue;
        Coll *c = inst_coll(in, coll);
        if (!c) continue;
        Rec *r = coll_find(c, keyvals, nkey);
        if (r) { if (which) *which = in; return r; }
    }
    return NULL;
}

/* -------------------------------------------------------------- instances */
Inst *inst_new(World *w, const Model *m, const Vendor *v, const char *id)
{
    Inst *in = rb_alloc(sizeof *in);
    snprintf(in->id, sizeof in->id, "%s", id);
    in->m = m;
    in->v = v;
    in->installed_day = w->day;
    in->cred_expires_day = m->credential_days ? w->day + m->credential_days : -1;
    in->rl_minute = -1;
    for (int i = 0; i < m->ncoll; i++) {
        Coll *c = &in->coll[in->ncoll++];
        memset(c, 0, sizeof *c);
        c->cs = &m->coll[i];
        /* Accounts do not release their keys; memberships do. It is in the
         * spec rather than special-cased here, because the next appliance
         * will want the other answer. */
        c->reuse = c->cs->reuse_key;
        coll_reserve(c, 8);
    }
    return in;
}

void inst_free(Inst *in)
{
    if (!in) return;
    for (int i = 0; i < in->ncoll; i++) {
        rb_free(in->coll[i].r);
        rb_free(in->coll[i].idx);
        rb_free(in->coll[i].idx2);
    }
    rb_free(in);
}

Coll *inst_coll(Inst *in, const char *name)
{
    for (int i = 0; i < in->ncoll; i++) if (!strcmp(in->coll[i].cs->name, name)) return &in->coll[i];
    return NULL;
}

Rec *appl_seed(Inst *in, const char *coll, Prov prov, int32_t day)
{
    Coll *c = inst_coll(in, coll);
    if (!c) return NULL;
    return coll_insert(c, prov, day);
}

void appl_replicate(Inst *dst, const Inst *src)
{
    for (int i = 0; i < dst->ncoll; i++) {
        if (!dst->coll[i].cs->replicated) continue;
        const Coll *sc = NULL;
        for (int j = 0; j < src->ncoll; j++)
            if (!strcmp(src->coll[j].cs->name, dst->coll[i].cs->name)) sc = &src->coll[j];
        if (!sc) continue;
        for (size_t r = 0; r < sc->nr; r++) {
            if (sc->r[r].dead) continue;
            Rec *n = coll_insert(&dst->coll[i], (Prov)sc->r[r].prov, sc->r[r].created_day);
            *n = sc->r[r];
            coll_index_rec(&dst->coll[i], n);
        }
    }
}

/* The first collection is the one that defines the instance's load. For a
 * directory that is accounts; for a print server, print queues. Declared by
 * position rather than by a spec field because every appliance so far has
 * exactly one collection that matters for capacity, and inventing a field for
 * it would be inventing a decision nobody has had to make yet. */
int inst_load_pct(const Inst *in)
{
    if (!in->ncoll || !in->m->capacity) return 0;
    size_t live = 0;
    const Coll *c = &in->coll[0];
    for (size_t i = 0; i < c->nr; i++) if (!c->r[i].dead) live++;
    return (int)((live * 100) / (size_t)in->m->capacity);
}

/* ------------------------------------------------------------------ calls */
static void body_field(Buf *b, bool xml, bool first, const char *k, const char *v)
{
    if (xml) buf_printf(b, "<%s>%s</%s>", k, v, k);
    else     buf_printf(b, "%s\"%s\":\"%s\"", first ? "" : ",", k, v);
}

static void body_rec(Buf *b, bool xml, const Rec *r)
{
    if (xml) {
        buf_puts(b, "<record>");
        for (int i = 0; i < r->nf; i++) body_field(b, true, i == 0, r->f[i].k, r->f[i].v);
        buf_puts(b, "</record>");
    } else {
        buf_putc(b, '{');
        for (int i = 0; i < r->nf; i++) body_field(b, false, i == 0, r->f[i].k, r->f[i].v);
        buf_putc(b, '}');
    }
}

static void result_err(ApiResult *out, bool xml, int status, const char *msg)
{
    out->status = status;
    buf_clear(&out->body);
    if (xml) buf_printf(&out->body, "<error code=\"%d\">%s</error>", status, msg);
    else     buf_printf(&out->body, "{\"error\":\"%s\",\"code\":%d}", msg, status);
}

static const char *arg_get(const Field *a, int n, const char *k)
{
    for (int i = 0; i < n; i++) if (!strcmp(a[i].k, k)) return a[i].v;
    return NULL;
}

/* Latency, after load. Past nominal capacity an appliance gets slower in
 * proportion (§5, Act III): at twice capacity, everything takes twice as
 * long, and the slow-performance tickets that generates are the load the
 * player themselves created. */
static int scaled_ms(const Inst *in, int base)
{
    int load = inst_load_pct(in);
    if (load <= 100) return base;
    return (int)(((int64_t)base * load) / 100);
}

void appl_call(World *w, Inst *in, const char *endpoint,
               const Field *args, int nargs, Prov prov, ApiResult *out)
{
    buf_init(&out->body);
    out->ms = 0;
    out->committed = false;

    /* The legacy vendor speaks XML and lies about status codes. Both are
     * decided once, here, and every path below respects them. */
    const bool xml   = (in->v->arch == VEN_LEGACY);
    const bool lies  = (in->v->arch == VEN_LEGACY);

    if (!in->m->has_api) {
        /* The cheap vendor. Not a failure to handle -- a fact to plan around,
         * at purchase time, which is the decision the mechanic is for. */
        result_err(out, false, RB_NO_API, "this appliance has no API; it is web only");
        return;
    }

    const Endpoint *ep = model_endpoint(in->m, endpoint);
    if (!ep) {
        result_err(out, xml, RB_NOT_FOUND, "no such endpoint");
        out->ms = 20;
        world_spend_ms(w, out->ms);
        return;
    }

    int cost = scaled_ms(in, ep->latency_ms);

    /* ---- rate limit. Per instance, per in-game minute. A rejected call
     * still costs a round trip, which is why a script that retries instantly
     * in a tight loop gets slower rather than faster. */
    if (ep->failure_modes & FM_RATE_LIMITED || in->v->arch == VEN_FLAKY) {
        int minute = world_minute(w);
        if (in->rl_minute != minute) { in->rl_minute = minute; in->rl_count = 0; }
        if (++in->rl_count > in->v->rate_limit) {
            result_err(out, xml, RB_RATE_LIMIT, "rate limited; retry after the minute");
            out->ms = 30;
            world_spend_ms(w, out->ms);
            in->calls++;
            return;
        }
    }

    /* ---- credential expiry. The scan-to-folder ticket's third branch
     * (handoff §6): has the service credential expired? */
    if (ep->needs_credential && in->cred_expires_day >= 0 && w->day > in->cred_expires_day) {
        result_err(out, xml, RB_EXPIRED, "service credential expired");
        out->ms = cost / 4;
        world_spend_ms(w, out->ms);
        in->calls++;
        return;
    }

    /* ---- an 8-second stall. Rare, and it exists to make a naive sequential
     * script feel slow enough that concurrency is worth discovering (§10). */
    if (ep->failure_modes & FM_STALL && rng_range(&w->rng, 0, 9999) < 40) cost += 8000;

    /* ---- transient failure, worse under load: 3% baseline rising to 8%
     * (§10). The vendor sets the floor; the instance's own overload sets how
     * far above it things get. */
    if (ep->failure_modes & FM_TRANSIENT) {
        int load = inst_load_pct(in);
        int bp = in->v->transient_bp + (load > 100 ? (load - 100) * 6 : 0);
        if (bp > 800) bp = 800;
        if (rng_range(&w->rng, 0, 9999) < bp) {
            result_err(out, xml, RB_TRANSIENT, "temporary failure; retry");
            out->ms = cost;
            world_spend_ms(w, out->ms);
            in->calls++;
            return;
        }
    }

    Coll *c = inst_coll(in, ep->coll);
    if (!c) { result_err(out, xml, RB_TRANSIENT, "collection missing"); return; }

    /* ---- required fields */
    for (int i = 0; i < ep->nrequired; i++) {
        if (!arg_get(args, nargs, ep->required[i])) {
            char msg[128];
            snprintf(msg, sizeof msg, "missing required field: %s", ep->required[i]);
            result_err(out, xml, RB_BAD_REQUEST, msg);
            out->ms = 20;
            world_spend_ms(w, out->ms);
            return;
        }
    }
    /* ---- and no fields the endpoint does not accept. Refused rather than
     * ignored: a typo'd field name that is silently dropped produces an
     * account that is subtly wrong and passes every check the player thought
     * to write. */
    for (int i = 0; i < nargs; i++) {
        bool known = false;
        for (int f = 0; f < ep->nfield; f++) if (!strcmp(args[i].k, ep->field[f])) known = true;
        if (ep->op == OP_LIST && ep->filter[0] && !strcmp(args[i].k, ep->filter)) known = true;
        if (!known) {
            char msg[128];
            snprintf(msg, sizeof msg, "unknown field: %s", args[i].k);
            result_err(out, xml, RB_BAD_REQUEST, msg);
            out->ms = 20;
            world_spend_ms(w, out->ms);
            return;
        }
    }

    const char *kv[SPEC_MAX_KEYS];
    for (int k = 0; k < c->cs->nkey; k++) {
        const char *v = arg_get(args, nargs, c->cs->key[k]);
        kv[k] = v ? v : "";
    }

    out->ms = cost;
    in->calls++;
    Rec *r = NULL;

    switch (ep->op) {
    case OP_LIST: {
        const char *filter = ep->filter[0] ? arg_get(args, nargs, ep->filter) : NULL;
        out->status = RB_OK;
        buf_puts(&out->body, xml ? "<records>" : "[");
        int n = 0;
        for (size_t i = 0; i < c->nr; i++) {
            if (c->r[i].dead) continue;
            if (filter) {
                const char *have = rec_get(&c->r[i], ep->filter);
                if (!have || strcmp(have, filter)) continue;
            }
            if (n++ && !xml) buf_putc(&out->body, ',');
            body_rec(&out->body, xml, &c->r[i]);
        }
        buf_puts(&out->body, xml ? "</records>" : "]");
        /* Listing a large collection costs more than listing a small one.
         * This is the pressure that makes a player stop pulling the whole
         * directory on every ticket and start filtering. */
        out->ms += (int)(c->nr / 50);
        break;
    }
    case OP_GET:
        r = coll_find(c, kv, c->cs->nkey);
        if (!r || r->dead) { result_err(out, xml, RB_NOT_FOUND, "no such record"); break; }
        out->status = RB_OK;
        body_rec(&out->body, xml, r);
        break;

    case OP_CREATE: {
        /* References first: an ordering mistake must fail before anything is
         * written, or the player is left cleaning up after their own script. */
        for (int i = 0; i < ep->nref; i++) {
            const char *val = arg_get(args, nargs, ep->reffield[i]);
            if (!val) continue;
            Coll *rc = inst_coll(in, ep->refcoll[i]);
            const char *one[1] = { val };
            Rec *target = rc ? coll_find(rc, one, 1) : NULL;
            if (!target || target->dead) {
                char msg[160];
                snprintf(msg, sizeof msg, "%s '%s' does not exist in %s",
                         ep->reffield[i], val, ep->refcoll[i]);
                result_err(out, xml, RB_MISSING_REF, msg);
                goto done;
            }
        }

        r = coll_find(c, kv, c->cs->nkey);
        /* A key that is identity rather than storage has to be free across
         * the whole service, not just on this box. */
        if (!r && c->cs->service_scope) {
            Inst *holder = NULL;
            Rec *elsewhere = service_find(w, in, c->cs->name, kv, c->cs->nkey, &holder);
            if (elsewhere) {
                char msg[200];
                snprintf(msg, sizeof msg, "%s is already taken on %s",
                         c->cs->key[0], holder ? holder->id : "another appliance");
                result_err(out, xml, RB_CONFLICT, msg);
                goto done;
            }
        }
        if (r) {
            if (r->dead && !c->reuse) {
                result_err(out, xml, RB_CONFLICT, "that key is spent and cannot be reused");
                break;
            }
            if (r->dead) {
                /* The key was released; make a fresh record rather than
                 * resurrecting the old one's fields. */
                coll_unindex(c, r);
                r = NULL;
            } else if (ep->nidem) {
                /* IDEMPOTENCY (handoff §8.3). Creating what already exists is
                 * not an error when the endpoint says so -- it is the correct
                 * answer to a retried batch, and it is the single most useful
                 * thing a player can learn from this game. 200, not 201, so a
                 * script can still tell the difference if it cares. */
                out->status = RB_OK;
                body_rec(&out->body, xml, r);
                break;
            } else {
                result_err(out, xml, RB_CONFLICT, "already exists");
                break;
            }
        }
        r = coll_insert(c, prov, w->day);
        for (int i = 0; i < ep->nfield; i++) {
            const char *v = arg_get(args, nargs, ep->field[i]);
            if (v) rec_set(r, ep->field[i], v);
        }
        coll_index_rec(c, r);
        out->status = RB_CREATED;
        body_rec(&out->body, xml, r);

        /* ---- THE NASTY ONE (handoff §10): 0.5% of writes commit and then
         * time out. Retrying creates a duplicate; not retrying leaves a gap.
         * The player learns to make operations idempotent because the game
         * punishes both alternatives, and it must be here from the first hour
         * of Act II or the lesson arrives too late to matter. */
        if (ep->failure_modes & FM_TIMEOUT_AFTER_COMMIT && rng_range(&w->rng, 0, 9999) < 50) {
            out->committed = true;
            result_err(out, xml, RB_TRANSIENT, "timeout");
            out->ms += 4000;
        }
        break;
    }
    case OP_UPDATE:
        r = coll_find(c, kv, c->cs->nkey);
        if (!r || r->dead) { result_err(out, xml, RB_NOT_FOUND, "no such record"); break; }
        for (int i = 0; i < ep->nfield; i++) {
            bool is_key = false;
            for (int k = 0; k < c->cs->nkey; k++) if (!strcmp(ep->field[i], c->cs->key[k])) is_key = true;
            if (is_key) continue;      /* the key is how you found it, not something to change */
            const char *v = arg_get(args, nargs, ep->field[i]);
            if (v) rec_set(r, ep->field[i], v);
        }
        out->status = RB_OK;
        body_rec(&out->body, xml, r);
        break;

    case OP_DELETE:
        r = coll_find(c, kv, c->cs->nkey);
        if (!r || r->dead) {
            /* Deleting what is not there is not an error, for the same reason
             * creating what is already there is not: a retried batch must be
             * able to finish. */
            out->status = RB_OK;
            buf_puts(&out->body, xml ? "<ok/>" : "{\"ok\":true}");
            break;
        }
        r->dead = true;
        if (c->reuse) coll_unindex(c, r);
        out->status = RB_OK;
        buf_puts(&out->body, xml ? "<ok/>" : "{\"ok\":true}");
        break;

    default:
        result_err(out, xml, RB_TRANSIENT, "unimplemented operation");
        break;
    }

done:
    /* ---- and the vendor's honesty, applied last, over whatever happened.
     * The legacy vendor returns 200 on failure. The error is still in the
     * body; the player must read it. This is a whole tier of script quality
     * expressed as one line, and it is the reason `verify everything` is
     * advice the game can teach instead of state. */
    if (lies && out->status >= 400) out->status = RB_OK;

    world_spend_ms(w, out->ms);
}

void inst_render(const Inst *in, Buf *out)
{
    buf_printf(out, "{\"id\":\"%s\",\"model\":\"%s\",\"vendor\":\"%s\",\"kind\":\"%s\","
                    "\"api\":%s,\"web\":%s,\"theme\":\"%s\",\"capacity\":%d,\"load_pct\":%d,"
                    "\"installed_day\":%d,\"calls\":%lld,\"records\":[",
               in->id, in->m->model, in->v->name, in->m->kind,
               in->m->has_api ? "true" : "false", in->m->has_web ? "true" : "false",
               in->m->theme, in->m->capacity, inst_load_pct(in),
               in->installed_day, (long long)in->calls);
    for (int i = 0; i < in->ncoll; i++) {
        size_t live = 0;
        for (size_t k = 0; k < in->coll[i].nr; k++) if (!in->coll[i].r[k].dead) live++;
        buf_printf(out, "%s{\"%s\":%zu}", i ? "," : "", in->coll[i].cs->name, live);
    }
    buf_puts(out, "]}");
}

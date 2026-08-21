/* proto.c — command dispatch.
 *
 * PROTOCOL NOTE, from the handoff §14 and paid for once already in NOMINAL:
 * this looks like a request/response protocol because that is what it is. It
 * is not HTTP, it does not become HTTP, and the in-game appliance API that
 * arrives at M1 will not be HTTP either — request in, response out, with
 * latency and rate limits as numbers. The lesson of the packet simulation is
 * that fidelity to a real protocol buys nothing and costs everything.
 */
#include "proto.h"
#include "ticket.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

/* ------------------------------------------------------------ tokenising */
/* Whitespace-separated words, no quoting. Deliberately primitive: tickets are
 * structured objects and arguments are identifiers (handoff decision 5), so
 * the day this needs a quoting rule is the day something has gone wrong with
 * the data model. */
#define MAX_ARGV 8

static int split(char *line, char *argv[MAX_ARGV])
{
    int argc = 0;
    char *p = line;
    while (*p && argc < MAX_ARGV) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = 0;
    }
    return argc;
}

static void ok(Buf *out, const char *fmt, ...)
{
    char tmp[512];
    va_list ap; va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    buf_printf(out, "+OK %s\n.\n", tmp);
}

static void err(Buf *out, const char *fmt, ...)
{
    char tmp[512];
    va_list ap; va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    buf_printf(out, "-ERR %s\n.\n", tmp);
}

static int dept_by_name(const char *s)
{
    for (int i = 0; i < RB_DEPT__N; i++)
        if (strcmp(s, rb_dept_name[i]) == 0) return i;
    return -1;
}

static void put_user(Buf *out, const User *u)
{
    buf_printf(out,
        "{\"id\":\"%s\",\"given\":\"%s\",\"family\":\"%s\","
        "\"dept\":\"%s\",\"prov\":\"%s\",\"hired_day\":%d,\"left_day\":%d}\n",
        u->id, u->given, u->family,
        rb_dept_name[u->dept], prov_name((Prov)u->prov), u->hired_day, u->left_day);
}

/* k=v arguments, which is what every appliance call takes. Splitting here
 * rather than in each verb keeps the argument shape identical between
 * api.call and form.submit — the same fields, whether a script sent them or a
 * player typed them into a form. That identity is what makes the macro
 * recorder possible at M4. */
static int parse_fields(char *argv[MAX_ARGV], int from, int argc, Field *f, int cap, char *err, size_t errcap)
{
    int n = 0;
    for (int i = from; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (!eq) { snprintf(err, errcap, "argument %d is not field=value: %s", i - from + 1, argv[i]); return -1; }
        if (n >= cap) { snprintf(err, errcap, "too many fields"); return -1; }
        *eq = 0;
        snprintf(f[n].k, sizeof f[n].k, "%s", argv[i]);
        snprintf(f[n].v, sizeof f[n].v, "%s", eq + 1);
        n++;
    }
    return n;
}

/* The response shape for any appliance call. The status code is on the +OK
 * line and the body follows, so a script can branch without parsing the body
 * — and so a script that ignores the status still gets the body, which is
 * how the legacy vendor catches people out. */
static void put_result(Buf *out, const ApiResult *r)
{
    buf_printf(out, "+OK %d %s%d ms\n", r->status, r->committed ? "committed " : "", r->ms);
    buf_put(out, r->body.p ? r->body.p : "", r->body.len);
    buf_puts(out, "\n.\n");
}

/* ---------------------------------------------------------------- session */
void proto_open(Session *s, World *w)
{
    s->w = w;
    /* Writes over the API are attributed to a script by default, because
     * that is what is on the other end of a socket. The Godot client will set
     * this to PROV_HAND for work the player does through a form, and that
     * distinction is the whole of the debt mechanic (handoff §11). */
    s->prov = PROV_SCRIPT;
    s->open = true;
}

void proto_hello(Session *s, Buf *out)
{
    buf_printf(out,
        "+OK RUNBOOK/1 %s — day %d, %d users\n"
        "type 'help'; every response ends with a lone '.'\n.\n",
        s->w->org, s->w->day, s->w->active);
}

static void cmd_help(Buf *out)
{
    /* THIS LIST IS A TECHNICAL CLAIM AND THE PROJECT RULE APPLIES TO IT
     * (handoff §13): every verb named here must exist and behave as
     * described, checked by running it. --mancheck arrives at M1 and will
     * execute this text against a live world. Until it does, adding a verb
     * here that does not work is the exact failure the rule exists to
     * prevent, so do not. */
    buf_puts(out,
        "+OK verbs\n"
        "help                          this\n"
        "world.info                    org, day, headcount\n"
        "world.hash                    the state fingerprint the gates compare\n"
        "world.dump                    the whole world, canonically\n"
        "day.advance [n]               advance n whole days (default 1)\n"
        "session.as hand|script|system attribute this session's writes\n"
        "user.list [active|all]        one user per line\n"
        "user.get <id|login>           one user\n"
        "user.add <given> <family> <dept>   returns the id and the derived login\n"
        "user.offboard <id>            idempotent\n"
        "depts                         valid department names\n"
        "\n"
        "ticket.list [open|closed|all] [n]   the queue; reading it settles it\n"
        "ticket.get <id>               one ticket\n"
        "ticket.check <id>             every acceptance check, and why it fails\n"
        "ticket.stats                  queue depth, SLA, who closed what\n"
        "ticket.types                  the types that exist, and what they verify\n"
        "\n"
        "models                        every appliance model there is a spec for\n"
        "appl.list                     appliances installed in this org\n"
        "appl.info <instance>          one appliance: load, record counts\n"
        "appl.doc <instance|model>     the manual: every endpoint, every field\n"
        "appl.forms <instance|model>   the web UI, as data\n"
        "api.call <instance> <endpoint> [field=value ...]\n"
        "form.submit <instance> <form> [field=value ...]\n"
        "quit\n"
        ".\n");
}

bool proto_exec(Session *s, const char *line, Buf *out)
{
    char buf[RB_LINE_MAX];
    snprintf(buf, sizeof buf, "%s", line);
    char *argv[MAX_ARGV];
    int argc = split(buf, argv);
    if (argc == 0) { buf_puts(out, ".\n"); return true; }

    const char *cmd = argv[0];
    World *w = s->w;

    if (!strcmp(cmd, "help"))  { cmd_help(out); return true; }
    if (!strcmp(cmd, "quit") || !strcmp(cmd, "exit")) {
        buf_puts(out, "+OK bye\n.\n");
        s->open = false;
        return false;
    }

    if (!strcmp(cmd, "world.info")) {
        buf_printf(out, "+OK {\"org\":\"%s\",\"seed\":%llu,\"day\":%d,\"minute\":%d,"
                        "\"minutes_left\":%d,\"day_minutes\":%d,\"users_total\":%zu,"
                        "\"users_active\":%d,\"appliances\":%zu}\n.\n",
                   w->org, (unsigned long long)w->seed, w->day, world_minute(w),
                   RB_DAY_MINUTES - world_minute(w),
                   RB_DAY_MINUTES, w->nusers, w->active, w->ninst);
        return true;
    }

    if (!strcmp(cmd, "world.hash")) {
        ok(out, "%016llx", (unsigned long long)world_hash(w));
        return true;
    }

    if (!strcmp(cmd, "world.dump")) {
        buf_puts(out, "+OK dump\n");
        world_dump(w, out);
        buf_puts(out, ".\n");
        return true;
    }

    if (!strcmp(cmd, "day.advance")) {
        int n = (argc > 1) ? atoi(argv[1]) : 1;
        if (n < 1 || n > 3650) { err(out, "day.advance takes 1..3650"); return true; }
        int hired = 0;
        for (int i = 0; i < n; i++) hired += world_day_advance(w);
        ok(out, "day %d, hired %d, active %d", w->day, hired, w->active);
        return true;
    }

    if (!strcmp(cmd, "session.as")) {
        if (argc < 2) { err(out, "session.as hand|script|system"); return true; }
        if      (!strcmp(argv[1], "hand"))   s->prov = PROV_HAND;
        else if (!strcmp(argv[1], "script")) s->prov = PROV_SCRIPT;
        else if (!strcmp(argv[1], "system")) s->prov = PROV_SYSTEM;
        else { err(out, "not a provenance: %s", argv[1]); return true; }
        ok(out, "writes attributed to %s", prov_name(s->prov));
        return true;
    }

    if (!strcmp(cmd, "depts")) {
        buf_puts(out, "+OK depts\n");
        for (int i = 0; i < RB_DEPT__N; i++) buf_printf(out, "%s\n", rb_dept_name[i]);
        buf_puts(out, ".\n");
        return true;
    }

    /* ---------------------------------------------------------- tickets */
    /* READING THE QUEUE SETTLES IT FIRST, and costs nothing.
     *
     * Handoff decision 9: tickets close by state verification, never by
     * player assertion. There is deliberately no "resolve" verb here, and
     * adding one would end the game. What there is instead is this: whenever
     * anyone looks at the queue, the game re-checks every open ticket against
     * the world, and the ones whose work is done are already closed by the
     * time the player sees the list.
     *
     * Verification is free because it is the oracle, not a resource. Charging
     * for it would teach players not to check their work, which is precisely
     * backwards. */
    if (!strcmp(cmd, "ticket.list")) {
        world_ticket_sweep(w);
        bool closed = argc > 1 && !strcmp(argv[1], "closed");
        bool all    = argc > 1 && !strcmp(argv[1], "all");
        int limit = 0;
        for (int i = 1; i < argc; i++) if (argv[i][0] >= '0' && argv[i][0] <= '9') limit = atoi(argv[i]);
        buf_puts(out, "+OK tickets\n");
        int shown = 0;
        for (size_t i = 0; i < w->ntick; i++) {
            bool is_open = w->tick[i].closed_day < 0;
            if (!all && (is_open == closed)) continue;
            if (limit && shown >= limit) break;
            ticket_render(w, &w->tick[i], out);
            buf_putc(out, '\n');
            shown++;
        }
        buf_puts(out, ".\n");
        return true;
    }

    if (!strcmp(cmd, "ticket.get")) {
        if (argc < 2) { err(out, "ticket.get <id>"); return true; }
        world_ticket_sweep(w);
        Ticket *t = world_ticket_find(w, argv[1]);
        if (!t) { err(out, "no such ticket: %s", argv[1]); return true; }
        buf_puts(out, "+OK ticket\n");
        ticket_render(w, t, out);
        buf_puts(out, "\n.\n");
        return true;
    }

    /* WHY IT WILL NOT CLOSE. Every check, its documentation, and for the ones
     * that fail, the reason. A ticket that refuses to close and will not say
     * why is the single most frustrating thing this game could do, and the
     * temptation to hide the reason -- to make the player "work it out" -- is
     * the diagnosis-as-content trap that killed every earlier attempt in this
     * lineage (handoff §2). The difficulty is the volume. It is never this. */
    if (!strcmp(cmd, "ticket.check")) {
        if (argc < 2) { err(out, "ticket.check <id>"); return true; }
        Ticket *t = world_ticket_find(w, argv[1]);
        if (!t) { err(out, "no such ticket: %s", argv[1]); return true; }
        Verdict v;
        ticket_evaluate(w, t, &v);
        if (v.all) world_ticket_sweep(w);
        buf_printf(out, "+OK %s %s\n", t->id, v.all ? "passes" : "does not pass yet");
        for (int i = 0; i < v.n; i++) {
            /* n/a, never PASS. A check that did not apply has proved nothing,
             * and showing it as a pass would teach the player to read a green
             * column that is partly decoration. */
            const char *mark = v.skipped[i] ? " n/a" : (v.passed[i] ? "PASS" : "    ");
            buf_printf(out, "%s %-28s %s%s%s\n", mark,
                       t->type->check[i].id, t->type->check[i].doc,
                       (v.passed[i] && !v.skipped[i]) ? "" : "  -- ",
                       (v.passed[i] && !v.skipped[i]) ? "" : v.why[i]);
        }
        buf_puts(out, ".\n");
        return true;
    }

    if (!strcmp(cmd, "ticket.stats")) {
        world_ticket_sweep(w);
        buf_puts(out, "+OK stats\n");
        world_ticket_stats(w, out);
        buf_puts(out, "\n.\n");
        return true;
    }

    if (!strcmp(cmd, "ticket.types")) {
        buf_puts(out, "+OK ticket types\n");
        for (size_t i = 0; i < w->specs->nticket; i++) {
            const TicketType *tt = &w->specs->ticket[i];
            buf_printf(out, "{\"id\":\"%s\",\"subject\":\"%s\",\"sla_minutes\":%d,\"doc\":\"%s\","
                            "\"acceptance\":[", tt->id, tt->subject_kind, tt->sla_minutes, tt->doc);
            for (int c = 0; c < tt->ncheck; c++)
                buf_printf(out, "%s{\"id\":\"%s\",\"doc\":\"%s\"}", c ? "," : "",
                           tt->check[c].id, tt->check[c].doc);
            buf_puts(out, "]}\n");
        }
        buf_puts(out, ".\n");
        return true;
    }

    /* ------------------------------------------------------- appliances */
    if (!strcmp(cmd, "appl.list")) {
        buf_puts(out, "+OK appliances\n");
        for (size_t i = 0; i < w->ninst; i++) { inst_render(w->inst[i], out); buf_putc(out, '\n'); }
        buf_puts(out, ".\n");
        return true;
    }

    if (!strcmp(cmd, "appl.info")) {
        if (argc < 2) { err(out, "appl.info <instance>"); return true; }
        Inst *in = world_inst(w, argv[1]);
        if (!in) { err(out, "no such appliance: %s", argv[1]); return true; }
        buf_puts(out, "+OK appliance\n");
        inst_render(in, out);
        buf_puts(out, "\n.\n");
        return true;
    }

    /* THE MANUAL. Generated from the same spec that drives the endpoints, so
     * it cannot document something that does not exist (handoff §13). This is
     * what --mancheck executes. */
    if (!strcmp(cmd, "appl.doc")) {
        if (argc < 2) { err(out, "appl.doc <instance|model>"); return true; }
        Inst *in = world_inst(w, argv[1]);
        const Model *m = in ? in->m : spec_model(w->specs, argv[1]);
        if (!m) { err(out, "no such appliance or model: %s", argv[1]); return true; }
        const Vendor *v = spec_vendor(w->specs, m->vendor);
        buf_printf(out, "+OK %s\n", m->model);
        buf_printf(out, "%s\n", m->doc);
        buf_printf(out, "vendor: %s (%s) — %s\n", v->name, arch_name(v->arch), v->doc);
        buf_printf(out, "interfaces: %s%s%s\n",
                   m->has_web ? "web" : "", (m->has_web && m->has_api) ? ", " : "",
                   m->has_api ? "api" : "");
        buf_printf(out, "capacity: %d at nominal; rate limit %d calls/minute\n",
                   m->capacity, v->rate_limit);
        for (int i = 0; i < m->ncoll; i++) {
            const CollSpec *cs = &m->coll[i];
            buf_printf(out, "collection %s: keyed by", cs->name);
            for (int k = 0; k < cs->nkey; k++) buf_printf(out, " %s", cs->key[k]);
            buf_printf(out, "%s; fields:", cs->reuse_key ? "" : " (keys are never reused)");
            for (int f = 0; f < cs->nfield; f++) buf_printf(out, " %s", cs->field[f]);
            buf_putc(out, '\n');
        }
        for (int i = 0; i < m->nep; i++) {
            const Endpoint *e = &m->ep[i];
            buf_printf(out, "\nendpoint %s (%s %s, %d ms)\n", e->id, op_name(e->op), e->coll, e->latency_ms);
            if (e->doc[0]) buf_printf(out, "  %s\n", e->doc);
            if (e->nfield) {
                buf_puts(out, "  fields:");
                for (int f = 0; f < e->nfield; f++) buf_printf(out, " %s", e->field[f]);
                buf_putc(out, '\n');
            }
            if (e->nrequired) {
                buf_puts(out, "  required:");
                for (int f = 0; f < e->nrequired; f++) buf_printf(out, " %s", e->required[f]);
                buf_putc(out, '\n');
            }
            if (e->nidem) {
                buf_puts(out, "  idempotent on:");
                for (int f = 0; f < e->nidem; f++) buf_printf(out, " %s", e->idem[f]);
                buf_putc(out, '\n');
            }
            for (int f = 0; f < e->nref; f++)
                buf_printf(out, "  %s must already exist in %s\n", e->reffield[f], e->refcoll[f]);
            if (e->failure_modes) {
                buf_puts(out, "  can fail with:");
                if (e->failure_modes & FM_RATE_LIMITED)         buf_puts(out, " rate_limited");
                if (e->failure_modes & FM_TRANSIENT)            buf_puts(out, " transient");
                if (e->failure_modes & FM_TIMEOUT_AFTER_COMMIT) buf_puts(out, " timeout_after_commit");
                if (e->failure_modes & FM_STALL)                buf_puts(out, " stall");
                buf_putc(out, '\n');
            }
            for (int x = 0; x < e->nexample; x++) buf_printf(out, "  example: %s\n", e->example[x]);
        }
        buf_puts(out, ".\n");
        return true;
    }

    /* The web UI, as data. No client renders it until M3; it is served now so
     * that when one does, it renders THIS rather than growing its own idea of
     * what the appliance can do (handoff decision 6). */
    if (!strcmp(cmd, "appl.forms")) {
        if (argc < 2) { err(out, "appl.forms <instance|model>"); return true; }
        Inst *in = world_inst(w, argv[1]);
        const Model *m = in ? in->m : spec_model(w->specs, argv[1]);
        if (!m) { err(out, "no such appliance or model: %s", argv[1]); return true; }
        buf_puts(out, "+OK forms\n");
        for (int i = 0; i < m->nform; i++) {
            const Form *f = &m->form[i];
            buf_printf(out, "{\"id\":\"%s\",\"title\":\"%s\",\"calls\":\"%s\",\"theme\":\"%s\",\"fields\":[",
                       f->id, f->title, f->calls, m->theme);
            for (int k = 0; k < f->nfield; k++) buf_printf(out, "%s\"%s\"", k ? "," : "", f->field[k]);
            buf_puts(out, "]}\n");
        }
        buf_puts(out, ".\n");
        return true;
    }

    if (!strcmp(cmd, "models")) {
        buf_puts(out, "+OK models\n");
        for (size_t i = 0; i < w->specs->nmodel; i++) {
            const Model *m = &w->specs->model[i];
            const Vendor *v = spec_vendor(w->specs, m->vendor);
            buf_printf(out, "{\"id\":\"%s\",\"model\":\"%s\",\"vendor\":\"%s\",\"arch\":\"%s\","
                            "\"kind\":\"%s\",\"api\":%s,\"cost\":%d}\n",
                       m->id, m->model, v->name, arch_name(v->arch), m->kind,
                       m->has_api ? "true" : "false", v->cost);
        }
        buf_puts(out, ".\n");
        return true;
    }

    /* --------------------------------------------------------- the calls */
    if (!strcmp(cmd, "api.call")) {
        if (argc < 3) { err(out, "api.call <instance> <endpoint> [field=value ...]"); return true; }
        Inst *in = world_inst(w, argv[1]);
        if (!in) { err(out, "no such appliance: %s", argv[1]); return true; }
        Field f[SPEC_MAX_FIELDS];
        char perr[128];
        int nf = parse_fields(argv, 3, argc, f, SPEC_MAX_FIELDS, perr, sizeof perr);
        if (nf < 0) { err(out, "%s", perr); return true; }
        ApiResult r;
        appl_call(w, in, argv[2], f, nf, s->prov, &r);
        put_result(out, &r);
        buf_free(&r.body);
        return true;
    }

    /* THE OTHER WAY IN, AND THE ONE ACT I IS PLAYED WITH.
     *
     * A form submission is the same call with three differences: it works on
     * appliances that have no API, it is attributed to a hand, and it costs
     * whole minutes of the day instead of milliseconds. That last one is the
     * Act I → Act II pressure expressed as a number (§10): two minutes is
     * nothing at five tickets a day and is the entire day at forty. */
    if (!strcmp(cmd, "form.submit")) {
        if (argc < 3) { err(out, "form.submit <instance> <form> [field=value ...]"); return true; }
        Inst *in = world_inst(w, argv[1]);
        if (!in) { err(out, "no such appliance: %s", argv[1]); return true; }
        const Form *form = NULL;
        for (int i = 0; i < in->m->nform; i++) if (!strcmp(in->m->form[i].id, argv[2])) form = &in->m->form[i];
        if (!form) { err(out, "no such form on %s: %s", in->id, argv[2]); return true; }
        Field f[SPEC_MAX_FIELDS];
        char perr[128];
        int nf = parse_fields(argv, 3, argc, f, SPEC_MAX_FIELDS, perr, sizeof perr);
        if (nf < 0) { err(out, "%s", perr); return true; }
        for (int i = 0; i < nf; i++) {
            bool offered = false;
            for (int k = 0; k < form->nfield; k++) if (!strcmp(f[i].k, form->field[k])) offered = true;
            if (!offered) { err(out, "the %s form has no %s field", form->id, f[i].k); return true; }
        }
        ApiResult r;
        /* A form is operated by a person, whatever session asked for it. */
        appl_call(w, in, form->calls, f, nf, PROV_HAND, &r);
        /* The API latency already came out of the day; a human filling in a
         * form costs the rest of the two minutes. */
        int human = RB_FORM_MINUTES * 60000 - r.ms;
        if (human > 0) { world_spend_ms(w, human); r.ms += human; }
        put_result(out, &r);
        buf_free(&r.body);
        return true;
    }

    if (!strcmp(cmd, "user.list")) {
        bool all = (argc > 1 && !strcmp(argv[1], "all"));
        int n = 0;
        buf_puts(out, "+OK users\n");
        for (size_t i = 0; i < w->nusers; i++) {
            if (!all && w->users[i].left_day >= 0) continue;
            put_user(out, &w->users[i]);
            n++;
        }
        buf_printf(out, ".\n");
        (void)n;
        return true;
    }

    if (!strcmp(cmd, "user.get")) {
        if (argc < 2) { err(out, "user.get <id>"); return true; }
        User *u = world_user_find(w, argv[1]);
        if (!u) { err(out, "no such user: %s", argv[1]); return true; }
        buf_puts(out, "+OK user\n");
        put_user(out, u);
        buf_puts(out, ".\n");
        return true;
    }

    if (!strcmp(cmd, "user.add")) {
        if (argc < 4) { err(out, "user.add <given> <family> <dept>"); return true; }
        int d = dept_by_name(argv[3]);
        if (d < 0) { err(out, "no such department: %s (try 'depts')", argv[3]); return true; }
        User *u = world_user_add(w, s->prov, argv[1], argv[2], (uint8_t)d);
        if (!u) { err(out, "%s", w->err); return true; }
        /* A person, and nothing else. No account, no mailbox, no group. What
         * comes back is an id and the login the org's convention WOULD give
         * them — a suggestion, not a reservation. The directory may already
         * have it, and finding that out is the player's job (handoff §8.1). */
        char suggest[RB_NAME_MAX];
        world_login_for(w, u, suggest, sizeof suggest);
        ok(out, "{\"id\":\"%s\",\"convention_login\":\"%s\"}", u->id, suggest);
        return true;
    }

    if (!strcmp(cmd, "user.offboard")) {
        if (argc < 2) { err(out, "user.offboard <id>"); return true; }
        if (!world_user_offboard(w, argv[1])) { err(out, "%s", w->err); return true; }
        ok(out, "offboarded %s", argv[1]);
        return true;
    }

    err(out, "unknown verb: %s (try 'help')", cmd);
    return true;
}

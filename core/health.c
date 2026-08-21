/* health.c — the --health gate.
 *
 * Handoff §13: "Pristine org boots; every appliance reachable; every endpoint
 * in every spec responds."
 *
 * Every section below either asserts something or prints the reason it
 * cannot. NOMINAL's gate suite spent months green while half of it was
 * measuring a placeholder, because a check with nothing to check reports
 * success. PENDING lines are how this one refuses to do that.
 */
#include "proto.h"
#include "ticket.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

static int fails, checks;

static void check(bool cond, const char *what)
{
    checks++;
    if (cond) printf("health: PASS  %s\n", what);
    else    { printf("health: FAIL  %s\n", what); fails++; }
}

static void pending(const char *what, const char *why)
{
    printf("health: PENDING  %s — %s\n", what, why);
}

static void note(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    printf("health:       ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

/* One call, with its body, for the checks that need to read what came back. */
static int call(World *w, Inst *in, const char *ep, const char *k1, const char *v1,
                const char *k2, const char *v2, const char *k3, const char *v3, Buf *body)
{
    Field f[3];
    int n = 0;
    const char *ks[3] = { k1, k2, k3 }, *vs[3] = { v1, v2, v3 };
    for (int i = 0; i < 3; i++)
        if (ks[i]) { snprintf(f[n].k, sizeof f[n].k, "%s", ks[i]);
                     snprintf(f[n].v, sizeof f[n].v, "%s", vs[i]); n++; }
    ApiResult r;
    appl_call(w, in, ep, f, n, PROV_SCRIPT, &r);
    int st = r.status;
    if (body) { buf_clear(body); buf_put(body, r.body.p ? r.body.p : "", r.body.len); }
    buf_free(&r.body);
    return st;
}

/* EVERY VERB `help` ADVERTISES MUST DISPATCH.
 *
 * The small version of --mancheck. `help` is the first document in the game,
 * so it is the first one held to the project rule (§13): a documented thing
 * that does not exist teaches the player to distrust everything. */
static void check_help_verbs(World *w)
{
    Session s;
    proto_open(&s, w);
    Buf help;
    buf_init(&help);
    proto_exec(&s, "help", &help);

    const char *p = help.p;
    int verbs = 0, bad = 0;
    while (p && *p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        char line[256];
        if (len >= sizeof line) len = sizeof line - 1;
        memcpy(line, p, len); line[len] = 0;
        p = nl ? nl + 1 : NULL;

        if (!line[0] || line[0] == '.' || line[0] == '+' || line[0] == '-') continue;
        if (line[0] == ' ' || line[0] == '\t') continue;
        char verb[64];
        size_t n = 0;
        while (line[n] && line[n] != ' ' && n < sizeof verb - 1) { verb[n] = line[n]; n++; }
        verb[n] = 0;
        if (!verb[0] || !strcmp(verb, "quit")) continue;

        Buf r;
        buf_init(&r);
        Session s2;
        proto_open(&s2, w);
        proto_exec(&s2, verb, &r);
        verbs++;
        /* Called with no arguments a verb may legitimately answer -ERR with
         * its usage; what it may not do is not exist. */
        if (r.p && strstr(r.p, "unknown verb")) { note("help advertises '%s', which does not dispatch", verb); bad++; }
        buf_free(&r);
    }
    buf_free(&help);
    check(verbs > 0, "help lists verbs");
    check(bad == 0, "every verb help advertises dispatches");
}

/* EVERY ENDPOINT IN EVERY SPEC RESPONDS — the §13 sentence, literally.
 *
 * It calls each endpoint with no arguments and accepts any answer except "no
 * such endpoint". A 400 for a missing required field is a pass: the endpoint
 * is there and it is checking. What this catches is a spec that declares an
 * endpoint the runtime cannot dispatch, which is the shape every content bug
 * from M8 onward will take. */
static void check_endpoints(World *w)
{
    int total = 0, missing = 0, noapi = 0;
    for (size_t i = 0; i < w->ninst; i++) {
        Inst *in = w->inst[i];
        for (int e = 0; e < in->m->nep; e++) {
            total++;
            Buf body;
            buf_init(&body);
            int st = call(w, in, in->m->ep[e].id, NULL, NULL, NULL, NULL, NULL, NULL, &body);
            if (st == RB_NO_API) noapi++;
            else if (st == RB_NOT_FOUND && strstr(body.p ? body.p : "", "no such endpoint")) {
                note("%s declares endpoint %s, which does not dispatch", in->m->id, in->m->ep[e].id);
                missing++;
            }
            buf_free(&body);
        }
    }
    check(total > 0, "the installed appliances declare endpoints");
    check(missing == 0, "every endpoint in every spec responds");
    if (noapi) note("%d endpoints are on web-only appliances and are unreachable from a script, by design", noapi);
}

int health_run(uint64_t seed, const char *specdir)
{
    fails = checks = 0;
    printf("health: seed %llu\n", (unsigned long long)seed);

    /* ---- specs load and validate */
    char serr[RB_ERR_MAX];
    Specs *specs = specs_load(specdir, serr, sizeof serr);
    check(specs != NULL, "every appliance spec loads and validates");
    if (!specs) { note("%s", serr); printf("health: %d checks, %d failed\n", checks, fails + 1); return 1; }
    note("%zu vendors, %zu appliance models", specs->nvendor, specs->nmodel);

    /* ---- a pristine org boots */
    World *w = world_new(seed, specs);
    check(w != NULL, "pristine org boots");
    if (!w) return 1;
    check(w->day == 0 && w->ms == 0, "the clock starts at day 0, minute 0");
    check(w->nusers == RB_START_USERS, "the org opens at the Act I headcount");
    check(w->active == RB_START_USERS, "every seeded user is active");
    check(w->ninst > 0, "the org has an appliance installed before the player arrives");

    Inst *dir = NULL;
    for (size_t i = 0; i < w->ninst; i++) if (!strcmp(w->inst[i]->m->kind, "directory")) dir = w->inst[i];
    check(dir != NULL, "there is a directory of record");
    if (!dir) { printf("health: %d checks, %d failed\n", checks, fails); return 1; }

    /* ---- identifiers are sound. Every one of these is something a player's
     * script will assume without being told, so the world owes it to them. */
    bool ids_ok = true, prov_ok = true;
    for (size_t i = 0; i < w->nusers; i++) {
        if (!w->users[i].id[0]) { ids_ok = false; break; }
        if (w->users[i].prov != PROV_SEED) prov_ok = false;
        for (size_t j = i + 1; j < w->nusers; j++)
            if (!strcmp(w->users[i].id, w->users[j].id)) ids_ok = false;
    }
    check(ids_ok, "every user has a unique, non-empty id");
    check(prov_ok, "the org the player inherited is attributed to nobody (seed)");

    /* ---- the directory matches the org. This is the state the very first
     * ticket type will be verified against, so if it is wrong on day zero
     * every acceptance check built on it is wrong too. */
    Coll *ac = inst_coll(dir, "accounts");
    Coll *mc = inst_coll(dir, "memberships");
    Coll *gc = inst_coll(dir, "groups");
    check(ac && ac->nr == w->nusers, "every person who was already here has an account");
    check(gc && gc->nr == RB_DEPT__N, "there is a group per department");
    check(mc && mc->nr == w->nusers, "every seeded account is in its department's group");

    bool logins_ok = ac != NULL, refs_ok = ac != NULL;
    for (size_t i = 0; ac && i < ac->nr; i++) {
        const char *l = rec_get(&ac->r[i], "login");
        const char *u = rec_get(&ac->r[i], "user_ref");
        if (!l || !*l) logins_ok = false;
        if (!u || !world_user_find(w, u)) refs_ok = false;
        for (size_t j = i + 1; j < ac->nr; j++) {
            const char *l2 = rec_get(&ac->r[j], "login");
            if (l && l2 && !strcmp(l, l2)) logins_ok = false;
        }
    }
    check(logins_ok, "every account login is unique and non-empty");
    check(refs_ok, "every account points at a person who exists");

    /* ---- and the org's own naming convention held. A directory that does
     * not follow the rule the in-game document states is a directory that
     * teaches the player the wrong rule. */
    bool convention_ok = true;
    for (size_t i = 0; ac && i < ac->nr && convention_ok; i++) {
        const char *u = rec_get(&ac->r[i], "user_ref");
        const char *l = rec_get(&ac->r[i], "login");
        User *usr = u ? world_user_find(w, u) : NULL;
        if (!usr || !l) { convention_ok = false; break; }
        char expect[RB_NAME_MAX];
        world_login_for(w, usr, expect, sizeof expect);
        /* Either the convention exactly, or the convention with a
         * de-collision suffix. Nothing else. */
        size_t n = strlen(expect);
        if (strncmp(l, expect, n) != 0) convention_ok = false;
        else for (const char *p = l + n; *p; p++) if (*p < '0' || *p > '9') convention_ok = false;
    }
    check(convention_ok, "every seeded account follows the org's naming convention");

    /* ---- the four lessons, asserted on live state (handoff §8).
     * They are checked here and not left to a comment because each one is the
     * kind of correctness someone simplifies away while reading the code that
     * implements it and not the reason for it. */
    Buf body;
    buf_init(&body);

    int st1 = call(w, dir, "create_account", "login", "hcheck1", "user_ref", w->users[0].id,
                   "display_name", "Health Check", &body);
    int st1b = call(w, dir, "create_account", "login", "hcheck1", "user_ref", w->users[0].id,
                    "display_name", "Health Check", &body);
    /* dept is required, so both of those must have been refused for the same
     * reason — which is itself worth asserting: a required field that is not
     * enforced is a field the player will forget and never be told about. */
    check(st1 == RB_BAD_REQUEST && st1b == RB_BAD_REQUEST, "a missing required field is refused, every time");

    Field f[4];
    snprintf(f[0].k, sizeof f[0].k, "login");        snprintf(f[0].v, sizeof f[0].v, "hcheck1");
    snprintf(f[1].k, sizeof f[1].k, "user_ref");     snprintf(f[1].v, sizeof f[1].v, "%s", w->users[0].id);
    snprintf(f[2].k, sizeof f[2].k, "display_name"); snprintf(f[2].v, sizeof f[2].v, "Health_Check");
    snprintf(f[3].k, sizeof f[3].k, "dept");         snprintf(f[3].v, sizeof f[3].v, "engineering");
    ApiResult r1, r2;
    appl_call(w, dir, "create_account", f, 4, PROV_SCRIPT, &r1);
    appl_call(w, dir, "create_account", f, 4, PROV_SCRIPT, &r2);
    size_t after = ac->nr;
    check((r1.status == RB_CREATED || r1.status == RB_TRANSIENT) && r2.status == RB_OK,
          "creating the same account twice is not an error (idempotency)");
    buf_free(&r1.body); buf_free(&r2.body);

    /* One record, not two. This is the assertion that would have caught the
     * duplicate-factory bug the whole mechanic is about. */
    int found = 0;
    for (size_t i = 0; i < ac->nr; i++) {
        const char *l = rec_get(&ac->r[i], "login");
        if (l && !strcmp(l, "hcheck1") && !ac->r[i].dead) found++;
    }
    check(found == 1, "and it leaves one account, not two");
    (void)after;

    int st_ref = call(w, dir, "add_member", "login", "hcheck1", "group", "dept-nonexistent", NULL, NULL, &body);
    check(st_ref == RB_MISSING_REF, "a membership in a group that does not exist is refused (ordering)");

    int st_ord = call(w, dir, "add_member", "login", "nobody-at-all", "group", "dept-engineering", NULL, NULL, &body);
    check(st_ord == RB_MISSING_REF, "a membership for an account that does not exist is refused (ordering)");

    call(w, dir, "delete_account", "login", "hcheck1", NULL, NULL, NULL, NULL, &body);
    int st_spent = call(w, dir, "create_account", "login", "hcheck1", "user_ref", w->users[0].id,
                        "display_name", "Health_Check", &body);
    /* dept is missing again here, so a 400 would be ambiguous — send it properly. */
    ApiResult r3;
    appl_call(w, dir, "create_account", f, 4, PROV_SCRIPT, &r3);
    check(r3.status == RB_CONFLICT, "a deleted account's login is spent and cannot be reused");
    buf_free(&r3.body);
    (void)st_spent;

    /* ---- offboarding a person is idempotent (§8.3) */
    int before = w->active;
    bool a = world_user_offboard(w, w->users[1].id);
    bool b = world_user_offboard(w, w->users[1].id);
    check(a && b && w->active == before - 1, "offboarding twice is not an error and does not count twice");
    check(!world_user_offboard(w, "u_nope"), "offboarding a stranger fails loudly");

    /* ---- the day budget is real */
    int32_t d0 = w->day;
    world_spend_ms(w, RB_DAY_MS + 1000);
    check(w->day == d0 + 1, "spending more than a day of work rolls into the next day");

    buf_free(&body);

    /* ---- the seed reaches the world */
    World *w2 = world_new(seed + 1, specs);
    check(world_hash(w) != world_hash(w2), "a different seed builds a different org");
    world_free(w2);

    /* ---- the oracle (handoff decision 9) */
    {
        Session s;
        proto_open(&s, w);
        Buf r;
        buf_init(&r);

        int32_t before_users = (int32_t)w->nusers;
        world_day_advance(w);
        check(w->ntick > 0, "a day's hires raise onboarding tickets");
        check((int32_t)w->nusers > before_users, "and the people they are about exist");

        Ticket *t = w->ntick ? &w->tick[0] : NULL;
        Verdict v;
        if (t) ticket_evaluate(w, t, &v);
        check(t && !v.all, "a fresh onboarding ticket does not pass its own checks");
        check(t && t->closed_day < 0, "and it is open");

        /* THERE MUST BE NO WAY TO CLOSE A TICKET BY SAYING SO.
         *
         * This is decision 9, and it is the one every other part of the game
         * rests on: the vacation test, the run report, the balance harness.
         * A "mark as done" verb would not look like a mistake in review -- it
         * would look like a convenience -- so it is asserted here, by name,
         * and the assertion is the argument. */
        const char *forbidden[] = { "ticket.resolve", "ticket.close", "ticket.done", "ticket.complete" };
        bool none = true;
        for (size_t i = 0; i < sizeof forbidden / sizeof forbidden[0]; i++) {
            buf_clear(&r);
            Session s2;
            proto_open(&s2, w);
            proto_exec(&s2, forbidden[i], &r);
            if (!r.p || !strstr(r.p, "unknown verb")) { note("'%s' exists; it must not", forbidden[i]); none = false; }
        }
        check(none, "no verb closes a ticket by assertion; only state does");

        /* And doing the work does close it, through the API, like a player. */
        if (t) {
            User *u = world_user_find(w, t->subject);
            char login[RB_NAME_MAX];
            if (u) world_login_for(w, u, login, sizeof login);
            const char *dept = u ? rb_dept_name[u->dept] : "engineering";
            char also[RB_NAME_MAX] = "", share_ov[RB_VAL_MAX] = "";
            for (int f = 0; f < t->nfields; f++) {
                if (!strcmp(t->fields[f].k, "also_dept"))      snprintf(also, sizeof also, "%s", t->fields[f].v);
                if (!strcmp(t->fields[f].k, "share_override")) snprintf(share_ov, sizeof share_ov, "%s", t->fields[f].v);
            }
            char cmd[RB_LINE_MAX];
            /* Retry generously: this is a health check, not a skill check. */
            for (int attempt = 0; attempt < 8; attempt++) {
                snprintf(cmd, sizeof cmd, "api.call directory_01 create_account login=%s user_ref=%s display_name=x dept=%s status=active", login, t->subject, dept);
                buf_clear(&r); proto_exec(&s, cmd, &r);
                snprintf(cmd, sizeof cmd, "api.call directory_01 update_account login=%s status=active", login);
                buf_clear(&r); proto_exec(&s, cmd, &r);
                snprintf(cmd, sizeof cmd, "api.call directory_01 add_member login=%s group=dept-%s", login, dept);
                buf_clear(&r); proto_exec(&s, cmd, &r);
                if (also[0]) { snprintf(cmd, sizeof cmd, "api.call directory_01 add_member login=%s group=dept-%s", login, also);
                               buf_clear(&r); proto_exec(&s, cmd, &r); }
                snprintf(cmd, sizeof cmd, "api.call mail_01 create_mailbox login=%s address=%s@harbrook.example quota_mb=2048 status=active", login, login);
                buf_clear(&r); proto_exec(&s, cmd, &r);
                snprintf(cmd, sizeof cmd, "api.call fileserver_01 create_home login=%s path=/home/%s quota_mb=8192", login, login);
                buf_clear(&r); proto_exec(&s, cmd, &r);
                snprintf(cmd, sizeof cmd, "api.call fileserver_01 grant_share login=%s share=%s access=rw",
                         login, share_ov[0] ? share_ov : "");
                if (!share_ov[0]) snprintf(cmd, sizeof cmd, "api.call fileserver_01 grant_share login=%s share=share-%s access=rw", login, dept);
                buf_clear(&r); proto_exec(&s, cmd, &r);
                if (ticket_settle(w, t)) break;
            }
            check(t->closed_day >= 0, "doing the work, through the API, closes the ticket");
            /* THE COUNTERS AGREE WITH THE QUEUE.
             * A performance fix once moved ticket closing off the code path
             * that maintained these, and the run report said "0 closed" about
             * a run that closed seven thousand. Nothing failed; it just lied. */
            {
                int32_t open_n = 0, closed_n = 0;
                for (size_t q = 0; q < w->ntick; q++)
                    (w->tick[q].closed_day < 0) ? open_n++ : closed_n++;
                check(open_n == w->open_count && closed_n == w->closed_total,
                      "the queue counters agree with the queue itself");
            }
            check(t->closed_prov == PROV_SCRIPT || t->closed_prov == PROV_SEED,
                  "and the ticket records who did it");
        }
        buf_free(&r);
    }

    check_endpoints(w);
    check_help_verbs(w);

    /* WHAT IS STILL MISSING, said out loud. The two milestones no gate can
     * finish: M3 asks whether Act I is pleasant and M4 asks whether the
     * relief of the first script lands, and both are questions for a human at
     * a keyboard (handoff §15). A suite that did not mention them would read
     * as though the game were done. */
    pending("the client", "M3; and no gate can answer whether Act I is pleasant");
    pending("scripting on the emulated machine, and the macro recorder", "M4");

    world_free(w);
    specs_free(specs);

    /* --play and --naive-gate are their own gates now; health does not
     * duplicate them, it points at them. */

    printf("health: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}

/* mancheck.c — every documented example, executed against a live world.
 *
 * THE PROJECT RULE, INHERITED AND NON-NEGOTIABLE (handoff §13): every
 * technical claim anywhere in this game — vendor manual, in-game doc, ticket
 * description, source comment — must be true of this world, verified by
 * running it. A vendor manual documenting an endpoint that doesn't exist
 * teaches the player to distrust everything, and the trust is the product.
 *
 * So the manual is not prose that describes the appliance. It is generated
 * from the spec that IS the appliance, and the examples in it are executed
 * here. An example that stops working fails the build, on the commit that
 * broke it, which is the only time it is cheap to fix.
 *
 * WHAT AN EXAMPLE MAY CONTAIN: any verb, with these placeholders substituted
 * against the live world before it runs.
 *
 *   {inst}      an installed instance of the model being documented
 *   {user}      the id of somebody who works here
 *   {login}     an account that exists on {inst}
 *   {newlogin}  a login that does not exist yet, stable within one model
 *
 * An example must answer +OK with a status below 400. If a documented call is
 * supposed to fail, it is not an example — it is a sentence in the doc.
 */
#include "proto.h"
#include <stdio.h>
#include <string.h>

static bool subst(const char *in, char *out, size_t cap,
                  const char *inst, const char *user, const char *login, const char *newlogin,
                  char *missing, size_t mcap)
{
    size_t o = 0;
    for (const char *p = in; *p; ) {
        if (*p == '{') {
            const char *end = strchr(p, '}');
            if (!end) { snprintf(missing, mcap, "unterminated placeholder"); return false; }
            char name[32];
            size_t n = (size_t)(end - p - 1);
            if (n >= sizeof name) { snprintf(missing, mcap, "placeholder too long"); return false; }
            memcpy(name, p + 1, n); name[n] = 0;
            const char *v = NULL;
            if      (!strcmp(name, "inst"))     v = inst;
            else if (!strcmp(name, "user"))     v = user;
            else if (!strcmp(name, "login"))    v = login;
            else if (!strcmp(name, "newlogin")) v = newlogin;
            if (!v) { snprintf(missing, mcap, "unknown placeholder {%s}", name); return false; }
            size_t vl = strlen(v);
            if (o + vl >= cap) { snprintf(missing, mcap, "example too long"); return false; }
            memcpy(out + o, v, vl); o += vl;
            p = end + 1;
        } else {
            if (o + 1 >= cap) { snprintf(missing, mcap, "example too long"); return false; }
            out[o++] = *p++;
        }
    }
    out[o] = 0;
    return true;
}

/* The status is on the +OK line: "+OK 201 400 ms". Anything else — a -ERR, or
 * a 4xx/5xx — is a documented call that does not work. */
static int response_status(const Buf *b)
{
    if (!b->p || b->len < 4) return -1;
    if (strncmp(b->p, "+OK", 3) != 0) return -1;
    int st = 0;
    if (sscanf(b->p + 3, " %d", &st) != 1) return 0;   /* verbs that answer +OK with no code */
    return st;
}

int mancheck_run(uint64_t seed, const char *specdir)
{
    printf("mancheck: seed %llu\n", (unsigned long long)seed);

    char serr[RB_ERR_MAX];
    Specs *specs = specs_load(specdir, serr, sizeof serr);
    if (!specs) { printf("mancheck: FAIL  specs do not load: %s\n", serr); return 1; }

    int ran = 0, bad = 0, undocumented = 0, unexampled = 0;

    for (size_t mi = 0; mi < specs->nmodel; mi++) {
        const Model *m = &specs->model[mi];

        /* A FRESH WORLD PER MODEL. The examples mutate state — they create
         * accounts and delete them — and an example that only passes because
         * a previous model's examples happened to run first is not a check,
         * it is a coincidence. */
        World *w = world_new(seed, specs);
        Session s;
        proto_open(&s, w);

        Inst *inst = NULL;
        for (size_t i = 0; i < w->ninst; i++) if (w->inst[i]->m == m) inst = w->inst[i];
        if (!inst) {
            /* A model nobody installs is still documented, and the doc still
             * has to be true, so install one to check it against. */
            inst = world_install(w, m->id, PROV_SYSTEM);
        }
        if (!inst) { printf("mancheck: FAIL  cannot install %s to check its manual\n", m->id); bad++; world_free(w); continue; }

        /* Something for {login} and {user} to point at. */
        const char *login = "";
        Coll *ac = inst->ncoll ? &inst->coll[0] : NULL;
        for (size_t i = 0; ac && i < ac->nr; i++)
            if (!ac->r[i].dead && ac->cs->nkey) { const char *l = rec_get(&ac->r[i], ac->cs->key[0]); if (l) { login = l; break; } }
        const char *user = w->nusers ? w->users[0].id : "";
        char newlogin[RB_NAME_MAX];
        snprintf(newlogin, sizeof newlogin, "mancheck%zu", mi);

        if (!m->doc[0]) { printf("mancheck: FAIL  model %s has no doc line\n", m->id); undocumented++; }

        for (int e = 0; e < m->nep; e++) {
            const Endpoint *ep = &m->ep[e];
            if (!ep->doc[0]) {
                printf("mancheck: FAIL  %s endpoint %s is undocumented\n", m->id, ep->id);
                undocumented++;
            }
            if (!ep->nexample && m->has_api) {
                /* Not fatal, but named. An endpoint with no example is an
                 * endpoint nothing proves the manual right about. */
                printf("mancheck: WARN  %s endpoint %s has no example\n", m->id, ep->id);
                unexampled++;
            }
            for (int x = 0; x < ep->nexample; x++) {
                char line[RB_LINE_MAX], why[128];
                if (!subst(ep->example[x], line, sizeof line, inst->id, user, login, newlogin, why, sizeof why)) {
                    printf("mancheck: FAIL  %s/%s example %d: %s\n", m->id, ep->id, x + 1, why);
                    bad++;
                    continue;
                }
                /* RETRY THE RETRYABLE, AND ONLY THAT.
                 *
                 * create_account declares `transient`, so roughly one run in
                 * fifty answers 500 and the example "fails" -- which would
                 * make this gate flap, and a gate that flaps is a gate people
                 * re-run until it is green, which is the same as no gate.
                 *
                 * The fix is not to suppress the failure. It is to do what
                 * the manual tells the player to do: 500 and 429 are
                 * documented as retryable, so retry them, three times, and
                 * fail if they never come good. Anything else -- a 400, a
                 * 404, a 409 -- is retried never, because the manual does not
                 * claim those go away. */
                Buf out;
                buf_init(&out);
                int st = -1;
                for (int attempt = 0; attempt < 3; attempt++) {
                    buf_clear(&out);
                    proto_exec(&s, line, &out);
                    st = response_status(&out);
                    if (st != RB_TRANSIENT && st != RB_RATE_LIMIT) break;
                }
                ran++;
                if (st < 0 || st >= 400) {
                    printf("mancheck: FAIL  %s/%s: %s\n", m->id, ep->id, line);
                    /* The answer, trimmed, because "it failed" is not a bug
                     * report and the next person to see this will be tired. */
                    char first[200];
                    size_t n = 0;
                    for (const char *p = out.p; p && *p && *p != '\n' && n < sizeof first - 1; p++) first[n++] = *p;
                    first[n] = 0;
                    printf("mancheck:       answered: %s\n", first);
                    bad++;
                }
                buf_free(&out);
            }
        }
        world_free(w);
    }

    specs_free(specs);
    printf("mancheck: %d examples run, %d failed, %d undocumented, %d without an example\n",
           ran, bad, undocumented, unexampled);
    if (!ran) {
        /* THE VACUOUS CASE, REFUSED. A mancheck that checked nothing must not
         * report success; that is the exact failure this whole file exists to
         * prevent, and it would be embarrassing to commit it here. */
        printf("mancheck: FAIL  no examples were run at all\n");
        return 1;
    }
    return (bad || undocumented) ? 1 : 0;
}

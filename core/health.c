/* health.c — the --health gate.
 *
 * Handoff §13: "Pristine org boots; every appliance reachable; every endpoint
 * in every spec responds." Two thirds of that sentence has nothing to check
 * yet, and this file SAYS SO OUT LOUD rather than skipping quietly.
 *
 * That is not pedantry. NOMINAL's gate suite spent months green while half of
 * it was measuring a placeholder, because a check with nothing to check
 * reports success. Every section below either asserts something or prints the
 * reason it cannot, and the summary counts both.
 */
#include "proto.h"
#include <stdio.h>
#include <string.h>

static int fails;
static int checks;

static void check(bool cond, const char *what)
{
    checks++;
    if (cond) { printf("health: PASS  %s\n", what); }
    else      { printf("health: FAIL  %s\n", what); fails++; }
}

static void pending(const char *what, const char *why)
{
    printf("health: PENDING  %s — %s\n", what, why);
}

/* Run one line against a scratch world and return the response. */
static void exec1(uint64_t seed, const char *line, Buf *out)
{
    World *w = world_new(seed);
    Session s;
    proto_open(&s, w);
    proto_exec(&s, line, out);
    world_free(w);
}

/* EVERY VERB `help` ADVERTISES MUST DISPATCH.
 *
 * The small version of --mancheck, which lands properly at M1 and will
 * execute the examples in every in-game document. The project rule (§13) is
 * that a documented thing that does not exist teaches the player to distrust
 * everything, and the trust is the product. `help` is the first document in
 * the game, so it is the first one held to it. */
static void check_help_verbs(uint64_t seed)
{
    Buf help;
    buf_init(&help);
    exec1(seed, "help", &help);

    const char *p = help.p;
    int verbs = 0, bad = 0;
    while (p && *p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        char line[256];
        if (len >= sizeof line) len = sizeof line - 1;
        memcpy(line, p, len); line[len] = 0;
        p = nl ? nl + 1 : NULL;

        /* Verb lines start in column 0 and are not the +OK or the terminator. */
        if (!line[0] || line[0] == '.' || line[0] == '+' || line[0] == '-') continue;
        if (line[0] == ' ' || line[0] == '\t') continue;
        char verb[64];
        size_t n = 0;
        while (line[n] && line[n] != ' ' && n < sizeof verb - 1) { verb[n] = line[n]; n++; }
        verb[n] = 0;
        if (!verb[0] || !strcmp(verb, "quit")) continue;   /* quit is checked below */

        Buf r;
        buf_init(&r);
        exec1(seed, verb, &r);
        verbs++;
        /* Called with no arguments a verb may legitimately answer -ERR with
         * its usage; what it may not do is not exist. */
        if (r.p && strstr(r.p, "unknown verb")) {
            printf("health:       help advertises '%s', which does not dispatch\n", verb);
            bad++;
        }
        buf_free(&r);
    }
    buf_free(&help);
    check(verbs > 0, "help lists verbs");
    check(bad == 0, "every verb help advertises dispatches");
}

int health_run(uint64_t seed)
{
    fails = checks = 0;
    printf("health: seed %llu\n", (unsigned long long)seed);

    /* ---- a pristine org boots */
    World *w = world_new(seed);
    check(w != NULL, "pristine org boots");
    if (!w) return 1;
    check(w->day == 0 && w->minute == 0, "the clock starts at day 0, minute 0");
    check(w->nusers == RB_START_USERS, "the org opens at the Act I headcount");
    check(w->active == RB_START_USERS, "every seeded user is active");

    /* ---- identifiers are sound. Every one of these is something a player's
     * script will assume without being told, so the world owes it to them. */
    bool ids_ok = true, logins_ok = true, prov_ok = true;
    for (size_t i = 0; i < w->nusers; i++) {
        if (!w->users[i].id[0] || !w->users[i].login[0]) { ids_ok = false; break; }
        if (w->users[i].prov != PROV_SEED) prov_ok = false;
        for (size_t j = i + 1; j < w->nusers; j++) {
            if (!strcmp(w->users[i].id, w->users[j].id))       ids_ok = false;
            if (!strcmp(w->users[i].login, w->users[j].login)) logins_ok = false;
        }
    }
    check(ids_ok, "every user has a unique, non-empty id");
    check(logins_ok, "every login is unique");
    check(prov_ok, "the org the player inherited is attributed to nobody (seed)");

    /* ---- the seed reaches the world at all */
    World *w2 = world_new(seed + 1);
    check(world_hash(w) != world_hash(w2), "a different seed builds a different org");
    world_free(w2);

    /* ---- a login is not free again when its owner leaves.
     * Asserted here rather than left to a comment in world.c because it is
     * the kind of correctness that gets "simplified" away by someone who
     * reads the de-collision loop and not the reason for it. */
    User *victim = &w->users[0];
    char taken[RB_NAME_MAX];
    snprintf(taken, sizeof taken, "%s", victim->login);
    char given[RB_NAME_MAX], family[RB_NAME_MAX];
    snprintf(given,  sizeof given,  "%s", victim->given);
    snprintf(family, sizeof family, "%s", victim->family);
    world_user_offboard(w, victim->id);
    User *twin = world_user_add(w, PROV_HAND, given, family, 0);
    check(twin && strcmp(twin->login, taken) != 0,
          "a departed user's login is not handed to the next arrival");

    /* ---- offboarding is idempotent (handoff §8.3) */
    int before = w->active;
    bool a = world_user_offboard(w, twin->id);
    bool b = world_user_offboard(w, twin->id);
    check(a && b && w->active == before - 1,
          "offboarding twice is not an error and does not count twice");
    check(!world_user_offboard(w, "u_nope"), "offboarding a stranger fails loudly");

    world_free(w);

    /* ---- the API answers */
    check_help_verbs(seed);

    /* ---- and what does not exist yet */
    pending("every appliance reachable",
            "no appliances until M1; the spec loader does not exist");
    pending("every endpoint in every spec responds",
            "no endpoints until M1");
    pending("--mancheck over in-game documents",
            "no documents until M1; 'help' is checked above as a stand-in");

    printf("health: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}

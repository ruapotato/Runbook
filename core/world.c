/* world.c — the org model.
 *
 * Every number in here that looks like balance is a starting value for the
 * harness (handoff §5). Every number in here that looks like structure — the
 * order the day's phases run in, the fact that logins de-collide against
 * departed users too — is not, and changing one changes the game.
 */
#include "world.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

const char *const rb_dept_name[RB_DEPT__N] = {
    "engineering", "sales", "support", "finance", "operations", "marketing"
};

const char *prov_name(Prov p)
{
    switch (p) {
    case PROV_SEED:   return "seed";
    case PROV_HAND:   return "hand";
    case PROV_SCRIPT: return "script";
    case PROV_SYSTEM: return "system";
    default:          return "?";
    }
}

/* ----------------------------------------------------------------- names */
/* Invented, and short on purpose. A small family-name table makes login
 * collisions happen on their own within a few in-game weeks, which is
 * exception class 1 (handoff §8) arriving as a consequence of growth rather
 * than as a scripted event. Widen these tables and you quietly delete a
 * mechanic. */
static const char *const GIVEN[] = {
    "Alma", "Bexley", "Corin", "Dara", "Emlin", "Fen", "Gale", "Hollis",
    "Isolde", "Jarek", "Kestral", "Lune", "Marek", "Nell", "Orin", "Perr",
    "Quill", "Rasha", "Soren", "Tavi", "Ulla", "Vero", "Wynn", "Yara"
};
static const char *const FAMILY[] = {
    "Ashcroft", "Barrow", "Calder", "Dunmore", "Ellery", "Fairbank",
    "Garrow", "Hale", "Ives", "Joss", "Keel", "Lowry",
    "Mercer", "Nash", "Orley", "Pike"
};
#define NGIVEN  (sizeof GIVEN  / sizeof GIVEN[0])
#define NFAMILY (sizeof FAMILY / sizeof FAMILY[0])

/* -------------------------------------------------------- login convention
 * First initial plus family name, lowercase. When that is taken, append a
 * number, starting at 2.
 *
 * IT DE-COLLIDES AGAINST DEPARTED USERS AS WELL AS ACTIVE ONES, and that is
 * the whole point of doing this in the model rather than in a form handler.
 * A login is not free again when its owner leaves — their mailbox, their home
 * folder and their entries in six other systems are still there. A script
 * that reuses it produces an account that looks correct and reads somebody
 * else's mail. */
static void login_derive(World *w, const char *given, const char *family, char *out, size_t cap)
{
    char base[RB_NAME_MAX];
    size_t n = 0;
    if (given[0]) base[n++] = (char)(given[0] | 0x20);
    for (const char *p = family; *p && n < sizeof base - 8; p++) {
        if (*p >= 'A' && *p <= 'Z')      base[n++] = (char)(*p | 0x20);
        else if (*p >= 'a' && *p <= 'z') base[n++] = *p;
        else if (*p >= '0' && *p <= '9') base[n++] = *p;
        /* everything else — apostrophes, hyphens, spaces — is dropped, which
         * is what every naming convention in the world does and is also how
         * two different people end up with the same base. */
    }
    base[n] = 0;

    snprintf(out, cap, "%s", base);
    for (int suffix = 2; world_user_by_login(w, out); suffix++)
        snprintf(out, cap, "%s%d", base, suffix);
}

/* --------------------------------------------------------- login index */
static uint64_t login_hash(const char *s)
{
    return rb_hash_str(rb_hash_init(), s);
}

static void lidx_insert(uint32_t *tab, size_t cap, const World *w, size_t i)
{
    size_t m = cap - 1;
    size_t k = (size_t)(login_hash(w->users[i].login) & m);
    while (tab[k]) k = (k + 1) & m;
    tab[k] = (uint32_t)(i + 1);
}

/* Keep the table under 70% full; past that, linear probing degrades badly. */
static void lidx_reserve(World *w, size_t want)
{
    if (w->lcap && want * 10 < w->lcap * 7) return;
    size_t cap = w->lcap ? w->lcap : 128;
    while (want * 10 >= cap * 7) cap *= 2;
    uint32_t *tab = rb_alloc(cap * sizeof *tab);
    for (size_t i = 0; i < w->nusers; i++) lidx_insert(tab, cap, w, i);
    rb_free(w->lidx);
    w->lidx = tab;
    w->lcap = cap;
}

/* ----------------------------------------------------------------- users */
static void users_reserve(World *w, size_t want)
{
    if (want <= w->ucap) return;
    size_t cap = w->ucap ? w->ucap : 64;
    while (cap < want) cap *= 2;
    w->users = rb_realloc(w->users, cap * sizeof *w->users);
    memset(w->users + w->ucap, 0, (cap - w->ucap) * sizeof *w->users);
    w->ucap = cap;
}

/* Ids are "u_%05d", handed out in order and never reused, so the number IS
 * the array index. The strcmp at the end is not paranoia about our own
 * formatting — it is what makes a malformed id from a player's script fail as
 * "no such user" instead of returning whoever happens to live at that slot. */
User *world_user_find(World *w, const char *id)
{
    if (id[0] != 'u' || id[1] != '_') return NULL;
    long n = strtol(id + 2, NULL, 10);
    if (n < 1 || (size_t)n > w->nusers) return NULL;
    User *u = &w->users[n - 1];
    return strcmp(u->id, id) == 0 ? u : NULL;
}

User *world_user_by_login(World *w, const char *login)
{
    if (!w->lcap) return NULL;
    size_t m = w->lcap - 1;
    size_t k = (size_t)(login_hash(login) & m);
    while (w->lidx[k]) {
        User *u = &w->users[w->lidx[k] - 1];
        if (strcmp(u->login, login) == 0) return u;
        k = (k + 1) & m;
    }
    return NULL;
}

User *world_user_add(World *w, Prov prov, const char *given, const char *family, uint8_t dept)
{
    if (dept >= RB_DEPT__N) {
        snprintf(w->err, sizeof w->err, "no such department: %u", dept);
        return NULL;
    }
    /* Derive the login while the world still holds only existing users. */
    char login[RB_NAME_MAX];
    login_derive(w, given, family, login, sizeof login);

    users_reserve(w, w->nusers + 1);
    User *u = &w->users[w->nusers++];
    memset(u, 0, sizeof *u);
    snprintf(u->id, sizeof u->id, "u_%05d", w->next_uid++);
    snprintf(u->given,  sizeof u->given,  "%s", given);
    snprintf(u->family, sizeof u->family, "%s", family);
    snprintf(u->login,  sizeof u->login,  "%s", login);
    lidx_reserve(w, w->nusers);
    lidx_insert(w->lidx, w->lcap, w, w->nusers - 1);
    u->dept      = dept;
    u->prov      = (uint8_t)prov;
    u->hired_day = w->day;
    u->left_day  = -1;
    w->active++;
    return u;
}

bool world_user_offboard(World *w, const char *id)
{
    User *u = world_user_find(w, id);
    if (!u) { snprintf(w->err, sizeof w->err, "no such user: %s", id); return false; }
    if (u->left_day >= 0) return true;   /* idempotent: see world.h */
    u->left_day = w->day;
    w->active--;
    return true;
}

/* ------------------------------------------------------------------ boot */
World *world_new(uint64_t seed)
{
    World *w = rb_alloc(sizeof *w);
    snprintf(w->org, sizeof w->org, "Harbrook Industries");
    w->seed = seed;
    rng_seed(&w->rng, seed);
    w->day = 0;
    w->minute = 0;
    w->next_uid = 1;
    w->users = NULL; w->nusers = w->ucap = 0;
    w->active = 0;
    w->lidx = NULL; w->lcap = 0;
    w->hire_milli = w->leave_milli = 0;
    w->err[0] = 0;

    /* The company on the player's first morning. These people predate the
     * player, so they are PROV_SEED and their records are as good or as bad
     * as the world says they are — the player does not get blamed for them,
     * and does not get credit for them either. */
    for (int i = 0; i < RB_START_USERS; i++) {
        const char *g = GIVEN[rng_range(&w->rng, 0, NGIVEN - 1)];
        const char *f = FAMILY[rng_range(&w->rng, 0, NFAMILY - 1)];
        uint8_t d = (uint8_t)rng_range(&w->rng, 0, RB_DEPT__N - 1);
        User *u = world_user_add(w, PROV_SEED, g, f, d);
        /* Hired at some point in the two years before the player arrived.
         * Tenure is not decoration: offboarding a four-year veteran turns up
         * records in systems a six-week hire was never in. */
        if (u) u->hired_day = -rng_range(&w->rng, 1, 720);
    }
    return w;
}

void world_free(World *w)
{
    if (!w) return;
    rb_free(w->users);
    rb_free(w->lidx);
    rb_free(w);
}

/* ------------------------------------------------------------------- day */
/* PHASE ORDER IS LOAD-BEARING, not stylistic. Attrition runs before intake,
 * so a leaver's login is already taken when the day's new hires are named,
 * and the collision lands the same way every time. Reverse the two and the
 * same seed produces a different org. */
int world_day_advance(World *w)
{
    w->day++;
    w->minute = 0;

    /* ---- attrition. ~1%/day, carried in thousandths so a 40-person company
     * loses someone about every fortnight rather than never. */
    w->leave_milli += (int64_t)w->active * RB_ATTRITION_MILLI;
    int leaving = (int)(w->leave_milli / 1000);
    w->leave_milli %= 1000;
    for (int i = 0; i < leaving && w->active > 0; i++) {
        /* Pick a slot, then walk forward to the next active user. Walking
         * (rather than rejection-sampling) keeps the draw count per day fixed,
         * which keeps the rng stream aligned across runs. */
        size_t start = (size_t)rng_range(&w->rng, 0, (int32_t)w->nusers - 1);
        for (size_t k = 0; k < w->nusers; k++) {
            User *u = &w->users[(start + k) % w->nusers];
            if (u->left_day < 0) { world_user_offboard(w, u->id); break; }
        }
    }

    /* ---- baseline growth, compounding on the headcount the player just
     * finished onboarding. This is the endogenous curve (handoff §4): there
     * is no external difficulty ramp, the users you provision are the load. */
    w->hire_milli += (int64_t)w->active * RB_GROWTH_MILLI;
    int hires = (int)(w->hire_milli / 1000);
    w->hire_milli %= 1000;

    /* ---- hiring waves, roughly weekly. The waves are what make a Tuesday
     * different from a Monday; steady growth alone is a ramp with no texture
     * and nothing to plan around. */
    if (w->day % RB_WAVE_DAYS == 0)
        hires += rng_range(&w->rng, RB_WAVE_MIN, RB_WAVE_MAX);

    /* ---- M0 STAND-IN, AND THE ONE SEAM M2 CUTS.
     *
     * A hire should arrive as an onboarding ticket and become a user only
     * when that ticket's acceptance checks pass (handoff §4, §7). There is no
     * ticket system yet, so the day provisions them directly and marks them
     * PROV_SEED — nobody did the work, so nobody may claim it.
     *
     * At M2 this loop is replaced by a call into the ticket generator and
     * NOTHING ELSE IN THIS FUNCTION CHANGES. That is why the hire count is
     * computed above and applied here, rather than fused into one loop. */
    for (int i = 0; i < hires; i++) {
        const char *g = GIVEN[rng_range(&w->rng, 0, NGIVEN - 1)];
        const char *f = FAMILY[rng_range(&w->rng, 0, NFAMILY - 1)];
        uint8_t d = (uint8_t)rng_range(&w->rng, 0, RB_DEPT__N - 1);
        world_user_add(w, PROV_SEED, g, f, d);
    }
    return hires;
}

/* ------------------------------------------------------------------ hash */
uint64_t world_hash(const World *w)
{
    uint64_t h = rb_hash_init();
    h = rb_hash_str(h, w->org);
    h = rb_hash_bytes(h, &w->seed,   sizeof w->seed);
    h = rb_hash_bytes(h, &w->day,    sizeof w->day);
    h = rb_hash_bytes(h, &w->minute, sizeof w->minute);
    h = rb_hash_bytes(h, &w->active, sizeof w->active);
    h = rb_hash_bytes(h, &w->next_uid, sizeof w->next_uid);
    /* The rng cursor is state: two worlds that look identical but will
     * diverge on the next draw are not the same world, and the gate exists
     * to catch exactly that. */
    h = rb_hash_bytes(h, &w->rng.s, sizeof w->rng.s);
    h = rb_hash_bytes(h, &w->hire_milli,  sizeof w->hire_milli);
    h = rb_hash_bytes(h, &w->leave_milli, sizeof w->leave_milli);
    for (size_t i = 0; i < w->nusers; i++) {
        const User *u = &w->users[i];
        h = rb_hash_str(h, u->id);
        h = rb_hash_str(h, u->login);
        h = rb_hash_str(h, u->given);
        h = rb_hash_str(h, u->family);
        h = rb_hash_bytes(h, &u->dept,      sizeof u->dept);
        h = rb_hash_bytes(h, &u->prov,      sizeof u->prov);
        h = rb_hash_bytes(h, &u->hired_day, sizeof u->hired_day);
        h = rb_hash_bytes(h, &u->left_day,  sizeof u->left_day);
    }
    return h;
}

/* ------------------------------------------------------------------ dump */
void world_dump(const World *w, Buf *out)
{
    buf_printf(out, "{\n");
    buf_printf(out, "  \"org\": \"%s\",\n", w->org);
    buf_printf(out, "  \"seed\": %llu,\n", (unsigned long long)w->seed);
    buf_printf(out, "  \"day\": %d,\n", w->day);
    buf_printf(out, "  \"minute\": %d,\n", w->minute);
    buf_printf(out, "  \"users_total\": %zu,\n", w->nusers);
    buf_printf(out, "  \"users_active\": %d,\n", w->active);
    buf_printf(out, "  \"hash\": \"%016llx\",\n", (unsigned long long)world_hash(w));
    buf_printf(out, "  \"users\": [\n");
    for (size_t i = 0; i < w->nusers; i++) {
        const User *u = &w->users[i];
        buf_printf(out,
            "    {\"id\":\"%s\",\"login\":\"%s\",\"given\":\"%s\",\"family\":\"%s\","
            "\"dept\":\"%s\",\"prov\":\"%s\",\"hired_day\":%d,\"left_day\":%d}%s\n",
            u->id, u->login, u->given, u->family,
            rb_dept_name[u->dept], prov_name((Prov)u->prov),
            u->hired_day, u->left_day,
            (i + 1 < w->nusers) ? "," : "");
    }
    buf_printf(out, "  ]\n}\n");
}

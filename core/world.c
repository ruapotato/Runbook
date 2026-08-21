/* world.c — the org model.
 *
 * Every number in here that looks like balance is a starting value for the
 * harness (handoff §5). Every number in here that looks like structure — the
 * order the day's phases run in, the fact that a person and their account are
 * different objects — is not, and changing one changes the game.
 */
#include "world.h"
#include "ticket.h"
#include "box.h"
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
/* Invented, and SIZED BY THE HARNESS, not by taste.
 *
 * Login collisions are exception class 1 (handoff §8), and the whole point is
 * that they arrive as a consequence of growth rather than as a scripted
 * event: the chance that a new hire's convention login is already taken is
 * roughly headcount over the number of distinct name pairs.
 *
 * So the table size IS the collision curve. 68 x 64 is about 4,350 pairs,
 * which puts a naive script's collision rate near 3%% at the 150-user Act I
 * wall, 18%% at the 800-user end of Act II, and past certainty in Act III --
 * the §8 ramp, produced by the growth model rather than dialled in.
 *
 * The first version of these tables had 384 pairs. The naive agent failed 99%%
 * of tickets on day one, which is not a hard game, it is a broken one, and
 * --naive is what said so. Do not shrink them without re-running it. */
static const char *const GIVEN[] = {
    "Alma", "Bexley", "Corin", "Dara", "Emlin", "Fen",
    "Gale", "Hollis", "Isolde", "Jarek", "Kestral", "Lune",
    "Marek", "Nell", "Orin", "Perr", "Quill", "Rasha",
    "Soren", "Tavi", "Ulla", "Vero", "Wynn", "Yara",
    "Ansel", "Brill", "Caedy", "Delph", "Edran", "Fira",
    "Grell", "Hesper", "Ivo", "Juno", "Kade", "Liora",
    "Mabry", "Noor", "Ovid", "Pell", "Quen", "Rowan",
    "Sable", "Thea", "Umber", "Vance", "Wray", "Xanthe",
    "Arden", "Belen", "Cass", "Doran", "Eira", "Falk",
    "Gwyn", "Hale", "Iris", "Joss", "Kiran", "Loel",
    "Mira", "Nyle", "Oren", "Prue", "Rhys", "Sten",
    "Tam", "Vail"
};
/* FAMILY NAMES ARE COMPOSED, NOT LISTED, AND THE REASON IS ARITHMETIC.
 *
 * The collision space is not given-names times family-names. The convention
 * keeps only the FIRST INITIAL of the given name, so the space is initials
 * times surnames -- about 25 times however many surnames there are. Two
 * hand-written tables of 68 and 96 names look like 6,528 combinations and
 * behave like 2,400, which put a naive script's collision rate at 6% before
 * anything exceptional had happened at all, and --naive-gate failed the Act I
 * band on that alone. It took an afternoon to see, because 6,528 is a
 * perfectly convincing number to have already checked.
 *
 * So the surnames are 40 stems by 12 endings: 480 plausible English surnames,
 * about 12,000 logins.
 *
 * AND THE SPACE IS DELIBERATELY GENEROUS, which is the opposite of the first
 * instinct. Collisions and the ticket exceptions both grow linearly with
 * headcount, but the §8 band needs the naive failure rate to grow SEVENFOLD
 * between the two sample points -- and collisions can only grow by the ratio
 * of the headcounts, about five. So collisions cannot carry the ramp. They
 * are the texture: about 1% of intake at the Act I wall, 7% by the end of
 * Act II, always present, never the whole story. The exceptions carry the
 * curve, because their rate is free to ramp on its own.
 *
 * Change either table and re-run --naive-gate; it is the only thing that
 * knows whether the game still works. */
static const char *const FAM_STEM[] = {
    "Ash", "Barr", "Cald", "Dun", "Ell", "Fair", "Gar", "Hal",
    "Ing", "Jarr", "Kel", "Lang", "Mar", "Neth", "Oak", "Pell",
    "Quen", "Rush", "Sal", "Tarl", "Umb", "Van", "Wick", "Yarr",
    "Ald", "Bram", "Cran", "Dray", "Eld", "Fen", "Glen", "Harr",
    "Kend", "Lind", "Mow", "Norr", "Orm", "Pres", "Sedge", "West"
};
static const char *const FAM_END[] = {
    "ford", "well", "ley", "ton", "wick", "croft",
    "stead", "more", "bury", "field", "dale", "grove"
};
#define NFAM_STEM (sizeof FAM_STEM / sizeof FAM_STEM[0])
#define NFAM_END  (sizeof FAM_END  / sizeof FAM_END[0])
#define NFAMILY   (NFAM_STEM * NFAM_END)

static void family_name(size_t i, char *out, size_t cap)
{
    snprintf(out, cap, "%s%s", FAM_STEM[i / NFAM_END], FAM_END[i % NFAM_END]);
}

#define NGIVEN  (sizeof GIVEN  / sizeof GIVEN[0])

/* ------------------------------------------------------- THE ORG POLICY
 *
 * First initial plus family name, lowercase, punctuation dropped.
 *
 * THIS IS NOT A SERVICE THE GAME PROVIDES. It is the convention the company
 * has always used, and it is applied here only to the forty people who were
 * already in the directory on the player's first morning. When the player
 * onboards somebody they apply it themselves — in a form at first, then in a
 * script — and de-collide it themselves, because the directory will tell them
 * a login is taken and nothing more.
 *
 * It lives in the model so the in-game document describing the convention and
 * the accounts that actually follow it cannot drift apart (handoff §13).
 * There is no de-collision here on purpose: the seeded org is built with the
 * collisions its own history produced, and cleaning them up is work the
 * player inherits rather than work the world did for them. */
void world_login_for(const World *w, const User *u, char *out, size_t cap)
{
    (void)w;
    char base[RB_NAME_MAX];
    size_t n = 0;
    if (u->given[0]) base[n++] = (char)(u->given[0] | 0x20);
    for (const char *p = u->family; *p && n < sizeof base - 8; p++) {
        if (*p >= 'A' && *p <= 'Z')      base[n++] = (char)(*p | 0x20);
        else if (*p >= 'a' && *p <= 'z') base[n++] = *p;
        else if (*p >= '0' && *p <= '9') base[n++] = *p;
        /* everything else — apostrophes, hyphens, spaces — is dropped, which
         * is what every naming convention in the world does and is also how
         * two different people end up with the same base. */
    }
    base[n] = 0;
    snprintf(out, cap, "%s", base);
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
    if (!id || id[0] != 'u' || id[1] != '_') return NULL;
    long n = strtol(id + 2, NULL, 10);
    if (n < 1 || (size_t)n > w->nusers) return NULL;
    User *u = &w->users[n - 1];
    return strcmp(u->id, id) == 0 ? u : NULL;
}

User *world_user_add(World *w, Prov prov, const char *given, const char *family, uint8_t dept)
{
    if (dept >= RB_DEPT__N) {
        snprintf(w->err, sizeof w->err, "no such department: %u", dept);
        return NULL;
    }
    users_reserve(w, w->nusers + 1);
    User *u = &w->users[w->nusers++];
    memset(u, 0, sizeof *u);
    snprintf(u->id, sizeof u->id, "u_%05d", w->next_uid++);
    snprintf(u->given,  sizeof u->given,  "%s", given);
    snprintf(u->family, sizeof u->family, "%s", family);
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

/* ----------------------------------------------------------- appliances */
Inst *world_inst(World *w, const char *id)
{
    for (size_t i = 0; i < w->ninst; i++) if (!strcmp(w->inst[i]->id, id)) return w->inst[i];
    return NULL;
}

Inst *world_install(World *w, const char *model_id, Prov prov)
{
    (void)prov;
    const Model *m = spec_model(w->specs, model_id);
    if (!m) { snprintf(w->err, sizeof w->err, "no such appliance model: %s", model_id); return NULL; }
    const Vendor *v = spec_vendor(w->specs, m->vendor);

    /* Instance ids are <kind>_NN, and they are what a player's script hard
     * codes on day one and regrets in Act III. That regret is intended: the
     * migration ticket (§6) is about exactly this. */
    char id[RB_ID_MAX];
    int n = 1;
    for (size_t i = 0; i < w->ninst; i++) if (!strcmp(w->inst[i]->m->kind, m->kind)) n++;
    /* Refused rather than truncated. Two appliances sharing an id because a
     * kind name was long is the kind of bug that presents, six hours later,
     * as a script writing accounts into the wrong directory. */
    if (n > 99) {
        /* Act III tops out around seven instances of a kind (4,000 users at
         * 600 apiece). Ninety-nine is not a limit anyone will meet; it is the
         * number that makes the id format provably two digits. */
        snprintf(w->err, sizeof w->err, "cannot install a 100th %s appliance", m->kind);
        return NULL;
    }
    if (strlen(m->kind) + 4 >= sizeof id) {
        snprintf(w->err, sizeof w->err, "appliance kind '%s' is too long to make an instance id", m->kind);
        return NULL;
    }
    snprintf(id, sizeof id, "%s_%02d", m->kind, n);

    Inst *in = inst_new(w, m, v, id);
    /* A new box comes up with the org's reference data on it. See the note on
     * CollSpec.replicated: the alternative is config management, and that is
     * the sequel. */
    for (size_t i = 0; i < w->ninst; i++)
        if (!strcmp(w->inst[i]->m->kind, m->kind)) { appl_replicate(in, w->inst[i]); break; }
    w->inst = rb_realloc(w->inst, (w->ninst + 1) * sizeof *w->inst);
    w->inst[w->ninst++] = in;
    return in;
}

/* Reference data goes on every instance of the kind, or the player's script
 * has to care which directory server it happened to ask. */
Rec *world_replicated_add(World *w, const char *kind, const char *coll, Prov prov)
{
    Rec *first = NULL;
    for (size_t i = 0; i < w->ninst; i++) {
        if (strcmp(w->inst[i]->m->kind, kind)) continue;
        Coll *c = inst_coll(w->inst[i], coll);
        if (!c) continue;
        Rec *r = coll_insert(c, prov, w->day);
        if (!first) first = r;
    }
    return first;
}

/* ------------------------------------------------------------------ boot */
/* THE COMPANY ON THE PLAYER'S FIRST MORNING.
 *
 * Forty people, fully provisioned across all three appliances: a directory
 * account and a department group, a mailbox, a home folder and a share grant.
 * All PROV_SEED: the player
 * did not do this work, so the migration that silently misses hand-made
 * records must not blame them for it, and the audit that finds it must not
 * credit them either.
 *
 * It is built by writing records directly rather than by calling endpoints,
 * because there is nobody to make the calls. This is the only path into an
 * appliance that does not go through appl_call(), it runs once, and it must
 * stay that way — the moment a second one appears, "everything the player can
 * do is reachable over the API" stops being true. */
static void seed_org(World *w)
{
    Inst *dir  = world_install(w, "veridian_dx",  PROV_SEED);
    Inst *mail = world_install(w, "veridian_post", PROV_SEED);
    Inst *fs   = world_install(w, "halcyon_fs9",  PROV_SEED);
    if (!dir || !mail || !fs) return;

    Coll *gc = inst_coll(dir, "groups");
    Coll *ac = inst_coll(dir, "accounts");
    Coll *mc = inst_coll(dir, "memberships");
    Coll *bc = inst_coll(mail, "mailboxes");
    Coll *hc = inst_coll(fs, "homes");
    Coll *sc = inst_coll(fs, "shares");
    Coll *tc = inst_coll(fs, "grants");

    /* THE APPLICATION CATALOGUE. Invented names, like every vendor and product
     * in this game (decision 8), and the reason they are here at all is that
     * "give this person access to the expenses system" is the second half of
     * the access domain (§6) and it is not a thing you can model without
     * something to grant. */
    static const char *const APPS[][2] = {
        { "ledger",    "Finance ledger and expense claims" },
        { "atlas",     "Customer records" },
        { "forge",     "Source control and builds" },
        { "signal",    "Support desk and phone queue" },
        { "cartogram", "Reporting and dashboards" },
        { "vault",     "Contracts and signed documents" },
    };
    Coll *apc = inst_coll(dir, "apps");
    for (size_t i = 0; i < sizeof APPS / sizeof APPS[0]; i++) {
        Rec *a = appl_seed(dir, "apps", PROV_SEED, w->day);
        rec_set(a, "name", APPS[i][0]);
        rec_set(a, "description", APPS[i][1]);
        coll_index_rec(apc, a);
    }

    /* Which department may use what. Everyone gets the support desk and the
     * reporting tool; the rest is by department. This is the shape the whole
     * access model rests on: PEOPLE are not given applications, groups are. */
    static const int DEPT_APPS[RB_DEPT__N][3] = {
        { 2, 4, -1 },   /* engineering: forge, cartogram   */
        { 1, 4, -1 },   /* sales:       atlas, cartogram   */
        { 3, 1, -1 },   /* support:     signal, atlas      */
        { 0, 5, -1 },   /* finance:     ledger, vault      */
        { 2, 3, -1 },   /* operations:  forge, signal      */
        { 1, 4, -1 },   /* marketing:   atlas, cartogram   */
    };
    Coll *enc = inst_coll(dir, "entitlements");

    for (int d = 0; d < RB_DEPT__N; d++) {
        Rec *g = appl_seed(dir, "groups", PROV_SEED, w->day);
        rec_setf(g, "name", "dept-%s", rb_dept_name[d]);
        rec_set(g, "kind", "department");
        rec_set(g, "dept", rb_dept_name[d]);
        rec_setf(g, "description", "Everyone in %s", rb_dept_name[d]);
        coll_index_rec(gc, g);

        for (int k = 0; k < 3 && DEPT_APPS[d][k] >= 0; k++) {
            Rec *e = appl_seed(dir, "entitlements", PROV_SEED, w->day);
            rec_setf(e, "group", "dept-%s", rb_dept_name[d]);
            rec_set(e, "app", APPS[DEPT_APPS[d][k]][0]);
            rec_set(e, "access", "write");
            coll_index_rec(enc, e);
        }

        Rec *sh = appl_seed(fs, "shares", PROV_SEED, w->day);
        rec_setf(sh, "name", "share-%s", rb_dept_name[d]);
        rec_setf(sh, "path", "/srv/%s", rb_dept_name[d]);
        rec_set(sh, "dept", rb_dept_name[d]);
        coll_index_rec(sc, sh);
    }

    for (size_t i = 0; i < w->nusers; i++) {
        User *u = &w->users[i];
        char login[RB_NAME_MAX];
        world_login_for(w, u, login, sizeof login);

        /* THE COLLISIONS THE COMPANY ALREADY HAS. Two people whose names
         * reduce to the same login is not a bug in the seed; it is the
         * history the player inherits, and the second one got a number
         * appended by whoever was doing this job before them. */
        char cand[RB_VAL_MAX];
        snprintf(cand, sizeof cand, "%s", login);
        for (int suffix = 2; suffix < 10000; suffix++) {
            const char *kv[1] = { cand };
            if (!coll_find(ac, kv, 1)) break;
            snprintf(cand, sizeof cand, "%.*s%d", (int)(sizeof cand - 6), login, suffix);
        }

        Rec *a = appl_seed(dir, "accounts", PROV_SEED, w->day);
        rec_set(a, "login", cand);
        rec_set(a, "user_ref", u->id);
        /* A NAME, WITH THE SPACE IN IT. It was "%s_%s" because the protocol could
         * not carry a space, which is a tail wagging a dog: the org's records
         * should look like records, and the protocol learned to quote. */
        rec_setf(a, "display_name", "%s %s", u->given, u->family);
        rec_set(a, "dept", rb_dept_name[u->dept]);
        rec_set(a, "status", "active");
        coll_index_rec(ac, a);
        (void)apc; (void)enc;

        Rec *m = appl_seed(dir, "memberships", PROV_SEED, w->day);
        rec_set(m, "login", cand);
        rec_setf(m, "group", "dept-%s", rb_dept_name[u->dept]);
        coll_index_rec(mc, m);

        Rec *b = appl_seed(mail, "mailboxes", PROV_SEED, w->day);
        rec_set(b, "login", cand);
        rec_setf(b, "address", "%s@harbrook.example", cand);
        rec_set(b, "quota_mb", "2048");
        rec_set(b, "status", "active");
        coll_index_rec(bc, b);

        Rec *h = appl_seed(fs, "homes", PROV_SEED, w->day);
        rec_set(h, "login", cand);
        rec_setf(h, "path", "/home/%s", cand);
        rec_set(h, "quota_mb", "8192");
        coll_index_rec(hc, h);

        Rec *t = appl_seed(fs, "grants", PROV_SEED, w->day);
        rec_set(t, "login", cand);
        rec_setf(t, "share", "share-%s", rb_dept_name[u->dept]);
        rec_set(t, "access", "rw");
        coll_index_rec(tc, t);
    }
}

World *world_new(uint64_t seed, Specs *specs)
{
    World *w = rb_alloc(sizeof *w);
    snprintf(w->org, sizeof w->org, "Harbrook Industries");
    w->seed = seed;
    rng_seed(&w->rng, seed);
    w->day = 0;
    w->ms = 0;
    w->next_uid = 1;
    w->specs = specs;
    w->err[0] = 0;

    /* The people who were already here. They predate the player, so their
     * records are as good or as bad as the world says they are — the player
     * does not get blamed for them, and does not get credit for them. */
    for (int i = 0; i < RB_START_USERS; i++) {
        const char *g = GIVEN[rng_range(&w->rng, 0, NGIVEN - 1)];
        char f[RB_NAME_MAX];
        family_name((size_t)rng_range(&w->rng, 0, NFAMILY - 1), f, sizeof f);
        uint8_t d = (uint8_t)rng_range(&w->rng, 0, RB_DEPT__N - 1);
        User *u = world_user_add(w, PROV_SEED, g, f, d);
        /* Hired at some point in the two years before the player arrived.
         * Tenure is not decoration: offboarding a four-year veteran turns up
         * records in systems a six-week hire was never in. */
        if (u) u->hired_day = -rng_range(&w->rng, 1, 720);
    }

    if (specs) seed_org(w);

    /* The people who started this morning, and the tickets saying so. They
     * are hired like anyone else and provisioned like nobody -- that is the
     * job. Raised after the org is seeded so their logins collide against a
     * directory that already has forty accounts in it. */
    const TicketType *onboard = specs ? spec_ticket(specs, "user.onboard") : NULL;
    for (int i = 0; onboard && i < RB_START_TICKETS; i++) {
        const char *g = GIVEN[rng_range(&w->rng, 0, NGIVEN - 1)];
        char f[RB_NAME_MAX];
        family_name((size_t)rng_range(&w->rng, 0, NFAMILY - 1), f, sizeof f);
        uint8_t d = (uint8_t)rng_range(&w->rng, 0, RB_DEPT__N - 1);
        User *u = world_user_add(w, PROV_SEED, g, f, d);
        if (u) world_ticket_exception(w, world_ticket_new(w, onboard, u->id));
    }
    return w;
}

/* INSTALLED ON FIRST USE, not at world_new().
 *
 * Booting a machine costs real milliseconds and a few megabytes, and the
 * balance harness builds a world per seed and never opens a terminal. Making
 * the emulator the price of admission for every gate would have been a slow
 * suite nobody runs -- which is the failure mode this project keeps writing
 * comments about. Anything that wants a shell asks for one. */
Box *world_box(World *w)
{
    if (!w->box) w->box = box_new(w, w->seed);
    return w->box;
}

void world_free(World *w)
{
    if (!w) return;
    box_free(w->box);
    rb_free(w->users);
    rb_free(w->tick);
    for (size_t i = 0; i < w->ninst; i++) inst_free(w->inst[i]);
    rb_free(w->inst);
    rb_free(w);
}

/* ----------------------------------------------------------------- clock */
int world_minute(const World *w) { return w->ms / 60000; }

/* RUNNING OUT OF DAY IS NOT AN ERROR (handoff §4, §12). It is the punishment,
 * and the punishment for falling behind is more work: the day rolls, growth
 * runs, and tomorrow's arrivals join the ones still waiting. There is no fail
 * screen here and there must not be one. */
int world_spend_ms(World *w, int ms)
{
    if (ms <= 0) return 0;
    w->ms += ms;
    int rolled = 0;
    while (w->ms >= RB_DAY_MS) {
        int over = w->ms - RB_DAY_MS;
        world_day_advance(w);       /* resets ms to 0 */
        w->ms = over > RB_DAY_MS ? 0 : over;
        rolled++;
        /* A single call cannot cost more than a day of work. If one ever
         * does, something has multiplied a latency by a load factor without
         * a ceiling, and silently eating months of simulated time would hide
         * it. Stop at one day and let the run report show the stall. */
        if (rolled > 1 && over >= RB_DAY_MS) break;
    }
    return rolled;
}

/* ------------------------------------------------------------------- day */
/* PHASE ORDER IS LOAD-BEARING, not stylistic. Attrition runs before intake,
 * so the same seed produces the same org every time. Reverse the two and the
 * rng stream moves under every run ever recorded. */
int world_day_advance(World *w)
{
    w->day++;
    w->ms = 0;

    /* ---- yesterday's work counts. Settling before anything else means
     * nobody is chased for a ticket that was finished last thing. */
    world_ticket_sweep(w);

    /* ---- attrition. ~1%/day, carried in thousandths so a 40-person company
     * loses someone about every fortnight rather than never. */
    w->leave_milli += (int64_t)w->active * RB_ATTRITION_MILLI;
    int leaving = (int)(w->leave_milli / 1000);
    w->leave_milli %= 1000;
    for (int i = 0; i < leaving && w->active > 0; i++) {
        /* Pick a slot, then walk forward to the next active user. Walking
         * (rather than rejection-sampling) keeps the draw count per day
         * fixed, which keeps the rng stream aligned across runs. */
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

    /* ---- THE INTAKE.
     *
     * A hire arrives as a person with nothing provisioned for them, and an
     * onboarding ticket that says so. They become a provisioned person only
     * when that ticket's acceptance checks pass against real state (§4, §7,
     * decision 9) — the player never marks anything done. */
    const TicketType *onboard = w->specs ? spec_ticket(w->specs, "user.onboard") : NULL;
    for (int i = 0; i < hires; i++) {
        const char *g = GIVEN[rng_range(&w->rng, 0, NGIVEN - 1)];
        char f[RB_NAME_MAX];
        family_name((size_t)rng_range(&w->rng, 0, NFAMILY - 1), f, sizeof f);
        uint8_t d = (uint8_t)rng_range(&w->rng, 0, RB_DEPT__N - 1);
        User *u = world_user_add(w, PROV_SEED, g, f, d);
        /* THE SEAM M0 CUT, NOW USED. The person is hired; nothing is
         * provisioned for them; a ticket says so. Everything downstream --
         * the queue, the SLA, the follow-ups, the win condition -- hangs off
         * this one line. */
        if (u && onboard) world_ticket_exception(w, world_ticket_new(w, onboard, u->id));
    }

    /* ---- has anything run out of room? Before the chasing, so a capacity
     * ticket raised today is not also chased today. */
    world_ticket_capacity(w);

    /* ---- and the chasing, last, so today's arrivals are not chased on the
     * day they arrive. */
    world_ticket_day(w);
    return hires;
}

/* ------------------------------------------------------------------ hash */
uint64_t world_hash(const World *w)
{
    uint64_t h = rb_hash_init();
    h = rb_hash_str(h, w->org);
    h = rb_hash_bytes(h, &w->seed,   sizeof w->seed);
    h = rb_hash_bytes(h, &w->day,    sizeof w->day);
    h = rb_hash_bytes(h, &w->ms,     sizeof w->ms);
    h = rb_hash_bytes(h, &w->active, sizeof w->active);
    h = rb_hash_bytes(h, &w->next_uid, sizeof w->next_uid);
    /* The rng cursor is state: two worlds that look identical but will
     * diverge on the next draw are not the same world, and the gate exists to
     * catch exactly that. */
    h = rb_hash_bytes(h, &w->rng.s, sizeof w->rng.s);
    h = rb_hash_bytes(h, &w->hire_milli,  sizeof w->hire_milli);
    h = rb_hash_bytes(h, &w->leave_milli, sizeof w->leave_milli);
    for (size_t i = 0; i < w->nusers; i++) {
        const User *u = &w->users[i];
        h = rb_hash_str(h, u->id);
        h = rb_hash_str(h, u->given);
        h = rb_hash_str(h, u->family);
        h = rb_hash_bytes(h, &u->dept,      sizeof u->dept);
        h = rb_hash_bytes(h, &u->prov,      sizeof u->prov);
        h = rb_hash_bytes(h, &u->hired_day, sizeof u->hired_day);
        h = rb_hash_bytes(h, &u->left_day,  sizeof u->left_day);
    }
    /* Appliance state is world state. A run that reproduces the headcount but
     * not the directory has reproduced nothing that matters. */
    for (size_t i = 0; i < w->ninst; i++) {
        const Inst *in = w->inst[i];
        h = rb_hash_str(h, in->id);
        h = rb_hash_str(h, in->m->id);
        h = rb_hash_bytes(h, &in->installed_day, sizeof in->installed_day);
        for (int ci = 0; ci < in->ncoll; ci++) {
            const Coll *c = &in->coll[ci];
            h = rb_hash_str(h, c->cs->name);
            for (size_t ri = 0; ri < c->nr; ri++) {
                const Rec *r = &c->r[ri];
                for (int f = 0; f < r->nf; f++) {
                    h = rb_hash_str(h, r->f[f].k);
                    h = rb_hash_str(h, r->f[f].v);
                }
                h = rb_hash_bytes(h, &r->prov, sizeof r->prov);
                h = rb_hash_bytes(h, &r->dead, sizeof r->dead);
            }
        }
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
    buf_printf(out, "  \"ms\": %d,\n", w->ms);
    buf_printf(out, "  \"users_total\": %zu,\n", w->nusers);
    buf_printf(out, "  \"users_active\": %d,\n", w->active);
    buf_printf(out, "  \"hash\": \"%016llx\",\n", (unsigned long long)world_hash(w));
    buf_printf(out, "  \"appliances\": [\n");
    for (size_t i = 0; i < w->ninst; i++) {
        buf_puts(out, "    ");
        inst_render(w->inst[i], out);
        buf_puts(out, i + 1 < w->ninst ? ",\n" : "\n");
    }
    buf_printf(out, "  ],\n");
    buf_printf(out, "  \"users\": [\n");
    for (size_t i = 0; i < w->nusers; i++) {
        const User *u = &w->users[i];
        buf_printf(out,
            "    {\"id\":\"%s\",\"given\":\"%s\",\"family\":\"%s\","
            "\"dept\":\"%s\",\"prov\":\"%s\",\"hired_day\":%d,\"left_day\":%d}%s\n",
            u->id, u->given, u->family,
            rb_dept_name[u->dept], prov_name((Prov)u->prov),
            u->hired_day, u->left_day,
            (i + 1 < w->nusers) ? "," : "");
    }
    buf_printf(out, "  ]\n}\n");
}

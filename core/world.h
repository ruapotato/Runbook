/* world.h — the org: its people, its clock, its growth.
 *
 * This is the model. Everything else in the program — the socket protocol,
 * the gates, and one day the Godot client — is a view of it or a client of
 * it. Nothing here knows what a window is.
 *
 * M0 SCOPE, said plainly so the next reader is not misled: there are no
 * appliances, no tickets and no API surface in this file yet. Those are M1
 * and M2. What is here is the substrate the determinism gate needs to have
 * anything to prove — an org that boots from a seed and changes over time.
 * A determinism gate over a world that never moves proves nothing, and a
 * vacuous gate is worse than no gate because it reports green.
 */
#ifndef RB_WORLD_H
#define RB_WORLD_H

#include "rb.h"

/* ----------------------------------------------------------- provenance */
/* How every object in the world came to exist (handoff §11). Recorded from
 * the first commit even though nothing reads it until M6's migration ticket,
 * because retrofitting provenance onto objects created before it existed is
 * exactly the debt the mechanic is about — and living it once, in our own
 * codebase, is not the intended lesson.
 *
 * PROV_SEED is the org as it stood on day one, before the player arrived.
 * It is not PROV_HAND: the player did not do that work, and the migration
 * that silently misses hand-made records must not blame them for the ones
 * they inherited. */
typedef enum {
    PROV_SEED = 0,
    PROV_HAND,
    PROV_SCRIPT,
    PROV_SYSTEM,
    PROV__N
} Prov;

const char *prov_name(Prov p);

/* ---------------------------------------------------------- departments */
/* Six, because department is what group membership, share paths and the
 * naming convention all branch on (handoff §6, §8) and six gives the
 * collision cases room without becoming content to memorise. */
#define RB_DEPT__N 6
extern const char *const rb_dept_name[RB_DEPT__N];

/* ---------------------------------------------------------------- users */
typedef struct {
    char    id[RB_ID_MAX];       /* u_00042 — stable, never reused, even after leaving */
    char    login[RB_NAME_MAX];  /* jdoe — the naming convention's output */
    char    given[RB_NAME_MAX];
    char    family[RB_NAME_MAX];
    uint8_t dept;
    uint8_t prov;
    int32_t hired_day;
    int32_t left_day;            /* -1 while active */
} User;

/* ---------------------------------------------------------------- clock */
/* The working day, in minutes. Manual onboarding costs ~12 of these, which
 * is where §5's "the wall lands at roughly 40 tickets/day" comes from. If
 * this number moves, that wall moves with it. */
#define RB_DAY_MINUTES 480

/* --------------------------------------------------------- growth model */
/* Handoff §5. STARTING VALUES FOR THE BALANCE HARNESS, NOT SACRED — the
 * shape (endogenous, compounding, no external difficulty curve) is locked;
 * these particular numbers are what --play and --naive will tune.
 *
 * Carried in thousandths as integers rather than as a double: growth is the
 * one thing that compounds over a whole run, so it is the one place a
 * cross-platform fp discrepancy would amplify instead of cancel. Integers
 * make the Linux/Windows determinism check free here. */
#define RB_GROWTH_MILLI    60   /* +6.0%/day baseline headcount */
#define RB_ATTRITION_MILLI 10   /* -1.0%/day */
#define RB_WAVE_DAYS        7   /* a hiring wave lands about weekly */
#define RB_WAVE_MIN        15
#define RB_WAVE_MAX        40
#define RB_START_USERS     40   /* Act I opens here (§5) */

/* ---------------------------------------------------------------- world */
typedef struct {
    char     org[RB_NAME_MAX];
    uint64_t seed;
    Rng      rng;

    int32_t  day;                /* day 0 is the player's first day */
    int32_t  minute;             /* minute of the working day, 0..RB_DAY_MINUTES */

    User    *users;
    size_t   nusers, ucap;

    /* Login index: open addressing, power of two, slot holds index+1 with 0
     * for empty. Derived state — it is not hashed and not dumped.
     *
     * It is here at M0 because the naive version was measurably fatal: a
     * linear scan per de-collision made a 120-day run take 111 seconds and a
     * 60-day run take 25 milliseconds, which is the shape of a gate that
     * quietly stops being run. The same lookup is on the path of every
     * onboarding a player's script will ever do.
     *
     * There are no tombstones because there are no deletions: a login is
     * never released, not even when its owner leaves. See login_derive(). */
    uint32_t *lidx;
    size_t    lcap;
    int32_t  next_uid;
    int32_t  active;             /* cached: users with left_day < 0 */

    /* Fractional headcount, carried in thousandths so a 6%/day growth on 40
     * users does not round to zero every day and then jump. */
    int64_t  hire_milli, leave_milli;

    char     err[RB_ERR_MAX];
} World;

/* Boot a pristine org from a seed: the company as it was the day before the
 * player was hired. Every user it creates is PROV_SEED. */
World *world_new(uint64_t seed);
void   world_free(World *w);

/* Add a user. `login` is derived from the name by the org's convention and
 * de-collided; the caller does not choose it, because in Act II the player's
 * script will not choose it either. Returns NULL and sets w->err on failure. */
User  *world_user_add(World *w, Prov prov, const char *given, const char *family, uint8_t dept);
User  *world_user_find(World *w, const char *id);
User  *world_user_by_login(World *w, const char *login);
/* Offboard. Idempotent on purpose (handoff §8.3): calling it twice is not an
 * error, it is the correct answer to a retried batch. */
bool   world_user_offboard(World *w, const char *id);

/* Advance one whole day: growth, waves, attrition. Returns the number of
 * users hired. */
int    world_day_advance(World *w);

/* The fingerprint the determinism gate compares. Covers everything a replay
 * must reproduce and nothing that is merely how we stored it — no pointers,
 * no capacities, no iteration-order accidents. */
uint64_t world_hash(const World *w);

/* Canonical world dump. Stable field order, integers only, no locale. This
 * is what --determinism diffs when the hashes disagree, so it has to be
 * readable by a human at 3am, not just comparable. */
void world_dump(const World *w, Buf *out);

#endif /* RB_WORLD_H */

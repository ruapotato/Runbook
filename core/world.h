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
#include "appl.h"

struct Ticket;
typedef struct Box Box;

/* ---------------------------------------------------------- departments */
/* Six, because department is what group membership, share paths and the
 * naming convention all branch on (handoff §6, §8) and six gives the
 * collision cases room without becoming content to memorise. */
#define RB_DEPT__N 6
extern const char *const rb_dept_name[RB_DEPT__N];

/* ---------------------------------------------------------------- users */
/* A PERSON, NOT AN ACCOUNT, and the distinction is the game.
 *
 * A User is what HR knows: somebody was hired, into a department, on a day.
 * Their login, mailbox, home folder and group memberships are not here --
 * they live in the appliances, and putting them there is the player's job.
 * Onboarding is the act of making a person's records exist across several
 * systems; if the world created them, there would be nothing to do.
 *
 * It is also what makes offboarding unforgiving (handoff §6). The person
 * leaves; whatever was never written down stays behind. */
typedef struct {
    char    id[RB_ID_MAX];       /* u_00042 — stable, never reused, even after leaving */
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
/* Carried in milliseconds, because an API call costs 150-500ms (§10) and a
 * minute-resolution clock would round every one of them to nothing -- at
 * which point the rate limit is the only throughput dial left and latency
 * stops being a thing the player can feel. 28,800,000 fits in an int32 with
 * two orders of magnitude to spare. */
#define RB_DAY_MS (RB_DAY_MINUTES * 60000)
/* What one form submission costs a human. Handoff §5: manual onboarding is 6
 * submissions across 3 appliances and about 12 in-game minutes, and the day
 * budget of 480 is what puts the Act I wall at roughly 40 tickets a day. Move
 * this and the wall moves with it. */
#define RB_FORM_MINUTES 2

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
/* AND FOUR PEOPLE WAITING ON THEIR FIRST MORNING.
 *
 * §5 puts Act I's opening rate at four tickets a day, and a player who
 * launches the game into an empty queue has to advance a day before anything
 * happens -- which teaches them, in the first ten seconds, that the game is
 * something that happens after you press a button. There is work on the desk
 * when you sit down. There always is. */
#define RB_START_TICKETS    4
/* Chases per day, per ticket that is past its SLA, in thousandths. Handoff
 * §5: "unresolved tickets roll over and spawn follow-ups at 0.4x per day
 * unresolved. This is the compounding pressure." */
#define RB_FOLLOWUP_MILLI 400
/* At what aggregate load a service starts complaining. Below the 85% the
 * ticket's acceptance demands, so that closing one buys real headroom instead
 * of leaving the player one hire away from the next. */
#define RB_CAPACITY_WARN_PCT 90
/* What it costs to stand up a new appliance instance, by hand. Handoff §5:
 * "provisioning a new instance by hand costs 40 in-game minutes, which does
 * not fit in a day at this volume, so it must be scripted." */
#define RB_INSTALL_MINUTES 40

/* ---------------------------------------------------------------- world */
struct World {
    char     org[RB_NAME_MAX];
    uint64_t seed;
    Rng      rng;

    int32_t  day;                /* day 0 is the player's first day */
    int32_t  ms;                 /* into the working day, 0..RB_DAY_MS */

    Specs   *specs;              /* borrowed; the caller owns them */
    Inst   **inst;
    size_t   ninst;
    int32_t  next_inst;

    User    *users;
    size_t   nusers, ucap;
    int32_t  next_uid;
    int32_t  active;             /* cached: users with left_day < 0 */

    /* Fractional headcount, carried in thousandths so a 6%/day growth on 40
     * users does not round to zero every day and then jump. */
    int64_t  hire_milli, leave_milli;

    /* THE QUEUE. Tickets are never deleted, only closed: the run report, the
     * SLA arithmetic and the M6 audit all read back over the whole history,
     * and a queue that forgets cannot be graded. */
    struct Ticket *tick;
    size_t   ntick, tcap;
    int32_t  next_tid;
    int32_t  open_count;
    int32_t  closed_total, breached_total, followups_total;

    /* THE WORKSTATION THE PLAYER IS SITTING AT.
     *
     * A whole emulated computer: RV64IM, a disk, an init, a package database
     * and /bin/sh, lifted from NOMINAL (handoff §14). It is not scenery --
     * decision 13 puts the player's scripts ON it, and /bin/rb is how a
     * script there reaches this world.
     *
     * It is created lazily, by world_box(), because most things that build a
     * World do not need one: the balance harness runs thousands of simulated
     * days and installing a machine for each would be paying for an emulator
     * to sit idle. */
    Box     *box;

    char     err[RB_ERR_MAX];
};

/* Boot a pristine org from a seed: the company as it was the day before the
 * player was hired. Every user it creates is PROV_SEED. */
World *world_new(uint64_t seed, Specs *specs);
void   world_free(World *w);

/* Hire a person. Nothing is provisioned for them -- that is the game.
 * Returns NULL and sets w->err on failure. */
User  *world_user_add(World *w, Prov prov, const char *given, const char *family, uint8_t dept);
User  *world_user_find(World *w, const char *id);
/* Offboard. Idempotent on purpose (handoff §8.3): calling it twice is not an
 * error, it is the correct answer to a retried batch. */
bool   world_user_offboard(World *w, const char *id);

/* Advance one whole day: growth, waves, attrition. Returns the number of
 * users hired. */
int    world_day_advance(World *w);

/* Spend in-game time. Rolls into the next day when the budget runs out, which
 * is not an error condition -- running out of day IS the pressure (§4). Every
 * roll runs a full world_day_advance(), so the users you have not onboarded
 * yet are joined by tomorrow's. Returns the number of days rolled. */
int    world_spend_ms(World *w, int ms);
int    world_minute(const World *w);

/* Appliances. */
Inst  *world_inst(World *w, const char *id);
Inst  *world_install(World *w, const char *model_id, Prov prov);
/* The org's login convention, applied to a person. It is ORG POLICY, not a
 * service: the player must reproduce it themselves when they onboard, and
 * getting it wrong is a real mistake with real consequences. This function
 * exists so that the forty people already in the directory on day one follow
 * the same rule the player is expected to follow -- and so the in-game
 * document describing it cannot drift from what the org actually did. */
void   world_login_for(const World *w, const User *u, char *out, size_t cap);

/* The player's workstation, installed and booted on first use. */
Box   *world_box(World *w);

/* The fingerprint the determinism gate compares. Covers everything a replay
 * must reproduce and nothing that is merely how we stored it — no pointers,
 * no capacities, no iteration-order accidents. */
uint64_t world_hash(const World *w);

/* Canonical world dump. Stable field order, integers only, no locale. This
 * is what --determinism diffs when the hashes disagree, so it has to be
 * readable by a human at 3am, not just comparable. */
void world_dump(const World *w, Buf *out);

#endif /* RB_WORLD_H */

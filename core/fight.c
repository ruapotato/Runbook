/* fight.c — the gate, and the balance harness.
 *
 * The old project's best habit was that no number was ever tuned by argument;
 * every one was set by running something. That habit is worth more than any
 * of the code it was attached to, so it comes across whole.
 *
 * TWO QUESTIONS, and they are the same two the old --play and --naive asked:
 *
 *   Does doing NOTHING lose?   If a player who only fires wins every time,
 *                              none of the decisions matter and this is the
 *                              ticket queue again in a spacesuit.
 *
 *   Does PLAYING WELL win?     If competent play still loses, the fight is
 *                              not a fight, it is a cutscene with buttons.
 *
 * The band between those two is where the game lives.
 */
#include "proto.h"
#include "box.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

/* ------------------------------------------------------------- the bots */
/* THE ONE WHO ONLY SHOOTS. Not a strawman: it is exactly what somebody does
 * in their first thirty seconds, before they have noticed that the ship is on
 * fire. */
static void bot_naive(World *w)
{
    Ship *s = &w->ship;
    char e[RB_ERR_MAX];
    if (s->weapon_charge >= 1.0) ship_fire(s, s->enemy.shields > 0 ? "shields" : "hull", e, sizeof e);
}

/* THE ONE WHO PLAYS. Everything it does is a thing a player can do with the
 * commands in help, and nothing it looks at is hidden from them. If this bot
 * needs something the interface cannot express, the interface is wrong.
 *
 * IT WAS REWRITTEN WHEN CREW STARTED WALKING. The old one sent whoever it
 * found first, which was free when people teleported and is now the single
 * most expensive mistake available: the wrong person is four seconds away
 * from a fire that doubles every three. Its win rate fell from 96% to 47%
 * overnight, and that was the gate correctly reporting that the reference
 * player had stopped being competent at the new game.
 *
 * So it does what a competent player does now:
 *   - the NEAREST idle person goes to the worst fire, counted in doorways
 *   - a burning room nobody is in gets its door opened, because fire dies in
 *     vacuum and that costs no walking at all
 *   - it strips their shields, then kills their guns, then their hull --
 *     because slowing what is shooting you is worth more than a head start
 *     on winning
 *   - anybody in vacuum with nothing to do leaves
 */
static void bot_playing(World *w)
{
    Ship *s = &w->ship;
    char e[RB_ERR_MAX];

    int worst = -1;
    double worst_fire = 0.05;
    for (int i = 0; i < s->nroom; i++)
        if (s->room[i].fire > worst_fire) { worst_fire = s->room[i].fire; worst = i; }

    if (worst >= 0) {
        /* VENT IT. A door onto a room with no air in it puts the fire out for
         * free, and free is the whole point when everybody is three rooms
         * away. Only if nobody is standing in it. */
        bool occupied = false;
        for (int i = 0; i < s->ncrew; i++)
            if (s->crew[i].alive && s->crew[i].room == worst && s->crew[i].step < 0)
                occupied = true;

        bool coming = false;
        for (int i = 0; i < s->ncrew; i++)
            if (s->crew[i].alive && s->crew[i].dest == worst) coming = true;

        if (!occupied && !coming) {
            /* NEAREST, IN DOORWAYS. This is the decision walking added, and
             * it is the one the gate is really measuring. */
            int best = -1, best_hops = 99;
            for (int i = 0; i < s->ncrew; i++) {
                if (!s->crew[i].alive) continue;
                if (s->room[s->crew[i].room].fire > 0.05) continue;
                if (s->crew[i].health < 0.4) continue;
                int hops = ship_path_len(s, s->crew[i].room, worst);
                if (hops < best_hops) { best_hops = hops; best = i; }
            }
            if (best >= 0) ship_send(s, s->crew[best].name, worst, false, e, sizeof e);
        }
    }

    /* Anybody standing in vacuum with nothing to do should not be. */
    for (int i = 0; i < s->ncrew; i++) {
        if (!s->crew[i].alive || s->crew[i].step >= 0) continue;
        Room *r = &s->room[s->crew[i].room];
        if (r->oxygen < 0.25 && r->fire <= 0.0) {
            int med = ship_room_of(s, SYS_MEDBAY);
            if (med >= 0 && s->room[med].oxygen > 0.5 && s->crew[i].dest != med)
                ship_send(s, s->crew[i].name, med, false, e, sizeof e);
        }
    }

    /* WHAT TO SHOOT AT. Their shields are in the way; their guns are what is
     * killing you; their hull is what ends it. In that order, which is the
     * order that keeps you alive long enough to get to the third one. */
    if (s->weapon_charge >= 1.0) {
        const char *aim = "hull";
        if (s->enemy.shields > 0) aim = "shields";
        else {
            for (int i = 0; i < s->enemy.nroom; i++)
                if (s->enemy.room[i].kind == SYS_WEAPONS &&
                    s->enemy.room[i].damage < s->enemy.room[i].cap) aim = "weapons";
        }
        ship_fire(s, aim, e, sizeof e);
    }
}

/* --------------------------------------------------- the rules, asserted
 *
 * These are not balance. They are the three things the model now promises
 * that nothing else would notice breaking: crew take time to cross the ship,
 * the teleporter is a trade rather than a shortcut, and what you shoot at
 * changes what happens to it. Each one is a rule a player will build a
 * strategy on, and a rule that silently stops holding is worse than one that
 * was never there.
 */
static int rules_pass, rules_fail;

/* THE COMPILER CHECKS THE FORMAT, because I got this wrong within a minute
 * of writing it: `rck(t > 1.0, "having taken %.1fs to walk it")` with no
 * argument, which printed "0.0s" beside a PASS. A gate that reports a number
 * it did not measure is worse than a gate that reports nothing. */
#if defined(__GNUC__)
__attribute__((format(printf, 2, 3)))
#endif
static void rck(bool ok, const char *fmt, ...)
{
    char what[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(what, sizeof what, fmt, ap);
    va_end(ap);
    printf("fight: %s  %s\n", ok ? "PASS " : "FAIL ", what);
    if (ok) rules_pass++; else rules_fail++;
}

static void check_rules(void)
{
    char e[RB_ERR_MAX];
    World *w = world_new(424242);
    Ship *s = &w->ship;
    ship_pause(s, false);

    /* --- walking takes time, and the distance is the time --- */
    int far = ship_room_of(s, SYS_COMPUTER);
    int start = s->crew[0].room;
    int hops = ship_path_len(s, start, far);
    rck(hops >= 2, "the computer is more than one room from the shields");

    rck(ship_send(s, s->crew[0].name, far, false, e, sizeof e), "somebody can be sent there");
    rck(s->crew[0].room != far, "and is NOT there the instant you ask");

    double t = 0;
    while (s->crew[0].room != far && t < 30.0) { ship_tick(s, 0.1); t += 0.1; }
    rck(s->crew[0].room == far, "they arrive under their own steam");
    rck(t > 1.0, "having taken %.1fs to walk it", t);

    /* --- a shut door on the route costs more --- */
    World *w2 = world_new(424242);
    Ship *s2 = &w2->ship;
    ship_pause(s2, false);
    ship_door(s2, 7, false, e, sizeof e);          /* seal the spine */
    ship_send(s2, s2->crew[0].name, far, false, e, sizeof e);
    double t2 = 0;
    while (s2->crew[0].room != far && t2 < 40.0) { ship_tick(s2, 0.1); t2 += 0.1; }
    rck(t2 > t, "a shut door on the way costs longer (%.1fs against %.1fs)", t2, t);

    /* --- the teleporter is a trade --- */
    World *w3 = world_new(424242);
    Ship *s3 = &w3->ship;
    rck(!ship_send(s3, s3->crew[0].name, far, true, e, sizeof e),
        "you cannot teleport through your own shields");
    ship_power(s3, "shields", 0, e, sizeof e);
    rck(!ship_send(s3, s3->crew[0].name, far, true, e, sizeof e),
        "nor with no power in the teleporter");
    ship_power(s3, "teleporter", 1, e, sizeof e);
    rck(ship_send(s3, s3->crew[0].name, far, true, e, sizeof e),
        "but with the shields down and the pad lit, you can");
    rck(s3->crew[0].room == far, "and they are there at once");

    /* --- what you shoot at is what gets hit --- */
    World *w4 = world_new(424242);
    Ship *s4 = &w4->ship;
    ship_pause(s4, false);
    s4->enemy.shields = 0;
    s4->enemy.shields_max = 0;
    s4->weapon_charge = 1.0;
    rck(ship_fire(s4, "engines", e, sizeof e), "you can aim at their engine room");
    for (int i = 0; i < 40; i++) ship_tick(s4, 0.1);
    int eng_dmg = 0, other_dmg = 0;
    for (int i = 0; i < s4->enemy.nroom; i++) {
        if (s4->enemy.room[i].kind == SYS_ENGINES) eng_dmg = s4->enemy.room[i].damage;
        else other_dmg += s4->enemy.room[i].damage;
    }
    rck(eng_dmg > 0 && other_dmg == 0,
        "and their engines take it, not some other room");

    world_free(w); world_free(w2); world_free(w3); world_free(w4);
}

/* ------------------------------------------------------------- one fight */
static bool play(uint64_t seed, void (*bot)(World *), double *secs, int *hull)
{
    World *w = world_new(seed);
    ship_pause(&w->ship, false);
    for (int t = 0; t < 4000 && !w->ship.over; t++) {
        ship_tick(&w->ship, 0.1);
        bot(w);
    }
    bool won = w->ship.won;
    if (secs) *secs = w->ship.clock;
    if (hull) *hull = (int)w->ship.hull;
    world_free(w);
    return won;
}

int fight_run(uint64_t seed, int runs, bool verbose)
{
    if (runs < 1) runs = 40;
    int naive_won = 0, played_won = 0;
    double naive_s = 0, played_s = 0;

    for (int i = 0; i < runs; i++) {
        double s1 = 0, s2 = 0;
        if (play(seed + (uint64_t)i, bot_naive, &s1, NULL)) naive_won++;
        if (play(seed + (uint64_t)i, bot_playing, &s2, NULL)) played_won++;
        naive_s += s1;
        played_s += s2;
    }

    int np = naive_won * 100 / runs;
    int pp = played_won * 100 / runs;
    printf("fight: %d fights, seed %llu\n", runs, (unsigned long long)seed);
    printf("fight:   only shooting     %3d%% won, %.0fs average\n", np, naive_s / runs);
    printf("fight:   actually playing  %3d%% won, %.0fs average\n", pp, played_s / runs);

    check_rules();

    int fails = rules_fail;
    /* THE BAND. Both numbers are first guesses and both are expected to move
     * once a person has played it; what must not move is the SHAPE -- doing
     * nothing has to be able to lose, and playing has to be able to win. */
    if (np > 70) {
        printf("fight: FAIL  a player who only shoots wins %d%% of the time --\n"
               "fight:       nothing else they could do matters\n", np);
        fails++;
    } else {
        printf("fight: PASS  doing nothing but shooting is not enough\n");
    }
    if (pp < 70) {
        printf("fight: FAIL  playing well only wins %d%% -- the fight is not winnable\n", pp);
        fails++;
    } else {
        printf("fight: PASS  putting fires out and moving people wins it\n");
    }
    if (pp - np < 20) {
        printf("fight: FAIL  playing well is only %d points better than not.\n"
               "fight:       the decisions are not doing anything\n", pp - np);
        fails++;
    } else {
        /* AND WHAT 100%% DOES NOT MEAN. bot_playing is a reference, not a
     * person: it re-evaluates every tick, counts doorways exactly, and never
     * gets distracted by the terminal it has open. A hundred percent says the
     * ceiling is reachable, not that the game is easy -- the number that says
     * anything about difficulty is the other one. */
    printf("fight: PASS  the decisions are worth %d points\n", pp - np);
    }

    if (verbose) {
        World *w = world_new(seed);
        ship_pause(&w->ship, false);
        int shown = 0;
        for (int t = 0; t < 4000 && !w->ship.over; t++) {
            ship_tick(&w->ship, 0.1);
            bot_playing(w);
            while (shown < w->ship.nlog) printf("fight:  %5.1fs  %s\n", w->ship.clock, w->ship.log[shown++]);
            if (w->ship.nlog >= 15) { w->ship.nlog = 0; shown = 0; }
        }
        world_free(w);
    }
    return fails ? 1 : 0;
}

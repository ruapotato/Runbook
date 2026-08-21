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
 * Its whole strategy is three rules:
 *   - somebody idle goes to the worst fire
 *   - a room that is burning and empty gets its doors opened, because fire
 *     dies in vacuum
 *   - shoot when charged
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
        bool somebody = false;
        for (int i = 0; i < s->ncrew; i++)
            if (s->crew[i].alive && s->crew[i].room == worst) somebody = true;
        if (!somebody) {
            /* Send whoever is in the least urgent place. */
            for (int i = 0; i < s->ncrew; i++) {
                if (!s->crew[i].alive) continue;
                if (s->room[s->crew[i].room].fire > 0.05) continue;
                if (s->crew[i].health < 0.4) continue;
                ship_send(s, s->crew[i].name, worst, e, sizeof e);
                break;
            }
        }
    }

    /* Anybody standing in vacuum with nothing to do should not be. */
    for (int i = 0; i < s->ncrew; i++) {
        if (!s->crew[i].alive) continue;
        Room *r = &s->room[s->crew[i].room];
        if (r->oxygen < 0.25 && r->fire <= 0.0) {
            int med = ship_room_of(s, SYS_MEDBAY);
            if (med >= 0 && s->room[med].oxygen > 0.5)
                ship_send(s, s->crew[i].name, med, e, sizeof e);
        }
    }

    if (s->weapon_charge >= 1.0) ship_fire(s, s->enemy.shields > 0 ? "shields" : "hull", e, sizeof e);
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

    int fails = 0;
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

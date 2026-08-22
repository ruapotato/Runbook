/* ship.c — the fight.
 *
 * Every number in here is a first guess. FTL's are the product of years of
 * play and ours are the product of an afternoon, so they are all in one place
 * at the top and they are all expected to move once somebody has actually
 * played it.
 */
#include "ship.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

/* ------------------------------------------------------------- the dials */
#define REACTOR_BARS      8
#define HULL_MAX          16.0
#define SHIELD_RECHARGE   3.5      /* seconds per layer                     */
#define WEAPON_SECONDS   14.0      /* to charge, at 1 bar                   */
/* TUNED UP FROM 7.0 WHEN CREW STARTED WALKING. Every second a person spends
 * in a doorway is a second they are not fighting a fire, and at a volley
 * every seven seconds a player who only shot won one fight in six -- harsh
 * enough that a first fight teaches nothing except that you lost. At ten it
 * is one in three, which is the shape a first fight should have: usually you
 * lose, sometimes you get away with it, and the difference is visible.
 *
 *     --fight, 40 fights:   only shooting 32%, playing well 100% */
#define ENEMY_SECONDS    10.0
/* VOLLEYS, NOT SHOTS, and this is the difference between a fight and a
 * metronome.
 *
 * The first version fired one shot at a time and the shields always caught
 * it: two layers, three seconds to recharge, nine seconds between their
 * shots. Nothing ever got through, so nothing ever burned, no crew ever
 * moved, and the whole interesting half of this file -- fire, air, repair --
 * never ran once. A perfectly regular stalemate that resolved by arithmetic.
 *
 * Three shots at once against two layers means one lands, every time. That
 * single change turns the shield allocation into the game's first real
 * decision: three bars in shields and nothing gets through, but they are
 * three bars the gun does not have. */
#define ENEMY_VOLLEY      3
#define PLAYER_VOLLEY     2
/* PER SECOND, INTO AN OPEN NEIGHBOUR, and this number is the difference
 * between a fire and a cascade.
 *
 * It was 35% a second to begin with. One hit in the reactor at fourteen
 * seconds had seven rooms alight by twenty-six, which is not a decision --
 * it is a cutscene. A fire has to be slow enough that going to fight it is a
 * CHOICE with a cost: the person you send is not manning their station while
 * they do it, and that is the whole mechanic.
 *
 * It also only spreads once it has taken hold, so a fire somebody reaches
 * quickly never spreads at all. Getting there fast should be rewarded. */
/* 0.05 -> 0.08 alongside walking. A fire that spreads slowly is a fire you
 * can always reach in time now that reaching it takes four seconds, and
 * "always in time" is not a decision. At 0.08 a second fire while somebody is
 * still crossing the ship is a real possibility, which is what makes sending
 * the NEAREST person -- rather than whoever you thought of first -- worth
 * anything. */
#define FIRE_SPREAD       0.08
#define FIRE_TAKES_HOLD   0.45
#define FIRE_GROWTH       0.20
#define FIRE_AIR_BURN     0.14     /* air a fire eats per second            */
#define FIRE_DAMAGE       0.30     /* system damage per second in the room  */
#define BREACH_VENT       0.22     /* air lost per second through a hole    */
#define OXYGEN_FILL       0.14     /* air replaced per second, per bar      */
#define SUFFOCATE         0.055    /* health lost per second with no air    */
#define REPAIR_RATE       0.34     /* damage repaired per second, per crew  */
#define EXTINGUISH        0.42     /* fire fought per second, per crew      */
#define HEAL_RATE         0.16     /* in the medbay, per bar                */
#define EVADE_PER_BAR     0.09

/* WALKING, AND WHY IT IS THIS SLOW.
 *
 * A hop is one doorway. Most trips are two hops -- out into the spine and
 * back into a room -- so the ship is about three and a half seconds across,
 * against a raider volley every seven. That is the number that makes "who is
 * nearest" a real question: it is long enough that sending the wrong person
 * costs you a volley, and short enough that sending the right one still
 * arrives in time to matter.
 *
 * A shut door has to be cycled, which is most of a hop again. That is the
 * cost of sealing a room, and it is why sealing the corridor is a mistake you
 * only make once. */
#define WALK_SECONDS      1.7
#define DOOR_CYCLE        1.2

/* A shot takes about a second to cross. See the note on Shot in ship.h --
 * that second is the one where you move somebody. */
#define SHOT_FLIGHT       1.05

/* Their engines, same rule as yours. Tuned DOWN from 0.07 by running the
 * harness: at 0.07 a player who only shoots won one fight in ten, because
 * every seventh shot missed and they never worked out that the engines were
 * why. Being beaten is fine; being beaten by a mechanic you cannot see is
 * not, and evasion is invisible until somebody tells you about it. */
#define ENEMY_EVADE_BAR   0.045

/* THEY REPAIR. Knocking out their guns used to end the fight on the spot --
 * the reference bot went from 47% to a hundred percent the moment it learned
 * to aim at them, which is not a skill being rewarded, it is a win button.
 *
 * Now the relief is temporary and you have to spend it. That is the right
 * shape: aiming at their weapons buys you thirty seconds, and what you do
 * with thirty seconds is the game. */
#define ENEMY_REPAIR      0.055


static const char *const SYSNAME[SYS__N] = {
    "none", "reactor", "shields", "engines", "weapons", "oxygen", "medbay", "computer",
    "teleporter"
};

const char *sys_name(SysKind k) { return (k >= 0 && k < SYS__N) ? SYSNAME[k] : "?"; }

SysKind sys_by_name(const char *name)
{
    for (int i = 1; i < SYS__N; i++) if (!strcmp(SYSNAME[i], name)) return (SysKind)i;
    return SYS_NONE;
}

void ship_log(Ship *s, const char *fmt, ...)
{
    char line[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (s->nlog >= 16) {
        for (int i = 1; i < 16; i++) memcpy(s->log[i - 1], s->log[i], sizeof s->log[0]);
        s->nlog = 15;
    }
    snprintf(s->log[s->nlog++], sizeof s->log[0], "%s", line);
}

/* ------------------------------------------------------------------ setup */
void ship_init(Ship *s, uint64_t seed)
{
    memset(s, 0, sizeof *s);
    s->seed = seed;
    rng_seed(&s->rng, seed);
    snprintf(s->name, sizeof s->name, "Kestrel");
    s->hull = s->hull_max = HULL_MAX;
    s->paused = true;              /* you start paused, looking at it */

    /* THE ROOMS, AND WHY THIS MANY. Eight is enough for a fire to have
     * somewhere to spread to and for "who is nearest" to be a question. It is
     * few enough to fit on a screen without a map you have to learn. */
    struct { const char *name; SysKind k; int cap; } layout[] = {
        { "reactor",    SYS_REACTOR,    0 },
        { "shields",    SYS_SHIELDS,    4 },
        { "engines",    SYS_ENGINES,    4 },
        { "weapons",    SYS_WEAPONS,    3 },
        { "oxygen",     SYS_OXYGEN,     2 },
        { "medbay",     SYS_MEDBAY,     2 },
        { "computer",   SYS_COMPUTER,   3 },
        { "corridor",   SYS_NONE,       0 },
        { "teleporter", SYS_TELEPORTER, 2 },
    };
    s->nroom = (int)(sizeof layout / sizeof layout[0]);
    for (int i = 0; i < s->nroom; i++) {
        Room *r = &s->room[i];
        snprintf(r->name, sizeof r->name, "%s", layout[i].name);
        r->sys.kind = layout[i].k;
        r->sys.cap = layout[i].cap;
        r->oxygen = 1.0;
        r->door_open = true;
        r->nadj = 0;
    }

    /* THE DECK PLAN, AS A GRAPH, and it matches the picture exactly.
     *
     * The client draws a spine corridor with rooms either side, engines aft
     * and the teleporter pad in the bow. If this list and that drawing ever
     * disagree, the game becomes unreadable in the specific way that is
     * hardest to debug: everything works, and nothing is where it looks. */
    static const int LINKS[][2] = {
        { 7, 0 }, { 7, 1 }, { 7, 2 }, { 7, 3 }, { 7, 4 }, { 7, 5 }, { 7, 6 },
        { 0, 4 },        /* reactor  shares a wall with oxygen   */
        { 5, 6 },        /* medbay   shares a wall with computer */
        { 1, 3 },        /* shields  shares a wall with weapons  */
        { 1, 8 }, { 3, 8 },   /* the teleporter pad, forward of both */
    };
    for (size_t i = 0; i < sizeof LINKS / sizeof LINKS[0]; i++) {
        int a = LINKS[i][0], b = LINKS[i][1];
        s->room[a].adj[s->room[a].nadj++] = b;
        s->room[b].adj[s->room[b].nadj++] = a;
    }

    /* A STARTING ALLOCATION THAT IS NOT OPTIMAL. If the ship arrives already
     * set up correctly there is nothing to decide in the first thirty
     * seconds, and the first thirty seconds are the ones that decide whether
     * anybody plays the second minute. Shields at 2, weapons at 2, and
     * nothing in the computer -- so the first thing a player wonders is what
     * that empty bar is for. */
    ship_system(s, SYS_SHIELDS)->bars = 2;
    ship_system(s, SYS_ENGINES)->bars = 1;
    ship_system(s, SYS_WEAPONS)->bars = 2;
    ship_system(s, SYS_OXYGEN)->bars = 1;
    ship_system(s, SYS_MEDBAY)->bars = 1;
    s->shields = 2;

    static const char *const NAMES[] = { "Vane", "Corrigan", "Ash" };
    s->ncrew = 3;
    for (int i = 0; i < s->ncrew; i++) {
        snprintf(s->crew[i].name, sizeof s->crew[i].name, "%s", NAMES[i]);
        s->crew[i].alive = true;
        s->crew[i].health = 1.0;
        s->crew[i].room = i == 0 ? ship_room_of(s, SYS_SHIELDS)
                        : i == 1 ? ship_room_of(s, SYS_ENGINES)
                                 : ship_room_of(s, SYS_WEAPONS);
        s->crew[i].dest = s->crew[i].room;
        s->crew[i].step = -1;
    }

    Enemy *e = &s->enemy;
    snprintf(e->name, sizeof e->name, "raider");
    /* LONG ENOUGH THAT DAMAGE ACCUMULATES. At twelve hull the fight was over
     * in fifty seconds and a player who did nothing but fire won every time,
     * which means none of the decisions mattered. The fires need time to cost
     * you something. */
    e->hull = e->hull_max = 18.0;
    e->shields = e->shields_max = 2;
    e->charge_rate = 1.0 / ENEMY_SECONDS;
    e->damage = 1.0;
    e->alive = true;

    /* THEIR ROOMS, WHICH ARE THE DECISION YOUR GUN OFFERS.
     *
     * Their hull is the only thing that ends the fight, so shooting it is
     * always tempting and often wrong: their shields are what stops you
     * reaching it, their weapons are what is killing you, and their engines
     * are why a third of your shots miss. Every one of those is a way of
     * saying "survive longer instead of winning sooner", which is the trade
     * FTL's combat is made of. */
    struct { const char *name; SysKind k; int cap; } ely[] = {
        { "shields", SYS_SHIELDS, 2 },
        { "weapons", SYS_WEAPONS, 2 },
        { "engines", SYS_ENGINES, 2 },
        { "hold",    SYS_NONE,    0 },
    };
    e->nroom = (int)(sizeof ely / sizeof ely[0]);
    for (int i = 0; i < e->nroom; i++) {
        snprintf(e->room[i].name, sizeof e->room[i].name, "%s", ely[i].name);
        e->room[i].kind = ely[i].k;
        e->room[i].cap = ely[i].cap;
    }

    ship_log(s, "a raider closes. shields up.");
    ship_log(s, "space to start. click a system to give it power.");
}

/* ------------------------------------------------------------------ state */
System *ship_system(Ship *s, SysKind k)
{
    for (int i = 0; i < s->nroom; i++) if (s->room[i].sys.kind == k) return &s->room[i].sys;
    return NULL;
}

int ship_room_of(const Ship *s, SysKind k)
{
    for (int i = 0; i < s->nroom; i++) if (s->room[i].sys.kind == k) return i;
    return -1;
}

int ship_power_total(const Ship *s) { (void)s; return REACTOR_BARS; }

double ship_evade(const Ship *s)
{
    const System *en = NULL;
    for (int i = 0; i < s->nroom; i++)
        if (s->room[i].sys.kind == SYS_ENGINES) en = &s->room[i].sys;
    if (!en) return 0.0;
    int w = en->bars;
    int max = en->cap - en->damage;
    if (w > max) w = max;
    if (w < 0) w = 0;
    return EVADE_PER_BAR * w * (en->manned ? 1.3 : 1.0);
}

double ship_enemy_evade(const Ship *s)
{
    for (int i = 0; i < s->enemy.nroom; i++) {
        if (s->enemy.room[i].kind != SYS_ENGINES) continue;
        int w = s->enemy.room[i].cap - s->enemy.room[i].damage;
        return ENEMY_EVADE_BAR * (w < 0 ? 0 : w);
    }
    return 0.0;
}

int ship_power_free(const Ship *s)
{
    int used = 0;
    for (int i = 0; i < s->nroom; i++) used += s->room[i].sys.bars;
    return REACTOR_BARS - used;
}

/* THE COMPUTER'S BARS ARE THE SCRIPTS' CPU, and that is the whole economy of
 * this game in one function. Zero bars, and nothing you wrote runs. Three,
 * and it runs well -- and the shields are three short.
 *
 * The numbers are instruction slices per tick, which the emulator counts
 * exactly, so this is a real budget and not a difficulty setting. */
int ship_compute_slices(const Ship *s)
{
    const System *c = NULL;
    for (int i = 0; i < s->nroom; i++)
        if (s->room[i].sys.kind == SYS_COMPUTER) c = &s->room[i].sys;
    if (!c) return 0;
    int working = c->bars;
    if (working > c->cap - c->damage) working = c->cap - c->damage;
    if (working < 0) working = 0;
    /* SLICES, NOT INSTRUCTIONS.
     *
     * kernel_tick takes a number of scheduling slices and spends a budget of
     * instructions on each. Returning an instruction count here asked for two
     * billion instructions per tick and hung the game the first time anybody
     * ran a script -- which is a bad way to find out you handed the wrong
     * unit across a boundary. Two slices per bar is a control loop running
     * comfortably; it is not enough to be a whole second computer. */
    return working * 2;
}

/* ------------------------------------------------------------ the routes */
/* BREADTH-FIRST OVER THE DECK PLAN. Nine rooms. It does not need to be
 * cleverer than this, and a route a player can work out by looking at the
 * screen is worth more than a route that is one hop shorter. */
static int path_bfs(const Ship *s, int from, int to, int *out_len)
{
    if (out_len) *out_len = 0;
    if (from == to || from < 0 || to < 0 || from >= s->nroom || to >= s->nroom)
        return -1;
    int prev[SHIP_ROOMS], q[SHIP_ROOMS], head = 0, tail = 0;
    for (int i = 0; i < s->nroom; i++) prev[i] = -2;
    prev[from] = -1;
    q[tail++] = from;
    while (head < tail) {
        int at = q[head++];
        if (at == to) break;
        const Room *r = &s->room[at];
        for (int i = 0; i < r->nadj; i++) {
            int n = r->adj[i];
            if (prev[n] != -2) continue;
            prev[n] = at;
            q[tail++] = n;
        }
    }
    if (prev[to] == -2) return -1;

    /* Walk back to the room after `from`, counting as we go. */
    int at = to, len = 0;
    while (prev[at] != from) {
        at = prev[at];
        len++;
        if (at < 0) return -1;
    }
    if (out_len) *out_len = len + 1;
    return at;
}

int ship_path_next(const Ship *s, int from, int to) { return path_bfs(s, from, to, NULL); }

int ship_crew_room(const Ship *s, const char *who)
{
    for (int i = 0; i < s->ncrew; i++)
        if (!strcmp(s->crew[i].name, who)) return s->crew[i].room;
    return -1;
}

int ship_path_len(const Ship *s, int from, int to)
{
    int len = 0;
    path_bfs(s, from, to, &len);
    return len;
}

/* How long this one doorway takes. A shut door has to be cycled. */
static double hop_seconds(const Ship *s, int from, int to)
{
    double t = WALK_SECONDS;
    if (!s->room[from].door_open) t += DOOR_CYCLE;
    if (!s->room[to].door_open) t += DOOR_CYCLE;
    return t;
}

/* --------------------------------------------------------------- commands */
static int working(const System *sy)
{
    int w = sy->bars;
    int max = sy->cap - sy->damage;
    if (w > max) w = max;
    return w < 0 ? 0 : w;
}

bool ship_power(Ship *s, const char *system, int bars, char *err, size_t errsz)
{
    SysKind k = sys_by_name(system);
    if (k == SYS_NONE) { snprintf(err, errsz, "no system called %s", system); return false; }
    System *sy = ship_system(s, k);
    if (!sy) { snprintf(err, errsz, "this ship has no %s", system); return false; }
    if (k == SYS_REACTOR) { snprintf(err, errsz, "the reactor makes power, it does not take it"); return false; }
    if (bars < 0) bars = 0;
    if (bars > sy->cap) {
        snprintf(err, errsz, "%s takes %d bars at most", system, sy->cap);
        return false;
    }
    int spare = ship_power_free(s) + sy->bars;
    if (bars > spare) {
        snprintf(err, errsz, "only %d bar%s spare -- take it from somewhere first",
                 spare, spare == 1 ? "" : "s");
        return false;
    }
    sy->bars = bars;
    /* LAYERS FOLLOW BARS AT ONCE, not on the next tick. `power shields 0`
     * followed immediately by `send Ash medbay now` was refused because the
     * layer that had just been unpowered was still notionally up -- which is
     * true for one fiftieth of a second and reads as the game not listening.
     * A shield you have stopped paying for is down. */
    if (k == SYS_SHIELDS) {
        int w = working(sy);
        if (s->shields > w) s->shields = w;
    }
    return true;
}

bool ship_send(Ship *s, const char *who, int room, bool now, char *err, size_t errsz)
{
    if (room < 0 || room >= s->nroom) { snprintf(err, errsz, "no room %d", room); return false; }
    for (int i = 0; i < s->ncrew; i++) {
        if (strcmp(s->crew[i].name, who)) continue;
        Crew *cr = &s->crew[i];
        if (!cr->alive) { snprintf(err, errsz, "%s is dead", who); return false; }

        if (now) {
            /* THE TELEPORTER, AND EVERY REASON IT MIGHT SAY NO -- each in the
             * model's own words, because "cannot teleport" tells a player
             * nothing about which of three different problems they have. */
            System *tp = ship_system(s, SYS_TELEPORTER);
            if (!tp) { snprintf(err, errsz, "the %s has no teleporter", s->name); return false; }
            if (working(tp) <= 0) {
                snprintf(err, errsz, "the teleporter has no power -- `power teleporter 1`");
                return false;
            }
            if (s->shields > 0) {
                snprintf(err, errsz,
                         "not through your own shields. drop them (`power shields 0`) "
                         "or let %s walk", who);
                return false;
            }
            cr->room = room;
            cr->dest = room;
            cr->step = -1;
            cr->transit = 0.0;
            return true;
        }

        cr->dest = room;
        if (cr->room == room) { cr->step = -1; cr->transit = 0.0; return true; }
        /* Re-aim mid-stride rather than snapping back: somebody halfway
         * through a doorway finishes crossing it and then turns round, which
         * is both what a person does and what the picture already shows. */
        if (cr->step < 0) {
            cr->step = ship_path_next(s, cr->room, room);
            cr->transit = 0.0;
            if (cr->step < 0) {
                snprintf(err, errsz, "there is no way from the %s to the %s",
                         s->room[cr->room].name, s->room[room].name);
                cr->dest = cr->room;
                return false;
            }
        }
        return true;
    }
    snprintf(err, errsz, "nobody aboard called %s", who);
    return false;
}

/* Room index on the enemy for a name, or -1. Accepts a system name or the
 * room's own name, because a player who has read the sensors window has seen
 * both and should not have to know which one this wants. */
static int enemy_room_named(const Ship *s, const char *name)
{
    if (!name || !name[0]) return -1;
    for (int i = 0; i < s->enemy.nroom; i++) {
        if (!strcmp(s->enemy.room[i].name, name)) return i;
        if (s->enemy.room[i].kind != SYS_NONE &&
            !strcmp(sys_name(s->enemy.room[i].kind), name)) return i;
    }
    return -1;
}

static int enemy_working(const EnemyRoom *r)
{
    int w = r->cap - r->damage;
    return w < 0 ? 0 : w;
}

/* Put a shot in the air. Returns false when the sky is full, which cannot
 * happen at these volley sizes and is checked anyway. */
static bool shot_launch(Ship *s, bool incoming, int target, int lane)
{
    for (int i = 0; i < SHOT_MAX; i++) {
        if (s->shot[i].live) continue;
        s->shot[i].live = true;
        s->shot[i].incoming = incoming;
        s->shot[i].t = 0.0;
        s->shot[i].target = target;
        s->shot[i].lane = lane;
        return true;
    }
    return false;
}

bool ship_fire(Ship *s, const char *target, char *err, size_t errsz)
{
    if (!s->enemy.alive) { snprintf(err, errsz, "nothing to shoot at"); return false; }
    if (s->weapon_charge < 1.0) {
        snprintf(err, errsz, "the gun is %d%% charged", (int)(s->weapon_charge * 100));
        return false;
    }

    /* WHAT YOU SHOOT AT IS THE DECISION THE GUN OFFERS. Their hull is the
     * only thing that ends the fight, which is exactly why aiming at it is
     * usually wrong: their shields are what stops you reaching it, their
     * weapons are what is killing you, and their engines are why you miss.
     *
     * Naming nothing keeps the old behaviour -- shields first, then hull --
     * so `fire` on its own is still a sentence that works. */
    int t = -1;
    if (target && target[0] && strcmp(target, "hull") != 0) {
        t = enemy_room_named(s, target);
        if (t < 0) {
            snprintf(err, errsz, "they have no %s -- try `enemy` to see what they have",
                     target);
            return false;
        }
    }

    s->weapon_charge = 0.0;
    for (int shot = 0; shot < PLAYER_VOLLEY; shot++)
        shot_launch(s, false, t, shot);
    ship_log(s, "you fire %d at their %s.", PLAYER_VOLLEY,
             t < 0 ? "hull" : s->enemy.room[t].name);
    return true;
}

/* ONE OF YOURS, ARRIVING. Split out of ship_fire because it now happens a
 * second later, and because the enemy's evasion has to be rolled when the
 * shot gets there rather than when it left. */
static void shot_hits_them(Ship *s, int target)
{
    Enemy *e = &s->enemy;
    if (!e->alive) return;

    int eng = -1;
    for (int i = 0; i < e->nroom; i++) if (e->room[i].kind == SYS_ENGINES) eng = i;
    double evade = eng >= 0 ? ENEMY_EVADE_BAR * enemy_working(&e->room[eng]) : 0.0;
    if (rng_unit(&s->rng) < evade) {
        ship_log(s, "they slip it. their engines are still good.");
        return;
    }

    if (e->shields > 0) {
        e->shields--;
        e->shield_t = 5.0;
        ship_log(s, "their shields take it, %d layer%s left.",
                 e->shields, e->shields == 1 ? "" : "s");
        return;
    }

    e->hull -= 1.0;
    if (target >= 0 && target < e->nroom) {
        EnemyRoom *r = &e->room[target];
        if (r->kind != SYS_NONE && r->damage < r->cap) {
            r->damage++;
            ship_log(s, "their %s is hit -- %d of %d gone.", r->name, r->damage, r->cap);
            /* Their shields room is their shields: knocking it out takes a
             * layer away and stops it coming back. */
            if (r->kind == SYS_SHIELDS) {
                e->shields_max = enemy_working(r);
                if (e->shields > e->shields_max) e->shields = e->shields_max;
            }
        } else {
            ship_log(s, "into the hull -- the raider is at %d%%.",
                     (int)(e->hull / e->hull_max * 100));
        }
    } else {
        ship_log(s, "into the hull -- the raider is at %d%%.",
                 (int)(e->hull / e->hull_max * 100));
    }

    if (e->hull <= 0) {
        e->hull = 0;
        e->alive = false;
        s->over = true;
        s->won = true;
        ship_log(s, "the raider breaks up. you are alive.");
    }
}

bool ship_door(Ship *s, int room, bool open, char *err, size_t errsz)
{
    if (room < 0 || room >= s->nroom) { snprintf(err, errsz, "no room %d", room); return false; }
    s->room[room].door_open = open;
    return true;
}

bool ship_pause(Ship *s, bool paused) { s->paused = paused; return true; }

/* ------------------------------------------------------------------- tick */
/* Rooms are a ring for the purposes of fire and air, which is a simplifying
 * lie that costs nothing at eight rooms: everything is next to two things. A
 * real layout arrives with the picture, because the picture is the only thing
 * that makes a layout mean anything. */
int ship_tick(Ship *s, double dt)
{
    if (s->paused || s->over || dt <= 0) return 0;
    int logs0 = s->nlog;
    s->clock += dt;

    /* ---- crew: walking.
     *
     * They cross one doorway at a time and only count as being in a room once
     * they have arrived, which is the rule that makes the picture honest: a
     * person shown halfway between two rooms is fighting neither fire. */
    for (int i = 0; i < s->ncrew; i++) {
        Crew *c = &s->crew[i];
        if (!c->alive) { c->step = -1; continue; }
        if (c->step < 0) {
            if (c->dest == c->room) continue;
            c->step = ship_path_next(s, c->room, c->dest);
            c->transit = 0.0;
            if (c->step < 0) { c->dest = c->room; continue; }
        }
        c->transit += dt / hop_seconds(s, c->room, c->step);
        if (c->transit < 1.0) continue;
        c->room = c->step;
        c->transit = 0.0;
        c->step = -1;
        if (c->room == c->dest) {
            ship_log(s, "%s reaches the %s.", c->name, s->room[c->room].name);
        } else {
            c->step = ship_path_next(s, c->room, c->dest);
            if (c->step < 0) c->dest = c->room;
        }
    }

    /* ---- crew: what you are doing is where you are standing */
    for (int i = 0; i < s->ncrew; i++) {
        Crew *c = &s->crew[i];
        if (!c->alive) continue;
        /* MID-STRIDE IS NOT A PLACE TO WORK. Somebody crossing a doorway is
         * not fighting the fire in either room, and letting them would make
         * walking free -- which is the whole thing this change is undoing. */
        if (c->step >= 0) continue;
        Room *r = &s->room[c->room];

        if (r->fire > 0.0) {
            r->fire -= EXTINGUISH * dt;
            if (r->fire < 0) r->fire = 0;
            c->health -= 0.05 * dt;         /* fighting a fire costs you */
        } else if (r->sys.damage > 0) {
            r->repair_acc += REPAIR_RATE * dt;
            while (r->repair_acc >= 1.0 && r->sys.damage > 0) {
                r->repair_acc -= 1.0;
                r->sys.damage--;
                ship_log(s, "%s repairs the %s.", c->name, r->name);
            }
        }

        if (r->oxygen < 0.15) c->health -= SUFFOCATE * dt;
        if (r->sys.kind == SYS_MEDBAY && working(&r->sys) > 0)
            c->health += HEAL_RATE * working(&r->sys) * dt;
        if (c->health > 1.0) c->health = 1.0;
        if (c->health <= 0.0) {
            c->alive = false;
            ship_log(s, "%s is gone.", c->name);
        }
    }

    /* ---- manned: computed, never ordered */
    for (int i = 0; i < s->nroom; i++) s->room[i].sys.manned = false;
    for (int i = 0; i < s->ncrew; i++)
        if (s->crew[i].alive && s->crew[i].step < 0)
            s->room[s->crew[i].room].sys.manned = true;

    /* ---- fire, air, breaches */
    for (int i = 0; i < s->nroom; i++) {
        Room *r = &s->room[i];
        if (r->fire > 0.0) {
            if (r->oxygen > 0.05) {
                r->fire += FIRE_GROWTH * dt * r->oxygen;
                if (r->fire > 1.0) r->fire = 1.0;
                r->oxygen -= FIRE_AIR_BURN * dt;
                /* FIRE PUTS ITSELF OUT IN VACUUM, which is the first clever
                 * thing a player discovers on their own: open the doors, let
                 * the room empty, close them again. Nobody has to be told. */
            } else {
                r->fire -= 0.5 * dt;
                if (r->fire < 0) r->fire = 0;
            }
            r->sys.damage += 0;
            r->burn_acc += FIRE_DAMAGE * dt * r->fire;
            while (r->burn_acc >= 1.0) {
                r->burn_acc -= 1.0;
                if (r->sys.kind != SYS_NONE && r->sys.damage < r->sys.cap) {
                    r->sys.damage++;
                    ship_log(s, "the %s burns.", r->name);
                }
            }
            if (r->door_open && r->fire >= FIRE_TAKES_HOLD) {
                for (int a = 0; a < r->nadj; a++) {
                    Room *n = &s->room[r->adj[a]];
                    if (n->door_open && n->oxygen > 0.1 && n->fire <= 0.0 &&
                        rng_unit(&s->rng) < FIRE_SPREAD * dt) {
                        n->fire = 0.15;
                        ship_log(s, "fire spreads to the %s.", n->name);
                    }
                }
            }
        }
        if (r->breach) r->oxygen -= BREACH_VENT * dt;
        if (r->oxygen < 0) r->oxygen = 0;
    }

    /* Air moves between open rooms, and the scrubbers put it back. */
    System *ox = ship_system(s, SYS_OXYGEN);
    double fill = ox ? OXYGEN_FILL * working(ox) : 0.0;
    for (int i = 0; i < s->nroom; i++) {
        Room *r = &s->room[i];
        if (!r->breach && r->fire <= 0.0) {
            r->oxygen += fill * dt;
            if (r->oxygen > 1.0) r->oxygen = 1.0;
        }
        if (!r->door_open) continue;
        for (int a = 0; a < r->nadj; a++) {
            Room *n = &s->room[r->adj[a]];
            if (!n->door_open) continue;
            double flow = (n->oxygen - r->oxygen) * 0.5 * dt;
            r->oxygen += flow;
            n->oxygen -= flow;
        }
    }

    /* ---- shields */
    System *sh = ship_system(s, SYS_SHIELDS);
    int layers = sh ? working(sh) : 0;
    if (s->shields > layers) s->shields = layers;
    if (s->shields < layers) {
        s->shield_t -= dt * (1.0 + (sh && sh->manned ? 0.4 : 0.0));
        if (s->shield_t <= 0) { s->shields++; s->shield_t = SHIELD_RECHARGE; }
    } else s->shield_t = SHIELD_RECHARGE;

    /* ---- your gun */
    System *wp = ship_system(s, SYS_WEAPONS);
    int wbars = wp ? working(wp) : 0;
    if (wbars > 0 && s->weapon_charge < 1.0) {
        double rate = (double)wbars / WEAPON_SECONDS * (wp->manned ? 1.25 : 1.0);
        s->weapon_charge += rate * dt;
        if (s->weapon_charge >= 1.0) {
            s->weapon_charge = 1.0;
            ship_log(s, "gun ready.");
        }
    }

    /* ---- theirs */
    Enemy *e = &s->enemy;
    if (e->alive) {
        if (e->shields < e->shields_max) {
            e->shield_t -= dt;
            if (e->shield_t <= 0) { e->shields++; e->shield_t = 5.0; }
        }
        /* THEIR GUN SLOWS WHEN YOU SHOOT IT. This is the whole reason to
         * aim at their weapons instead of their hull: it does not bring the
         * fight closer to an end, it makes the rest of it survivable. */
        /* Their damage control, such as it is. */
        for (int i = 0; i < e->nroom; i++) {
            if (e->room[i].damage <= 0) { e->room[i].fix_acc = 0; continue; }
            e->room[i].fix_acc += ENEMY_REPAIR * dt;
            if (e->room[i].fix_acc >= 1.0) {
                e->room[i].fix_acc -= 1.0;
                e->room[i].damage--;
                if (e->room[i].kind == SYS_SHIELDS)
                    e->shields_max = enemy_working(&e->room[i]);
                ship_log(s, "they patch their %s.", e->room[i].name);
            }
        }

        int ewp = -1;
        for (int i = 0; i < e->nroom; i++) if (e->room[i].kind == SYS_WEAPONS) ewp = i;
        double wfrac = 1.0;
        if (ewp >= 0 && e->room[ewp].cap > 0)
            wfrac = (double)enemy_working(&e->room[ewp]) / (double)e->room[ewp].cap;
        if (wfrac < 0.0) wfrac = 0.0;

        e->charge += e->charge_rate * wfrac * dt;
        if (e->charge >= 1.0 && wfrac > 0.0) {
            e->charge = 0.0;
            for (int shot = 0; shot < ENEMY_VOLLEY; shot++)
                shot_launch(s, true, -1, shot);
            ship_log(s, "they fire %d.", ENEMY_VOLLEY);
        }
    }

    /* ---- what is in the air.
     *
     * A shot crosses the gap and THEN resolves. Every roll that decides what
     * it does -- evasion, which room, fire, breach -- happens on arrival, so
     * the second it spends in flight is a second in which moving somebody or
     * dropping a shield still changes the outcome. That second is the only
     * reason the tactical view is worth looking at. */
    for (int i = 0; i < SHOT_MAX; i++) {
        Shot *sh_ = &s->shot[i];
        if (!sh_->live) continue;
        sh_->t += dt / SHOT_FLIGHT;
        if (sh_->t < 1.0) continue;
        sh_->live = false;
        if (s->over) continue;

        if (!sh_->incoming) { shot_hits_them(s, sh_->target); continue; }

        System *en = ship_system(s, SYS_ENGINES);
        double evade = en ? EVADE_PER_BAR * working(en) * (en->manned ? 1.3 : 1.0) : 0.0;
        if (rng_unit(&s->rng) < evade) { ship_log(s, "it goes wide."); continue; }
        if (s->shields > 0) {
            s->shields--;
            s->shield_t = SHIELD_RECHARGE;
            continue;
        }
        int hit = (int)rng_range(&s->rng, 0, (int32_t)s->nroom - 1);
        Room *r = &s->room[hit];
        s->hull -= s->enemy.damage;
        if (r->sys.kind != SYS_NONE && r->sys.damage < r->sys.cap) r->sys.damage++;
        ship_log(s, "a hit in the %s.", r->name);
        if (rng_unit(&s->rng) < 0.45) {
            r->fire = 0.25;
            ship_log(s, "FIRE in the %s.", r->name);
        }
        if (rng_unit(&s->rng) < 0.30) {
            r->breach = true;
            ship_log(s, "BREACH in the %s -- it is losing air.", r->name);
        }
        if (s->hull <= 0) {
            s->hull = 0;
            s->over = true;
            s->won = false;
            ship_log(s, "the ship comes apart. the disk survives.");
        }
    }

    /* ---- everybody dead is also over */
    if (!s->over) {
        bool any = false;
        for (int i = 0; i < s->ncrew; i++) if (s->crew[i].alive) any = true;
        if (!any) {
            s->over = true;
            s->won = false;
            ship_log(s, "nobody is left to fly her.");
        }
    }
    return s->nlog - logs0;
}

/* ----------------------------------------------------------------- render */
void ship_render(const Ship *s, Buf *out)
{
    buf_printf(out, "{\"ship\":\"%s\",\"hull\":%d,\"hull_max\":%d,\"shields\":%d,"
                    "\"power_free\":%d,\"power_total\":%d,\"weapon\":%d,"
                    "\"clock\":%d,\"paused\":%s,\"over\":%s,\"won\":%s,",
               s->name, (int)s->hull, (int)s->hull_max, s->shields,
               ship_power_free(s), ship_power_total(s), (int)(s->weapon_charge * 100),
               (int)s->clock, s->paused ? "true" : "false",
               s->over ? "true" : "false", s->won ? "true" : "false");
    buf_printf(out, "\"enemy\":{\"name\":\"%s\",\"hull\":%d,\"shields\":%d,\"charge\":%d},",
               s->enemy.name, (int)s->enemy.hull, s->enemy.shields, (int)(s->enemy.charge * 100));
    buf_puts(out, "\"rooms\":[");
    for (int i = 0; i < s->nroom; i++) {
        const Room *r = &s->room[i];
        buf_printf(out, "%s{\"n\":%d,\"name\":\"%s\",\"system\":\"%s\",\"bars\":%d,"
                        "\"cap\":%d,\"damage\":%d,\"oxygen\":%d,\"fire\":%d,"
                        "\"breach\":%s,\"door\":\"%s\"}",
                   i ? "," : "", i, r->name, sys_name(r->sys.kind), r->sys.bars,
                   r->sys.cap, r->sys.damage, (int)(r->oxygen * 100), (int)(r->fire * 100),
                   r->breach ? "true" : "false", r->door_open ? "open" : "shut");
    }
    buf_puts(out, "],\"crew\":[");
    for (int i = 0; i < s->ncrew; i++) {
        const Crew *cr = &s->crew[i];
        buf_printf(out, "%s{\"name\":\"%s\",\"room\":%d,\"walking_to\":%d,"
                        "\"across\":%d,\"dest\":%d,\"health\":%d,\"alive\":%s}",
                   i ? "," : "", cr->name, cr->room, cr->step,
                   (int)(cr->transit * 100), cr->dest,
                   (int)(cr->health * 100), cr->alive ? "true" : "false");
    }
    buf_puts(out, "]}");
}

void ship_render_enemy(const Ship *s, Buf *out)
{
    const Enemy *e = &s->enemy;
    for (int i = 0; i < e->nroom; i++) {
        const EnemyRoom *r = &e->room[i];
        buf_printf(out, "{\"n\":%d,\"name\":\"%s\",\"system\":\"%s\","
                        "\"cap\":%d,\"damage\":%d,\"working\":%d}\n",
                   i, r->name, sys_name(r->kind), r->cap, r->damage,
                   r->cap - r->damage < 0 ? 0 : r->cap - r->damage);
    }
}

void ship_render_shots(const Ship *s, Buf *out)
{
    for (int i = 0; i < SHOT_MAX; i++) {
        const Shot *sh = &s->shot[i];
        if (!sh->live) continue;
        buf_printf(out, "{\"from\":\"%s\",\"across\":%d,\"target\":%d,\"lane\":%d}\n",
                   sh->incoming ? "them" : "you", (int)(sh->t * 100), sh->target, sh->lane);
    }
}

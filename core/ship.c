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
#define ENEMY_SECONDS     7.0
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
#define FIRE_SPREAD       0.05
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

static const char *const SYSNAME[SYS__N] = {
    "none", "reactor", "shields", "engines", "weapons", "oxygen", "medbay", "computer"
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
        { "reactor",  SYS_REACTOR,  0 },
        { "shields",  SYS_SHIELDS,  4 },
        { "engines",  SYS_ENGINES,  4 },
        { "weapons",  SYS_WEAPONS,  3 },
        { "oxygen",   SYS_OXYGEN,   2 },
        { "medbay",   SYS_MEDBAY,   2 },
        { "computer", SYS_COMPUTER, 3 },
        { "corridor", SYS_NONE,     0 },
    };
    s->nroom = (int)(sizeof layout / sizeof layout[0]);
    for (int i = 0; i < s->nroom; i++) {
        Room *r = &s->room[i];
        snprintf(r->name, sizeof r->name, "%s", layout[i].name);
        r->sys.kind = layout[i].k;
        r->sys.cap = layout[i].cap;
        r->oxygen = 1.0;
        r->door_open = true;
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
    return true;
}

bool ship_send(Ship *s, const char *who, int room, char *err, size_t errsz)
{
    if (room < 0 || room >= s->nroom) { snprintf(err, errsz, "no room %d", room); return false; }
    for (int i = 0; i < s->ncrew; i++) {
        if (strcmp(s->crew[i].name, who)) continue;
        if (!s->crew[i].alive) { snprintf(err, errsz, "%s is dead", who); return false; }
        s->crew[i].room = room;
        return true;
    }
    snprintf(err, errsz, "nobody aboard called %s", who);
    return false;
}

bool ship_fire(Ship *s, const char *target, char *err, size_t errsz)
{
    if (!s->enemy.alive) { snprintf(err, errsz, "nothing to shoot at"); return false; }
    if (s->weapon_charge < 1.0) {
        snprintf(err, errsz, "the gun is %d%% charged", (int)(s->weapon_charge * 100));
        return false;
    }
    /* WHAT YOU SHOOT AT IS A DECISION, and at this size it is the only one
     * the weapon offers: their shields, so your next shot lands, or their
     * hull, which is the one that ends it. */
    int t = 1;
    if (target && !strcmp(target, "hull")) t = 0;
    else if (target && !strcmp(target, "shields")) t = 1;
    else if (target && target[0]) { snprintf(err, errsz, "aim at hull or shields"); return false; }

    (void)t;
    s->weapon_charge = 0.0;
    Enemy *e = &s->enemy;

    /* A BURST. The first shot goes into whatever is in the way and the second
     * follows it through the hole -- which is why a volley beats a shield and
     * a single shot never does. */
    int stripped = 0, landed = 0;
    for (int shot = 0; shot < PLAYER_VOLLEY; shot++) {
        if (e->shields > 0) {
            e->shields--;
            e->shield_t = 5.0;
            stripped++;
        } else {
            e->hull -= 1.0;
            landed++;
        }
    }
    if (stripped && landed)
        ship_log(s, "you fire. %d through their shields, %d into the hull -- %d%% left.",
                 stripped, landed, (int)(e->hull / e->hull_max * 100));
    else if (landed)
        ship_log(s, "you fire. both into the hull -- the raider is at %d%%.",
                 (int)(e->hull / e->hull_max * 100));
    else
        ship_log(s, "you fire. their shields take it, %d layer%s left.",
                 e->shields, e->shields == 1 ? "" : "s");

    if (e->hull <= 0) {
        e->hull = 0;
        e->alive = false;
        s->over = true;
        s->won = true;
        ship_log(s, "the raider breaks up. you are alive.");
    }
    return true;
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
static int nbr(const Ship *s, int i, int d) { return (i + d + s->nroom) % s->nroom; }

int ship_tick(Ship *s, double dt)
{
    if (s->paused || s->over || dt <= 0) return 0;
    int logs0 = s->nlog;
    s->clock += dt;

    /* ---- crew: what you are doing is where you are standing */
    for (int i = 0; i < s->ncrew; i++) {
        Crew *c = &s->crew[i];
        if (!c->alive) continue;
        Room *r = &s->room[c->room];

        if (r->fire > 0.0) {
            r->fire -= EXTINGUISH * dt;
            if (r->fire < 0) r->fire = 0;
            c->health -= 0.05 * dt;         /* fighting a fire costs you */
        } else if (r->sys.damage > 0) {
            static double acc[SHIP_ROOMS];
            acc[c->room] += REPAIR_RATE * dt;
            while (acc[c->room] >= 1.0 && r->sys.damage > 0) {
                acc[c->room] -= 1.0;
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
        if (s->crew[i].alive) s->room[s->crew[i].room].sys.manned = true;

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
            static double burn[SHIP_ROOMS];
            burn[i] += FIRE_DAMAGE * dt * r->fire;
            while (burn[i] >= 1.0) {
                burn[i] -= 1.0;
                if (r->sys.kind != SYS_NONE && r->sys.damage < r->sys.cap) {
                    r->sys.damage++;
                    ship_log(s, "the %s burns.", r->name);
                }
            }
            if (r->door_open && r->fire >= FIRE_TAKES_HOLD) {
                for (int d = -1; d <= 1; d += 2) {
                    Room *n = &s->room[nbr(s, i, d)];
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
        for (int d = -1; d <= 1; d += 2) {
            Room *n = &s->room[nbr(s, i, d)];
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
        e->charge += e->charge_rate * dt;
        if (e->charge >= 1.0) {
            e->charge = 0.0;
            System *en = ship_system(s, SYS_ENGINES);
            double evade = en ? EVADE_PER_BAR * working(en) * (en->manned ? 1.3 : 1.0) : 0.0;
            int absorbed = 0, through = 0, missed = 0;
            for (int shot = 0; shot < ENEMY_VOLLEY; shot++) {
                if (rng_unit(&s->rng) < evade) { missed++; continue; }
                if (s->shields > 0) { s->shields--; s->shield_t = SHIELD_RECHARGE; absorbed++; continue; }
                through++;
                int hit = (int)rng_range(&s->rng, 0, (int32_t)s->nroom - 1);
                Room *r = &s->room[hit];
                s->hull -= e->damage;
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
            }
            if (!through)
                ship_log(s, "they fire three. %d absorbed, %d missed. nothing got in.",
                         absorbed, missed);
            if (s->hull <= 0) {
                s->hull = 0;
                s->over = true;
                s->won = false;
                ship_log(s, "the ship comes apart. the disk survives.");
            }
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
    for (int i = 0; i < s->ncrew; i++)
        buf_printf(out, "%s{\"name\":\"%s\",\"room\":%d,\"health\":%d,\"alive\":%s}",
                   i ? "," : "", s->crew[i].name, s->crew[i].room,
                   (int)(s->crew[i].health * 100), s->crew[i].alive ? "true" : "false");
    buf_puts(out, "]}");
}

/* ship.h — one ship, in real time.
 *
 * THE PREMISE, AFTER A PLAYTEST KILLED THE LAST ONE.
 *
 * The ticket queue failed for three reasons and none of them were the
 * setting: there were no decisions in the first hour, there was nothing to
 * watch, and automating something produced no spectacle. A script closed a
 * ticket and a number changed.
 *
 * So: a ship, in real time, being shot at. FTL's shape, because FTL's ship IS
 * the picture -- fire spreading, air venting, somebody running to a station --
 * and because it is nothing but decisions. Power is scarce and every bar you
 * give the shields is a bar the weapons do not have.
 *
 * WHAT MAKES IT THIS PROJECT AND NOT A CLONE: every action is a command, and
 * the console shows it. Click "power to shields" and the console says
 *
 *     power shields 3
 *
 * so the API is taught during a fight rather than announced in a menu. And
 * the ship's computer is a REAL computer -- the RV64IM machine that was
 * already here -- so a script you write runs on it, in the fight, while
 * things are happening.
 *
 * THE COST OF AUTOMATION IS POWER, and that is the design's answer to the one
 * thing that could ruin it. A script that reacts perfectly in zero time
 * deletes the game; a script that reacts perfectly but eats two bars the
 * shields wanted is a TRADE. The computer draws from the same reactor as
 * everything else, and the emulator counts instructions, so a tighter script
 * is literally worth more power. Optimisation becomes a mechanic instead of a
 * chore.
 *
 * AND THE DISK SURVIVES. You lose the ship; you keep /root/scripts. Run three
 * you have a fire routine, run ten you have a library. The thing that
 * ratchets is your own code -- which is Factorio's belts-feeding-belts
 * translated honestly, and the only progression system this game needs.
 */
#ifndef RB_SHIP_H
#define RB_SHIP_H

#include "rb.h"

/* ------------------------------------------------------------------ rooms */
/* Small on purpose. FTL's ships are eighteen rooms and most of the interest
 * is in six of them; a first fight that fits on one screen without scrolling
 * is a first fight somebody can read. */
#define SHIP_ROOMS      10
#define SHIP_CREW_MAX   6
#define SHIP_NAME_MAX   32
/* The corridor touches seven rooms, so this is not a guess. */
#define SHIP_ADJ_MAX    8
/* Shots in flight, both ways, at once. A volley of three plus yours of two,
 * with room for them to overlap. */
#define SHOT_MAX        16
#define ENEMY_ROOMS     4

/* ---------------------------------------------------------------- systems */
/* One system per room, at most. The order is the order they appear in the
 * power display, so it is the order a player learns them in. */
typedef enum {
    SYS_NONE = 0,
    SYS_REACTOR,      /* makes the power everything else spends            */
    SYS_SHIELDS,      /* layers that absorb a hit and recharge             */
    SYS_ENGINES,      /* evasion: the chance a shot simply misses          */
    SYS_WEAPONS,      /* charge rate                                       */
    SYS_OXYGEN,       /* replaces the air a breach takes out               */
    SYS_MEDBAY,       /* heals whoever is standing in it                   */
    SYS_COMPUTER,     /* THE ONE THAT MAKES THIS GAME: instruction budget  */
    /* THE EXCEPTION TO WALKING, AND IT COSTS YOU THE SHIELD.
     *
     * Crew walk. That is the rule, and it is what makes "who is nearest" a
     * real question instead of a formality. The teleporter is the one way
     * round it, and it only works with your own shields DOWN -- you cannot
     * put somebody through a shield you are hiding behind.
     *
     * So it is not a convenience, it is a trade made under fire: drop the
     * shield for a moment to put the right person in the right room now. */
    SYS_TELEPORTER,
    SYS__N
} SysKind;

const char *sys_name(SysKind k);
SysKind     sys_by_name(const char *name);

typedef struct {
    SysKind kind;
    int     bars;        /* power currently routed here                     */
    int     cap;         /* the most it can take, undamaged                 */
    int     damage;      /* bars knocked out; cap - damage is what works    */
    bool    manned;      /* somebody is standing at it -- computed per tick */
} System;

/* ------------------------------------------------------------------ rooms */
typedef struct {
    char    name[SHIP_NAME_MAX];
    System  sys;
    double  oxygen;      /* 0..1                                            */
    double  fire;        /* 0..1, spreads, eats air, damages the system     */
    bool    breach;      /* a hole: air leaves faster than oxygen replaces  */
    bool    door_open;
    /* WHICH ROOMS THIS ONE OPENS ONTO.
     *
     * It used to be (i +/- 1) % nroom -- a ring, not a ship. Fire spread from
     * the computer to the corridor because they were next to each other in an
     * array, and a player looking at the deck plan could not predict where a
     * fire would go next, which is most of what a deck plan is for.
     *
     * Now it is the real plan: everything opens onto the spine corridor, and
     * a few rooms share a wall with each other. Fire spreads along it, air
     * flows along it, and crew walk along it. */
    int     adj[SHIP_ADJ_MAX];
    int     nadj;
    /* PARTIAL PROGRESS, ON THE SHIP RATHER THAN IN A STATIC.
     *
     * Repairing and burning both accumulate towards a whole bar of damage,
     * and both accumulators used to be `static double [SHIP_ROOMS]` inside
     * ship_tick -- file-scope state, shared by every Ship in the process.
     * One fight per process hid it completely; the balance harness runs
     * forty in a row, so fight two started with fight one's half-finished
     * repairs. Determinism you only have when you run one thing is not
     * determinism. */
    double  repair_acc;
    double  burn_acc;
} Room;

/* ------------------------------------------------------------------- crew */
typedef struct {
    char   name[SHIP_NAME_MAX];
    int    room;
    double health;       /* 0..1 */
    bool   alive;
    /* THEY WALK. A playtester watched somebody appear in a burning room the
     * instant it was clicked and said, correctly, that they teleport.
     *
     * `dest` is where they were told to go. `step` is the room they are
     * walking INTO right now, or -1 when they are standing still, and
     * `transit` is how far across the doorway they are. Everything a player
     * cares about follows from that: somebody two rooms away is four seconds
     * away, a shut door on the route costs them longer, and "who is nearest"
     * stops being a formality. */
    int    dest;
    int    step;
    double transit;
    /* WHAT THEY ARE DOING, decided by where they are standing rather than by
     * an order. Standing in a room with a fire, they fight it; with damage,
     * they repair it; with a working system, they man it. That is FTL's rule
     * and it is the right one: it means "send somebody there" is the whole
     * interface, and it is one command. */
    double busy;
} Crew;

/* ------------------------------------------------------------------ enemy */
/* ONE ENEMY, AND IT IS NOT CLEVER. The first fight has to be readable: it
 * charges, it fires, it hits a room. Everything interesting in the first hour
 * should come from your own ship being on fire, not from an opponent with
 * plans. */
/* A ROOM ON THE OTHER SHIP. Fewer fields than yours on purpose: you cannot
 * see their air or their fires, only what your gun did to their machinery. */
typedef struct {
    char    name[SHIP_NAME_MAX];
    SysKind kind;
    int     cap;
    int     damage;
    double  fix_acc;     /* see Room.repair_acc -- same trap, same fix */
} EnemyRoom;

typedef struct {
    char   name[SHIP_NAME_MAX];
    double hull, hull_max;
    int    shields, shields_max;
    double shield_t;      /* seconds until the next layer comes back        */
    double charge;        /* 0..1, fires at 1                               */
    double charge_rate;
    double damage;
    bool   alive;
    /* WHAT YOU CAN SHOOT AT, which is the decision the gun offers.
     *
     * Their hull ends the fight and nothing else does -- but their shields
     * are what stops you reaching it, their weapons are what is killing you,
     * and their engines are why you keep missing. Choosing between "end it
     * sooner" and "survive long enough to" is the whole of FTL's combat and
     * it costs one enum and four rooms. */
    EnemyRoom room[ENEMY_ROOMS];
    int    nroom;
} Enemy;

/* A SHOT, IN FLIGHT. Volleys used to resolve in the instant they were fired,
 * which meant the most dramatic thing in the game -- three shots crossing the
 * gap towards a ship you are still deciding how to defend -- happened in no
 * time at all and could not be drawn. A shot now takes about a second to
 * arrive, and that second is the one where you move somebody. */
typedef struct {
    bool   live;
    bool   incoming;     /* theirs, aimed at you */
    double t;            /* 0..1 across the gap */
    int    target;       /* room index on the receiving ship */
    int    lane;         /* which line it flies along, for drawing */
} Shot;

/* ------------------------------------------------------------------- ship */
typedef struct Ship {
    char    name[SHIP_NAME_MAX];
    double  hull, hull_max;

    Room    room[SHIP_ROOMS];
    int     nroom;
    Crew    crew[SHIP_CREW_MAX];
    int     ncrew;

    int     shields;           /* layers up now                             */
    double  shield_t;          /* seconds until the next layer recharges    */
    double  weapon_charge;     /* 0..1                                      */
    int     weapon_target;     /* which enemy system to shoot at            */

    Enemy   enemy;
    Shot    shot[SHOT_MAX];

    /* THE CLOCK. Real seconds, and it stops when the player pauses -- which
     * is FTL's most important mechanic and the reason a real-time game can be
     * about thinking. Everything below is derived from this. */
    double  clock;
    bool    paused;
    bool    over;
    bool    won;

    /* WHAT JUST HAPPENED, for the console. Not a history: the last few lines,
     * because a fight produces more events than anybody reads and the ones
     * that matter are the recent ones. */
    char    log[16][96];
    int     nlog;

    uint64_t seed;
    Rng      rng;
} Ship;

/* Build the first fight. Deterministic in the seed, like everything else. */
void   ship_init(Ship *s, uint64_t seed);

/* Advance by `dt` seconds. Does nothing while paused, which is the point of
 * paused. Returns the number of log lines added, so a caller can tell whether
 * anything worth printing happened. */
int    ship_tick(Ship *s, double dt);

/* ----------------------------------------------------------- the commands
 *
 * EVERY ONE OF THESE IS WHAT A BUTTON DOES. The UI has no other way to touch
 * the ship, so the console can mirror every click exactly -- which is the
 * whole teaching mechanism, and it only works if there is nothing the buttons
 * can do that these cannot.
 *
 * They return false and fill `err` when refused, and the refusal is in the
 * model's own words: "the reactor only has 2 bars spare" beats "invalid". */
bool   ship_power(Ship *s, const char *system, int bars, char *err, size_t errsz);
/* `now` asks for the teleporter instead of a walk. It is refused, with the
 * reason, when there is no teleporter, when it has no power, or when your own
 * shields are up. */
bool   ship_send(Ship *s, const char *who, int room, bool now, char *err, size_t errsz);
/* The next room on the way from `from` to `to`, or -1 if there is no route.
 * Breadth-first over Room.adj, which is nine rooms and does not need to be
 * cleverer than that. */
int    ship_path_next(const Ship *s, int from, int to);
int    ship_path_len(const Ship *s, int from, int to);
/* Which room somebody is in, or -1. */
int    ship_crew_room(const Ship *s, const char *who);
bool   ship_fire(Ship *s, const char *target, char *err, size_t errsz);
bool   ship_door(Ship *s, int room, bool open, char *err, size_t errsz);
bool   ship_pause(Ship *s, bool paused);

/* ------------------------------------------------------------------ state */
int    ship_power_free(const Ship *s);
int    ship_power_total(const Ship *s);
/* The chance a shot at you simply misses, 0..1, and the same for them. The
 * tactical view shows both, because "why do I keep missing" is a question
 * with an answer and the answer is their engine room. */
double ship_evade(const Ship *s);
double ship_enemy_evade(const Ship *s);
System *ship_system(Ship *s, SysKind k);
int    ship_room_of(const Ship *s, SysKind k);
/* How much of the machine's instruction budget the computer is paying for.
 * Zero bars means scripts do not run, which is a thing a player finds out
 * once and never forgets. */
int    ship_compute_slices(const Ship *s);
void   ship_render(const Ship *s, Buf *out);
/* The other ship, room by room, for the sensors window and for a script that
 * wants to decide what to shoot at. */
void   ship_render_enemy(const Ship *s, Buf *out);
/* Everything in flight, for the tactical view. */
void   ship_render_shots(const Ship *s, Buf *out);
void   ship_log(Ship *s, const char *fmt, ...);

#endif

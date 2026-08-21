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
#define SHIP_ROOMS      8
#define SHIP_CREW_MAX   6
#define SHIP_NAME_MAX   32

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
} Room;

/* ------------------------------------------------------------------- crew */
typedef struct {
    char   name[SHIP_NAME_MAX];
    int    room;
    double health;       /* 0..1 */
    bool   alive;
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
typedef struct {
    char   name[SHIP_NAME_MAX];
    double hull, hull_max;
    int    shields, shields_max;
    double shield_t;      /* seconds until the next layer comes back        */
    double charge;        /* 0..1, fires at 1                               */
    double charge_rate;
    double damage;
    bool   alive;
} Enemy;

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
bool   ship_send(Ship *s, const char *who, int room, char *err, size_t errsz);
bool   ship_fire(Ship *s, const char *target, char *err, size_t errsz);
bool   ship_door(Ship *s, int room, bool open, char *err, size_t errsz);
bool   ship_pause(Ship *s, bool paused);

/* ------------------------------------------------------------------ state */
int    ship_power_free(const Ship *s);
int    ship_power_total(const Ship *s);
System *ship_system(Ship *s, SysKind k);
int    ship_room_of(const Ship *s, SysKind k);
/* How much of the machine's instruction budget the computer is paying for.
 * Zero bars means scripts do not run, which is a thing a player finds out
 * once and never forgets. */
int    ship_compute_slices(const Ship *s);
void   ship_render(const Ship *s, Buf *out);
void   ship_log(Ship *s, const char *fmt, ...);

#endif

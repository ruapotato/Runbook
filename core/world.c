/* world.c — a run: the ship, the machine, and the clock they share. */
#include "world.h"
#include "box.h"
#include <string.h>
#include <stdio.h>

World *world_new(uint64_t seed)
{
    World *w = rb_alloc(sizeof *w);
    memset(w, 0, sizeof *w);
    w->seed = seed;
    ship_init(&w->ship, seed);
    return w;
}

void world_free(World *w)
{
    if (!w) return;
    box_free(w->box);
    rb_free(w);
}

Box *world_box(World *w)
{
    if (!w->box) w->box = box_new(w, w->seed);
    return w->box;
}

/* THE TICK, AND THE ONE LINE THAT MAKES THIS GAME WHAT IT IS.
 *
 * The ship advances, and then the machine gets a slice of instructions
 * proportional to the power in the computer. Zero bars and nothing you wrote
 * runs; three bars and it runs well, and the shields are three bars short.
 *
 * That is the whole economy. A script is not free and it is not magic: it is
 * a thing you are paying the reactor for, in the same currency as the guns.
 */
int world_tick(World *w, double dt)
{
    int logs = ship_tick(&w->ship, dt);
    if (!w->ship.paused && w->box) {
        int slices = ship_compute_slices(&w->ship);
        if (slices > 0) box_run_slices(w->box, slices);
    }
    return logs;
}

uint64_t world_hash(const World *w)
{
    uint64_t h = rb_hash_init();
    const Ship *s = &w->ship;
    h = rb_hash_bytes(h, &w->seed, sizeof w->seed);
    h = rb_hash_bytes(h, &s->hull, sizeof s->hull);
    h = rb_hash_bytes(h, &s->shields, sizeof s->shields);
    h = rb_hash_bytes(h, &s->clock, sizeof s->clock);
    h = rb_hash_bytes(h, &s->rng.s, sizeof s->rng.s);
    h = rb_hash_bytes(h, &s->weapon_charge, sizeof s->weapon_charge);
    for (int i = 0; i < s->nroom; i++) {
        const Room *r = &s->room[i];
        h = rb_hash_str(h, r->name);
        h = rb_hash_bytes(h, &r->sys.bars, sizeof r->sys.bars);
        h = rb_hash_bytes(h, &r->sys.damage, sizeof r->sys.damage);
        h = rb_hash_bytes(h, &r->oxygen, sizeof r->oxygen);
        h = rb_hash_bytes(h, &r->fire, sizeof r->fire);
        h = rb_hash_bytes(h, &r->breach, sizeof r->breach);
        h = rb_hash_bytes(h, &r->door_open, sizeof r->door_open);
    }
    for (int i = 0; i < s->ncrew; i++) {
        h = rb_hash_str(h, s->crew[i].name);
        h = rb_hash_bytes(h, &s->crew[i].room, sizeof s->crew[i].room);
        h = rb_hash_bytes(h, &s->crew[i].health, sizeof s->crew[i].health);
    }
    h = rb_hash_bytes(h, &s->enemy.hull, sizeof s->enemy.hull);
    h = rb_hash_bytes(h, &s->enemy.shields, sizeof s->enemy.shields);
    h = rb_hash_bytes(h, &s->enemy.charge, sizeof s->enemy.charge);
    return h;
}

void world_dump(const World *w, Buf *out)
{
    buf_printf(out, "{\n  \"seed\": %llu,\n  \"hash\": \"%016llx\",\n  \"ship\": ",
               (unsigned long long)w->seed, (unsigned long long)world_hash(w));
    ship_render(&w->ship, out);
    buf_puts(out, "\n}\n");
}

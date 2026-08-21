/* world.h — a run.
 *
 * One ship, one fight, one computer to fly it from. That is the whole model
 * now; the org, the ticket queue and the appliance specs that used to live
 * here are gone, and the commit that removed them says why.
 *
 * WHAT SURVIVED THE PIVOT, and why it was worth keeping: the emulated
 * machine, the language on it, the macro recorder, the socket, the gate
 * discipline and determinism. None of those were about the old premise. The
 * only thing that was about the old premise was the old premise.
 */
#ifndef RB_WORLD_H
#define RB_WORLD_H

#include "rb.h"
#include "ship.h"
#include "recorder.h"

typedef struct Box Box;

struct World {
    Ship     ship;
    uint64_t seed;

    /* THE COMPUTER YOU FLY HER FROM. A real RV64IM machine with a real disk,
     * a real shell and a real Python on it -- and the ship's `computer`
     * system is its power supply, so how much of it you can use is a thing
     * you decide with the same bars the shields want.
     *
     * IT IS CREATED LAZILY. Booting a machine costs milliseconds that a
     * headless balance run does not want to spend. */
    Box     *box;

    /* THE RECORDER, unchanged from the last design and the best thing in it:
     * it watches the commands your clicking produces and writes them out as a
     * Python script. It fits this game better than the last one -- what it
     * records is a fight. */
    Recorder rec;

    char     err[RB_ERR_MAX];
};

typedef struct World World;

World *world_new(uint64_t seed);
void   world_free(World *w);
Box   *world_box(World *w);
/* Advance the fight. Also gives the machine its slice of instructions, which
 * is how a script gets to run while things are happening -- and how many it
 * gets is the computer's power. */
int    world_tick(World *w, double dt);
uint64_t world_hash(const World *w);
void   world_dump(const World *w, Buf *out);

#endif

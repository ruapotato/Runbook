/* recorder.h — the macro recorder, and the most important accessibility
 * feature in the game.
 *
 * Handoff decision 15, verbatim: "Macro recorder is the on-ramp to scripting.
 * The UI->script transition is a cliff for most players. This is the single
 * most important accessibility feature in the game."
 *
 * And the goal it serves, in David's words: make programmers out of regular
 * people, rather than attract programmers to the game. Those are different
 * products. A game that attracts programmers can hand you a blank editor and
 * an API reference; a game that MAKES programmers has to put a working
 * script, about work you just did, in front of you before you have decided
 * you are not the sort of person who writes them.
 *
 * SO IT DOES NOT RECORD KEYSTROKES AND IT DOES NOT EMIT A MACRO. It watches
 * the API calls your clicking produced -- which is all your clicking ever
 * was (decision 7) -- and writes them out as a Python script you can read.
 *
 * §16.2 asked what exactly it should emit: "Probably a flat sequence of named
 * calls with literal arguments, then teach loops by showing the player the
 * repetition." That is exactly what it does, and the second half is the
 * teaching moment: do one person and you get six lines you can read. Do TWO
 * and it notices they were the same six lines with three things changed, and
 * hands you the loop -- with the three things in a list at the top, so the
 * next edit anybody makes is adding a row to it.
 *
 * Nobody is taught what a variable is. They are shown one holding their own
 * work, with their own name in it.
 */
#ifndef RB_RECORDER_H
#define RB_RECORDER_H

#include "appl.h"
#include "spec.h"

#define REC_MAX_STEPS 512

typedef struct {
    char  target[RB_ID_MAX];             /* the appliance                    */
    char  action[RB_NAME_MAX];           /* the endpoint                     */
    Field arg[SPEC_MAX_FIELDS];
    int   nargs;
    bool  by_hand;                       /* a form, rather than a typed call */
} RecStep;

typedef struct {
    bool    on;
    char    name[RB_NAME_MAX];
    int32_t started_day;
    RecStep step[REC_MAX_STEPS];
    int     nstep;
    bool    overflowed;
} Recorder;

typedef struct World World;

void recorder_start(World *w, const char *name);
void recorder_stop(World *w);
/* Record one write. Reads are not recorded: a script that lists accounts to
 * decide something is a script the player will write themselves, and putting
 * every glance they took into the transcript would bury the six lines that
 * matter. */
void recorder_step(World *w, const char *target, const char *action,
                   const Field *args, int nargs, bool by_hand);
void recorder_clear(World *w);
/* The script. Python, because that is what decision 14 chose and what the
 * recorder is an on-ramp TO. */
void recorder_script(const World *w, Buf *out);
void recorder_status(const World *w, Buf *out);

#endif

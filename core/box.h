/* box.h — the workstation, and the API it can reach.
 *
 * THE THING SITTING UNDER THE DESKTOP. Handoff §14 lifts NOMINAL's machine
 * layer as a library, and this file is where the game and that machine meet:
 * one emulated RV64IM computer with a real disk, a real init, a real package
 * database and a real /bin/sh, plus the one bridge that lets a program on it
 * ask the game a question.
 *
 * WHY A WHOLE EMULATED COMPUTER, when a scripting sandbox would have done:
 * decision 13. "Player scripts run on the emulated machine. The moat. No
 * other game in this space has a real interpreter on a real machine." A
 * script here is not a game feature with a syntax; it is a file on a disk,
 * run by a shell, on a CPU. You can `cat` it, `cp` it, break it, and fix it,
 * and everything you learn doing that is true of every Unix box you will ever
 * touch. That is not a claim a scripting sandbox can make.
 */
#ifndef RB_BOX_H
#define RB_BOX_H

#include "rb.h"

typedef struct Box Box;
typedef struct World World;

/* Install and boot the player's workstation. Costs no in-game time: the
 * machine is on when they sit down, the way it is on every morning. */
Box  *box_new(World *w, uint64_t seed);
void  box_free(Box *b);

/* One shell command line, as the persistent session -- so `cd` sticks, and so
 * do variables. This is the only entry point the terminal uses, which is the
 * same rule NOMINAL applied for the same reason: two front ends onto one
 * shell cannot disagree about what the machine did. */
void  box_sh(Box *b, const char *line, Buf *out);
bool  box_up(const Box *b);
/* Whatever the boot had to say for itself, for the terminal's banner. */
void  box_boot_log(const Box *b, Buf *out);

/* The filesystem, for a file browser that is not a second opinion: both of
 * these go to the machine's own Vfs. */
void  box_list(Box *b, const char *path, Buf *out);
bool  box_read(Box *b, const char *path, Buf *out);
bool  box_write(Box *b, const char *path, const char *data, size_t len);

#endif

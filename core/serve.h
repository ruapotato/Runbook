/* serve.h — the local TCP listener. See serve.c. */
#ifndef RB_SERVE_H
#define RB_SERVE_H
#include "world.h"
int serve_run(World *w, int port, bool verbose);
#endif

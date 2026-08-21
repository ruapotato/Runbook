/* proto.h — the API. The only way anything talks to the world.
 *
 * Handoff decision 7: the UI is a client of the API from commit one. So the
 * API exists on commit one, before there is a UI to be a client of it. When
 * the Godot shell arrives at M3 it opens a session here and speaks the same
 * verbs the socket does; "find the API" is then a discovery of something that
 * was always underneath, not a feature that gets bolted on for Act II.
 *
 * Line in, response out. Every response ends with a lone "." on its own line
 * so a dumb client — telnet, a shell script, a reference agent — can find the
 * end without parsing the body.
 */
#ifndef RB_PROTO_H
#define RB_PROTO_H

#include "world.h"

typedef struct {
    World *w;
    Prov   prov;   /* what this session's writes are attributed to */
    bool   open;
} Session;

void proto_open(Session *s, World *w);
void proto_hello(Session *s, Buf *out);
/* Execute one line. Returns false when the session should close. */
bool proto_exec(Session *s, const char *line, Buf *out);

#endif /* RB_PROTO_H */

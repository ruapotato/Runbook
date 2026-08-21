/* proto.c — the commands, and the reason the console can mirror every click.
 *
 * EVERY ACTION IS ONE OF THESE. The UI has no other way to touch the ship, so
 * when a player clicks "power to shields" the console can honestly print
 *
 *     power shields 3
 *
 * because that is what the button did. That is the whole teaching mechanism
 * of this game -- the API is learned during a fight, from watching your own
 * clicking narrated -- and it only works if there is nothing a button can do
 * that a line here cannot.
 *
 * It is also why the macro recorder can exist at all, and why a script you
 * write is a first-class way to play rather than a bolted-on convenience.
 *
 * Line in, response out. Every response ends with a lone "." so a dumb client
 * -- telnet, a shell script, a reference agent -- can find the end without
 * parsing the body.
 */
#include "proto.h"
#include "box.h"
#include "recorder.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#define MAX_ARGV 10

/* Whitespace-separated words, with quoting, because a value may be a name
 * with a space in it and refusing one is a game telling a player that people
 * are not allowed two names. */
static int split(char *line, char *argv[MAX_ARGV])
{
    int argc = 0;
    char *p = line;
    while (*p && argc < MAX_ARGV) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        argv[argc++] = p;
        char *w = p;
        bool q = false;
        while (*p && (q || (*p != ' ' && *p != '\t'))) {
            if (*p == '"') { q = !q; p++; continue; }
            *w++ = *p++;
        }
        bool more = (*p != 0);
        *w = 0;
        if (more) p++;
    }
    return argc;
}

static void ok(Buf *out, const char *fmt, ...)
{
    char tmp[512];
    va_list ap; va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    buf_printf(out, "+OK %s\n.\n", tmp);
}

static void err(Buf *out, const char *fmt, ...)
{
    char tmp[512];
    va_list ap; va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    buf_printf(out, "-ERR %s\n.\n", tmp);
}

void proto_open(Session *s, World *w)
{
    s->w = w;
    s->open = true;
}

void proto_hello(Session *s, Buf *out)
{
    const Ship *sh = &s->w->ship;
    buf_printf(out,
        "+OK %s -- a %s is closing.\n"
        "type 'help'; every response ends with a lone '.'\n.\n",
        sh->name, sh->enemy.name);
}

static void cmd_help(Buf *out)
{
    /* THE PROJECT RULE APPLIES TO THIS LIST (it is the first document in the
     * game): every verb named here must exist and behave as described, and
     * the gate checks it by running them. */
    buf_puts(out,
        "+OK what you can do\n"
        "\n"
        "  power <system> <bars>   route power. systems: shields engines weapons\n"
        "                          oxygen medbay computer\n"
        "  send <crew> <room>      send somebody somewhere. what they do is\n"
        "                          decided by what is in the room they are in:\n"
        "                          fight the fire, repair the damage, or man it\n"
        "  fire [hull|shields]     shoot, when the gun is charged\n"
        "  door <room> open|shut   a shut door stops fire and holds air\n"
        "  pause / resume          time stops. thinking is free\n"
        "\n"
        "  ship                    the whole ship, as one object\n"
        "  status / enemy          your numbers, and theirs, separately\n"
        "  rooms                   one line per room\n"
        "  crew                    one line per person\n"
        "  log                     what just happened\n"
        "\n"
        "  sh <command>            a shell on the ship's computer\n"
        "  run <script>            start a script running IN the fight\n"
        "\n"
        "  rec.start / rec.stop    watch what you do and write it down\n"
        "  rec.script / rec.save   what you did, as a Python script\n"
        "\n"
        "quit\n"
        ".\n");
}

bool proto_exec(Session *s, const char *line, Buf *out)
{
    char buf[RB_LINE_MAX];
    snprintf(buf, sizeof buf, "%s", line);
    char *argv[MAX_ARGV];
    int argc = split(buf, argv);
    if (argc == 0) { buf_puts(out, ".\n"); return true; }

    const char *cmd = argv[0];
    World *w = s->w;
    Ship *sh = &w->ship;
    char e[RB_ERR_MAX];

    if (!strcmp(cmd, "help")) { cmd_help(out); return true; }
    if (!strcmp(cmd, "quit") || !strcmp(cmd, "exit")) {
        buf_puts(out, "+OK bye\n.\n");
        s->open = false;
        return false;
    }

    /* ------------------------------------------------------- the actions */
    if (!strcmp(cmd, "power")) {
        if (argc < 3) { err(out, "power <system> <bars>"); return true; }
        if (!ship_power(sh, argv[1], atoi(argv[2]), e, sizeof e)) { err(out, "%s", e); return true; }
        recorder_step(w, line);
        ok(out, "%s at %d, %d spare", argv[1], atoi(argv[2]), ship_power_free(sh));
        return true;
    }

    if (!strcmp(cmd, "send")) {
        if (argc < 3) { err(out, "send <crew> <room>"); return true; }
        int room = atoi(argv[2]);
        /* A room may be named as well as numbered, because "send Vane
         * weapons" is what a person says and "send Vane 3" is what a script
         * ends up writing. Both, and the recorder keeps whichever you used. */
        for (int i = 0; i < sh->nroom; i++)
            if (!strcmp(sh->room[i].name, argv[2])) room = i;
        if (!ship_send(sh, argv[1], room, e, sizeof e)) { err(out, "%s", e); return true; }
        recorder_step(w, line);
        ok(out, "%s is in the %s", argv[1], sh->room[room].name);
        return true;
    }

    if (!strcmp(cmd, "fire")) {
        if (!ship_fire(sh, argc > 1 ? argv[1] : "", e, sizeof e)) { err(out, "%s", e); return true; }
        recorder_step(w, line);
        ok(out, "fired");
        return true;
    }

    if (!strcmp(cmd, "door")) {
        if (argc < 3) { err(out, "door <room> open|shut"); return true; }
        int room = atoi(argv[1]);
        for (int i = 0; i < sh->nroom; i++)
            if (!strcmp(sh->room[i].name, argv[1])) room = i;
        bool open = !strcmp(argv[2], "open");
        if (!ship_door(sh, room, open, e, sizeof e)) { err(out, "%s", e); return true; }
        recorder_step(w, line);
        ok(out, "%s door %s", sh->room[room].name, open ? "open" : "shut");
        return true;
    }

    if (!strcmp(cmd, "pause"))  { ship_pause(sh, true);  ok(out, "paused");  return true; }
    if (!strcmp(cmd, "resume")) { ship_pause(sh, false); ok(out, "running"); return true; }

    /* THE CLOCK, FOR EVERYTHING THAT IS NOT A PERSON. The desktop ticks from
     * its own frame loop; a socket session, a gate and a reference agent tick
     * with this. One implementation of what a second does. */
    if (!strcmp(cmd, "tick")) {
        double secs = argc > 1 ? atof(argv[1]) : 0.1;
        if (secs < 0 || secs > 60) { err(out, "tick takes 0..60 seconds"); return true; }
        world_tick(w, secs);
        ok(out, "%ds, hull %d, they are at %d%%",
           (int)sh->clock, (int)sh->hull, (int)(sh->enemy.hull / sh->enemy.hull_max * 100));
        return true;
    }

    /* -------------------------------------------------------- the state */
    if (!strcmp(cmd, "ship")) {
        buf_puts(out, "+OK ship\n");
        ship_render(sh, out);
        buf_puts(out, "\n.\n");
        return true;
    }

    /* THE SHIP'S OWN NUMBERS, FLAT. `ship` answers with one nested object,
     * which is the right shape for a script that wants everything at once
     * and the wrong shape for anything that flattens keys: the ship has a
     * hull and so does the raider, and one of them wins. So the two halves
     * are also available separately, which is what the bridge window reads
     * and what a script checking one thing wants anyway. */
    if (!strcmp(cmd, "status")) {
        buf_puts(out, "+OK status\n");
        buf_printf(out, "{\"ship\":\"%s\",\"hull\":%d,\"hull_max\":%d,\"shields\":%d,"
                        "\"power_free\":%d,\"power_total\":%d,\"weapon\":%d,\"clock\":%d,"
                        "\"paused\":%s,\"over\":%s,\"won\":%s}\n",
                   sh->name, (int)sh->hull, (int)sh->hull_max, sh->shields,
                   ship_power_free(sh), ship_power_total(sh), (int)(sh->weapon_charge * 100),
                   (int)sh->clock, sh->paused ? "true" : "false",
                   sh->over ? "true" : "false", sh->won ? "true" : "false");
        buf_puts(out, ".\n");
        return true;
    }

    if (!strcmp(cmd, "enemy")) {
        buf_puts(out, "+OK enemy\n");
        buf_printf(out, "{\"name\":\"%s\",\"hull\":%d,\"hull_max\":%d,\"shields\":%d,\"charge\":%d}\n",
                   sh->enemy.name, (int)sh->enemy.hull, (int)sh->enemy.hull_max,
                   sh->enemy.shields, (int)(sh->enemy.charge * 100));
        buf_puts(out, ".\n");
        return true;
    }

    if (!strcmp(cmd, "rooms")) {
        buf_puts(out, "+OK rooms\n");
        for (int i = 0; i < sh->nroom; i++) {
            const Room *r = &sh->room[i];
            buf_printf(out, "{\"n\":%d,\"name\":\"%s\",\"system\":\"%s\",\"bars\":%d,\"cap\":%d,"
                            "\"damage\":%d,\"oxygen\":%d,\"fire\":%d,\"breach\":%s,\"door\":\"%s\"}\n",
                       i, r->name, sys_name(r->sys.kind), r->sys.bars, r->sys.cap,
                       r->sys.damage, (int)(r->oxygen * 100), (int)(r->fire * 100),
                       r->breach ? "true" : "false", r->door_open ? "open" : "shut");
        }
        buf_puts(out, ".\n");
        return true;
    }

    if (!strcmp(cmd, "crew")) {
        buf_puts(out, "+OK crew\n");
        for (int i = 0; i < sh->ncrew; i++)
            buf_printf(out, "{\"name\":\"%s\",\"room\":%d,\"where\":\"%s\",\"health\":%d,\"alive\":%s}\n",
                       sh->crew[i].name, sh->crew[i].room, sh->room[sh->crew[i].room].name,
                       (int)(sh->crew[i].health * 100), sh->crew[i].alive ? "true" : "false");
        buf_puts(out, ".\n");
        return true;
    }

    if (!strcmp(cmd, "log")) {
        buf_puts(out, "+OK log\n");
        for (int i = 0; i < sh->nlog; i++) buf_printf(out, "%s\n", sh->log[i]);
        buf_puts(out, ".\n");
        return true;
    }

    /* ------------------------------------------------------- the machine */
    if (!strcmp(cmd, "sh")) {
        if (argc < 2) { err(out, "sh <command>  -- a shell on the ship's computer"); return true; }
        const char *rest = line;
        while (*rest == ' ') rest++;
        rest += 2;
        while (*rest == ' ') rest++;
        Buf shout;
        buf_init(&shout);
        box_sh(world_box(w), rest, &shout);
        buf_puts(out, "+OK\n");
        if (shout.len) buf_put(out, shout.p, shout.len);
        if (shout.len && shout.p[shout.len - 1] != '\n') buf_putc(out, '\n');
        buf_puts(out, ".\n");
        buf_free(&shout);
        return true;
    }

    /* RUN IT IN THE FIGHT. `sh py script.py` runs it now, to completion, while
     * everything waits. `run script.py` starts it as a daemon on the ship's
     * computer, so it keeps going while the raider keeps shooting -- which is
     * the entire point of writing one. */
    if (!strcmp(cmd, "run")) {
        if (argc < 2) { err(out, "run <script>  -- start it running in the fight"); return true; }
        if (!box_start(world_box(w), argv[1], e, sizeof e)) { err(out, "%s", e); return true; }
        if (ship_compute_slices(sh) <= 0)
            ok(out, "%s started -- but the computer has no power, so it will not run", argv[1]);
        else
            ok(out, "%s is running", argv[1]);
        return true;
    }

    /* ------------------------------------------------------ the recorder */
    if (!strcmp(cmd, "rec.start")) {
        recorder_start(w, argc > 1 ? argv[1] : "recorded");
        ok(out, "recording. play the way you normally would, then rec.stop");
        return true;
    }
    if (!strcmp(cmd, "rec.stop")) {
        recorder_stop(w);
        buf_puts(out, "+OK stopped\n");
        recorder_status(w, out);
        buf_puts(out, "\n.\n");
        return true;
    }
    if (!strcmp(cmd, "rec.status")) {
        buf_puts(out, "+OK recorder\n");
        recorder_status(w, out);
        buf_puts(out, "\n.\n");
        return true;
    }
    if (!strcmp(cmd, "rec.script")) {
        buf_puts(out, "+OK script\n");
        recorder_script(w, out);
        buf_puts(out, ".\n");
        return true;
    }
    if (!strcmp(cmd, "rec.clear")) { recorder_clear(w); ok(out, "cleared"); return true; }
    if (!strcmp(cmd, "rec.save")) {
        char path[RB_VAL_MAX];
        if (argc > 1) snprintf(path, sizeof path, "%s", argv[1]);
        else          snprintf(path, sizeof path, "/root/scripts/%s.py", w->rec.name);
        Buf script;
        buf_init(&script);
        recorder_script(w, &script);
        Box *b = world_box(w);
        Buf ignore;
        buf_init(&ignore);
        box_sh(b, "mkdir /root/scripts", &ignore);
        buf_free(&ignore);
        bool wrote = box_write(b, path, script.p ? script.p : "", script.len);
        if (wrote) ok(out, "wrote %s -- start it with: run %s", path, path);
        else       err(out, "could not write %s", path);
        buf_free(&script);
        return true;
    }

    /* IT IS NOT A SHELL, and saying so is better than "unknown". */
    static const char *const SHELLISH[] = { "ls", "cd", "cat", "pwd", "echo", "ps", "grep" };
    for (size_t i = 0; i < sizeof SHELLISH / sizeof SHELLISH[0]; i++)
        if (!strcmp(cmd, SHELLISH[i])) {
            err(out, "'%s' is a program on the ship's computer -- try: sh %s", cmd, line);
            return true;
        }

    err(out, "no such command: %s (try 'help')", cmd);
    return true;
}

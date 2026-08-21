/* proto.c — command dispatch.
 *
 * PROTOCOL NOTE, from the handoff §14 and paid for once already in NOMINAL:
 * this looks like a request/response protocol because that is what it is. It
 * is not HTTP, it does not become HTTP, and the in-game appliance API that
 * arrives at M1 will not be HTTP either — request in, response out, with
 * latency and rate limits as numbers. The lesson of the packet simulation is
 * that fidelity to a real protocol buys nothing and costs everything.
 */
#include "proto.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

/* ------------------------------------------------------------ tokenising */
/* Whitespace-separated words, no quoting. Deliberately primitive: tickets are
 * structured objects and arguments are identifiers (handoff decision 5), so
 * the day this needs a quoting rule is the day something has gone wrong with
 * the data model. */
#define MAX_ARGV 8

static int split(char *line, char *argv[MAX_ARGV])
{
    int argc = 0;
    char *p = line;
    while (*p && argc < MAX_ARGV) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = 0;
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

static int dept_by_name(const char *s)
{
    for (int i = 0; i < RB_DEPT__N; i++)
        if (strcmp(s, rb_dept_name[i]) == 0) return i;
    return -1;
}

static void put_user(Buf *out, const User *u)
{
    buf_printf(out,
        "{\"id\":\"%s\",\"login\":\"%s\",\"given\":\"%s\",\"family\":\"%s\","
        "\"dept\":\"%s\",\"prov\":\"%s\",\"hired_day\":%d,\"left_day\":%d}\n",
        u->id, u->login, u->given, u->family,
        rb_dept_name[u->dept], prov_name((Prov)u->prov), u->hired_day, u->left_day);
}

/* ---------------------------------------------------------------- session */
void proto_open(Session *s, World *w)
{
    s->w = w;
    /* Writes over the API are attributed to a script by default, because
     * that is what is on the other end of a socket. The Godot client will set
     * this to PROV_HAND for work the player does through a form, and that
     * distinction is the whole of the debt mechanic (handoff §11). */
    s->prov = PROV_SCRIPT;
    s->open = true;
}

void proto_hello(Session *s, Buf *out)
{
    buf_printf(out,
        "+OK RUNBOOK/1 %s — day %d, %d users\n"
        "type 'help'; every response ends with a lone '.'\n.\n",
        s->w->org, s->w->day, s->w->active);
}

static void cmd_help(Buf *out)
{
    /* THIS LIST IS A TECHNICAL CLAIM AND THE PROJECT RULE APPLIES TO IT
     * (handoff §13): every verb named here must exist and behave as
     * described, checked by running it. --mancheck arrives at M1 and will
     * execute this text against a live world. Until it does, adding a verb
     * here that does not work is the exact failure the rule exists to
     * prevent, so do not. */
    buf_puts(out,
        "+OK verbs\n"
        "help                          this\n"
        "world.info                    org, day, headcount\n"
        "world.hash                    the state fingerprint the gates compare\n"
        "world.dump                    the whole world, canonically\n"
        "day.advance [n]               advance n whole days (default 1)\n"
        "session.as hand|script|system attribute this session's writes\n"
        "user.list [active|all]        one user per line\n"
        "user.get <id|login>           one user\n"
        "user.add <given> <family> <dept>   returns the id and the derived login\n"
        "user.offboard <id>            idempotent\n"
        "depts                         valid department names\n"
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

    if (!strcmp(cmd, "help"))  { cmd_help(out); return true; }
    if (!strcmp(cmd, "quit") || !strcmp(cmd, "exit")) {
        buf_puts(out, "+OK bye\n.\n");
        s->open = false;
        return false;
    }

    if (!strcmp(cmd, "world.info")) {
        buf_printf(out, "+OK {\"org\":\"%s\",\"seed\":%llu,\"day\":%d,\"minute\":%d,"
                        "\"day_minutes\":%d,\"users_total\":%zu,\"users_active\":%d}\n.\n",
                   w->org, (unsigned long long)w->seed, w->day, w->minute,
                   RB_DAY_MINUTES, w->nusers, w->active);
        return true;
    }

    if (!strcmp(cmd, "world.hash")) {
        ok(out, "%016llx", (unsigned long long)world_hash(w));
        return true;
    }

    if (!strcmp(cmd, "world.dump")) {
        buf_puts(out, "+OK dump\n");
        world_dump(w, out);
        buf_puts(out, ".\n");
        return true;
    }

    if (!strcmp(cmd, "day.advance")) {
        int n = (argc > 1) ? atoi(argv[1]) : 1;
        if (n < 1 || n > 3650) { err(out, "day.advance takes 1..3650"); return true; }
        int hired = 0;
        for (int i = 0; i < n; i++) hired += world_day_advance(w);
        ok(out, "day %d, hired %d, active %d", w->day, hired, w->active);
        return true;
    }

    if (!strcmp(cmd, "session.as")) {
        if (argc < 2) { err(out, "session.as hand|script|system"); return true; }
        if      (!strcmp(argv[1], "hand"))   s->prov = PROV_HAND;
        else if (!strcmp(argv[1], "script")) s->prov = PROV_SCRIPT;
        else if (!strcmp(argv[1], "system")) s->prov = PROV_SYSTEM;
        else { err(out, "not a provenance: %s", argv[1]); return true; }
        ok(out, "writes attributed to %s", prov_name(s->prov));
        return true;
    }

    if (!strcmp(cmd, "depts")) {
        buf_puts(out, "+OK depts\n");
        for (int i = 0; i < RB_DEPT__N; i++) buf_printf(out, "%s\n", rb_dept_name[i]);
        buf_puts(out, ".\n");
        return true;
    }

    if (!strcmp(cmd, "user.list")) {
        bool all = (argc > 1 && !strcmp(argv[1], "all"));
        int n = 0;
        buf_puts(out, "+OK users\n");
        for (size_t i = 0; i < w->nusers; i++) {
            if (!all && w->users[i].left_day >= 0) continue;
            put_user(out, &w->users[i]);
            n++;
        }
        buf_printf(out, ".\n");
        (void)n;
        return true;
    }

    if (!strcmp(cmd, "user.get")) {
        if (argc < 2) { err(out, "user.get <id|login>"); return true; }
        User *u = world_user_find(w, argv[1]);
        if (!u) u = world_user_by_login(w, argv[1]);
        if (!u) { err(out, "no such user: %s", argv[1]); return true; }
        buf_puts(out, "+OK user\n");
        put_user(out, u);
        buf_puts(out, ".\n");
        return true;
    }

    if (!strcmp(cmd, "user.add")) {
        if (argc < 4) { err(out, "user.add <given> <family> <dept>"); return true; }
        int d = dept_by_name(argv[3]);
        if (d < 0) { err(out, "no such department: %s (try 'depts')", argv[3]); return true; }
        User *u = world_user_add(w, s->prov, argv[1], argv[2], (uint8_t)d);
        if (!u) { err(out, "%s", w->err); return true; }
        /* The derived login is returned rather than echoed back from input,
         * because de-collision may have changed it and a script that assumes
         * otherwise is one of the bugs Act II is about (handoff §8.1). */
        ok(out, "{\"id\":\"%s\",\"login\":\"%s\"}", u->id, u->login);
        return true;
    }

    if (!strcmp(cmd, "user.offboard")) {
        if (argc < 2) { err(out, "user.offboard <id>"); return true; }
        if (!world_user_offboard(w, argv[1])) { err(out, "%s", w->err); return true; }
        ok(out, "offboarded %s", argv[1]);
        return true;
    }

    err(out, "unknown verb: %s (try 'help')", cmd);
    return true;
}

/* /usr/bin/svc — what is actually running.
 *
 * `ps` shows processes; this shows SERVICES, which is a different question.
 * A service can be defined and disabled, defined and running, or defined and
 * dead in a loop the boot console scrolled past twenty lines ago. On a
 * machine that boots and is still wrong, this is where you look first.
 *
 * AND WHERE THE REPAIR HAPPENS. It used to be a read-only instrument with
 * two verbs that only changed the NEXT boot, so a player who had diagnosed a
 * running-and-wrong machine perfectly had nothing to do but power cycle it --
 * the same ritual as a machine that will not boot at all, and the one act
 * that destroys the evidence for the whole class of fault where a process is
 * out of step with a file. start, stop, restart and reload act on the machine
 * as it stands; enable and disable act on the next boot; and most real
 * repairs are one from each half.
 */
#include "gsys.h"

static char body[4096], name[128], path[192], procbuf[512], pname[128];

static void get(const char *b, const char *k, char *out, u64 cap, const char *dflt)
{
    g_copy(out, dflt, cap);
    u64 kl = g_strlen(k);
    const char *p = b;
    while (*p) {
        const char *nl = p; while (*nl && *nl != '\n') nl++;
        const char *s = p;
        while (*s == ' ' || *s == '\t') s++;
        if (*s != '#') {
            u64 i = 0;
            while (i < kl && s + i < nl && s[i] == k[i]) i++;
            if (i == kl && s + i < nl && (s[i] == ':' || s[i] == '=')) {
                const char *v = s + i + 1;
                while (v < nl && (*v == ' ' || *v == '\t')) v++;
                u64 j = 0;
                while (v + j < nl && j + 1 < cap) { out[j] = v[j]; j++; }
                while (j && (out[j-1] == ' ' || out[j-1] == '\r')) j--;
                out[j] = 0;
                return;
            }
        }
        p = *nl ? nl + 1 : nl;
    }
}

/* /proc uses "key value", the unit files use "key: value". Reusing the unit
 * parser on /proc matched nothing and reported every running service as DEAD
 * -- a diagnostic tool that lies is worse than no tool. */
static void proc_field(const char *b, const char *k, char *out, u64 cap)
{
    out[0] = 0;
    u64 kl = g_strlen(k);
    const char *p = b;
    while (*p) {
        const char *nl = p; while (*nl && *nl != '\n') nl++;
        u64 i = 0;
        while (i < kl && p + i < nl && p[i] == k[i]) i++;
        if (i == kl && p + i < nl && p[i] == ' ') {
            const char *v = p + i + 1;
            u64 j = 0;
            while (v + j < nl && j + 1 < cap) { out[j] = v[j]; j++; }
            out[j] = 0;
            return;
        }
        p = *nl ? nl + 1 : nl;
    }
}

/* Is this exec currently a live process? /proc is the truth; the unit file is
 * only an intention. */
static int is_running(const char *exec)
{
    static char pdir[64];
    for (int i = 0; i < 256; i++) {
        if (g_readdir("/proc", i, pname) < 0) break;
        g_copy(pdir, "/proc/", sizeof pdir);
        g_cat(pdir, pname, sizeof pdir);
        g_cat(pdir, "/status", sizeof pdir);
        if (g_slurp(pdir, procbuf, sizeof procbuf) < 0) continue;
        static char nm[128], st[32];
        proc_field(procbuf, "name", nm, sizeof nm);
        proc_field(procbuf, "state", st, sizeof st);
        if (g_streq(nm, exec) && g_streq(st, "running")) return 1;
    }
    return 0;
}

/* Rewrite one `key: value` line of a unit file, in place. */
static int set_field(const char *unit, const char *key, const char *val)
{
    static char up[192], nb[4096];
    g_copy(up, "/etc/services.d/", sizeof up);
    g_cat(up, unit, sizeof up);
    g_cat(up, ".svc", sizeof up);
    i64 n = g_slurp(up, body, sizeof body);
    if (n < 0) return 0;
    u64 o = 0, kl = g_strlen(key);
    int hit = 0;
    u64 i = 0;
    while (i < (u64)n) {
        u64 e = i; while (e < (u64)n && body[e] != '\n') e++;
        u64 k = 0;
        while (k < kl && i + k < e && body[i + k] == key[k]) k++;
        if (k == kl && i + kl < e && body[i + kl] == ':') {
            for (u64 q = 0; q < kl && o + 1 < sizeof nb; q++) nb[o++] = key[q];
            if (o + 2 < sizeof nb) { nb[o++] = ':'; nb[o++] = ' '; }
            for (const char *q = val; *q && o + 1 < sizeof nb; q++) nb[o++] = *q;
            hit = 1;
        } else {
            for (u64 q = i; q < e && o + 1 < sizeof nb; q++) nb[o++] = body[q];
        }
        if (o + 1 < sizeof nb) nb[o++] = '\n';
        i = e < (u64)n ? e + 1 : (u64)n;
    }
    if (!hit) return 0;
    int fd = g_open(up, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return 0;
    sysc(SYS_write, fd, (i64)nb, (i64)o);
    g_close(fd);
    return 1;
}

/* WHICH FILE IS THIS NAME?
 *
 * The table shows the unit's `name:` field, which is usually the filename
 * and is not always -- a unit whose name line has been lost shows up as
 * "httpd.svc" -- and a player types what they can see. Both spellings find
 * the same file. Leaves the unit's text in `body`. */
static char upath[192];
static int find_unit(const char *want)
{
    static char dn[128], nm[64];
    for (int i = 0; i < 256; i++) {
        if (g_readdir("/etc/services.d", i, dn) < 0) break;
        if (!g_endswith(dn, ".svc")) continue;
        g_copy(upath, "/etc/services.d/", sizeof upath);
        g_cat(upath, dn, sizeof upath);
        if (g_slurp(upath, body, sizeof body) < 0) continue;
        get(body, "name", nm, sizeof nm, dn);
        if (g_streq(nm, want) || g_streq(dn, want)) return 1;
    }
    return 0;
}

/* THE SAME SENTENCE THE BOOT PRINTS.
 *
 * svcinit distinguishes four ways a service refuses to start, and a player
 * who has read one of them on the console should meet the identical words
 * here -- a second vocabulary for the same four failures would be a second
 * thing to learn for no reason. */
static const char *why_not(i64 rc)
{
    if (rc == SPAWN_ENOENT)  return ": not found";
    if (rc == SPAWN_EPERM)   return ": present, and not executable";
    if (rc == SPAWN_ENOEXEC) return ": will not load -- check `ldd` on it";
    if (rc == SPAWN_EFAULT)  return ": started and would not stay up";
    if (rc == SPAWN_EDEPTH)  return ": too many services are already running";
    return ": failed to start";
}

/* Start what the unit file says, reading it FROM DISK every time. That is
 * what makes `svc restart` the repair for a daemon holding a stale config:
 * the file is read now, not remembered from the boot. */
static int start_unit(const char *want, int restarted)
{
    static char nm[64], exec[160], en[16], rl[32], bnd[192], rs[16];
    if (!find_unit(want)) {
        g_puts("svc: "); g_puts(want);
        g_putln(": no such unit in /etc/services.d");
        return 1;
    }
    get(body, "name",    nm,   sizeof nm,   want);
    get(body, "exec",    exec, sizeof exec, "");
    get(body, "enabled", en,   sizeof en,   "yes");
    get(body, "runlevel", rl,  sizeof rl,   "3");
    get(body, "restart", rs,   sizeof rs,   "no");
    get(body, "bind",    bnd,  sizeof bnd,  "");
    if (!exec[0]) {
        g_puts("svc: "); g_puts(nm);
        if (bnd[0])
            g_putln(": that unit only binds a directory into place at boot."
                    " There is no process to start.");
        else
            g_putln(": the unit has no exec line, so there is nothing to start."
                    " That is an invalid unit, not a disabled one.");
        return 1;
    }
    int pol = 0;
    if (g_streq(rs, "on-failure")) pol = 1;
    else if (g_streq(rs, "always")) pol = 2;

    i64 rc = g_svcstart(exec, nm, pol);
    if (rc == SPAWN_EBUSY) {
        g_puts("svc: "); g_puts(nm);
        g_putln(" is already running -- nothing to do.");
        g_puts("  `svc status "); g_puts(nm); g_putln("` says since when.");
        return 0;
    }
    if (rc != 0) {
        g_puts("svc: "); g_puts(nm); g_puts(": "); g_puts(exec);
        g_putln(why_not(rc));
        if (rc == SPAWN_EFAULT) {
            g_puts("  what it said on the way down is above, and in `dmesg -f ");
            g_puts(nm); g_putln("`.");
        }
        return 1;
    }
    g_puts("svc: "); g_puts(nm);
    if (restarted)
        g_putln(" restarted -- it has re-read its unit and its config from disk.");
    else { g_puts(" started -- "); g_putln(exec); }
    /* IT IS RUNNING NOW AND THAT IS NOT THE SAME AS FIXED. A unit that is
     * still `enabled: no` comes back to nothing at the next boot, and a
     * machine that is only healthy until somebody reboots it is a ticket
     * that reopens next week with your name on it. */
    if (!g_streq(en, "yes")) {
        g_puts("  the unit is still `enabled: no`, so it will not start at the"
               " next boot -- `svc enable "); g_puts(nm); g_putln("`.");
    } else {
        int here = 0;
        for (const char *q = rl; *q; q++) if (*q == '3') here = 1;
        if (!here) {
            g_puts("  the unit's runlevel is "); g_puts(rl);
            g_putln(", so it will not start at the next boot either.");
        }
    }
    return 0;
}

void _start(void)
{
    /* `svc disable <name>` used to ignore its argument entirely, print the
     * whole table, and exit clean -- a silent no-op that a playtester
     * reasonably read as a broken command. Either it does the thing or it
     * says it cannot; doing neither is the one unacceptable answer. */
    static char arg[192];
    g_getarg(arg, sizeof arg);
    char *v[GARGS];
    int argn = g_argv(arg, v);
    if (argn >= 1) {
        /* `svc status <name>` -- why is THIS one unhappy.
         *
         * The table could say running or DEAD and nothing else, so on a
         * machine that boots with a service quietly down there was no way to
         * ask the follow-up question. The kernel has always recorded what the
         * service said when it died and how many times it was restarted. */
        if (g_streq(v[0], "status")) {
            if (argn < 2) { g_putln("usage: svc status <name>"); g_exit(1); }
            static char info[1024];
            i64 n2 = sysc(SYS_svcinfo, (i64)v[1], (i64)info, sizeof info - 1);
            if (n2 <= 0) {
                g_puts("svc: "); g_puts(v[1]);
                g_putln(": nothing by that name has been started on this boot.");
                g_putln("  `svc` lists the units; a unit that is disabled or in");
                g_putln("  another runlevel was never started, so there is");
                g_putln("  nothing to report about it.");
                g_exit(1);
            }
            info[n2] = 0;
            g_write(1, info, (u64)n2);
            g_exit(0);
        }

        /* THE VERBS THAT ACT.
         *
         * `svc` could say what was wrong and could only change what happens
         * at the NEXT boot, so every ticket where the machine is up and one
         * service is wrong ended in `rcon power cycle` -- the same ritual as
         * a machine that will not boot at all, and the one act that destroys
         * the evidence for a daemon out of step with its config file. The
         * diagnosis was a different job and the repair collapsed back into
         * the old one. These four are the repair. */
        if (g_streq(v[0], "start") || g_streq(v[0], "stop") ||
            g_streq(v[0], "restart") || g_streq(v[0], "reload")) {
            if (argn < 2) {
                g_puts("usage: svc "); g_puts(v[0]); g_putln(" <name>");
                g_exit(1);
            }
            if (g_streq(v[0], "start")) g_exit(start_unit(v[1], 0));

            /* THE TABLE'S NAME AND THE FILE'S NAME ARE NOT ALWAYS THE SAME,
             * and the kernel only knows the one the unit declared. A player
             * who typed the filename got "nothing by that name has been
             * started on this boot" about a service they could see running
             * two lines above. Resolve it the same way `start` does, and fall
             * back to what was typed when no unit file matches -- a service
             * can be in the table with its file since deleted. */
            static char cname[64], cen[16];
            g_copy(cname, v[1], sizeof cname);
            g_copy(cen, "yes", sizeof cen);
            if (find_unit(v[1])) {
                get(body, "name", cname, sizeof cname, v[1]);
                get(body, "enabled", cen, sizeof cen, "yes");
            }

            if (g_streq(v[0], "reload")) {
                int rc = g_svcreload(cname);
                if (rc == SVCCTL_ENOSVC) {
                    g_puts("svc: "); g_puts(v[1]);
                    g_putln(": nothing by that name has been started on this boot.");
                    g_exit(1);
                }
                if (rc == SVCCTL_ENOTRUN) {
                    g_puts("svc: "); g_puts(v[1]);
                    g_putln(" is not running, so there is nothing to reload.");
                    g_puts("  `svc start "); g_puts(v[1]); g_putln("` starts it.");
                    g_exit(1);
                }
                /* A DAEMON THAT WOULD IGNORE YOU MUST SAY SO. Reporting a
                 * reload that did not happen is the same lie as a console
                 * faking a boot: the file would still not be loaded and the
                 * player would have crossed the real fix off the list. */
                if (rc != 0) {
                    g_puts("svc: "); g_puts(v[1]);
                    g_putln(" does not re-read its configuration on a signal.");
                    g_puts("  the HUP went nowhere. `svc restart "); g_puts(v[1]);
                    g_putln("` is how this one picks up a change.");
                    g_exit(1);
                }
                g_puts("svc: "); g_puts(v[1]);
                g_putln(" reloaded -- it has re-read its configuration.");
                g_putln("  what it loaded is in /run/<name>.state; if that still");
                g_putln("  disagrees with the file, the file is not the one it reads.");
                g_exit(0);
            }

            int rc = g_svcstop(cname);
            if (g_streq(v[0], "stop")) {
                if (rc == SVCCTL_ENOSVC) {
                    g_puts("svc: "); g_puts(v[1]);
                    g_putln(": nothing by that name has been started on this boot.");
                    g_exit(1);
                }
                if (rc == SVCCTL_ENOTRUN) {
                    g_puts("svc: "); g_puts(v[1]);
                    g_putln(" is not running already.");
                    g_exit(1);
                }
                g_puts("svc: "); g_puts(v[1]); g_putln(" stopped.");
                /* SAY WHICH TOMORROW THIS IS. Telling someone their stopped
                 * service comes back at the next boot, when the unit says
                 * `enabled: no` and it never will, is the same wrong answer
                 * the table used to give about the same two facts. */
                if (g_streq(cen, "yes")) {
                    g_putln("  the unit is untouched, so it starts again at the next");
                    g_putln("  boot -- `svc disable` is what stops that.");
                } else {
                    g_putln("  the unit is `enabled: no`, so nothing brings it back at");
                    g_putln("  the next boot either -- `svc enable` is what changes that.");
                }
                g_exit(0);
            }
            /* restart: down, then up, off the files as they are NOW. */
            if (rc == SVCCTL_ENOSVC) {
                g_puts("svc: "); g_puts(v[1]);
                g_putln(" was not started on this boot; starting it.");
            }
            g_exit(start_unit(v[1], 1));
        }

        int off = g_streq(v[0], "disable");
        if (off || g_streq(v[0], "enable")) {
            if (argn < 2) {
                g_puts("usage: svc "); g_puts(v[0]); g_putln(" <name>");
                g_exit(1);
            }
            if (!set_field(v[1], "enabled", off ? "no" : "yes")) {
                g_puts("svc: "); g_puts(v[1]);
                g_putln(": no such unit in /etc/services.d");
                g_exit(1);
            }
            g_puts("svc: "); g_puts(v[1]);
            g_putln(off ? " disabled -- it will not start at the next boot"
                        : " enabled -- it will start at the next boot");
            g_putln("(the unit file is a package file: `pkg verify` will now");
            g_putln(" report it as CHANGED, which is correct -- you changed it)");
            /* AND IT HAS NOT HAPPENED YET. These two verbs write a file and
             * nothing else, which is the single easiest thing to misread on
             * this machine: the unit says yes, the table still says nothing
             * is running it, and the repair looks like it did not work. */
            g_puts(off ? "  it is still running now -- `svc stop "
                       : "  nothing has started yet -- `svc start ");
            g_puts(v[1]); g_putln("` does that.");
            g_exit(0);
        }
        g_puts("svc: unknown command: "); g_putln(v[0]);
        g_putln("usage: svc                      every unit and its state");
        g_putln("       svc status <name>        why THIS one is unhappy");
        g_putln("       svc start|stop|restart|reload <name>   now");
        g_putln("       svc enable|disable <name>              at the next boot");
        g_exit(1);
    }

    int any_dead = 0, any_odd = 0;
    g_putln("SERVICE          STATE          EXEC");
    for (int i = 0; i < 256; i++) {
        if (g_readdir("/etc/services.d", i, name) < 0) break;
        if (!g_endswith(name, ".svc")) continue;
        g_copy(path, "/etc/services.d/", sizeof path);
        g_cat(path, name, sizeof path);
        if (g_slurp(path, body, sizeof body) < 0) continue;

        static char nm[64], exec[160], en[16], rl[32];
        get(body, "name",     nm,   sizeof nm,   name);
        get(body, "exec",     exec, sizeof exec, "(none)");
        get(body, "enabled",  en,   sizeof en,   "yes");
        get(body, "runlevel", rl,   sizeof rl,   "3");

        /* A service that is not meant to run at this runlevel is not dead,
         * it is simply not here -- calling it DEAD sends the player looking
         * for a fault that does not exist. */
        int here = 0;
        for (const char *q = rl; *q; q++)
            if (*q == '3') here = 1;

        /* A UNIT WITH NO PROGRAM IN IT IS NOT A DEAD SERVICE.
         *
         * A unit whose only job is a `bind` starts nothing, so nothing is
         * running it, so this reported it DEAD -- and the legend underneath
         * then told the player to go and find out why a service had failed.
         * There is no service. Saying what it actually does is the difference
         * between a clue and a wild goose chase, and what it does is the
         * whole fault when one of these turns up unowned. */
        static char bnd[192];
        get(body, "bind", bnd, sizeof bnd, "");

        /* WHAT THE UNIT SAYS AND WHAT IS ACTUALLY RUNNING ARE TWO FACTS.
         *
         * This printed the unit's intention and stopped: a service somebody
         * had disabled and left running read `disabled` while its process sat
         * there in `ps` holding its port. Nobody could produce that state
         * before -- disabling a unit only ever changed the next boot -- and
         * `svc stop` produces it in one command, so the table has to be able
         * to say both halves. `disabled` is about the NEXT boot. */
        int up = !(bnd[0] && g_streq(exec, "(none)")) && is_running(exec);
        const char *state;
        if (bnd[0] && g_streq(exec, "(none)")) state = "namespace";
        else if (!g_streq(en, "yes"))   { state = up ? "disabled, up" : "disabled";
                                          if (up) any_odd = 1; }
        else if (!here)                 { state = up ? "not at rl3, up" : "not at rl3";
                                          if (up) any_odd = 1; }
        else if (up)                    state = "running";
        else                          { state = "DEAD"; any_dead = 1; }

        g_puts(nm);
        for (u64 k = g_strlen(nm); k < 17; k++) g_puts(" ");
        g_puts(state);
        for (u64 k = g_strlen(state); k < 15; k++) g_puts(" ");
        if (bnd[0] && g_streq(exec, "(none)")) {
            g_puts("bind ");
            g_putln(bnd);
        } else {
            g_putln(exec);
        }
    }
    /* Only explain DEAD when something is. The legend on every healthy
     * machine is furniture the eye stops seeing. */
    if (any_dead || any_odd) {
        g_putln("");
        if (any_dead)
            g_putln("DEAD means the unit is enabled and nothing is running it.");
        if (any_odd)
            g_putln("`, up` means the unit would not start at the next boot and the"
                    " process is\nrunning anyway. `svc stop <name>` is what stops it"
                    " now.");
    }
    g_exit(0);
}

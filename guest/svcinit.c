/* /sbin/svcinit — bring up the services in /etc/services.d.
 *
 * Reads every .svc unit, honours `enabled` and `runlevel`, orders by `after`,
 * and starts what is left. The unit grammar is NomnixOS's: `key: value` per
 * line, '#' comments.
 *
 * A required unit that will not start takes the boot down. That is the whole
 * reason a chmod on one daemon is a ticket.
 */
#include "gsys.h"

#define UNITS 32

static char names[UNITS][64];
static char execs[UNITS][160];
static char afters[UNITS][64];
/* Units we did NOT load, and why. A unit that is disabled or belongs to
 * another runlevel is skipped entirely, which is right -- but anything
 * ordered AFTER it then waits for a name that is simply not in the list, and
 * "waiting for net" with no explanation is the single most confusing thing an
 * init system can print. Keeping the reason costs two small arrays. */
static char skipname[UNITS][64];
static char skipwhy[UNITS][40];
static int  nskip = 0;
static char descs[UNITS][96];
static char unitname[UNITS][64];
static int  started[UNITS];
static int  nunits;

static char body[4096];
static char level[16];
static char crit[UNITS][8];
static char restart[UNITS][16];
static int  failed_critical;

/* Pull `key` out of a `key: value` config body into `out`. */
static void get(const char *b, const char *key, char *out, u64 cap, const char *dflt)
{
    g_copy(out, dflt, cap);
    u64 kl = g_strlen(key);
    const char *p = b;
    while (*p) {
        const char *nl = p;
        while (*nl && *nl != '\n') nl++;
        const char *s = p;
        while (*s == ' ' || *s == '\t') s++;
        if (*s != '#') {
            u64 i = 0;
            while (i < kl && s + i < nl && s[i] == key[i]) i++;
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

static int wanted_at_level(const char *rl)
{
    const char *p = rl;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (p[0] == level[0] && (p[1] == ' ' || p[1] == 0)) return 1;
        while (*p && *p != ' ') p++;
    }
    return 0;
}

void _start(void)
{
    if (g_getarg(level, sizeof level) <= 0) g_copy(level, "3", sizeof level);

    static char name[128], path[256];
    for (int i = 0; i < 512; i++) {
        i64 n = g_readdir("/etc/services.d", i, name);
        if (n < 0) break;
        if (!g_endswith(name, ".svc")) continue;
        if (nunits >= UNITS) break;

        g_copy(path, "/etc/services.d/", sizeof path);
        g_cat(path, name, sizeof path);
        if (g_slurp(path, body, sizeof body) < 0) {
            g_puts("svcinit: ");
            g_puts(path);
            g_putln(": cannot read");
            g_exit(1);
        }

        static char en[16], rl[32], nm0[64];
        get(body, "name", nm0, sizeof nm0, name);
        get(body, "enabled", en, sizeof en, "yes");
        if (!g_streq(en, "yes")) {
            if (nskip < UNITS) {
                g_copy(skipname[nskip], nm0, sizeof skipname[0]);
                g_copy(skipwhy[nskip], "it is disabled", sizeof skipwhy[0]);
                nskip++;
            }
            continue;
        }
        get(body, "runlevel", rl, sizeof rl, "3");
        if (!wanted_at_level(rl)) {
            if (nskip < UNITS) {
                g_copy(skipname[nskip], nm0, sizeof skipname[0]);
                g_copy(skipwhy[nskip], "it is not in this runlevel", sizeof skipwhy[0]);
                nskip++;
            }
            continue;
        }

        /* A UNIT MAY BRING A DIRECTORY INTO PLACE BEFORE ANYTHING STARTS.
         *
         * `bind: TARGET AT` is the unit-file spelling of what rc.boot can
         * already do, and it is how a configuration-management agent ships
         * its own copy of a config tree: it binds it over the real one and
         * every service started afterwards reads the agent's files instead.
         * Nothing is corrupt when that happens -- `pkg verify` is perfectly
         * clean, the file you `cat` is the right one, and the daemon is
         * reading another one entirely. `ns <pid>` is the tool.
         *
         * Applied here, during the read, so it is in place before any service
         * is started; a unit that only binds needs no exec line and is not a
         * service at all. */
        {
            static char bnd[192];
            get(body, "bind", bnd, sizeof bnd, "");
            if (bnd[0]) {
                char *bv[GARGS];
                int bn = g_argv(bnd, bv);
                if (bn >= 2 && g_bind(bv[0], bv[1]) == 0) {
                    g_puts("svcinit: ");
                    g_puts(nm0);
                    g_puts(": bound ");
                    g_puts(bv[0]);
                    g_puts(" over ");
                    g_putln(bv[1]);
                } else {
                    g_puts("svcinit: ");
                    g_puts(nm0);
                    g_putln(": bind failed");
                }
                static char ex0[160];
                get(body, "exec", ex0, sizeof ex0, "");
                if (!ex0[0]) continue;      /* a namespace unit, not a service */
            }
        }

        int u = nunits++;
        g_copy(unitname[u], name, sizeof unitname[u]);
        get(body, "name",        names[u],  sizeof names[u],  name);
        /* `critical: yes` means the machine is not usable without it. Anything
         * else is reported and stepped over -- a box where sshd is down is a
         * different (and lesser) problem than a box that will not boot, and
         * conflating them would be wrong. */
        get(body, "critical",    crit[u],   sizeof crit[u],   "no");
        get(body, "restart",     restart[u], sizeof restart[u], "no");
        get(body, "exec",        execs[u],  sizeof execs[u],  "");
        get(body, "after",       afters[u], sizeof afters[u], "");
        get(body, "description", descs[u],  sizeof descs[u],  "");
    }

    for (int round = 0; round < UNITS + 1; round++) {
        int moved = 0;
        for (int u = 0; u < nunits; u++) {
            if (started[u]) continue;
            if (afters[u][0]) {
                int ready = 0;
                for (int v = 0; v < nunits; v++)
                    if (started[v] && g_streq(names[v], afters[u])) ready = 1;
                if (!ready) continue;
            }
            if (!execs[u][0]) {
                g_puts("svcinit: ");
                g_puts(unitname[u]);
                g_putln(": no exec line");
                g_exit(1);
            }
            /* Actually START it. A service that is merely present and
             * executable has not started; one that reads a missing config
             * and exits has started and failed, which is a different fault
             * with a different fix. */
            /* on-failure is the usual policy; a service that says nothing
             * gets no restart, which is the safe reading. */
            int pol = 0;
            if (g_streq(restart[u], "on-failure")) pol = 1;
            else if (g_streq(restart[u], "always")) pol = 2;
            i64 rc = g_svcstart(execs[u], names[u], pol);
            int bad = (rc != 0);
            /* SAY WHICH KIND OF FAILURE IT WAS.
             *
             * "failed to start" was the same five words for a unit pointing at
             * a path that does not exist, a binary a hardening script had
             * taken the execute bit off, a library at the wrong version, and a
             * daemon that read its config and gave up -- four different
             * afternoons behind one sentence, and the last line the console
             * prints is the line a player reads first. The kernel already
             * distinguishes them and hands back the reason; there was simply
             * nothing here that looked at it. */
            const char *why = ": failed to start";
            if (rc == SPAWN_ENOENT)       why = ": not found";
            else if (rc == SPAWN_EPERM)   why = ": present, and not executable";
            else if (rc == SPAWN_ENOEXEC) why = ": will not load -- check `ldd` on it";
            else if (rc == SPAWN_EFAULT)  why = ": started and would not stay up";
            if (bad) {
                g_puts("svcinit: ");
                g_puts(names[u]);
                g_puts(": ");
                g_puts(execs[u]);
                g_puts(why);
                if (g_streq(crit[u], "yes")) {
                    g_putln("  [critical]");
                    failed_critical = 1;
                    g_exit(1);
                }
                g_putln("  [degraded, continuing]");
                started[u] = 1;
                moved = 1;
                continue;
            }
            g_puts("svcinit: started ");
            g_puts(names[u]);
            if (descs[u][0]) { g_puts(" -- "); g_puts(descs[u]); }
            g_puts("\n");
            started[u] = 1;
            moved = 1;
        }
        if (!moved) break;
    }

    for (int u = 0; u < nunits; u++) {
        if (started[u]) continue;
        const char *dep = afters[u][0] ? afters[u] : "?";
        g_puts("svcinit: ");
        g_puts(names[u]);
        g_puts(": waiting for ");
        g_puts(dep);
        /* SAY WHY IT IS NEVER COMING. Waiting forever for a name is the
         * confusing half of this fault; the useful half is that the thing
         * being waited on was skipped on purpose, and this is where that is
         * known. */
        int said = 0;
        for (int k = 0; k < nskip && !said; k++)
            if (g_streq(skipname[k], dep)) {
                g_puts(" -- which never starts because ");
                g_puts(skipwhy[k]);
                said = 1;
            }
        if (!said) {
            int exists = 0;
            for (int v = 0; v < nunits; v++)
                if (g_streq(names[v], dep)) exists = 1;
            if (!exists) g_puts(" -- and no unit by that name is installed");
            else         g_puts(" -- which did not start either");
        }
        g_putln("");
        if (g_streq(crit[u], "yes")) g_exit(1);
    }
    g_exit(failed_critical ? 1 : 0);
}

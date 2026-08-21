/* /usr/bin/nomde — the display server.
 *
 * It was a one-line stub that never ran, because nothing on this system was
 * ever at runlevel 5. That made the graphical stack the one part of the
 * machine you could not break, which is exactly backwards: David wants a
 * desktop you can debug the way you debug a broken X11 session, and you
 * cannot debug something that was never running.
 *
 * So it is a real daemon with real dependencies, and every one of them is a
 * genuine failure mode:
 *
 *   /etc/nomde/nomde.conf        where the socket and the app registry live
 *   /usr/share/applications      the .desktop entries; empty means no menu
 *   /run/nomde/requests          the socket clients write to
 *
 * It publishes what it actually loaded to /run/nomde.state, like every other
 * daemon here, so `svc status nomde` and the file can disagree when somebody
 * edits the config and does not reload.
 */
#include "gsys.h"

static char conf[2048];
static char nm[160];

/* WHAT THIS PROCESS ACTUALLY LOADED. Two lines, the shape every daemon here
 * uses: which file was read, and what its first real line said. The kernel
 * compares the second against the file named by the first, which is how
 * "running with a stale configuration" becomes something the machine can
 * notice -- and, once it has been reloaded, stop noticing.
 *
 * THIS USED TO BE WRITTEN ONCE, AT STARTUP, AND THE HUP HANDLER ONLY RE-READ
 * THE FILE INTO MEMORY. So the one repair the previous administrator's notes,
 * a wiki page and a blog post all name -- `kill -HUP` the pid -- did nothing
 * anybody could see on this daemon: the process had genuinely reloaded and
 * the machine went on reporting it as stale, for ever. Every other daemon on
 * the disk republishes; this one is the display server, which is exactly the
 * one a player is most likely to be pointed at. */
static int publish(void)
{
    int sfd = g_open("/run/nomde.state", O_WRONLY | O_CREAT | O_TRUNC);
    if (sfd < 0) return -1;
    sysc(SYS_write, sfd, (i64)"/etc/nomde/nomde.conf\n", 22);
    {
        char *q = conf;
        while (*q) {
            char *nl = q; while (*nl && *nl != '\n') nl++;
            char save = *nl; *nl = 0;
            char *t = g_trim(q);
            if (*t && *t != '#') {
                sysc(SYS_write, sfd, (i64)t, (i64)g_strlen(t));
                *nl = save;
                break;
            }
            *nl = save; q = *nl ? nl + 1 : nl;
        }
    }
    sysc(SYS_write, sfd, (i64)"\n", 1);
    g_close(sfd);
    return 0;
}

void _start(void)
{
    if (g_slurp("/etc/nomde/nomde.conf", conf, sizeof conf) < 0) {
        g_putln("nomde: /etc/nomde/nomde.conf: cannot read -- refusing to start");
        g_exit(1);
    }
    if (!g_contains(conf, "applications")) {
        g_putln("nomde: /etc/nomde/nomde.conf: no applications directory "
                "configured -- refusing to start");
        g_exit(1);
    }

    /* Count the .desktop entries. A desktop with no applications is running
     * but useless, and saying so is more helpful than an empty menu. */
    int apps = 0;
    for (int i = 0; i < 128; i++) {
        if (g_readdir("/usr/share/applications", i, nm) < 0) break;
        if (g_endswith(nm, ".desktop")) apps++;
    }
    if (!apps)
        g_putln("nomde: warning: no .desktop entries in /usr/share/applications");

    /* The socket. Clients append a name; the desktop reads and clears it. */
    int fd = g_open("/run/nomde/requests", O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        g_putln("nomde: /run/nomde/requests: cannot create -- refusing to start");
        g_putln("  is /run there, and is the root filesystem writable?");
        g_exit(1);
    }
    g_close(fd);

    if (publish() < 0) {
        g_putln("nomde: /run/nomde.state: cannot write state -- refusing to start");
        g_exit(1);
    }

    g_puts("nomde: display server up, ");
    g_putn(apps);
    g_putln(" application(s) registered");

    for (;;) {
        if (g_sigpend() == SIG_HUP) {
            /* re-read on HUP, like anything else that keeps config in RAM,
             * and SAY SO: a reload nobody can observe is not a repair. */
            if (g_slurp("/etc/nomde/nomde.conf", conf, sizeof conf) >= 0)
                publish();
        }
    }
}

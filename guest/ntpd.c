/* /usr/sbin/ntpd — the time daemon.
 *
 * Reads its configuration at startup and then stays running. If the config is
 * missing it says so and exits, which is a service failing to start rather
 * than a service that was never there -- a different fault with a different
 * fix.
 */
#include "gsys.h"
static char conf[2048];
static const char *CONF[] = { "/etc/ntp.conf", 0 };

static void publish(void)
{
    /* the first non-comment line of the config, as loaded */
    static char state[256];
    state[0] = 0;
    char *q = conf;
    while (*q) {
        char *nl = q; while (*nl && *nl != '\n') nl++;
        char save = *nl; *nl = 0;
        char *t = g_trim(q);
        if (*t && *t != '#') { g_copy(state, t, sizeof state); *nl = save; break; }
        *nl = save; q = *nl ? nl + 1 : nl;
    }
    /* Two lines: which file was loaded, and what it said. The kernel compares
     * the second against the file named by the first, which is how "running
     * with a stale configuration" becomes a state the machine can notice
     * rather than a thing only a person could spot. */
    int fd = g_open("/run/ntpd.state", O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        /* A daemon that cannot write its state file is not running properly,
         * whatever it thinks. This used to return quietly, which meant that
         * deleting /run -- something a careless cleanup really does -- left
         * every service reporting itself healthy while the machine had no
         * idea what any of them had loaded. Silence here is how a fault stops
         * being a fault. */
        g_puts("ntpd: ");
        g_puts("/run/ntpd.state");
        g_putln(": cannot write state -- refusing to start");
        g_exit(1);
    }
    sysc(SYS_write, fd, (i64)CONF[0], (i64)g_strlen(CONF[0]));
    sysc(SYS_write, fd, (i64)"\n", 1);
    sysc(SYS_write, fd, (i64)state, (i64)g_strlen(state));
    sysc(SYS_write, fd, (i64)"\n", 1);
    g_close(fd);
}

static const char *KEY = "server";
void _start(void)
{
    for (int i = 0; CONF[i]; i++) {
        if (g_slurp(CONF[i], conf, sizeof conf) < 0) {
            g_puts("ntpd: ");
            g_puts(CONF[i]);
            g_putln(": cannot read -- refusing to start");
            g_exit(1);
        }
    }
    /* The config is there. Is it USABLE? A file that exists and does not say
     * the one thing this daemon needs is a completely different fault from a
     * file that is missing, and it fails later and less obviously. */
    {
        int ok = 0;
        char *q = conf;
        while (*q) {
            char *nl = q; while (*nl && *nl != '\n') nl++;
            char save = *nl; *nl = 0;
            char *t = g_trim(q);
            if (*t && *t != '#') {
                u64 k = 0;
                while (KEY[k] && t[k] == KEY[k]) k++;
                if (!KEY[k]) ok = 1;      /* empty KEY: any real line will do */
            }
            *nl = save; q = *nl ? nl + 1 : nl;
            if (ok) break;
        }
        if (!ok) {
            g_puts("ntpd: ");
            g_puts(CONF[0]);
            g_putln(": no time server configured -- refusing to start");
            g_exit(1);
        }
    }

    /* THE DRIFT FILE IS A FILE, and a time daemon that cannot keep its drift
     * loses its calibration at every restart, so it says so rather than
     * pretending. This is what makes /var/lib/ntp a directory worth owning:
     * before it, deleting it cost nothing and the package manifest was
     * describing something no program ever touched. */
    {
        static char df[192];
        df[0] = 0;
        char *q = conf;
        while (*q && !df[0]) {
            char *nl = q; while (*nl && *nl != '\n') nl++;
            char save = *nl; *nl = 0;
            char *t = g_trim(q);
            if (t[0] == 'd' && g_contains(t, "driftfile")) {
                char *v = t + 9;
                while (*v == ' ' || *v == '\t' || *v == '=') v++;
                g_copy(df, v, sizeof df);
            }
            *nl = save; q = *nl ? nl + 1 : nl;
        }
        if (df[0]) {
            int fd = g_open(df, O_WRONLY | O_CREAT | O_TRUNC);
            if (fd < 0) {
                g_puts("ntpd: ");
                g_puts(df);
                g_putln(": cannot write the drift file -- refusing to start");
                g_exit(1);
            }
            const char *z = "0.000\n";
            sysc(SYS_write, fd, (i64)z, (i64)g_strlen(z));
            g_close(fd);
        }
    }

    /* Publish what was actually loaded. The file on disk says what the
     * machine is SUPPOSED to do; this says what the running process is
     * actually doing, and the two drift the moment somebody edits a config
     * and does not reload. That gap is invisible without this. */
    publish();

    /* Up. A daemon spends its life here, looking occasionally to see whether
     * anyone has asked it to re-read its configuration. */
    for (;;) {
        if (g_sigpend() == SIG_HUP) {
            if (g_slurp(CONF[0], conf, sizeof conf) >= 0) publish();
        }
    }
}

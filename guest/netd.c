/* /usr/sbin/netd — the network daemon.
 *
 * Reads its configuration at startup and then stays running. If the config is
 * missing it says so and exits, which is a service failing to start rather
 * than a service that was never there -- a different fault with a different
 * fix.
 */
#include "gsys.h"
static char conf[2048];
static const char *CONF[] = { "/etc/net/interfaces", 0 };

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
    int fd = g_open("/run/netd.state", O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        /* A daemon that cannot write its state file is not running properly,
         * whatever it thinks. This used to return quietly, which meant that
         * deleting /run -- something a careless cleanup really does -- left
         * every service reporting itself healthy while the machine had no
         * idea what any of them had loaded. Silence here is how a fault stops
         * being a fault. */
        g_puts("netd: ");
        g_puts("/run/netd.state");
        g_putln(": cannot write state -- refusing to start");
        g_exit(1);
    }
    sysc(SYS_write, fd, (i64)CONF[0], (i64)g_strlen(CONF[0]));
    sysc(SYS_write, fd, (i64)"\n", 1);
    sysc(SYS_write, fd, (i64)state, (i64)g_strlen(state));
    sysc(SYS_write, fd, (i64)"\n", 1);
    g_close(fd);
}

/* IS THIS A SOCKET THE MACHINE REALLY HAS, AND NOT THE FIRST ONE?
 *
 * The kernel found the cards; udev's one rule names the first of them. So a
 * config naming the second socket on the back of a two-socket server is
 * describing something real, while a config whose FIRST card has been renamed
 * -- the fault this whole check is here to catch -- always names the device at
 * the head of the kernel's own list, which this refuses. Subinterfaces are
 * skipped: a card is what has no dot in it. */
static int later_socket(const char *card)
{
    static char ifs[8192];
    if (!card[0]) return 0;
    i64 n = g_netinfo(NETINFO_IFACE, ifs, sizeof ifs);
    if (n <= 0) return 0;
    ifs[n < (i64)sizeof ifs ? n : (i64)sizeof ifs - 1] = 0;
    int idx = 0;
    char *q = ifs;
    while (*q) {
        char *nl = q; while (*nl && *nl != '\n') nl++;
        char save = *nl; *nl = 0;
        if (*q != ' ' && *q != '\t') {          /* a device line, not a detail */
            static char nm[64];
            u64 k = 0;
            while (q[k] && q[k] != ':' && q[k] != ' ' && k < sizeof nm - 1) {
                nm[k] = q[k]; k++;
            }
            nm[k] = 0;
            if (q[k] == ':' && !g_contains(nm, ".")) {
                if (idx > 0 && g_streq(nm, card)) { *nl = save; return 1; }
                idx++;
            }
        }
        *nl = save; q = *nl ? nl + 1 : nl;
    }
    return 0;
}

static const char *KEY = "iface";
void _start(void)
{
    for (int i = 0; CONF[i]; i++) {
        if (g_slurp(CONF[i], conf, sizeof conf) < 0) {
            g_puts("netd: ");
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
            g_puts("netd: ");
            g_puts(CONF[0]);
            g_putln(": no interface is configured -- refusing to start");
            g_exit(1);
        }
    }

    /* DOES THE DEVICE EXIST? The config names an interface; udev is what
     * decides what interfaces are called. Configuring eth0 on a machine where
     * udev has named the device something else fails in a way that looks like
     * nothing at all is wrong -- both files are valid, both are what somebody
     * intended, and they disagree.
     *
     * WHAT AN `iface` LINE NAMES IS NOT ALWAYS A CARD, and reading it as if it
     * always were is what made every floor server built the way D27 recommends
     * a landmine armed for its next reboot. `iface eth0.12` names a TAGGED
     * SUBINTERFACE of eth0: writing it down is what says the card underneath
     * carries vlan 12, so naming it names eth0 as surely as `iface eth0` does.
     * A server addressed only on vlans -- the whole point of a per-floor vlan
     * server -- has no `iface eth0` stanza at all, and this refused to start on
     * it, on a box with a clean `pkg verify` and nothing whatever wrong. So the
     * card is the part before the dot.
     *
     * AND A SECOND SOCKET IS NOT A WRONG NAME EITHER. udev's one net rule
     * names this machine's FIRST network device; a box with two sockets in the
     * back really has an eth1, the kernel found it, and a config that names it
     * is describing a card that is there. So a name that is not the one udev
     * gives is still refused -- that is the fault this check exists for, and a
     * config edited to name a card the box does not have has to fail loudly --
     * unless the kernel itself is holding a device by that name and it is not
     * the first one, which no rename can produce and only a real second socket
     * can. */
    {
        static char rules[2048], want[64], card[64];
        want[0] = 0;
        /* the name from our own config: "iface eth0" */
        {
            char *q = conf;
            while (*q) {
                char *nl = q; while (*nl && *nl != '\n') nl++;
                char save = *nl; *nl = 0;
                char *t = g_trim(q);
                if (t[0] == 'i' && t[1] == 'f' && t[2] == 'a' && t[3] == 'c' &&
                    t[4] == 'e' && (t[5] == ' ' || t[5] == '\t')) {
                    char *w = t + 5;
                    while (*w == ' ' || *w == '\t') w++;
                    u64 k = 0;
                    while (w[k] && w[k] != ' ' && w[k] != '\t' && k < sizeof want - 1) {
                        want[k] = w[k]; k++;
                    }
                    want[k] = 0;
                }
                *nl = save; q = *nl ? nl + 1 : nl;
                if (want[0]) break;
            }
        }
        /* the card underneath it: eth0.12 rides on eth0, eth0 is itself */
        {
            u64 k = 0;
            while (want[k] && want[k] != '.' && k < sizeof card - 1) {
                card[k] = want[k]; k++;
            }
            card[k] = 0;
        }
        if (want[0] &&
            g_slurp("/etc/udev/rules.d/50-default.rules", rules, sizeof rules) >= 0) {
            /* NAME="..." on the net rule */
            static char named[64];
            named[0] = 0;
            char *q = rules;
            while (*q) {
                char *nl = q; while (*nl && *nl != '\n') nl++;
                char save = *nl; *nl = 0;
                char *t = g_trim(q);
                if (g_contains(t, "net") && g_contains(t, "NAME=")) {
                    char *n2 = t;
                    while (*n2 && !(n2[0] == 'N' && n2[1] == 'A' && n2[2] == 'M' &&
                                    n2[3] == 'E' && n2[4] == '=')) n2++;
                    if (*n2) {
                        n2 += 5;
                        if (*n2 == '"') n2++;
                        u64 k = 0;
                        while (n2[k] && n2[k] != '"' && k < sizeof named - 1) {
                            named[k] = n2[k]; k++;
                        }
                        named[k] = 0;
                    }
                }
                *nl = save; q = *nl ? nl + 1 : nl;
                if (named[0]) break;
            }
            if (named[0] && !g_streq(named, card) && !later_socket(card)) {
                g_puts("netd: ");
                g_puts(CONF[0]);
                g_puts(": configures ");
                g_puts(want);
                if (!g_streq(want, card)) {
                    g_puts(" (a subinterface of ");
                    g_puts(card);
                    g_puts(")");
                }
                g_puts(", but udev names this machine's network device ");
                g_puts(named);
                g_putln("");
                g_putln("  (see /etc/udev/rules.d/50-default.rules)");
                g_putln("  refusing to start: there is no such interface");
                g_exit(1);
            }
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

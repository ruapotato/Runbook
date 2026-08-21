/* /bin/arp — the neighbour cache, and the one repair you can make to it.
 *
 * WHY IT IS WORTH A PROGRAM OF ITS OWN when `ip neigh` prints the same
 * table: because the ARP cache is where three different faults show up as
 * three different lines, and the columns here are the ones that say which.
 *
 *   an entry with a mac          that neighbour answered. The wire between
 *                                you and it works, whatever else does not.
 *   (incomplete)                 you asked and nothing answered. The address
 *                                is on this wire and no card holds it: a
 *                                typo in an address or a mask, or a machine
 *                                that is off.
 *   the WRONG mac                two machines answered for one address, and
 *                                the cache believes whichever spoke last.
 *                                That is a duplicate address, and it is the
 *                                one fault that looks like nothing at all
 *                                until you compare this table with the other
 *                                machine's.
 *
 * THE INTERFACE COLUMN IS NOT DECORATION. The kernel records which card an
 * entry was learned on, at the moment it learned it. On a router with four
 * of them, an address answering on the wrong card is a cable in the wrong
 * port, and no other tool on this machine will tell you.
 *
 * arp -d IS A REAL DELETE. It removes that entry from the running cache, so
 * the next packet to that address asks again. That is the repair after
 * somebody swaps a machine and the old MAC is still being used -- and if
 * there was no such entry it says so, rather than reporting a success it
 * did not have.
 */
#include "gsys.h"

static char buf[16384];
static char arg[512];

static char *line_next(char *s)
{
    while (*s && *s != '\n') s++;
    if (!*s) return s;
    *s++ = 0;
    return s;
}

static int words(char *s, char **v, int max)
{
    int n = 0;
    while (*s && n < max) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;
        v[n++] = s;
        while (*s && *s != ' ' && *s != '\t') s++;
        if (*s) *s++ = 0;
    }
    return n;
}

static void pad(const char *s, int w)
{
    g_puts(s);
    int n = (int)g_strlen(s);
    for (int i = n; i < w; i++) g_puts(" ");
}

/* One pass over the cache. `only` is an address to show alone, or 0. */
static int table(const char *only, int bsd)
{
    i64 n = g_netinfo(NETINFO_ARP, buf, sizeof buf);
    if (n < 0) { g_putln("arp: the kernel has no network state to report"); g_exit(1); }
    /* NO CARD AT ALL. The kernel says so in one line rather than an empty
     * list, and a parser that treated it as data would print nonsense at
     * exactly the moment a player most needs a straight answer. */
    if (g_contains(buf, "no network interface")) {
        g_putln("this machine has no network card in the world at all.");
        g_exit(1);
    }
    if (n == 0) {
        g_putln("(the arp cache is empty -- nothing on this wire has answered yet)");
        g_putln("  it fills when this machine talks to a neighbour: try `ping`.");
        return 0;
    }
    int shown = 0;
    int header = 0;
    char *s = buf;
    while (*s) {
        char *l = s;
        s = line_next(s);
        if (!*l) continue;
        char *v[10];
        int w = words(l, v, 10);
        if (w < 2) continue;
        if (only && !g_streq(v[0], only)) continue;
        const char *dev = "?", *age = "?";
        for (int i = 0; i < w; i++) {
            if (g_streq(v[i], "dev") && i + 1 < w) dev = v[i + 1];
            if (g_streq(v[i], "age") && i + 1 < w) age = v[i + 1];
        }
        int incomplete = g_streq(v[1], "(incomplete)");
        if (bsd) {
            /* The BSD spelling, which is what `arp -a` has always printed.
             * The name is `?` because nothing here does a reverse lookup:
             * there is no PTR zone on this network and inventing a name
             * would be the one dishonest character in the line. */
            g_puts("? ("); g_puts(v[0]); g_puts(") ");
            if (incomplete) g_puts("-- no entry (a request is out, unanswered)");
            else { g_puts("at "); g_puts(v[1]); g_puts(" [ether]"); }
            g_puts(" on "); g_putln(dev);
        } else {
            if (!header) {
                g_putln("ADDRESS          HWTYPE  HWADDRESS          FLAGS  IFACE   AGE");
                header = 1;
            }
            pad(v[0], 17);
            pad(incomplete ? "" : "ether", 8);
            pad(incomplete ? "(incomplete)" : v[1], 19);
            pad(incomplete ? "" : "C", 7);
            pad(dev, 8);
            g_putln(age);
        }
        shown++;
    }
    return shown;
}

static void usage(void)
{
    g_putln("usage: arp [-a] [-n] [<address>]      the cache");
    g_putln("       arp -d <address>               forget one neighbour");
    g_putln("");
    g_putln("-a is the BSD one-line-per-entry spelling. -n is numeric, which");
    g_putln("is the only thing this arp does: nothing here resolves a name");
    g_putln("backwards. man arp.");
}

void _start(void)
{
    arg[0] = 0;
    g_getarg(arg, sizeof arg);
    char *v[GARGS];
    int n = g_argv(arg, v);

    int bsd = 0;
    const char *del = 0, *one = 0;
    for (int i = 0; i < n; i++) {
        if (g_streq(v[i], "-a")) { bsd = 1; continue; }
        if (g_streq(v[i], "-n")) { continue; }   /* always numeric anyway */
        if (g_streq(v[i], "-d")) {
            if (i + 1 >= n) { g_putln("arp: -d wants an address"); g_exit(2); }
            del = v[++i];
            continue;
        }
        if (v[i][0] == '-') {
            g_puts("arp: no such option: "); g_putln(v[i]);
            usage();
            g_exit(2);
        }
        if (!one) one = v[i];
    }

    if (del) {
        /* The kernel parses the address, because the kernel is what owns
         * the cache: an address this program accepted and the stack did not
         * would delete nothing and report a success. */
        if (g_netctl(NETCTL_ARPDEL, (i64)del, 0) < 0) {
            g_puts("arp: no entry deleted for "); g_putln(del);
            g_putln("  either nothing is cached for it, or that is not an address.");
            g_putln("  `arp` for what the cache holds.");
            g_exit(1);
        }
        g_puts("deleted "); g_putln(del);
        g_putln("  the next packet to it will ask again.");
        g_exit(0);
    }

    int shown = table(one, bsd);
    if (one && !shown) {
        g_puts("arp: nothing cached for "); g_putln(one);
        g_putln("  this machine has not spoken to it, or it never answered.");
        g_exit(1);
    }
    g_exit(0);
}

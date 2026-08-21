/* /bin/ip — the modern one, because that is what a person types now.
 *
 * WHAT IT IS. Four questions, in the shape iproute2 answers them:
 *
 *   ip addr     the addresses and masks the cards really hold
 *   ip link     the cards themselves: mac, admin state, carrier
 *   ip route    the routing table the kernel really looks in
 *   ip neigh    the ARP cache: who has really answered
 *
 * EVERY LINE COMES OUT OF THE RUNNING STACK. Nothing here reads
 * /etc/net/interfaces. That file is what somebody INTENDED; this is what the
 * machine has, and the gap between the two is a whole class of fault --
 * netd not running, a daemon that never reloaded, a dhcp lease that never
 * arrived. A tool that read the config would paper over exactly the faults
 * it exists to find.
 *
 * WHAT IT WILL NOT DO, and says so rather than pretending: it does not
 * CONFIGURE. There is no `ip addr add`, no `ip link set`, no `ip route add`.
 * The address on a card here comes from the interfaces file by way of netd,
 * and a command that appeared to set one and was quietly undone the next
 * time the network daemon looked at the disk would be worse than no command
 * at all. Edit the file and reload the daemon; that is the real repair on
 * this machine, and `ip` is how you check it landed.
 */
#include "gsys.h"

static char buf[16384];
static char arg[512];

/* ---------------------------------------------------------------- text */
/* Cut `s` at the next newline and return the start of the line after it.
 * Nothing is copied: these programs have no allocator, so parsing is done
 * in place on the buffer the kernel filled. */
static char *line_next(char *s)
{
    while (*s && *s != '\n') s++;
    if (!*s) return s;
    *s++ = 0;
    return s;
}

/* Split a line into words, in place. Returns how many. */
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

static int starts(const char *s, const char *p)
{
    while (*p) { if (*s++ != *p++) return 0; }
    return 1;
}

/* ------------------------------------------------------------- ip addr */
/* The kernel hands us its own shape:
 *
 *   eth0: UP LOWER_UP link/ether 52:54:00:00:00:04
 *       inet 10.0.2.15/24
 *       vlan 10
 *       RX 12  TX 14  dropped 0
 *
 * and this is the reformat into the one iproute2 prints. The flags in the
 * angle brackets are the two the stack really has -- the admin state of the
 * interface and the carrier on the port -- and no others are invented.
 *
 * MTU. 1500 is printed because it is true of this stack: if_tx() refuses a
 * payload longer than 1500 bytes, everywhere, and there is no per-interface
 * MTU to set. It is a constant of the machine, not a per-card setting, and
 * the man page says so.
 */
static void show(int link_only)
{
    i64 n = g_netinfo(NETINFO_IFACE, buf, sizeof buf);
    if (n < 0) { g_putln("ip: the kernel has no network state to report"); g_exit(1); }
    /* NO CARD AT ALL. The kernel says so in one line rather than an empty
     * list, and a parser that treated it as data would print nonsense at
     * exactly the moment a player most needs a straight answer. */
    if (g_contains(buf, "no network interface")) {
        g_putln("this machine has no network card in the world at all.");
        g_exit(1);
    }
    if (n == 0) {
        g_putln("(no interface is configured -- this machine is on no network)");
        g_putln("  `svc status net` for the daemon that configures it.");
        return;
    }
    int idx = 0;
    char *s = buf;
    while (*s) {
        char *l = s;
        s = line_next(s);
        if (!*l) continue;
        if (l[0] == ' ') {
            /* A continuation line: inet, vlan or the counters. */
            char *v[8];
            char tmp[128];
            g_copy(tmp, l, sizeof tmp);
            int w = words(tmp, v, 8);
            if (!w) continue;
            if (g_streq(v[0], "inet")) {
                if (link_only) continue;
                if (g_streq(v[1], "none")) {
                    g_putln("    inet none -- this card has no address");
                } else {
                    g_puts("    inet "); g_puts(v[1]); g_putln(" scope global");
                }
                continue;
            }
            if (g_streq(v[0], "vlan")) {
                g_puts("    vlan id "); g_putln(v[1]);
                continue;
            }
            if (g_streq(v[0], "RX")) {
                if (link_only) continue;
                g_puts("    RX packets "); g_puts(v[1]);
                g_puts("  TX packets "); g_puts(v[3]);
                g_puts("  dropped "); g_putln(v[5]);
                continue;
            }
            continue;
        }
        /* A header line. "eth0:" then the states then link/ether then a mac. */
        char *v[8];
        int w = words(l, v, 8);
        if (w < 2) continue;
        idx++;
        g_putn(idx); g_puts(": "); g_puts(v[0]); g_puts(" <");
        g_puts(v[1]);                       /* UP or DOWN, the admin state */
        if (w > 2 && !g_streq(v[2], "link/ether")) {
            g_puts(","); g_puts(v[2]);      /* LOWER_UP, NO-CARRIER, DOWN  */
        }
        g_putln("> mtu 1500");
        /* The mac is whatever followed link/ether, and it is the card's. */
        for (int i = 2; i + 1 < w; i++)
            if (g_streq(v[i], "link/ether")) {
                g_puts("    link/ether "); g_putln(v[i + 1]);
                break;
            }
    }
}

/* ------------------------------------------------------------ ip route */
static void route(void)
{
    i64 n = g_netinfo(NETINFO_ROUTE, buf, sizeof buf);
    if (n <= 0) {
        g_putln("(the routing table is empty -- this machine cannot send anywhere)");
        return;
    }
    /* The kernel already prints these in iproute2's own syntax, so there is
     * nothing to reformat and nothing to add. Passing the text through
     * untouched is the honest thing: every route here is one the stack
     * really looks in. */
    g_puts(buf);
    if (buf[n - 1] != '\n') g_putln("");
}

/* ------------------------------------------------------------ ip neigh */
/* The cache, in `ip neigh` shape. The state at the end is not decoration:
 * the stack re-sends an ARP request for any entry older than 120 seconds
 * before it uses it, so REACHABLE and STALE are the two things an entry can
 * really be to this machine, and the age it is derived from is the kernel's.
 */
static void neigh(void)
{
    i64 n = g_netinfo(NETINFO_ARP, buf, sizeof buf);
    if (n <= 0) {
        g_putln("(the arp cache is empty -- nothing on this wire has answered yet)");
        return;
    }
    char *s = buf;
    while (*s) {
        char *l = s;
        s = line_next(s);
        if (!*l) continue;
        /* ip  mac|(incomplete)  [ether]  dev  NAME  age  Ns */
        char *v[10];
        int w = words(l, v, 10);
        if (w < 2) continue;
        const char *dev = "?";
        i64 age = -1;
        for (int i = 0; i < w; i++) {
            if (g_streq(v[i], "dev") && i + 1 < w) dev = v[i + 1];
            if (g_streq(v[i], "age") && i + 1 < w) {
                char t[16];
                g_copy(t, v[i + 1], sizeof t);
                for (int k = 0; t[k]; k++) if (t[k] == 's') t[k] = 0;
                if (!g_num(t, &age)) age = -1;
            }
        }
        g_puts(v[0]); g_puts(" dev "); g_puts(dev);
        if (g_streq(v[1], "(incomplete)")) {
            g_putln("  INCOMPLETE");
        } else {
            g_puts(" lladdr "); g_puts(v[1]);
            g_putln(age >= 0 && age > 120 ? "  STALE" : "  REACHABLE");
        }
    }
}

static void usage(void)
{
    g_putln("usage: ip { addr | link | route | neigh } [show]");
    g_putln("  addr    the addresses and masks the cards really hold");
    g_putln("  link    the cards: mac, admin state, carrier");
    g_putln("  route   the table the kernel really looks in");
    g_putln("  neigh   the arp cache: who has really answered");
    g_putln("");
    g_putln("it SHOWS and does not CONFIGURE: there is no `ip addr add` on");
    g_putln("this machine. /etc/net/interfaces and `svc reload net` are the");
    g_putln("repair -- `ed /etc/net/interfaces ,n` prints it with line numbers");
    g_putln("and `man ed` is how to change one. This is how you check it");
    g_putln("landed. man ip.");
}

void _start(void)
{
    arg[0] = 0;
    g_getarg(arg, sizeof arg);
    char *v[GARGS];
    int n = g_argv(arg, v);
    if (!n) { usage(); g_exit(2); }

    /* iproute2 lets any unambiguous prefix stand for the object, and half
     * the muscle memory in the world is `ip a`. */
    const char *o = v[0];
    int show_only = 1;
    /* A verb after the object. `show` and `list` are the ones that read;
     * anything else would be a change, and this program does not make
     * changes -- so it says which, rather than ignoring the word. */
    /* THE VERB FIRST, THEN THE REST OF THE LINE. `ip addr add 10.0.0.1/24 dev
     * eth0` has to be answered as "this ip does not configure" -- complaining
     * about the third word instead sends the reader looking for a filter they
     * never asked for. */
    if (n > 1) {
        if (!g_streq(v[1], "show") && !g_streq(v[1], "list") && !g_streq(v[1], "sh")) {
            g_puts("ip: this ip only shows; it cannot `"); g_puts(v[1]);
            g_putln("`.");
            g_putln("  an address on this machine comes from /etc/net/interfaces");
            g_putln("  by way of netd:");
            g_putln("    ed /etc/net/interfaces ,n      what is in it now");
            g_putln("    ed /etc/net/interfaces 2c \"  address 10.0.2.50/24\" . w");
            g_putln("    svc reload net");
            g_putln("  then look again. `man ed` for the rest of it.");
            g_exit(2);
        }
        show_only = 1;
    }
    if (n > 2) {
        /* `ip addr show eth0` filters, and a filter this program does not
         * apply would print every card and look like it had applied one. */
        g_puts("ip: this ip has no filter: it cannot show only `");
        g_puts(v[2]); g_putln("`.");
        g_putln("  it prints every interface; read the one you want.");
        g_exit(2);
    }
    (void)show_only;

    if (g_streq(o, "a") || starts("address", o) || g_streq(o, "addr")) { show(0); g_exit(0); }
    if (g_streq(o, "l") || starts("link", o))    { show(1); g_exit(0); }
    if (g_streq(o, "r") || starts("route", o))   { route(); g_exit(0); }
    if (g_streq(o, "n") || starts("neighbour", o) || starts("neighbor", o) ||
        g_streq(o, "neigh"))                     { neigh(); g_exit(0); }

    g_puts("ip: no such object: "); g_putln(o);
    usage();
    g_exit(2);
}

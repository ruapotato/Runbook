/* /bin/tcpdump — the frames themselves, one line each.
 *
 * WHAT IT READS. The kernel keeps a ring of every frame that really crossed
 * one of this machine's cards, taken at the card, in both directions, with
 * the fields read out of the headers that were on the wire: the two MAC
 * addresses, the ethertype, the protocol, the addresses, the ports, the TCP
 * flags, the ICMP type, the length. Every filter below compares one of those
 * fields. Nothing here searches the text of a line, and nothing is derived
 * from a configuration file.
 *
 * IT IS NOT LIVE, AND THAT IS THE ONE DIFFERENCE FROM THE REAL PROGRAM.
 * Nothing on this machine runs while your shell waits, so there is nobody to
 * print a frame to as it arrives. The capture is a ring the stack fills as
 * frames pass; this prints what is in it. So the shape of a session is:
 *
 *     tcpdump --capture on          start the ring (it clears it)
 *     ping 10.0.2.2                 make the traffic
 *     tcpdump icmp                  read it back
 *
 * The ring is off until asked because a ring nobody reads is memory nobody
 * is paying for, and it holds the last 256 frames.
 *
 * IT SEES ONE MACHINE. The frames are this box's -- what its card sent and
 * what its card accepted. There is no promiscuous mode on this network, so a
 * frame addressed to somebody else was never handed up here and is not in
 * the ring. If you want the other end of a conversation, run tcpdump there
 * too; that is the real technique and it is the real answer.
 *
 * WHAT IT REFUSES. A filter it cannot apply is REFUSED BY NAME rather than
 * ignored, because a filter that is silently dropped turns "no packets
 * matched" into evidence of the wrong thing entirely.
 */
#include "gsys.h"

static char buf[32768];
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

/* The fields the kernel writes, in order. The comment in netstack.c that
 * builds them is the other half of this contract. */
enum { F_MS, F_IF, F_DIR, F_SMAC, F_DMAC, F_TYPE, F_PROTO, F_SRC, F_DST,
       F_SPORT, F_DPORT, F_LEN, F_INFO, F_COUNT };

static void usage(void)
{
    g_putln("usage: tcpdump [-i <iface>] [-c <count>] [-Q in|out] [filter ...]");
    g_putln("       tcpdump --capture on | off");
    g_putln("");
    g_putln("filters: arp  icmp  tcp  udp  ip  host <addr>  port <n>");
    g_putln("         `and` between them; every one must match. There is no");
    g_putln("         `or`, no `not`, and no src/dst -- see man tcpdump.");
}

void _start(void)
{
    arg[0] = 0;
    g_getarg(arg, sizeof arg);
    char *v[GARGS];
    int n = g_argv(arg, v);

    const char *want_if = 0, *want_proto = 0, *want_host = 0, *want_port = 0;
    const char *want_dir = 0;
    i64 count = 0;

    for (int i = 0; i < n; i++) {
        char *a = v[i];
        if (g_streq(a, "--capture")) {
            if (i + 1 >= n) { g_putln("tcpdump: --capture wants on or off"); g_exit(2); }
            if (g_streq(v[i + 1], "on")) {
                g_netctl(NETCTL_PCAP, 1, 0);
                g_putln("capturing. the ring is empty now and fills as frames pass;");
                g_putln("make the traffic, then `tcpdump` to read it back.");
                g_exit(0);
            }
            if (g_streq(v[i + 1], "off")) {
                g_netctl(NETCTL_PCAP, 0, 0);
                g_putln("stopped. what was captured is still there to read.");
                g_exit(0);
            }
            g_putln("tcpdump: --capture wants on or off");
            g_exit(2);
        }
        if (g_streq(a, "-i")) {
            if (i + 1 >= n) { g_putln("tcpdump: -i wants an interface"); g_exit(2); }
            want_if = v[++i];
            continue;
        }
        if (g_streq(a, "-c")) {
            if (i + 1 >= n || !g_num(v[i + 1], &count) || count < 1) {
                g_putln("tcpdump: -c wants a count of one or more");
                g_exit(2);
            }
            i++;
            continue;
        }
        if (g_streq(a, "-Q")) {
            if (i + 1 >= n) { g_putln("tcpdump: -Q wants in or out"); g_exit(2); }
            want_dir = v[++i];
            if (!g_streq(want_dir, "in") && !g_streq(want_dir, "out")) {
                g_putln("tcpdump: -Q wants in or out");
                g_exit(2);
            }
            continue;
        }
        if (a[0] == '-') {
            /* BY NAME. -w, -r, -X, -e, -v: every one of them is a real
             * tcpdump flag and none of them is implemented here, so saying
             * "no such option" would be a second lie on top of the first. */
            g_puts("tcpdump: this tcpdump has no "); g_putln(a);
            g_putln("  it has -i, -c, -Q and a small filter. There is no capture");
            g_putln("  file to write or read, and no hex dump: the kernel keeps");
            g_putln("  the fields, not the bytes. man tcpdump.");
            g_exit(2);
        }
        /* The filter expression. */
        if (g_streq(a, "and")) continue;
        if (g_streq(a, "arp") || g_streq(a, "icmp") || g_streq(a, "tcp") ||
            g_streq(a, "udp") || g_streq(a, "ip")) {
            if (want_proto && !g_streq(want_proto, a)) {
                g_putln("tcpdump: two protocols cannot both match: there is no `or`");
                g_exit(2);
            }
            want_proto = a;
            continue;
        }
        if (g_streq(a, "host")) {
            if (i + 1 >= n) { g_putln("tcpdump: host wants an address"); g_exit(2); }
            want_host = v[++i];
            continue;
        }
        if (g_streq(a, "port")) {
            if (i + 1 >= n) { g_putln("tcpdump: port wants a number"); g_exit(2); }
            want_port = v[++i];
            i64 dummy;
            if (!g_num(want_port, &dummy)) {
                g_puts("tcpdump: not a port number: "); g_putln(want_port);
                g_exit(2);
            }
            continue;
        }
        /* REFUSED BY NAME. src, dst, net, portrange, vlan, `not`, `or`: all
         * of them are real pcap syntax and none of them is here. */
        g_puts("tcpdump: this tcpdump cannot filter on `"); g_puts(a);
        g_putln("`.");
        usage();
        g_exit(2);
    }

    /* AN INTERFACE THIS MACHINE DOES NOT HAVE. Left to the filter, that
     * would print "0 frames" -- which reads as "the wire is quiet" and is
     * really "you named a card that is not in this box". */
    if (want_if) {
        char ifs[4096];
        i64 in = g_netinfo(NETINFO_IFACE, ifs, sizeof ifs);
        int found = 0;
        for (i64 k = 0; k < in; k++) {
            if (k && ifs[k - 1] != '\n') continue;
            u64 j = 0;
            while (want_if[j] && ifs[k + j] == want_if[j]) j++;
            if (!want_if[j] && ifs[k + j] == ':') { found = 1; break; }
        }
        if (!found) {
            g_puts("tcpdump: this machine has no interface called "); g_putln(want_if);
            g_putln("  `ip link` for the ones it has.");
            g_exit(2);
        }
    }

    i64 got = g_netinfo(NETINFO_PCAP, buf, sizeof buf);
    if (got < 0) { g_putln("tcpdump: the kernel has no network state to report"); g_exit(1); }
    /* NO CARD AT ALL. The kernel says so in one line rather than an empty
     * list, and a parser that treated it as data would print nonsense at
     * exactly the moment a player most needs a straight answer. */
    if (g_contains(buf, "no network interface")) {
        g_putln("this machine has no network card in the world at all.");
        g_exit(1);
    }
    if (got == 0) {
        g_putln("nothing captured.");
        g_putln("  the capture is off until you start it, and starting it clears");
        g_putln("  the ring: `tcpdump --capture on`, make the traffic, then read.");
        g_exit(1);
    }

    int shown = 0, seen = 0;
    char *s = buf;
    while (*s) {
        char *l = s;
        s = line_next(s);
        if (!*l) continue;
        char *f[F_COUNT + 4];
        int w = words(l, f, F_COUNT + 4);
        if (w < F_LEN + 1) continue;
        seen++;
        if (want_if && !g_streq(f[F_IF], want_if)) continue;
        if (want_dir && !g_streq(f[F_DIR], want_dir)) continue;
        if (want_proto) {
            if (g_streq(want_proto, "ip")) {
                if (!g_streq(f[F_TYPE], "ipv4")) continue;
            } else if (!g_streq(f[F_PROTO], want_proto)) continue;
        }
        if (want_host && !g_streq(f[F_SRC], want_host) && !g_streq(f[F_DST], want_host))
            continue;
        if (want_port && !g_streq(f[F_SPORT], want_port) &&
            !g_streq(f[F_DPORT], want_port)) continue;

        /* One line, in tcpdump's shape, out of the fields that were in the
         * header. The leading number is the stack's own clock in
         * milliseconds of wire time -- there is no wall clock down here. */
        g_puts(f[F_MS]); g_puts(" ");
        g_puts(f[F_IF]); g_puts(" ");
        g_puts(g_streq(f[F_DIR], "in") ? "In  " : "Out ");
        if (g_streq(f[F_PROTO], "arp")) {
            /* A REQUEST NAMES THE TARGET AND A REPLY NAMES THE SENDER, and
             * they are not the same address in the same column. Printing
             * both the same way would make a reply read as a question about
             * the machine that answered it. */
            if (w > F_INFO && g_streq(f[F_INFO], "is-at")) {
                g_puts("arp reply "); g_puts(f[F_SRC]);
                g_puts(" is-at "); g_puts(f[F_SMAC]);
            } else {
                g_puts("arp who-has "); g_puts(f[F_DST]);
                g_puts(" tell "); g_puts(f[F_SRC]);
            }
        } else if (g_streq(f[F_TYPE], "ipv4")) {
            g_puts("IP "); g_puts(f[F_SRC]);
            if (!g_streq(f[F_SPORT], "-")) { g_puts("."); g_puts(f[F_SPORT]); }
            g_puts(" > "); g_puts(f[F_DST]);
            if (!g_streq(f[F_DPORT], "-")) { g_puts("."); g_puts(f[F_DPORT]); }
            g_puts(": "); g_puts(f[F_PROTO]);
            if (w > F_INFO && !g_streq(f[F_INFO], "-")) { g_puts(" "); g_puts(f[F_INFO]); }
        } else {
            g_puts(f[F_SMAC]); g_puts(" > "); g_puts(f[F_DMAC]);
            g_puts(": ethertype "); g_puts(f[F_TYPE]);
        }
        g_puts(", length "); g_putln(f[F_LEN]);
        shown++;
        if (count && shown >= count) break;
    }

    g_putn(shown); g_puts(" frame"); g_puts(shown == 1 ? "" : "s");
    if (count && shown >= count) g_putln(" shown -- -c stopped it there");
    else { g_puts(" shown of "); g_putn(seen); g_putln(" captured"); }
    if (!shown && seen) g_putln("  nothing matched that filter. `tcpdump` alone shows all of them.");
    g_exit(0);
}

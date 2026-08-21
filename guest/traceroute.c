/* /bin/traceroute — the path, counted by TTL, from this machine.
 *
 * IT IS THE REAL MECHANISM. A probe goes out with a TTL of 1, and the first
 * router decrements it to zero and sends back an ICMP time-exceeded with its
 * own address as the source. Then 2, and the second router does. The list
 * below is the source address of each of those errors, in the order they
 * came back -- the stack produces them for anybody's packets, and this
 * program is only the thing that counts.
 *
 * WHY IT MATTERS HERE. `ping` tells you it did not work. This tells you HOW
 * FAR it got, which on a building with a router per floor is the difference
 * between a fault on your wire and a fault four floors up. The first hop
 * that stops answering is the first place worth walking to.
 *
 * WHAT IT DOES NOT PRINT, and why: no times. A hop here is a real exchange
 * over the real queue, but the stack times a probe in whole milliseconds of
 * wire time and does not hand a per-hop round trip back to a program. Three
 * columns of "1 ms" would look like a measurement and be a constant, so
 * there are none. `ping` measures, and says so.
 */
#include "gsys.h"

static char buf[4096];
static char arg[512];
static char host[128];
static char addr[64];

static int looks_like_name(const char *s)
{
    for (; *s; s++)
        if (!((*s >= '0' && *s <= '9') || *s == '.')) return 1;
    return 0;
}

/* The last address on the last line, so we can say whether the destination
 * itself is what finally answered. */
static void last_hop(char *out, u64 cap)
{
    out[0] = 0;
    i64 e = (i64)g_strlen(buf);
    while (e > 0 && (buf[e - 1] == '\n' || buf[e - 1] == ' ')) e--;
    if (e <= 0) return;
    i64 st = e;
    while (st > 0 && buf[st - 1] != '\n') st--;
    i64 p = st;
    while (p < e && buf[p] != ' ') p++;      /* the hop number */
    while (p < e && buf[p] == ' ') p++;
    u64 i = 0;
    while (p < e && i + 1 < cap) out[i++] = buf[p++];
    out[i] = 0;
}

/* The first line, copied out, so the one-word answers the kernel gives for
 * "it could not even start" can be compared without trimming the hop list
 * that the same buffer may be holding. */
static void first_line(char *out, u64 cap)
{
    u64 i = 0;
    while (buf[i] && buf[i] != '\n' && i + 1 < cap) { out[i] = buf[i]; i++; }
    out[i] = 0;
}

void _start(void)
{
    arg[0] = 0;
    g_getarg(arg, sizeof arg);
    char *v[GARGS];
    int n = g_argv(arg, v);

    const char *target = 0;
    for (int i = 0; i < n; i++) {
        if (v[i][0] == '-') {
            /* REFUSED BY NAME. -m would be a lie: the number of probes is
             * the stack's, not this program's, and an -m nobody honoured
             * would look like a setting. */
            g_puts("traceroute: no such option: "); g_putln(v[i]);
            g_putln("usage: traceroute <address or name>");
            g_putln("  twelve hops, always: that is what the kernel counts to.");
            g_exit(2);
        }
        if (!target) target = v[i];
    }
    if (!target) {
        g_putln("usage: traceroute <address or name>");
        g_putln("  the path, counted by ttl. `netstat -r` first if it says");
        g_putln("  there is no route: nothing is sent when there is nothing to try.");
        g_exit(2);
    }

    g_copy(host, target, sizeof host);
    g_copy(addr, target, sizeof addr);
    if (looks_like_name(host)) {
        /* Resolved separately, and reported separately, for the same reason
         * ping does it: a name that does not resolve is not a path that does
         * not work, and one repair is the resolver and the other is a route. */
        if (g_dns(host, addr, sizeof addr) < 0) {
            g_puts("traceroute: cannot resolve "); g_puts(host);
            g_putln(": no answer from the resolver");
            g_putln("  `cat /etc/resolv.conf` says which one it asked.");
            g_exit(2);
        }
    }

    g_puts("traceroute to "); g_puts(host);
    if (!g_streq(host, addr)) { g_puts(" ("); g_puts(addr); g_puts(")"); }
    g_putln(", 12 hops max");

    i64 r = g_traceroute(addr, buf, sizeof buf);
    if (r < 0) {
        g_putln("traceroute: the kernel could not run the trace");
        g_exit(1);
    }
    char head[32];
    first_line(head, sizeof head);
    if (g_streq(head, "ifdown")) {
        g_putln("traceroute: the interface it would go out of is down");
        g_putln("  nothing was sent. `ip link`, and `netstat -P` for the port.");
        g_exit(1);
    }
    if (g_streq(head, "badaddr")) {
        g_puts("traceroute: not an address: "); g_putln(addr);
        g_exit(2);
    }
    if (g_streq(head, "noroute")) {
        g_putln("traceroute: this machine has no route to it -- nothing was sent");
        g_putln("  `ip route` for the table it looked in.");
        g_exit(1);
    }

    g_puts(buf);
    if (r > 0 && buf[r - 1] != '\n') g_putln("");

    /* NOT ONE HOP ANSWERED. On this system the commonest reason is not the
     * path at all: a machine ships `policy drop`, and the ICMP that carries
     * every answer a traceroute has is dropped on the way back IN. Saying so
     * here is the difference between reading twelve stars as "the network is
     * gone" and as "this box is not listening to the answers". */
    int answered = 0;
    for (i64 k = 0; k < r; k++)
        if (buf[k] == ' ' && buf[k + 1] != '*') answered = 1;

    char last[64];
    last_hop(last, sizeof last);
    if (!answered) {
        g_putln("nothing answered at any ttl. before blaming the path, check");
        g_putln("  that THIS machine accepts icmp coming back: `netstat -F`.");
        g_putln("  a pristine box ships `policy drop` and hears none of it.");
    }
    if (!g_streq(last, addr)) {
        g_puts("the trace stops there: "); g_puts(addr);
        g_putln(" never answered.");
        g_putln("  a `*` is a hop that sent nothing back -- a filter, or a");
        g_putln("  router that is not there. The last address that DID answer");
        g_putln("  is the last place on the path that is working.");
        g_exit(1);
    }
    g_exit(0);
}

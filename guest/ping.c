/* /bin/ping — an echo request, down the wire, from this machine.
 *
 * WHY THIS EXISTS. Every other network tool on this system reads state:
 * netstat -i says what address the card has, -r what the routing table
 * holds, -A who has answered an ARP. All of that is what the machine
 * BELIEVES. This is the one program that makes it try, and it is the first
 * thing anybody types.
 *
 * IT DOES NOT FLATTEN THE FAILURES. The stack under it produces six distinct
 * answers and they are six different repairs:
 *
 *   reply                          it works
 *   no answer                      it left, nothing came back: a firewall
 *                                  dropping it, or a machine that is off
 *   destination net unreachable    a ROUTER on the path had no route -- so
 *                                  the fault is beyond your gateway
 *   destination host unreachable   the last hop got no ARP answer: the
 *                                  address is on a wire nobody holds it on
 *   time exceeded in transit       the ttl ran out, which is a routing loop
 *   network is unreachable         THIS machine has no route at all: the
 *                                  packet never left. `netstat -r`
 *   interface down                 no carrier, or no address. `netstat -P`
 *
 * "no route" and "host unreachable" both look like "it does not work" and
 * they are a wrong gateway and a wrong address respectively. Printing them
 * as one message would cost the player the diagnosis, so it does not.
 *
 * A NAME IS RESOLVED FIRST, and separately, so that "the name did not
 * resolve" is never reported as "the host did not answer" -- which is the
 * difference between a broken resolver and a broken route.
 */
#include "gsys.h"

static char arg[512];
static char host[128];
static char addr[64];

/* Something with a letter in it is a name; everything else is handed to the
 * kernel, which is what really decides whether it parses as an address. */
static int looks_like_name(const char *s)
{
    for (; *s; s++)
        if (!((*s >= '0' && *s <= '9') || *s == '.')) return 1;
    return 0;
}

static void code_line(int r)
{
    switch (r) {
    case NPING_TIMEOUT:
        g_putln("no answer -- it went out and nothing came back");
        break;
    case NPING_NET_UNREACH:
        g_putln("destination net unreachable -- a router on the path has no "
                "route for it");
        break;
    case NPING_HOST_UNREACH:
        g_putln("destination host unreachable -- the last hop got no arp "
                "answer for it");
        break;
    case NPING_TTL:
        g_putln("time exceeded in transit -- the ttl ran out, which is a "
                "routing loop");
        break;
    default:
        g_putln("?");
        break;
    }
}

void _start(void)
{
    arg[0] = 0;
    g_getarg(arg, sizeof arg);
    char *v[GARGS];
    int n = g_argv(arg, v);

    int count = 3;
    const char *target = 0;
    for (int i = 0; i < n; i++) {
        if (g_streq(v[i], "-c")) {
            i64 c = 0;
            if (i + 1 >= n || !g_num(v[i + 1], &c) || c < 1) {
                g_putln("ping: -c wants a count of one or more");
                g_exit(2);
            }
            count = (int)c;
            i++;
            continue;
        }
        if (v[i][0] == '-') {
            g_puts("ping: no such option: "); g_putln(v[i]);
            g_putln("usage: ping [-c count] <address or name>");
            g_exit(2);
        }
        if (!target) target = v[i];
    }
    if (!target) {
        g_putln("usage: ping [-c count] <address or name>");
        g_putln("  three echo requests by default. `netstat -r` says whether");
        g_putln("  this machine has a route to try in the first place.");
        g_exit(2);
    }

    g_copy(host, target, sizeof host);
    g_copy(addr, target, sizeof addr);
    if (looks_like_name(host)) {
        /* The resolver, for real, over UDP to whatever /etc/resolv.conf
         * names. A name that does not resolve is not a network that does not
         * work, and saying so is the whole reason this is a separate step. */
        if (g_dns(host, addr, sizeof addr) < 0) {
            g_puts("ping: cannot resolve "); g_puts(host);
            g_putln(": no answer from the resolver");
            g_putln("  `cat /etc/resolv.conf` says which one it asked.");
            g_exit(2);
        }
    }

    g_puts("PING "); g_puts(host);
    if (!g_streq(host, addr)) { g_puts(" ("); g_puts(addr); g_puts(")"); }
    g_putln(": 16 data bytes");

    int got = 0, sent = 0;
    for (int i = 0; i < count; i++) {
        int rtt = 0;
        int r = g_ping(addr, &rtt);
        /* NOTHING LEFT THE MACHINE. A local refusal is not a lost packet and
         * it does not get counted as one: there is no point sending the
         * other two, and the repair is on this box. */
        if (r == NPING_NO_ROUTE) {
            g_puts("ping: network is unreachable -- this machine has no route "
                   "to "); g_putln(addr);
            g_putln("  nothing was sent. `netstat -r` for the table it looked in.");
            g_exit(1);
        }
        if (r == NPING_IF_DOWN) {
            g_putln("ping: the interface it would go out of is down");
            g_putln("  nothing was sent. `netstat -i` for the address and the");
            g_putln("  carrier, `netstat -P` for the port itself.");
            g_exit(1);
        }
        sent++;
        g_puts("  seq="); g_putn(i);
        g_puts(" ");
        if (r == NPING_OK) {
            got++;
            g_puts("reply from "); g_puts(addr);
            g_puts(" time="); g_putn(rtt); g_putln(" ms");
        } else {
            code_line(r);
        }
    }

    g_puts("--- "); g_puts(host); g_putln(" ping statistics ---");
    g_putn(sent); g_puts(" sent, ");
    g_putn(got);  g_puts(" received, ");
    g_putn(sent ? (sent - got) * 100 / sent : 100);
    g_putln("% packet loss");
    g_exit(got ? 0 : 1);
}

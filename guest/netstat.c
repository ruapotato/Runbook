/* /bin/netstat — what the network is actually doing.
 *
 * DERIVED, NEVER DECLARED, and now derived from the network rather than from
 * the files that configure it. Every line below comes out of the running
 * stack: an address that was really assigned, a neighbour that really
 * answered an ARP request, a socket really sitting in that TCP state, a port
 * that really has a cable in it.
 *
 * That distinction is the whole reason this program is worth having. The old
 * version read /etc/net/interfaces and printed the address the file asked
 * for, which meant a machine whose network daemon had died reported an
 * address it did not have. It could not have been otherwise -- there was
 * nothing else to read. Now there is.
 *
 *   netstat        sockets: what is listening, and what is connected
 *   netstat -i     interfaces: address, mask, carrier, packet counts
 *   netstat -r     the routing table, connected routes included
 *   netstat -A     the ARP cache: who answered, and how long ago
 *   netstat -P     the physical port: link, speed, duplex, errors
 *   netstat -F     the running firewall rules, and what they have dropped
 *   netstat -w     the packet capture, one frame per line
 *   netstat -W     start capturing (it is off until you ask)
 *
 * If a player cannot see a frame they cannot diagnose a network, which is
 * what -w is for. It is off by default because a capture nobody asked for is
 * a ring buffer nobody is paying for.
 */
#include "gsys.h"

static char buf[16384];
static char arg[256];

static void show(int op, const char *empty)
{
    i64 n = g_netinfo(op, buf, sizeof buf);
    if (n < 0) { g_putln("netstat: the kernel has no network state to report"); return; }
    if (n == 0) { g_putln(empty); return; }
    g_puts(buf);
    if (buf[n - 1] != '\n') g_putln("");
}

void _start(void)
{
    arg[0] = 0;
    g_getarg_raw(arg, sizeof arg);
    char *t = g_trim(arg);

    if (g_streq(t, "-i")) {
        show(NETINFO_IFACE, "no interfaces are configured");
        g_exit(0);
    }
    if (g_streq(t, "-r")) {
        g_putln("DESTINATION      GATEWAY / DEVICE");
        show(NETINFO_ROUTE,
             "the routing table is empty -- this machine cannot send anywhere");
        g_exit(0);
    }
    if (g_streq(t, "-A")) {
        g_putln("ADDRESS          HARDWARE ADDRESS");
        show(NETINFO_ARP,
             "the arp cache is empty -- nothing on this wire has answered yet");
        g_exit(0);
    }
    if (g_streq(t, "-P")) {
        show(NETINFO_PORT, "this machine has no network port");
        g_exit(0);
    }
    if (g_streq(t, "-F")) {
        g_putln("CHAIN    PROTO MATCH        VERDICT");
        show(NETINFO_FW,
             "the filter is empty -- every packet is accepted");
        g_exit(0);
    }
    if (g_streq(t, "-W")) {
        g_netctl(NETCTL_TRACE, 1, 0);
        g_putln("capturing. `netstat -w` shows what has been seen since now.");
        g_exit(0);
    }
    if (g_streq(t, "-w")) {
        i64 n = g_netinfo(NETINFO_TRACE, buf, sizeof buf);
        if (n <= 0) {
            g_putln("nothing captured.");
            g_putln("  the capture is off until you start it: netstat -W");
            g_exit(0);
        }
        g_puts(buf);
        g_exit(0);
    }
    if (t[0] == '-') {
        g_puts("netstat: no such option: "); g_putln(t);
        g_putln("usage: netstat [-i interfaces | -r routes | -A arp |");
        g_putln("                -P port | -F firewall |");
        g_putln("                -W capture on | -w show capture]");
        g_exit(1);
    }

    /* No argument: the sockets. Real ones -- a service that died has no
     * socket here, and a service running with a stale configuration is
     * listening on the port it actually loaded, not the one the file now
     * says. That is the difference this program exists to show. */
    g_putln("PROTO  LOCAL ADDRESS         PEER                  STATE");
    i64 n = g_netinfo(NETINFO_SOCK, buf, sizeof buf);
    if (n <= 0)
        g_putln("(nothing is listening -- no network service has a socket open)");
    else
        g_puts(buf);

    g_putln("");
    g_putln("interface:");
    show(NETINFO_IFACE, "  none configured");
    g_exit(0);
}

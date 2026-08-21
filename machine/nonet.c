/* nonet.c — there is no network, and this file says so.
 *
 * kernel.c is lifted from NOMINAL unchanged, and NOMINAL's game had a network
 * in it: DNS, HTTP, ping, traceroute, a firewall, ARP. Its guest userland has
 * programs that call those syscalls, so the symbols have to exist or nothing
 * links.
 *
 * THEY MUST NOT WORK. Handoff §2, first line of the anti-goals: "No packet,
 * frame, ARP, VLAN, DHCP, or routing simulation. Not 'later.' Not
 * 'lightweight.' Cabling is not a mechanic here." Decision 4 explains why --
 * NOMINAL learned at cost that correct packets make a terrible game, so
 * everything in RUNBOOK is networked by default and depth lives in state
 * rather than routes.
 *
 * So: every network call fails, the same way, with the same sentence. A
 * player who types `ping` gets told this machine has no network stack rather
 * than a plausible-looking reply, because a plausible-looking reply is the
 * first step back down the road §2 was written to close.
 *
 * If somebody ever wants the network back, the right move is not to fill this
 * file in. It is to re-read decision 4.
 */
#include "nom.h"
#include "machine.h"

static const char *NO_NET =
    "this machine has no network stack -- everything here is reachable already\n";

bool netsite_dns(Machine *m, const char *name, char *out, size_t cap)
{
    (void)m; (void)name;
    if (cap) out[0] = 0;
    return false;
}

bool netsite_http(Machine *m, const char *ip, const char *path, Buf *out)
{
    (void)m; (void)ip; (void)path;
    if (out) buf_puts(out, NO_NET);
    return false;
}

void netsite_info(Machine *m, int op, Buf *out)
{
    (void)m; (void)op;
    if (out) buf_puts(out, NO_NET);
}

int netsite_ping(Machine *m, const char *dst, int *rtt)
{
    (void)m; (void)dst;
    if (rtt) *rtt = 0;
    return -1;
}

void netsite_fw_clear(Machine *m) { (void)m; }
void netsite_fw_add(Machine *m, int chain, int proto, int dport, int drop)
{ (void)m; (void)chain; (void)proto; (void)dport; (void)drop; }
void netsite_trace(Machine *m, int on) { (void)m; (void)on; }
void netsite_pcap(Machine *m, int on)  { (void)m; (void)on; }

void netsite_traceroute(Machine *m, const char *dst, Buf *out)
{
    (void)m; (void)dst;
    if (out) buf_puts(out, NO_NET);
}

int netsite_arp_del(Machine *m, const char *addr) { (void)m; (void)addr; return -1; }
void netsite_detach(Machine *m) { (void)m; }

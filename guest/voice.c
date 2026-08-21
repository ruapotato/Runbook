/* /bin/voice — what this machine's calls actually sounded like.
 *
 * WHY THIS EXISTS, in the words of the playtester who needed it and did not
 * have it. They sat down at a call centre agent's desk on a day the tenancy
 * scored nought out of eighteen calls with twenty-nine per cent of its audio
 * concealed, and typed everything a person types:
 *
 *   ping 198.51.100.1   3 of 3, 8 ms, 0% loss
 *   traceroute          a clean two hops
 *   ip addr             20,175 packets, 0 dropped
 *   netstat -P          the card's own counters, and nothing on them
 *
 * Every one of those answers was TRUE. The desk's own card really did drop
 * nothing: the audio was thrown away, or held past its turn to be played, on
 * a port somewhere else in the building. And the calls were over -- the busy
 * period ended hours ago, `ss` shows no sockets, and a live reading of a
 * stream that no longer exists is an empty screen.
 *
 * So this program reads the one piece of network state on this machine that
 * OUTLIVES the thing it measured: the record of the calls it has finished,
 * kept on the node the way an interface's tx/rx counters are kept. It is the
 * same measurement `service` reads from the landlord's side -- the same
 * concealment, off the same packets -- asked from the chair instead.
 *
 *   voice        the calls this machine has finished, and the verdict on the
 *                worst of them
 *   voice -l     the calls in progress at this instant. Usually none
 *
 * IT SPEAKS WHEN THE CALLS WERE FINE TOO. A tool that only prints when
 * something is wrong teaches you to ignore it when it is silent, and "the
 * calls from this desk were clear, so the problem is not the network under
 * it" is a diagnosis and not a shrug.
 *
 * There is no flag to clear the record and no flag to place a call. This
 * machine's phone system dials it; there is nothing here to dial with, and a
 * flag that reset the evidence is one keystroke from destroying the only
 * copy of it.
 */
#include "gsys.h"

static char buf[16384];
static char arg[256];

void _start(void)
{
    arg[0] = 0;
    g_getarg_raw(arg, sizeof arg);
    char *t = g_trim(arg);

    int op = NETINFO_VOICE;
    if (g_streq(t, "-l")) {
        op = NETINFO_VOICENOW;
    } else if (t[0]) {
        g_puts("voice: no such option: "); g_putln(t);
        g_putln("usage: voice        the calls this machine has finished");
        g_putln("       voice -l     the calls in progress right now");
        g_exit(2);
    }

    i64 n = g_netinfo(op, buf, sizeof buf);
    if (n < 0) {
        g_putln("voice: the kernel has no network state to report");
        g_exit(1);
    }
    /* NO CARD AT ALL is a different answer from NO CALLS, and a machine with
     * no card in the world has never been on one for a reason a player can
     * fix. netsite says so in one line; do not dress it up as data. */
    if (g_contains(buf, "no network interface")) {
        g_putln("this machine has no network card in the world at all,");
        g_putln("  so nothing has ever been dialled to it or from it.");
        g_exit(1);
    }
    if (n == 0) { g_putln("no calls"); g_exit(0); }
    g_puts(buf);
    if (buf[n - 1] != '\n') g_putln("");
    g_exit(0);
}

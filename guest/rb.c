/* /bin/rb — the company's API, from this machine.
 *
 * This is the moat, and it is forty lines.
 *
 * Handoff decision 13: the player's scripts run on the emulated machine. This
 * program is what makes that worth anything -- without it the machine is a
 * very elaborate text editor. With it, every verb the desktop's buttons send
 * and every verb the reference agent plays through is one shell command away,
 * which means everything on this box can automate the job:
 *
 *     rb ticket.list open
 *     for t in $(rb ticket.list open); do rb ticket.check $t; done
 *     rb api.call directory_01 create_account login=abarrow ...
 *
 * IT IS THE SAME PROTOCOL AS EVERYTHING ELSE. Not a subset, not a
 * convenience wrapper, not a second API kept in step by hand -- the same
 * line, going to the same dispatcher, as the socket and the forms. That is
 * decision 7 all the way down: anything the player can do, a script can do,
 * and anything a script can do, the game can test.
 *
 * §14's protocol note stands: it LOOKS like an HTTP-shaped call because that
 * shape is familiar. There is no wire, no headers and no network.
 */
#include "gsys.h"

/* One page of response. Long answers -- a thousand-account listing -- are
 * truncated rather than dropped, and the caller can filter server-side with
 * the endpoint's own filter argument, which is what a filter is for. */
#define RB_OUT 65536
static char out[RB_OUT];

void _start(void)
{
    char arg[GARG_MAX];
    if (g_getarg(arg, sizeof arg) <= 0) {
        g_puts("usage: rb <verb> [arguments]\n"
               "       rb help          every verb there is\n"
               "\n"
               "The same API the desktop's forms send. Whatever you can do by\n"
               "clicking, you can do here, and whatever you can do here you can\n"
               "put in a script.\n");
        g_exit(2);
    }

    i64 n = sysc(SYS_rbapi, (i64)arg, (i64)out, (i64)sizeof out);
    if (n < 0) {
        g_puts("rb: this machine is not attached to a RUNBOOK world\n");
        g_exit(1);
    }
    if (n >= (i64)sizeof out) n = (i64)sizeof out - 1;
    out[n] = 0;
    g_write(1, out, (u64)n);

    /* THE EXIT CODE IS THE ANSWER, so `if rb ... ; then` works and a script
     * can branch without parsing. -ERR and any status of 400 or more are a
     * failure; everything else is not. The legacy vendor answers 200 to
     * everything and puts the error in the body, so a script that trusts
     * THIS and nothing else will still be wrong about Halcyon -- which is
     * exactly the lesson, and it would be a shame to spoil it here. */
    if (out[0] == '-') g_exit(1);
    i64 code = 0, i = 4;
    if (out[0] == '+' && out[1] == 'O' && out[2] == 'K') {
        while (out[i] == ' ') i++;
        while (out[i] >= '0' && out[i] <= '9') { code = code * 10 + (out[i] - '0'); i++; }
    }
    g_exit(code >= 400 ? 1 : 0);
}

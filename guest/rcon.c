/* /usr/bin/rcon — the remote console on somebody else's machine.
 *
 * A support engineer does not sit at the broken box. They sit at their own
 * workstation and reach the customer's machine through its service processor
 * -- the little computer on the motherboard that is powered whether or not
 * the host is, which is the entire reason you can fix a machine that will not
 * boot. iDRAC, iLO, IPMI: same idea, different badge.
 *
 *   rcon connect <address>   attach to the console
 *   rcon status              power, media, boot device
 *   rcon power off|on|cycle
 *   rcon media insert|eject  the rescue medium, in the virtual drive
 *   rcon boot disk|media     what it boots from next time
 *   rcon console             what the machine has said since it powered on
 *
 * This is a REAL program on the workstation. It reaches the other machine
 * through a syscall, not through the desktop, so everything it can do is
 * available from a script and from a terminal with no GUI anywhere near it.
 */
#include "gsys.h"

static char buf[60000];

static void usage(void)
{
    g_putln("usage: rcon connect <address> | status | console");
    g_putln("       rcon power off|on|cycle");
    g_putln("       rcon media insert|eject");
    g_putln("       rcon boot disk|media");
}

void _start(void)
{
    static char arg[256];
    g_getarg(arg, sizeof arg);
    char *v[GARGS];
    int n = g_argv(arg, v);
    if (n < 1) { usage(); g_exit(1); }

    if (g_streq(v[0], "connect")) {
        if (n < 2) { g_putln("rcon: connect needs an address"); g_exit(1); }
        /* The address goes with the request. Without it the service processor
         * was asked "attach me" and not "attach me to THIS", so it said yes
         * to anything -- including a typo, for a whole ticket, silently. */
        i64 rc0 = sysc(SYS_sp, SP_CONNECT, 0, (i64)v[1]);
        if (rc0 == -3) {
            g_puts("rcon: no route to "); g_putln(v[1]);
            g_putln("  that machine is not on any network you can reach.");
            g_putln("  you will have to work through the person in front of it:");
            g_putln("    ask               -- what they can do from there");
            g_putln("    ask 2 <command>   -- they type it and read back what");
            g_putln("                         they can see of the answer");
            g_exit(1);
        }
        if (rc0 != 0) {
            /* NOT the same sentence as the one above. "No route" means the
             * machine is not on any network you can reach; this means the
             * network is fine and there is no service processor at the
             * address you typed. */
            g_puts("rcon: nothing answers at "); g_putln(v[1]);
            g_putln("  no service processor replied there. Either that is not");
            g_putln("  their address or you have a digit wrong -- the customer");
            g_putln("  can read it off the sticker on the front of the machine.");
            g_exit(1);
        }
        g_puts("rcon: attached to "); g_putln(v[1]);
        g_putln("  the service processor is up even though the machine may not be.");
        g_putln("  `rcon console` shows what it has said; `rcon power cycle`");
        g_putln("  restarts it; `rcon media insert` puts the rescue medium in.");
        g_exit(0);
    }

    i64 st = sysc(SYS_sp, SP_STATUS, 0, 0);
    if (st == -3) {
        g_putln("rcon: that machine is not on any network you can reach.");
        g_exit(1);
    }
    if (st < 0) {
        g_putln("rcon: no machine is reachable from here.");
        g_exit(1);
    }
    /* A service processor answers nobody who has not attached to it. */
    if (!(st & 2)) {
        g_putln("rcon: not attached to anything.");
        g_putln("  `rcon connect <address>` first -- the customer can read the");
        g_putln("  address off the sticker on the front of the machine.");
        g_exit(1);
    }

    if (g_streq(v[0], "status")) {
        g_puts("power   ");
        if (!(st & 1))      g_putln("OFF -- nothing is running in it");
        else if (st & 16)   g_putln("on, and it finished booting");
        else                g_putln("on, but it did NOT finish booting");
        g_puts("console "); g_putln((st & 2) ? "attached" : "not attached");
        g_puts("media   "); g_putln((st & 4) ? "rescue medium inserted" : "empty");
        g_puts("boot    "); g_putln((st & 8) ? "the attached medium" : "the disk");
        /* WHAT IS RUNNING, which is a different question from what it boots
         * next time and was the one nobody could ask. This status said "media
         * empty / boot the disk" while the rescue image was live, and both of
         * those lines were true about the NEXT boot. */
        if (st & 1) {
            g_puts("running ");
            g_putln((st & 32) ? "the rescue medium -- the customer's disk is "
                                "/dev/sda1, not mounted"
                              : "the customer's own disk");
        }
        g_exit(0);
    }

    if (g_streq(v[0], "console")) {
        i64 got = sysc(SYS_sp, SP_CONSOLE, 0, (i64)buf);
        if (got <= 0) { g_putln("rcon: the console is empty -- is it powered on?"); g_exit(1); }
        g_write(1, buf, (u64)got);
        g_exit(0);
    }

    if (g_streq(v[0], "power")) {
        if (n < 2) { usage(); g_exit(1); }
        int a = g_streq(v[1], "off") ? 0 : g_streq(v[1], "on") ? 1 : 2;
        if (!g_streq(v[1], "off") && !g_streq(v[1], "on") && !g_streq(v[1], "cycle")) {
            usage(); g_exit(1);
        }
        i64 prc = sysc(SYS_sp, SP_POWER, a, 0);
        if (prc == -4) {
            g_putln("rcon: it is already powered on.");
            g_putln("  `rcon power cycle` restarts it; `rcon power off` stops it.");
            g_exit(1);
        }
        if (a == 0) { g_putln("rcon: powered off."); g_exit(0); }
        g_putln(a == 1 ? "rcon: powered on." : "rcon: power cycled.");
        /* Show what it said coming up: that is the whole point of a console. */
        i64 got = sysc(SYS_sp, SP_CONSOLE, 0, (i64)buf);
        if (got > 0) { g_putln(""); g_write(1, buf, (u64)got); }
        g_exit(0);
    }

    if (g_streq(v[0], "media")) {
        if (n < 2) { usage(); g_exit(1); }
        int in = g_streq(v[1], "insert");
        if (!in && !g_streq(v[1], "eject")) { usage(); g_exit(1); }
        sysc(SYS_sp, SP_MEDIA, in, 0);
        g_putln(in ? "rcon: rescue medium inserted in the virtual drive."
                   : "rcon: virtual drive emptied.");
        if (in) g_putln("  `rcon boot media` then `rcon power cycle` to boot it.");
        g_exit(0);
    }

    if (g_streq(v[0], "boot")) {
        if (n < 2) { usage(); g_exit(1); }
        int m = g_streq(v[1], "media");
        if (!m && !g_streq(v[1], "disk")) { usage(); g_exit(1); }
        i64 rc = sysc(SYS_sp, SP_BOOTDEV, m, 0);
        if (rc == -2) {
            g_putln("rcon: there is nothing in the virtual drive.");
            g_putln("  `rcon media insert` first.");
            g_exit(1);
        }
        g_putln(m ? "rcon: next boot is from the attached medium."
                  : "rcon: next boot is from the disk.");
        g_exit(0);
    }

    usage();
    g_exit(1);
}

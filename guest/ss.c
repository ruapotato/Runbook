/* /bin/ss — the sockets, in the shape iproute2 prints them.
 *
 * BE HONEST ABOUT WHAT THIS IS: it is netstat's socket list, in ss's
 * columns, with ss's flags. It asks the kernel the same question netstat
 * does -- NETINFO_SOCK, the real socket table -- and there is no second
 * source of truth for it to read. It exists because `ss -ltn` is what a
 * person types now, and a machine that answers `command not found` to the
 * standard question teaches the player to distrust the answers it does
 * give. The man page says the same thing in as many words.
 *
 * WHAT IT CANNOT DO, and does not pretend to: -p. On a real system that
 * column comes from the process that holds the file descriptor. This kernel
 * binds a socket for a SERVICE, not for a process id it records, so there is
 * nothing to print there and the flag is refused by name rather than printing
 * an empty column that looks like "nobody owns it".
 */
#include "gsys.h"

static char buf[16384];
static char arg[256];

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

void _start(void)
{
    arg[0] = 0;
    g_getarg(arg, sizeof arg);
    char *v[GARGS];
    int n = g_argv(arg, v);

    int tcp = 0, udp = 0, listening = 0;
    for (int i = 0; i < n; i++) {
        const char *a = v[i];
        if (a[0] != '-') {
            g_puts("ss: this ss takes no filter expression: "); g_putln(a);
            g_putln("usage: ss [-t] [-u] [-l] [-a] [-n]");
            g_exit(2);
        }
        for (int k = 1; a[k]; k++) {
            switch (a[k]) {
            case 't': tcp = 1; break;
            case 'u': udp = 1; break;
            case 'l': listening = 1; break;
            case 'a': break;              /* every socket: the default here */
            case 'n': break;              /* numeric: the only thing it is  */
            case 'p':
                g_putln("ss: there is no -p on this machine.");
                g_putln("  a socket here belongs to a SERVICE, not to a process id");
                g_putln("  the kernel records, so the column would always be empty.");
                g_putln("  `svc status <name>` is the question you want, and `ps`.");
                g_exit(2);
                break;
            default: {
                char o[3] = { '-', a[k], 0 };
                g_puts("ss: no such option: "); g_putln(o);
                g_putln("usage: ss [-t tcp] [-u udp] [-l listening] [-a] [-n]");
                g_exit(2);
            }
            }
        }
    }

    i64 got = g_netinfo(NETINFO_SOCK, buf, sizeof buf);
    if (got < 0) { g_putln("ss: the kernel has no network state to report"); g_exit(1); }

    /* NO CARD AT ALL. The kernel says so in one line rather than an empty
     * list, and a parser that treated it as data would print nonsense at
     * exactly the moment a player most needs a straight answer. */
    if (g_contains(buf, "no network interface")) {
        g_putln("this machine has no network card in the world at all.");
        g_exit(1);
    }
    g_putln("NETID STATE        LOCAL ADDRESS:PORT     PEER ADDRESS:PORT");
    int shown = 0;
    char *s = buf;
    while (*s) {
        char *l = s;
        s = line_next(s);
        if (!*l) continue;
        /* proto  local  peer  state -- what net_dump_sockets writes. */
        char *f[8];
        int w = words(l, f, 8);
        if (w < 4) continue;
        if (tcp && !g_streq(f[0], "tcp")) continue;
        if (udp && !g_streq(f[0], "udp")) continue;
        if (listening && !g_streq(f[3], "LISTEN") && !g_streq(f[3], "OPEN")) continue;
        pad(f[0], 6);
        pad(f[3], 13);
        pad(f[1], 23);
        g_putln(f[2]);
        shown++;
    }
    if (!shown) {
        if (listening)
            g_putln("(nothing is listening -- no network service has a socket open)");
        else
            g_putln("(this machine holds no sockets at all)");
        g_exit(1);
    }
    g_exit(0);
}

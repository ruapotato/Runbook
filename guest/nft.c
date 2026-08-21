/* /usr/sbin/nft — the firewall daemon.
 *
 * Reads its configuration at startup and then stays running. If the config is
 * missing it says so and exits, which is a service failing to start rather
 * than a service that was never there -- a different fault with a different
 * fix.
 */
#include "gsys.h"
static char conf[2048];
static const char *CONF[] = { "/etc/nftables.conf", 0 };

static void publish(void)
{
    /* the first non-comment line of the config, as loaded */
    static char state[256];
    state[0] = 0;
    char *q = conf;
    while (*q) {
        char *nl = q; while (*nl && *nl != '\n') nl++;
        char save = *nl; *nl = 0;
        char *t = g_trim(q);
        if (*t && *t != '#') { g_copy(state, t, sizeof state); *nl = save; break; }
        *nl = save; q = *nl ? nl + 1 : nl;
    }
    /* Two lines: which file was loaded, and what it said. The kernel compares
     * the second against the file named by the first, which is how "running
     * with a stale configuration" becomes a state the machine can notice
     * rather than a thing only a person could spot. */
    int fd = g_open("/run/nft.state", O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        /* A daemon that cannot write its state file is not running properly,
         * whatever it thinks. This used to return quietly, which meant that
         * deleting /run -- something a careless cleanup really does -- left
         * every service reporting itself healthy while the machine had no
         * idea what any of them had loaded. Silence here is how a fault stops
         * being a fault. */
        g_puts("nft: ");
        g_puts("/run/nft.state");
        g_putln(": cannot write state -- refusing to start");
        g_exit(1);
    }
    sysc(SYS_write, fd, (i64)CONF[0], (i64)g_strlen(CONF[0]));
    sysc(SYS_write, fd, (i64)"\n", 1);
    sysc(SYS_write, fd, (i64)state, (i64)g_strlen(state));
    sysc(SYS_write, fd, (i64)"\n", 1);
    g_close(fd);
}

/* Is `w` the next word of `p`, followed by a space or a delimiter? */
static char *word(char *p, const char *w)
{
    u64 k = 0;
    while (w[k] && p[k] == w[k]) k++;
    if (w[k]) return 0;
    char c = p[k];
    if (c && c != ' ' && c != '\t' && c != '{') return 0;
    p += k;
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

/* Pull the next decimal out of p, advancing it. Returns -1 at the end. */
static int next_port(char **pp)
{
    char *p = *pp;
    while (*p && (*p < '0' || *p > '9')) {
        if (*p == '}' || *p == '\n') { *pp = p; return -1; }
        p++;
    }
    if (*p < '0' || *p > '9') { *pp = p; return -1; }
    int v = 0;
    while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
    *pp = p;
    return v;
}

/* chain<<24 | proto<<16 | dport, and a drop flag: the shape SYS_netctl takes. */
static void rule(int proto, int dport, int drop)
{
    g_netctl(NETCTL_FWADD, ((i64)0 << 24) | ((i64)proto << 16) | (i64)dport, drop);
}

static void install(void)
{
    g_netctl(NETCTL_FWCLEAR, 0, 0);
    int policy_drop = 0, have_policy = 0;

    char *q = conf;
    while (*q) {
        char *nl = q; while (*nl && *nl != '\n') nl++;
        char save = *nl; *nl = 0;
        char *t = g_trim(q);

        /* "type filter hook input priority 0; policy drop;" -- the policy is
         * remembered and applied last, because that is what a policy is. */
        if (g_contains(t, "policy")) {
            if (g_contains(t, "policy drop")) { policy_drop = 1; have_policy = 1; }
            else if (g_contains(t, "policy accept")) { policy_drop = 0; have_policy = 1; }
        }

        /* WHICH PROTOCOL THIS LINE IS ABOUT. `icmp` is here because without
         * it the only repair available for a box that would not answer a
         * ping was turning the whole chain policy to accept -- which opens
         * every port on the machine to fix one thing, and is not what any
         * administrator would write. `ip protocol icmp` is the same rule
         * spelled the long way, and both spellings appear in real rulesets. */
        int proto = 0;
        char *r = word(t, "icmp");
        if (r) proto = 1;
        else if ((r = word(t, "tcp")) != 0) proto = 6;
        else if ((r = word(t, "udp")) != 0) proto = 17;
        else {
            char *ip = word(t, "ip");
            char *pr = ip ? word(ip, "protocol") : 0;
            if (pr) {
                if ((r = word(pr, "icmp")) != 0) proto = 1;
                else if ((r = word(pr, "tcp")) != 0) proto = 6;
                else if ((r = word(pr, "udp")) != 0) proto = 17;
            }
        }
        if (r) {
            /* Accept unless the line says otherwise: an nftables rule with
             * no verdict falls through, and treating a rule we could not
             * read as a DROP would lock somebody out of a machine over a
             * typo. */
            int drop = g_contains(r, "drop") ? 1 : 0;
            int verdict = drop || g_contains(r, "accept");
            char *d = word(r, "dport");
            if (d && verdict) {
                for (;;) {
                    int p = next_port(&d);
                    if (p < 0) break;
                    rule(proto, p, drop);
                }
            } else if (!d && verdict) {
                /* A whole protocol, every port of it. This is the only shape
                 * icmp has -- it has no ports to name -- and it is what
                 * `icmp accept` means. Port 0 in the kernel's filter is "any
                 * port", so this is one rule and not sixty-five thousand. */
                rule(proto, 0, drop);
            }
        }
        *nl = save; q = *nl ? nl + 1 : nl;
    }
    /* Everything that matched nothing. */
    if (have_policy && policy_drop) rule(0, 0, 1);
}

static const char *KEY = "table";
/* Somebody typed `nft list ruleset`, because that is what you type on Linux.
 * On this machine nft is the DAEMON and nothing else: svcinit starts it with
 * no arguments, it loads the file and then sits in the poll loop below. Run
 * from a shell it did exactly that -- sat there -- until the cpu budget ran
 * out and the kernel killed it with "still running after 40000000
 * instructions", which explains nothing to somebody who was asking a
 * question. Any argument at all is that person, so answer them. */
static char argbuf[256];

static void say_what_it_is(void)
{
    g_putln("nft is the daemon that loads /etc/nftables.conf. It takes no");
    g_putln("arguments -- svcinit starts it, and it stays running.");
    g_putln("  netstat -F              the ruleset it is ACTUALLY running,");
    g_putln("                          with what each rule has dropped");
    g_putln("  cat /etc/nftables.conf  what it is supposed to be running");
    g_putln("  ed /etc/nftables.conf   change it: `,n` numbers the lines,");
    g_putln("                          `man ed` is the editor");
    g_putln("  svc reload nftables     re-read the file after editing it");
    g_putln("  man nft                 what this parser understands");
}

void _start(void)
{
    argbuf[0] = 0;
    g_getarg(argbuf, sizeof argbuf);
    {
        char *v[GARGS];
        if (g_argv(argbuf, v) > 0) { say_what_it_is(); g_exit(0); }
    }
    for (int i = 0; CONF[i]; i++) {
        if (g_slurp(CONF[i], conf, sizeof conf) < 0) {
            g_puts("nft: ");
            g_puts(CONF[i]);
            g_putln(": cannot read -- refusing to start");
            g_exit(1);
        }
    }
    /* The config is there. Is it USABLE? A file that exists and does not say
     * the one thing this daemon needs is a completely different fault from a
     * file that is missing, and it fails later and less obviously. */
    {
        int ok = 0;
        char *q = conf;
        while (*q) {
            char *nl = q; while (*nl && *nl != '\n') nl++;
            char save = *nl; *nl = 0;
            char *t = g_trim(q);
            if (*t && *t != '#') {
                u64 k = 0;
                while (KEY[k] && t[k] == KEY[k]) k++;
                if (!KEY[k]) ok = 1;      /* empty KEY: any real line will do */
            }
            *nl = save; q = *nl ? nl + 1 : nl;
            if (ok) break;
        }
        if (!ok) {
            g_puts("nft: ");
            g_puts(CONF[0]);
            g_putln(": no ruleset -- refusing to start");
            g_exit(1);
        }
    }

    /* INSTALL THE RULES, so that the file is not merely readable but LOADED.
     *
     * This is the difference between a daemon that validates a config and a
     * firewall. Until now nft parsed /etc/nftables.conf, decided it looked
     * like a ruleset, and stopped -- so a rule saying `drop` dropped nothing
     * and a port that was supposed to be closed was open. Now each line
     * becomes a rule in the kernel's filter, and a packet that matches it is
     * really discarded, on the way in, before anything above IP sees it.
     *
     * The subset is small and it is the subset this file's own shipped
     * ruleset uses:
     *
     *   policy drop / policy accept    what happens to what no rule matched
     *   tcp dport { 22, 80 } accept    a set of ports
     *   tcp dport 8080 drop            one port
     *   udp dport 53 accept
     *   icmp accept                    a protocol with no ports in it
     *   ip protocol icmp accept        the same rule, spelled in full
     *   tcp accept                     every port of one protocol
     *
     * ICMP IS NOT A LUXURY HERE. The shipped ruleset is `policy drop` plus
     * two tcp ports, and there is no connection tracking beyond a socket
     * this machine already holds -- so a pristine box does not answer a
     * ping, and the echo REPLIES to its own pings are dropped on the way in
     * too. That is honest and it is a good puzzle. What it must not be is a
     * puzzle with one answer: without `icmp` the only repair this parser
     * could express was flipping the whole policy to accept, which opens
     * every port on the machine to fix one thing.
     *
     * Order matters and is preserved: the first rule that matches decides,
     * and the chain policy goes on the end, which is where a policy is. */
    install();

    /* Publish what was actually loaded. The file on disk says what the
     * machine is SUPPOSED to do; this says what the running process is
     * actually doing, and the two drift the moment somebody edits a config
     * and does not reload. That gap is invisible without this. */
    publish();

    /* Up. A daemon spends its life here, looking occasionally to see whether
     * anyone has asked it to re-read its configuration. */
    for (;;) {
        if (g_sigpend() == SIG_HUP) {
            /* A reload replaces the ruleset. It does not add to it, which is
             * what made an edited config that removed a rule leave the rule
             * running until the next reboot. */
            if (g_slurp(CONF[0], conf, sizeof conf) >= 0) { install(); publish(); }
        }
    }
}

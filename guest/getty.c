/* /sbin/getty — open a terminal and offer a login.
 *
 * The last thing a boot does, and the one that decides whether "it booted"
 * and "it works" are the same sentence. getty validates the account it is
 * about to hand the machine to: the entry has to be in /etc/passwd, the home
 * directory has to be there, and the login shell has to exist and be
 * executable. A machine whose root shell is missing comes all the way up,
 * starts every service, and is still useless.
 */
#include "gsys.h"

static char passwd[8192], issue[512], line[256];

/* colon-separated: name:x:uid:gid:gecos:home:shell */
static int field(const char *rec, int want, char *out, u64 cap)
{
    int f = 0;
    u64 o = 0;
    out[0] = 0;
    for (u64 i = 0; ; i++) {
        if (rec[i] == ':' || rec[i] == 0) {
            if (f == want) { out[o] = 0; return 1; }
            f++;
            o = 0;
            if (rec[i] == 0) return 0;
            continue;
        }
        if (f == want && o + 1 < cap) out[o++] = rec[i];
    }
}

void _start(void)
{
    static char who[64];
    if (g_getarg(who, sizeof who) <= 0) g_copy(who, "root", sizeof who);

    if (g_slurp("/etc/passwd", passwd, sizeof passwd) < 0) {
        g_putln("getty: /etc/passwd: cannot read -- no accounts, no login");
        g_exit(1);
    }

    static char name[64], home[128], shell[128];
    int found = 0;
    char *p = passwd;
    while (*p && !found) {
        char *nl = p; while (*nl && *nl != '\n') nl++;
        char save = *nl; *nl = 0;
        g_copy(line, p, sizeof line);
        *nl = save; p = *nl ? nl + 1 : nl;
        char *t = g_trim(line);
        if (!*t || *t == '#') continue;
        if (!field(t, 0, name, sizeof name)) continue;
        if (!g_streq(name, who)) continue;
        found = 1;
        field(t, 5, home, sizeof home);
        field(t, 6, shell, sizeof shell);
    }

    if (!found) {
        g_puts("getty: no account for ");
        g_puts(who);
        g_putln(" in /etc/passwd");
        g_exit(1);
    }
    if (!shell[0]) {
        g_puts("getty: ");
        g_puts(who);
        g_putln(": no login shell in /etc/passwd");
        g_exit(1);
    }

    NomStat st;
    if (g_stat(shell, &st) != 0) {
        g_puts("getty: ");
        g_puts(who);
        g_puts("'s login shell ");
        g_puts(shell);
        g_putln(" does not exist");
        g_exit(1);
    }
    if (st.kind != NOM_KIND_FILE) {
        /* A DIRECTORY IS NOT A SHELL. This matters more than it sounds: one
         * extra colon in a passwd line shifts every field left, the home
         * directory lands in the shell column, and the path exists and is
         * "executable" because directories are. Without this the account
         * looked fine and the machine was quietly unusable. */
        g_puts("getty: ");
        g_puts(who);
        g_puts("'s login shell ");
        g_puts(shell);
        g_putln(" is not a program (check the field order in /etc/passwd)");
        g_exit(1);
    }
    if (!(st.mode & 0111)) {
        g_puts("getty: ");
        g_puts(who);
        g_puts("'s login shell ");
        g_puts(shell);
        g_putln(" is not executable");
        g_exit(1);
    }
    /* AND IS IT A LOGIN SHELL AT ALL.
     *
     * /etc/shells is the list of shells an account may log in with, and a
     * hardening pass that prunes it is a real afternoon: the shell is there,
     * it is executable, the account is fine, and login is refused because the
     * path is not on the list. It was a file nothing on this machine read,
     * which is worse than not having it. A machine with no /etc/shells at all
     * is not a machine with no valid shells -- that is a missing file, and it
     * is not this program's business to invent a policy for it. */
    {
        static char shells[2048], sline[256];
        if (g_slurp("/etc/shells", shells, sizeof shells) > 0) {
            int listed = 0;
            char *s = shells;
            while (*s && !listed) {
                char *nl = s; while (*nl && *nl != '\n') nl++;
                char save = *nl; *nl = 0;
                g_copy(sline, s, sizeof sline);
                *nl = save; s = *nl ? nl + 1 : nl;
                char *t = g_trim(sline);
                if (!*t || *t == '#') continue;
                if (g_streq(t, shell)) listed = 1;
            }
            if (!listed) {
                g_puts("getty: ");
                g_puts(who);
                g_puts("'s login shell ");
                g_puts(shell);
                g_putln(" is not listed in /etc/shells");
                g_putln("       the account is refused a terminal -- no login");
                g_exit(1);
            }
        }
    }

    /* PASSWD AND SHADOW HAVE TO AGREE.
     *
     * The account exists and its shell is fine, and there is still no way in:
     * the password lives in a second file, and an account with no line in
     * /etc/shadow cannot be authenticated at all. It is exactly what a
     * half-finished user migration leaves behind, and it is invisible in
     * /etc/passwd, which is where everybody looks. */
    {
        static char shadow[8192], sline[256], sname[64];
        if (g_slurp("/etc/shadow", shadow, sizeof shadow) < 0) {
            g_putln("getty: /etc/shadow: cannot read -- no passwords, no login");
            g_exit(1);
        }
        int has = 0;
        char *s = shadow;
        while (*s && !has) {
            char *nl = s; while (*nl && *nl != '\n') nl++;
            char save = *nl; *nl = 0;
            g_copy(sline, s, sizeof sline);
            *nl = save; s = *nl ? nl + 1 : nl;
            char *t = g_trim(sline);
            if (!*t || *t == '#') continue;
            if (field(t, 0, sname, sizeof sname) && g_streq(sname, who)) has = 1;
        }
        if (!has) {
            g_puts("getty: ");
            g_puts(who);
            g_putln(" is in /etc/passwd but has no entry in /etc/shadow");
            g_putln("       the account cannot be authenticated -- no login");
            g_exit(1);
        }
    }

    if (home[0] && g_stat(home, &st) != 0) {
        /* Not fatal on a real system and not fatal here, but it is the sort
         * of thing worth saying out loud. */
        g_puts("getty: warning: home directory ");
        g_puts(home);
        g_putln(" is missing");
    }

    if (g_slurp("/etc/issue", issue, sizeof issue) > 0) {
        g_puts("\n");
        g_puts(g_trim(issue));
        g_puts("\n");
    }
    static char host[64];
    if (g_slurp("/etc/hostname", host, sizeof host) > 0) g_puts(g_trim(host));
    else g_puts("localhost");
    g_putln(" login:");
    g_exit(0);
}

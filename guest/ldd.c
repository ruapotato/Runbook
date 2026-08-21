/* /usr/bin/ldd — what does this program need, and can it be found?
 *
 * The one tool that turns a library fault from guesswork into reading. Every
 * other route to the same answer is indirect: run the program and read the
 * loader's complaint, or reinstall packages until one of them helps. ldd
 * asks the question directly, for a binary you have not run and may not be
 * able to run.
 *
 * It resolves the same way the loader does -- /etc/ld.so.conf, in order --
 * and compares the version the library declares against the version the
 * program asked for. That is deliberate: an ldd that disagrees with what
 * happens when you actually run the thing is worse than no ldd at all, so it
 * reads the dependency list out of the ELF through the same code the loader
 * uses rather than keeping its own idea of the format.
 */
#include "gsys.h"

static char needs[512];
/* The root filesystem the program actually belongs to. Empty means "this
 * one"; set when the binary lives under a mount. */
static char rootpfx[128];
static char conf[512];
static char libbuf[4096];

/* The version a library declares about itself, from its first line:
 * "\x7fELF (stub) zlib 1.3" -> "1.3". Same rule the loader applies. */
static int lib_version(const char *path, char *out, u64 outsz)
{
    out[0] = 0;
    i64 n = g_slurp(path, libbuf, sizeof libbuf);
    if (n < 0) return 0;
    u64 e = 0;
    while (e < (u64)n && libbuf[e] != '\n') e++;
    libbuf[e] = 0;
    /* the last space-separated word of the first line */
    char *last = libbuf;
    for (char *q = libbuf; *q; q++) if (*q == ' ') last = q + 1;
    g_copy(out, last, outsz);
    return out[0] != 0;
}

/* Walk /etc/ld.so.conf in order, exactly as the loader does. A library that
 * exists but sits in a directory nobody lists is not found, and saying so is
 * the whole point of this tool. */
static int find_lib(const char *soname, char *out, u64 outsz)
{
    static char cpath[192];
    g_copy(cpath, rootpfx, sizeof cpath);
    g_cat(cpath, "/etc/ld.so.conf", sizeof cpath);
    if (g_slurp(cpath, conf, sizeof conf) < 0)
        g_copy(conf, "/lib\n/usr/lib\n", sizeof conf);
    char *p = conf;
    while (*p) {
        char *nl = p; while (*nl && *nl != '\n') nl++;
        char save = *nl; *nl = 0;
        char *dir = g_trim(p);
        if (*dir && *dir != '#') {
            static char cand[256];
            g_copy(cand, rootpfx, sizeof cand);
            g_cat(cand, dir, sizeof cand);
            g_cat(cand, "/", sizeof cand);
            g_cat(cand, soname, sizeof cand);
            NomStat st;
            if (g_stat(cand, &st) == 0) { g_copy(out, cand, outsz); *nl = save; return 1; }
        }
        *nl = save;
        p = *nl ? nl + 1 : nl;
    }
    return 0;
}

/* WHICH ROOT DO WE RESOLVE AGAINST?
 *
 * `ldd /mnt/usr/sbin/httpd` was reading the dependency list out of the
 * mounted disk and then looking for the libraries on the RESCUE medium --
 * whose /lib holds two files. So it reported libz.so.1 as "not found" while
 * the library sat in /mnt/lib, plainly visible in `ls`, and manufactured a
 * fault that did not exist. A playtester lost time chasing it and was right
 * to call it the worst bug in the game: a tool that invents faults is worse
 * than no tool, and `man ldd` promises exactly this case works.
 *
 * A binary under a mounted root is resolved against THAT root. Found by
 * walking the prefixes of the path and taking the longest one that looks like
 * a root filesystem -- that is, one with an /etc/ld.so.conf in it. */
static void find_root(const char *prog)
{
    rootpfx[0] = 0;
    u64 n = g_strlen(prog);
    for (u64 i = n; i > 1; i--) {
        if (prog[i] != '/') continue;
        static char cand[192];
        u64 k = 0;
        while (k < i && k < sizeof cand - 20) { cand[k] = prog[k]; k++; }
        cand[k] = 0;
        static char probe[256];
        g_copy(probe, cand, sizeof probe);
        g_cat(probe, "/etc/ld.so.conf", sizeof probe);
        NomStat st;
        if (g_stat(probe, &st) == 0) { g_copy(rootpfx, cand, sizeof rootpfx); return; }
    }
}

void _start(void)
{
    static char arg[256];
    g_getarg(arg, sizeof arg);
    char *v[GARGS];
    int argn = g_argv(arg, v);
    if (argn < 1) { g_putln("usage: ldd [-r <root>] <program>"); g_exit(1); }

    int pi = 0;
    for (int i = 0; i + 1 < argn; i++)
        if (g_streq(v[i], "-r") || g_streq(v[i], "--root")) {
            g_copy(rootpfx, v[i + 1], sizeof rootpfx);
            pi = i + 2;
        }
    if (pi >= argn) { g_putln("usage: ldd [-r <root>] <program>"); g_exit(1); }
    v[0] = v[pi];
    if (!rootpfx[0]) find_root(v[0]);

    i64 got = sysc(SYS_needs, (i64)v[0], (i64)needs, sizeof needs - 1);
    if (got < 0) {
        g_puts("ldd: "); g_puts(v[0]);
        g_putln(": cannot read (is it there, and is it a program?)");
        g_exit(1);
    }
    needs[got] = 0;
    if (rootpfx[0]) {
        g_puts("(resolving against the root filesystem at ");
        g_puts(rootpfx);
        g_putln(")");
    }
    if (!got) { g_putln("\tstatically linked"); g_exit(0); }

    int bad = 0;
    char *p = needs;
    while (*p) {
        char *nl = p; while (*nl && *nl != '\n') nl++;
        char save = *nl; *nl = 0;
        static char line[160];
        g_copy(line, p, sizeof line);
        *nl = save; p = *nl ? nl + 1 : nl;

        char *t = g_trim(line);
        if (!*t) continue;
        /* "libz.so.1 1.3" */
        static char soname[96], want[64];
        soname[0] = want[0] = 0;
        u64 i = 0, k = 0;
        while (t[i] && t[i] != ' ' && k < sizeof soname - 1) soname[k++] = t[i++];
        soname[k] = 0;
        while (t[i] == ' ') i++;
        k = 0;
        while (t[i] && t[i] != ' ' && k < sizeof want - 1) want[k++] = t[i++];
        want[k] = 0;

        static char path[256], have[64];
        g_puts("\t");
        g_puts(soname);
        if (!find_lib(soname, path, sizeof path)) {
            g_putln(" => not found");
            bad++;
            continue;
        }
        g_puts(" => ");
        g_puts(path);
        if (want[0] && lib_version(path, have, sizeof have)) {
            g_puts(" (");
            g_puts(have);
            g_puts(")");
            /* Same comparison the loader makes: newer satisfies older. */
            int hmaj = 0, hmin = 0, wmaj = 0, wmin = 0;
            const char *q = have;
            while (*q >= '0' && *q <= '9') hmaj = hmaj * 10 + (*q++ - '0');
            if (*q == '.') { q++; while (*q >= '0' && *q <= '9') hmin = hmin * 10 + (*q++ - '0'); }
            q = want;
            while (*q >= '0' && *q <= '9') wmaj = wmaj * 10 + (*q++ - '0');
            if (*q == '.') { q++; while (*q >= '0' && *q <= '9') wmin = wmin * 10 + (*q++ - '0'); }
            if (hmaj < wmaj || (hmaj == wmaj && hmin < wmin)) {
                g_puts("  -- TOO OLD, this program needs ");
                g_puts(want);
                bad++;
            }
        }
        g_puts("\n");
    }
    g_exit(bad ? 1 : 0);
}

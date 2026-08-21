/* /usr/bin/pkg — the package database, and the repair verb.
 *
 *   pkg list                 what is installed
 *   pkg owns <path>          which package would I be reinstalling
 *   pkg verify [name]        which files differ from what was shipped
 *   pkg reinstall <name>     put them back, from the repository
 *
 * verify works by hashing each installed file and comparing against the
 * manifest in /var/lib/pkg/<name>/files. The manifest is ON THE DISK, so it
 * can itself be damaged -- and when it is, verify says so rather than
 * reporting a clean system, because a check that cannot fail is worthless.
 */
#include "gsys.h"

static char arg[256], manifest[8192], filebuf[65536], path[256];
/* The pristine bytes, fetched from the repository. Shared by `diff` and by
 * `verify` -- verify pulls them only for a file that already failed its hash,
 * so it can say WHICH WAY the file differs rather than only that it does. */
static char shipped[65536];

/* Operate on a filesystem mounted somewhere else, without chrooting into it.
 * This is not a convenience: when the customer's libc is the wrong version,
 * NOTHING on their disk will run -- so you cannot chroot in and use their
 * tools, and repairing from outside is the only way back. rpm and dpkg both
 * have this for exactly the same reason. */
static char root[128];

static void rooted(const char *p2)
{
    g_copy(path, root, sizeof path);
    g_cat(path, p2, sizeof path);
}

/* IS THERE A PACKAGE DATABASE UNDER THAT ROOT AT ALL.
 *
 * `pkg --root /mnt owns /boot/initrd-6.4.11` answered "no package owns that
 * path -- nothing installed this file ... removing it is usually safe. `rm
 * <path>` if you are sure" on a /mnt with nothing mounted on it. The true
 * answer, once the disk is actually mounted, is kernel-default. So the tool
 * told the player, in its own confident voice, to delete the kernel's initrd
 * -- and it is the exact advice that solves a DIFFERENT ticket, an orphan
 * file no package owns, so a player who has seen that ticket will trust it.
 *
 * Every verb here reads the manifests under <root>/var/lib/pkg, and with no
 * such directory every one of them finds nothing and reports nothing found.
 * "I looked and there was none" and "I could not look" are opposite answers.
 * Asked about a root it cannot read, pkg says so and stops. */
static int root_readable(void)
{
    if (!root[0]) return 1;
    rooted("/var/lib/pkg");
    NomStat st;
    if (g_stat(path, &st) != 0 || st.kind != NOM_KIND_DIR) return 0;
    return 1;
}

static int read_manifest(const char *pkg)
{
    rooted("/var/lib/pkg/");
    g_cat(path, pkg, sizeof path);
    g_cat(path, "/files", sizeof path);
    return g_slurp(path, manifest, sizeof manifest) >= 0;
}

/* one manifest line: "<mode> <hash> <path>" */
static int split3(char *line, char **a, char **b, char **c)
{
    char *v[GARGS];
    if (g_argv(line, v) < 3) return 0;
    *a = v[0]; *b = v[1]; *c = v[2];
    return 1;
}

static unsigned long parse_hex(const char *s)
{
    unsigned long v = 0;
    for (; *s; s++) {
        unsigned d;
        if (*s >= '0' && *s <= '9') d = (unsigned)(*s - '0');
        else if (*s >= 'a' && *s <= 'f') d = (unsigned)(*s - 'a' + 10);
        else break;
        v = v * 16 + d;
    }
    return v;
}

static unsigned parse_oct(const char *s)
{
    unsigned v = 0;
    for (; *s >= '0' && *s <= '7'; s++) v = v * 8 + (unsigned)(*s - '0');
    return v;
}

/* Every finding names the package that owns it, because the next thing the
 * player does is reinstall something and the whole point is reinstalling the
 * RIGHT thing. A verify that only prints paths makes you look the owner up
 * by hand, every time. */
/* THE TWO KINDS OF FINDING, counted apart.
 *
 * `pkg reinstall` treats them differently -- it keeps an edited /etc file and
 * replaces everything else -- and the closing line of `pkg verify` used to
 * promise that reinstall "puts them back" for both. It does not, deliberately,
 * and the previous administrator's notes celebrate that it does not. A file
 * somebody CHANGED is a decision; a file that is missing or truncated is
 * damage; they need different advice and the summary now gives it. */
static int g_edited;      /* an /etc file whose contents differ: reinstall KEEPS */
static int g_damaged;     /* missing, repointed, wrong mode: reinstall restores  */
/* AND THE THIRD KIND, WHICH VERIFY USED TO CALL THE FIRST.
 *
 * A playtester repaired three servers after a mains failure. The file the
 * unclean shutdown had cut in half came back CHANGED, and the summary told
 * them a reinstall would not put it back, because "the edit may be a decision
 * somebody made on purpose". `pkg diff` on the same file said -- correctly --
 * "155 byte(s) SHORT -- it was truncated, not edited". Verify had the same
 * two sizes in front of it and did not use them.
 *
 * A file that is byte-for-byte what shipped and then stops was not edited
 * into that shape by somebody working on the config: that is the shape a
 * write interrupted part way through leaves. It is not proof -- a person who
 * deleted the tail by hand leaves exactly the same bytes, and the summary
 * says so rather than claiming more than the bytes support -- but it is the
 * sentence that decides whether the player reinstalls or goes hunting for
 * whose decision it was, and here the reinstall IS the repair. */
static int g_cut;         /* installed is a strict prefix of what shipped       */
static int g_cut_etc;     /* ... and under /etc, where putting it back needs -f */

static int under_etc(const char *p)
{
    return p[0] == '/' && p[1] == 'e' && p[2] == 't' && p[3] == 'c' && p[4] == '/';
}

static void finding(const char *pkg, const char *path, const char *what)
{
    g_puts(pkg);
    for (u64 k = g_strlen(pkg); k < 16; k++) g_puts(" ");
    g_puts(path);
    /* A path longer than the column ran straight into the status word:
     * "/etc/udev/rules.d/50-default.rulesCHANGED". Pad to the column when it
     * fits, and always emit at least one space when it does not. */
    u64 pl = g_strlen(path);
    if (pl < 34) { for (u64 k = pl; k < 34; k++) g_puts(" "); }
    else g_puts(" ");
    g_putln(what);
}

/* Print one file's shipped bytes against its installed bytes. Reachable two
 * ways -- by path, and by package name, which diffs every file that moved. */
/* Is this content something a person can read? A package file may be an ELF
 * image, and dumping thirty kilobytes of NULs and control characters at the
 * terminal is not a diff, it is an accident. */
static int is_text(const char *b, i64 n)
{
    if (n > 4 && b[0] == 0x7f && b[1] == 'E' && b[2] == 'L' && b[3] == 'F') return 0;
    i64 lim = n < 400 ? n : 400;
    for (i64 i = 0; i < lim; i++) {
        unsigned char ch = (unsigned char)b[i];
        if (ch == 0) return 0;
        if (ch < 9 || (ch > 13 && ch < 32)) return 0;
    }
    return 1;
}

/* One byte, as a person reads it. */
static void show_byte(char c, int present)
{
    if (!present) { g_puts("end of file"); return; }
    unsigned char u = (unsigned char)c;
    if (u == '\n')      g_puts("a newline");
    else if (u == '\t') g_puts("a tab");
    else if (u == ' ')  g_puts("a space");
    else if (u < 32 || u > 126) { g_puts("byte "); g_putoct(u, 3); }
    else { char b[4]; b[0] = '\''; b[1] = (char)u; b[2] = '\''; b[3] = 0; g_puts(b); }
}

static void diff_path(const char *owner, const char *path)
{
    /* A symlink has no contents to compare, and pretending otherwise produced
     * output that flatly contradicted `stat`: "shipped 0 bytes, installed 20
     * bytes" for a link whose target did not exist. Say what it actually is. */
    static char tgt[256], rp0[300];
    g_copy(rp0, root, sizeof rp0);
    g_cat(rp0, path, sizeof rp0);
    i64 tl = g_readlink(rp0, tgt, sizeof tgt - 1);
    if (tl >= 0) {
        tgt[tl] = 0;
        g_puts("--- "); g_puts(path); g_puts("  is a symlink -> "); g_putln(tgt);
        NomStat st0;
        if (g_stat(rp0, &st0) != 0)
            g_putln("    and the target does not exist: this link is DANGLING");
        return;
    }

    i64 want = g_repo(owner, path, filebuf);
    if (want < 0) {
        g_puts("pkg: "); g_puts(path);
        g_putln(": cannot fetch the shipped copy");
        return;
    }
    for (i64 k = 0; k < want; k++) shipped[k] = filebuf[k];
    shipped[want] = 0;

    /* Read the INSTALLED copy through --root, so this works on a disk mounted
     * elsewhere. It did not, which meant diff was unusable on exactly the
     * machines --root exists for: the ones whose own libc is broken, where
     * chroot is not an option at all. */
    static char rpath[300];
    g_copy(rpath, root, sizeof rpath);
    g_cat(rpath, path, sizeof rpath);
    i64 have = g_slurp(rpath, filebuf, sizeof filebuf);
    if (have < 0) {
        g_puts("pkg: "); g_puts(rpath);
        g_putln(": cannot read what is installed");
        return;
    }

    /* Binary content is summarised, never dumped. A playtester ran
     * `pkg diff sysinit` and got pages of raw ELF; the useful facts are the
     * two sizes and the fact that they differ. */
    /* is_text looks at the first 400 bytes; a short config file that differs
     * somewhere later was being called binary purely because the two sides
     * were the same length, leaving the player with no content at all and no
     * way to see what was wrong. Judge the CONTENT, never the sizes. */
    if (!is_text(shipped, want) || !is_text(filebuf, have)) {
        g_puts("--- "); g_puts(path); g_puts("  shipped by "); g_puts(owner);
        g_puts(" ("); g_putn(want); g_putln(" bytes, binary)");
        g_puts("+++ "); g_puts(path); g_puts("  installed now (");
        g_putn(have); g_putln(" bytes, binary)");
        if (want == have) g_putln("    same size, contents differ");
        else if (have < want) g_putln("    SHORTER than it shipped -- truncated?");
        else g_putln("    LONGER than it shipped");
        /* AND STILL SAY WHERE. Returning here left the player with a file
         * declared "binary", no content, and nothing to act on -- a
         * NUL-corrupted unit file is exactly the case where you most need to
         * know which byte went. It falls through to the byte-level report
         * now, which prints a readable name for unprintable bytes. */
    } else {

    g_puts("--- "); g_puts(path); g_puts("  shipped by ");
    g_puts(owner); g_puts(" ("); g_putn(want); g_putln(" bytes)");
    g_write(1, shipped, (u64)want);
    if (want && shipped[want - 1] != '\n') g_puts("\n");
    g_puts("+++ "); g_puts(path); g_puts("  installed now (");
    g_putn(have); g_putln(" bytes)");
    g_write(1, filebuf, (u64)have);
    /* A file with no trailing newline used to glue the next prompt onto the
     * end of its last line. */
    if (have && filebuf[have - 1] != '\n') g_puts("\n");
    }

    /* WHEN THE TWO HALVES LOOK THE SAME, SAY WHAT ACTUALLY DIFFERS.
     * A file truncated by one byte printed two visually identical blocks and
     * the only signal was the byte counts in the headers. A playtester lost
     * ten minutes to that and called it -- rightly -- unfair, because a diff
     * that cannot show you the difference is worse than no diff. */
    int same = (want == have);
    if (same) for (i64 k = 0; k < want; k++) if (shipped[k] != filebuf[k]) { same = 0; break; }
    if (same) { g_putln("    (identical -- nothing to fix here)"); return; }

    i64 lim = want < have ? want : have;
    i64 k = 0;
    while (k < lim && shipped[k] == filebuf[k]) k++;
    if (k == lim && want != have) {
        g_puts("    the two are identical for the first ");
        g_putn(lim);
        g_putln(" bytes and then one of them stops.");
        if (have < want) {
            g_puts("    the installed copy is ");
            g_putn(want - have);
            g_putln(" byte(s) SHORT -- it was truncated, not edited.");
            /* Only say "the trailing newline" when that is ALL that is
             * missing. It was said for a 43-byte truncation -- two whole
             * config lines -- which reads as a stale hardcoded string and
             * cost a playtester real confidence in the tool. */
            if (want - have == 1 && shipped[want - 1] == '\n')
                g_putln("    what is missing is the trailing newline.");
        } else {
            g_puts("    the installed copy has ");
            g_putn(have - want);
            g_putln(" byte(s) MORE on the end.");
        }
    } else {
        g_puts("    first difference at byte ");
        g_putn(k);
        g_putln(":");
        /* Which LINE, because nobody counts bytes. */
        i64 lineno = 1;
        for (i64 q = 0; q < k; q++) if (shipped[q] == '\n') lineno++;
        g_puts("    (line "); g_putn(lineno); g_putln(")");
        g_puts("      shipped   has ");
        show_byte(k < want ? shipped[k] : -1, k < want);
        g_puts("\n      installed has ");
        show_byte(k < have ? filebuf[k] : -1, k < have);
        g_puts("\n");
    }
}

static int verify_one(const char *pkg, int *bad)
{
    if (!read_manifest(pkg)) {
        g_puts("pkg: "); g_puts(pkg);
        g_putln(": no such package (or its manifest is unreadable)");
        return -1;          /* not a finding: a bad question */
    }
    char *p = manifest;
    while (*p) {
        char *nl = p; while (*nl && *nl != '\n') nl++;
        char save = *nl; *nl = 0;
        static char line[300];
        g_copy(line, p, sizeof line);
        *nl = save; p = *nl ? nl + 1 : nl;
        char *t = g_trim(line);
        if (!*t || *t == '#') continue;
        char *mode, *hash, *fp;
        if (!split3(t, &mode, &hash, &fp)) continue;

        /* A symlink is checked by its target, not its contents. stat follows
         * links, so a dangling one fails stat -- which is exactly the report
         * we want, but it has to be attributed to the link itself. */
        static char real[300];
        g_copy(real, root, sizeof real);
        g_cat(real, fp, sizeof real);
        if (g_streq(mode, "link")) {
            static char tgt[256];
            i64 tl = g_readlink(real, tgt, sizeof tgt);
            if (tl < 0)      { finding(pkg, fp, "MISSING (symlink)"); (*bad)++; g_damaged++; }
            else if (g_hash(tgt, (u64)tl) != parse_hex(hash))
                             { finding(pkg, fp, "REPOINTED"); (*bad)++; g_damaged++; }
            continue;
        }

        NomStat st;
        if (g_stat(real, &st) != 0) {
            finding(pkg, fp, "MISSING"); (*bad)++; g_damaged++;
            continue;
        }
        /* A DIRECTORY the package owns. There are no contents to compare, so
         * the questions are: is it still there, is it still a directory, and
         * can anything still get into it. That last one is the fault that
         * used to be invisible -- a directory with its execute bit off hides
         * a tree of perfectly good files and no manifest line mentioned it. */
        if (g_streq(mode, "dir")) {
            if (st.kind != NOM_KIND_DIR) { finding(pkg, fp, "NOT A DIRECTORY"); (*bad)++; g_damaged++; }
            else if ((unsigned)st.mode != parse_oct(hash)) {
                finding(pkg, fp, "MODE");
                g_puts("                 mode is "); g_putoct((unsigned)st.mode, 4);
                g_puts(", package shipped "); g_putoct(parse_oct(hash), 4); g_puts("\n");
                (*bad)++; g_damaged++;
            }
            continue;
        }
        i64 n = g_slurp(real, filebuf, sizeof filebuf);
        if (n < 0) { finding(pkg, fp, "UNREADABLE"); (*bad)++; g_damaged++; continue; }
        unsigned long h = g_hash(filebuf, (u64)n);
        if (h != parse_hex(hash)) {
            /* WHICH WAY does it differ. The pristine copy comes from the
             * repository, not from this disk, so this works on the machine
             * whose disk is the damaged one -- and it is fetched only for a
             * file that has already failed its hash. */
            i64 want = g_repo(pkg, fp, shipped);
            int cut = (want > n);
            for (i64 k = 0; cut && k < n; k++)
                if (shipped[k] != filebuf[k]) cut = 0;
            if (cut) {
                finding(pkg, fp, "TRUNCATED");
                g_puts("                 "); g_putn(want - n);
                g_puts(" byte(s) of "); g_putn(want);
                g_putln(" are gone from the END; the rest is the shipped file");
                (*bad)++; g_cut++;
                if (under_etc(fp)) g_cut_etc++;
            } else {
                finding(pkg, fp, "CHANGED"); (*bad)++;
                if (under_etc(fp)) g_edited++; else g_damaged++;
            }
        } else if ((unsigned)st.mode != parse_oct(mode)) {
            /* THE STATUS WORD WENT MISSING HERE, and only here: a file whose
             * contents are right and whose permissions are wrong printed the
             * package, the path, and then a blank column, with the two modes
             * on the line below. A directory in the same state printed MODE.
             * /usr/share/doc/openssh/known-issues says, in as many words,
             * that verify "says MODE and not CHANGED" -- so the machine was
             * contradicting its own documentation, and anything reading the
             * output a line at a time saw two fields where every other
             * finding has three. (The dead `msg` buffer that used to sit
             * here, holding the string "MODE is ", is where the word went.) */
            finding(pkg, fp, "MODE");
            g_puts("                 mode is "); g_putoct((unsigned)st.mode, 4);
            g_puts(", package shipped "); g_putoct(parse_oct(mode), 4); g_puts("\n");
            (*bad)++; g_damaged++;
        }
    }
    return 1;
}

static void each_package(void (*fn)(const char *))
{
    static char name[64], dir[160];
    g_copy(dir, root, sizeof dir);
    g_cat(dir, "/var/lib/pkg", sizeof dir);
    for (int i = 0; i < 128; i++) {
        if (g_readdir(dir, i, name) < 0) break;
        fn(name);
    }
}

static int g_bad;
static void verify_cb(const char *n) { verify_one(n, &g_bad); }

static void list_cb(const char *n)
{
    static char p2[192], ver[256];
    g_copy(p2, root, sizeof p2);
    g_cat(p2, "/var/lib/pkg/", sizeof p2);
    g_cat(p2, n, sizeof p2);
    g_cat(p2, "/version", sizeof p2);
    g_puts(n);
    for (u64 k = g_strlen(n); k < 18; k++) g_puts(" ");
    if (g_slurp(p2, ver, sizeof ver) > 0) g_puts(g_trim(ver));
    g_puts("\n");
}

void _start(void)
{
    g_getarg(arg, sizeof arg);
    char *v[GARGS];
    int n = g_argv(arg, v);
    root[0] = 0;
    if (n >= 2 && g_streq(v[0], "--root")) {
        g_copy(root, v[1], sizeof root);
        for (int i = 0; i + 2 < n; i++) v[i] = v[i + 2];
        n -= 2;
    }
    if (n < 1) {
        g_putln("usage: pkg [--root DIR] list|owns|verify|diff <path>|<pkg>|reinstall [--force]|upgrade");
        g_putln("  --root repairs a filesystem mounted elsewhere, without");
        g_putln("         chrooting into it -- which you cannot do when the");
        g_putln("         disk's own libc is broken");
        g_exit(1);
    }

    if (!root_readable()) {
        g_puts("pkg: "); g_puts(root);
        g_putln(" has no package database -- there is no");
        g_puts("     "); g_puts(root); g_putln("/var/lib/pkg to read.");
        g_putln("");
        g_putln("Either nothing is mounted there, or what is mounted is not a");
        g_putln("NomnixOS root. Every answer this tool gives comes out of that");
        g_putln("directory, so without it there is nothing to say -- and a");
        g_putln("confident answer about a filesystem that is not there is worse");
        g_putln("than no answer.");
        g_puts("     mount /dev/sda1 "); g_putln(root);
        g_puts("     ls "); g_putln(root);
        g_exit(2);
    }

    if (g_streq(v[0], "list")) { each_package(list_cb); g_exit(0); }

    if (g_streq(v[0], "owns") || g_streq(v[0], "owner")) {
        if (n < 2) { g_putln("usage: pkg owns <path>"); g_exit(1); }
        static char name[64];
        for (int i = 0; i < 128; i++) {
            rooted("/var/lib/pkg");
            static char pkgdir[160];
            g_copy(pkgdir, path, sizeof pkgdir);
            if (g_readdir(pkgdir, i, name) < 0) break;
            if (!read_manifest(name)) continue;
            char *p = manifest;
            while (*p) {
                char *nl = p; while (*nl && *nl != '\n') nl++;
                char save = *nl; *nl = 0;
                static char line[300];
                g_copy(line, p, sizeof line);
                *nl = save; p = *nl ? nl + 1 : nl;
                char *a, *b, *c;
                char *t = g_trim(line);
                if (!*t || !split3(t, &a, &b, &c)) continue;
                if (g_streq(c, v[1])) { g_putln(name); g_exit(0); }
            }
        }
        /* Asking about a directory is a reasonable question, so answer it:
         * which packages own anything underneath. */
        u64 qlen = g_strlen(v[1]);
        int found = 0;
        for (int i = 0; i < 128; i++) {
            /* THROUGH --root, like the loop above it. This one read the
             * WORKSTATION'S database while claiming to answer about the
             * mounted disk, so `pkg --root /mnt owns /etc` listed packages
             * off the rescue medium. */
            rooted("/var/lib/pkg");
            static char pkgdir2[160];
            g_copy(pkgdir2, path, sizeof pkgdir2);
            if (g_readdir(pkgdir2, i, name) < 0) break;
            if (!read_manifest(name)) continue;
            char *p = manifest;
            int hit = 0;
            while (*p && !hit) {
                char *nl = p; while (*nl && *nl != '\n') nl++;
                char save = *nl; *nl = 0;
                static char line[300];
                g_copy(line, p, sizeof line);
                *nl = save; p = *nl ? nl + 1 : nl;
                char *a, *b, *cc;
                char *t = g_trim(line);
                if (!*t || !split3(t, &a, &b, &cc)) continue;
                u64 m2 = 0;
                while (m2 < qlen && cc[m2] == v[1][m2]) m2++;
                if (m2 == qlen && (cc[m2] == '/' || qlen == 1)) hit = 1;
            }
            if (hit) { g_puts("  "); g_putln(name); found++; }
        }
        if (!found) {
            /* A playtester hit an orphan service four times and never worked
             * out that nothing owning it was the CLUE. Say so. */
            g_putln("no package owns that path");
            g_putln("");
            g_putln("nothing installed this file. If the system is trying to");
            g_putln("use it, either it was dropped there by hand or by an");
            g_putln("installer that is not managed here -- and removing it is");
            g_putln("usually safe. `rm <path>` if you are sure.");
        }
        else { g_puts("(packages owning files under "); g_puts(v[1]); g_putln(")"); }
        g_exit(found ? 0 : 1);
    }

    if (g_streq(v[0], "upgrade")) {
        /* Refetch every file from the repository. What arrives depends on the
         * CHANNEL in /etc/pkg/repos.d, so this is exactly as safe or as
         * dangerous as that configuration is. */
        static char rp[192], nm3[64];
        int files = 0, pkgs = 0;
        rooted("/var/lib/pkg");
        static char pdir[192];
        g_copy(pdir, path, sizeof pdir);
        for (int i = 0; i < 128; i++) {
            if (g_readdir(pdir, i, nm3) < 0) break;
            if (!read_manifest(nm3)) continue;
            pkgs++;
            char *p = manifest;
            while (*p) {
                char *nl = p; while (*nl && *nl != '\n') nl++;
                char save = *nl; *nl = 0;
                static char line[300];
                g_copy(line, p, sizeof line);
                *nl = save; p = *nl ? nl + 1 : nl;
                char *mode, *hash, *fp;
                char *t = g_trim(line);
                if (!*t || !split3(t, &mode, &hash, &fp)) continue;
                if (g_streq(mode, "link") || g_streq(mode, "dir")) continue;
                i64 got = g_repo(nm3, fp, filebuf);
                if (got < 0) continue;
                g_copy(rp, root, sizeof rp);
                g_cat(rp, fp, sizeof rp);
                int fd = g_open(rp, O_WRONLY | O_CREAT | O_TRUNC);
                if (fd < 0) continue;
                sysc(SYS_write, fd, (i64)filebuf, got);
                g_close(fd);
                sysc(SYS_chmod, (i64)rp, (i64)parse_oct(mode), 0);
                files++;
            }
        }
        g_putn(pkgs); g_puts(" packages, "); g_putn(files);
        g_putln(" files fetched from the configured repository");
        g_putln("(what you get depends on the channel in /etc/pkg/repos.d)");
        g_exit(0);
    }

    if (g_streq(v[0], "diff")) {
        /* Show what a CHANGED file actually says, against what the package
         * shipped. This is the tool that makes local edits fair: a diff that
         * reads like an admin's deliberate change ("# hardened after the
         * audit") is not the same as one that reads like damage, and only a
         * person can tell the difference. */
        if (n < 2) { g_putln("usage: pkg diff <path>|<package>"); g_exit(1); }

        /* An argument with no slash in it is a PACKAGE, not a path. The game's
         * own advice says "`pkg diff` first, then `pkg reinstall --force
         * <name>`", which reads -- correctly -- as though both take a package.
         * A playtester ran `pkg diff shadow` three times, got "no package owns
         * that path", and wrote the feature off as broken. It was not broken;
         * it only answered a question nobody was asking. So: diff a name and
         * you get every file in that package that no longer matches. */
        int by_name = 1;
        for (const char *q = v[1]; *q; q++) if (*q == 0x2f) by_name = 0;
        if (by_name) {
            if (!read_manifest(v[1])) {
                g_puts("pkg: "); g_puts(v[1]);
                g_putln(": no such package, and not a path either");
                g_exit(1);
            }
            static char paths[64][160];
            int np = 0;
            char *p = manifest;
            while (*p && np < 64) {
                char *nl = p; while (*nl && *nl != '\n') nl++;
                char save = *nl; *nl = 0;
                static char line[300];
                g_copy(line, p, sizeof line);
                *nl = save; p = *nl ? nl + 1 : nl;
                char *mode, *hash, *fp;
                char *t = g_trim(line);
                if (!*t || *t == '#' || !split3(t, &mode, &hash, &fp)) continue;
                if (g_streq(mode, "link") || g_streq(mode, "dir")) continue;
                static char rp[300];
                g_copy(rp, root, sizeof rp);
                g_cat(rp, fp, sizeof rp);
                i64 hn = g_slurp(rp, filebuf, sizeof filebuf);
                if (hn < 0) continue;
                if (g_hash(filebuf, (u64)hn) != parse_hex(hash))
                    g_copy(paths[np++], fp, sizeof paths[0]);
            }
            if (!np) {
                g_puts("pkg: every file in "); g_puts(v[1]);
                g_putln(" matches what was shipped -- nothing to diff");
                g_exit(0);
            }
            for (int i = 0; i < np; i++) diff_path(v[1], paths[i]);
            g_exit(0);
        }

        static char owner[64];
        owner[0] = 0;
        static char nm2[64], ddir[192];
        g_copy(ddir, root, sizeof ddir);
        g_cat(ddir, "/var/lib/pkg", sizeof ddir);
        for (int i = 0; i < 128 && !owner[0]; i++) {
            if (g_readdir(ddir, i, nm2) < 0) break;
            if (!read_manifest(nm2)) continue;
            char *p = manifest;
            while (*p) {
                char *nl = p; while (*nl && *nl != '\n') nl++;
                char save = *nl; *nl = 0;
                static char line[300];
                g_copy(line, p, sizeof line);
                *nl = save; p = *nl ? nl + 1 : nl;
                char *a, *b, *cc;
                char *t = g_trim(line);
                if (!*t || !split3(t, &a, &b, &cc)) continue;
                if (g_streq(cc, v[1])) { g_copy(owner, nm2, sizeof owner); break; }
            }
        }
        if (!owner[0]) { g_putln("pkg: no package owns that path"); g_exit(1); }

        diff_path(owner, v[1]);
        g_exit(0);
    }


    if (g_streq(v[0], "verify")) {
        g_bad = 0; g_edited = 0; g_damaged = 0; g_cut = 0; g_cut_etc = 0;
        if (n >= 2) {
            if (verify_one(v[1], &g_bad) < 0) g_exit(1);   /* unknown package */
        } else {
            each_package(verify_cb);
        }
        if (!g_bad) g_putln("all files match their packages");
        else {
            /* WHAT REINSTALL WILL ACTUALLY DO WITH EACH OF THESE.
             *
             * The old line said "`pkg reinstall <package>` puts them back" of
             * every finding, and reinstall refuses to put back an edited /etc
             * file -- on purpose, loudly, with `keeping locally modified` --
             * so the tool's own closing advice taught the opposite of the
             * tool's behaviour. Whichever the player believed, one of them
             * was lying to them. */
            g_puts("\n"); g_putn(g_bad); g_putln(" file(s) differ.");
            if (g_damaged) {
                g_puts("  "); g_putn(g_damaged);
                g_putln(" missing, unreadable, repointed or the wrong mode: that is");
                g_putln("  damage rather than somebody's decision, and");
                g_putln("  `pkg reinstall <package>` puts those back.");
            }
            if (g_cut) {
                g_puts("  "); g_putn(g_cut);
                g_putln(" cut short: what is on the disk is the shipped file byte for");
                g_putln("  byte, and then it stops. That is the shape an interrupted");
                g_putln("  write leaves -- a power cut, a full disk, a disk that gave");
                g_putln("  up mid-write. It is not proof: somebody who deleted the end");
                g_putln("  of the file by hand would leave the same bytes, and nothing");
                g_putln("  here can tell those two apart. `pkg diff <path>` prints how");
                g_putln("  much is missing so you can judge it.");
                if (g_cut_etc) {
                    g_putln("  These are under /etc, so a plain reinstall would KEEP the");
                    g_putln("  short copy. `pkg reinstall --force <package>` puts the whole");
                    g_putln("  file back and leaves the short one as <path>.pkgsave.");
                } else {
                    g_putln("  `pkg reinstall <package>` puts the whole file back.");
                }
            }
            if (g_edited) {
                g_puts("  "); g_putn(g_edited);
                g_putln(" under /etc with CHANGED contents: a reinstall does NOT put");
                g_putln("  those back. It keeps a local edit and says `keeping locally");
                g_putln("  modified`, because the edit may be a decision somebody made");
                g_putln("  on purpose. `pkg diff <path>` shows what changed. If the edit");
                g_putln("  IS the fault, either edit it back, or");
                g_putln("  `pkg reinstall --force <package>` -- which overwrites it and");
                g_putln("  leaves the old copy beside it as <path>.pkgsave.");
            }
        }
        g_exit(g_bad ? 1 : 0);
    }

    if (g_streq(v[0], "reinstall")) {
        /* --force overwrites configuration files that have been edited. Without
         * it they are left alone, which is what dpkg does and for the same
         * reason: a package ships a default, an administrator makes a decision,
         * and a reinstall that silently reverts the decision has destroyed
         * somebody's work to fix a problem that was somewhere else. */
        int force = 0;
        for (int i = 1; i < n; i++) {
            if (g_streq(v[i], "--force") || g_streq(v[i], "-f")) {
                force = 1;
                for (int k = i; k + 1 < n; k++) v[k] = v[k + 1];
                n--;
                break;
            }
        }
        if (n < 2) { g_putln("usage: pkg reinstall <name>"); g_exit(1); }
        if (!read_manifest(v[1])) {
            g_puts("pkg: "); g_puts(v[1]); g_putln(": no such package");
            g_exit(1);
        }
        int done = 0, failed = 0, kept = 0, saved = 0, hostsaved = 0;
        char *p = manifest;
        while (*p) {
            char *nl = p; while (*nl && *nl != '\n') nl++;
            char save = *nl; *nl = 0;
            static char line[300];
            g_copy(line, p, sizeof line);
            *nl = save; p = *nl ? nl + 1 : nl;
            char *mode, *hash, *fp;
            char *t = g_trim(line);
            if (!*t || !split3(t, &mode, &hash, &fp)) continue;
            /* Pull the pristine bytes from the repository, which is not on
             * this disk -- that is why this works on a wrecked machine. */
            /* A configuration file is one under /etc. If it differs from what
             * the package shipped, somebody changed it on purpose until
             * proven otherwise. */
            if (!force && fp[0] == '/' && fp[1] == 'e' && fp[2] == 't' &&
                fp[3] == 'c' && fp[4] == '/') {
                static char cur[300];
                g_copy(cur, root, sizeof cur);
                g_cat(cur, fp, sizeof cur);
                i64 have = g_slurp(cur, filebuf, sizeof filebuf);
                if (have >= 0 && g_hash(filebuf, (u64)have) != parse_hex(hash)) {
                    g_puts("  keeping locally modified ");
                    g_putln(fp);
                    kept++;
                    continue;
                }
            }
            if (g_streq(mode, "link") || g_streq(mode, "dir")) {
                /* Restored explicitly, through the call whose whole job is to
                 * write. Fetching used to do this as a side effect, which
                 * meant `pkg diff` on a dangling symlink repaired it. */
                if (sysc(SYS_restore, (i64)v[1], (i64)fp, 0) != 0) {
                    g_puts("  cannot restore "); g_putln(fp); failed++;
                } else done++;
                continue;
            }
            static char rp[300];
            g_copy(rp, root, sizeof rp);
            g_cat(rp, fp, sizeof rp);

            /* THE ONE FILE WHOSE CONTENTS ARE THE MACHINE'S NAME.
             *
             * A playtester force-reinstalled `filesystem` on a server they
             * had called srv3 and the box came back called node-4097. That
             * is CORRECT: /etc/hostname is package content here -- the
             * filesystem package ships each machine's factory identity, the
             * way aaa_base ships /etc/HOSTNAME -- so --force puts the factory
             * name back like any other overwritten config. What was wrong is
             * that nothing said so. One line about a .pkgsave is not "this
             * machine is now called something else", and everything that
             * reads the name (`uname -n`, the login banner, getty) reads this
             * file, so the rename is silent until the next thing to print it.
             *
             * Remembered BEFORE the write, because after it the old name is
             * only in the .pkgsave. */
            static char oldhost[128];
            int ishost = g_streq(fp, "/etc/hostname");
            oldhost[0] = 0;
            if (ishost) {
                i64 hn = g_slurp(rp, oldhost, sizeof oldhost - 1);
                if (hn >= 0) oldhost[hn] = 0; else oldhost[0] = 0;
            }

            /* --force ON AN EDITED CONFIG KEEPS A COPY.
             *
             * dpkg writes .dpkg-old, rpm writes .rpmsave, and both do it for
             * the reason a playtester ran into head-on: --force is the only
             * way past a config the package manager is protecting, it has no
             * undo, and the message telling you off afterwards is the first
             * you hear of it. Now there is something to go back to.
             *
             * Only for /etc, only when it actually differs, and never for a
             * .pkgsave of a .pkgsave. */
            if (force && fp[0] == '/' && fp[1] == 'e' && fp[2] == 't' &&
                fp[3] == 'c' && fp[4] == '/' && !g_endswith(fp, ".pkgsave")) {
                static char sv[65536];
                i64 cur = g_slurp(rp, sv, sizeof sv);
                if (cur >= 0 && g_hash(sv, (u64)cur) != parse_hex(hash)) {
                    static char svp[320];
                    g_copy(svp, rp, sizeof svp);
                    g_cat(svp, ".pkgsave", sizeof svp);
                    int sfd = g_open(svp, O_WRONLY | O_CREAT | O_TRUNC);
                    if (sfd >= 0) {
                        sysc(SYS_write, sfd, (i64)sv, cur);
                        g_close(sfd);
                        g_puts("  saved your "); g_puts(fp);
                        g_puts(" as "); g_puts(fp); g_putln(".pkgsave");
                        saved++;
                        if (ishost) hostsaved = 1;
                    }
                }
            }

            i64 got = g_repo(v[1], fp, filebuf);
            if (got < 0) { g_puts("  cannot fetch "); g_putln(fp); failed++; continue; }
            int fd = g_open(rp, O_WRONLY | O_CREAT | O_TRUNC);
            if (fd < 0) { g_puts("  cannot write "); g_putln(rp); failed++; continue; }
            sysc(SYS_write, fd, (i64)filebuf, got);
            g_close(fd);
            sysc(SYS_chmod, (i64)rp, (i64)parse_oct(mode), 0);
            done++;

            /* AND IF THAT FILE WAS THE MACHINE'S NAME, SAY THE MACHINE HAS
             * BEEN RENAMED. Both names, in the same sentence, because the
             * whole harm is that the box now answers to one name here and is
             * known by another everywhere else. */
            if (ishost) {
                static char newhost[128];
                i64 c = got < (i64)sizeof newhost - 1 ? got : (i64)sizeof newhost - 1;
                for (i64 k = 0; k < c; k++) newhost[k] = filebuf[k];
                newhost[c] = 0;
                static char was[128];
                g_copy(was, g_trim(oldhost), sizeof was);
                char *now = g_trim(newhost);
                if (was[0] && !g_streq(was, now)) {
                    g_putln("  ** THIS MACHINE HAS BEEN RENAMED.");
                    g_puts("  ** it was `"); g_puts(was);
                    g_puts("` and it is now `"); g_puts(now); g_putln("`.");
                    g_putln("  ** /etc/hostname is package content: the filesystem package");
                    g_putln("  ** ships each machine's factory name, so a forced reinstall");
                    g_putln("  ** puts the factory name back like any other config file.");
                    g_putln("  ** `uname -n` and the login banner read that file and will");
                    g_putln("  ** say the new name from now on. Nothing outside this");
                    g_putln("  ** machine has been told, so anything that knew it by the");
                    g_putln("  ** old name still calls it that.");
                    if (hostsaved) {
                        g_puts("  **   cp /etc/hostname.pkgsave /etc/hostname");
                        g_putln("   puts it back");
                    } else {
                        g_puts("  **   echo "); g_puts(was);
                        g_putln(" > /etc/hostname   puts it back");
                    }
                }
            }
        }
        g_puts(v[1]); g_puts(": "); g_putn(done); g_puts(" files restored");
        if (kept)   { g_puts(", "); g_putn(kept); g_puts(" kept"); }
        if (failed) { g_puts(", "); g_putn(failed); g_puts(" failed"); }
        g_puts("\n");
        if (saved) {
            g_putln("  the edited copies are beside the originals as .pkgsave --");
            g_putln("  `cat <file>.pkgsave` to see what was there, and");
            g_putln("  `cp <file>.pkgsave <file>` to put it back.");
        }
        if (kept) {
            g_putln("  those files were edited on this machine and have been");
            g_putln("  left alone. If one of them is the fault, look at it with");
            g_putln("  `pkg diff` first, then `pkg reinstall --force <name>`.");
        }
        g_exit(failed ? 1 : 0);
    }

    g_puts("pkg: unknown command: "); g_putln(v[0]);
    g_exit(1);
}

/* /bin/stat — what the kernel thinks a path is.
 *
 * A SYMLINK IS A KIND, NOT A DETOUR.
 *
 * ~nomowner/notes.txt sends the player here in as many words: "/boot/vmnomuz
 * is a SYMLINK. When somebody deletes the versioned image, `ls /boot` looks
 * completely fine. stat it." And stat printed `kind file`, the mode and size
 * of whatever was on the far end, and not one word about the link -- so `ls`
 * knew it was a link and where it pointed, and the tool the game teaches you
 * to reach for was the one that hid it. A dangling kernel symlink is a real
 * fault in this game; this is the command for finding it.
 *
 * So stat does NOT follow the link. It says it is a link, says what it names,
 * and then says whether that name resolves -- and when it does, it describes
 * the target too, because "it is a link and it is fine" is also an answer.
 */
#include "gsys.h"
static char arg[GARG_MAX];
static char target[256];          /* NOM_PATH_MAX on the host side */

static void kindline(const char *label, const NomStat *st)
{
    g_puts(label);
    g_putln(st->kind == NOM_KIND_DIR  ? "directory" :
            st->kind == NOM_KIND_LINK ? "symlink" :
            st->kind == NOM_KIND_DEV  ? "device" : "file");
}

static int one(const char *path)
{
    /* readlink first, because g_stat FOLLOWS and would answer about the far
     * end -- or fail outright when there is no far end, which is the case
     * worth naming most clearly of all. */
    i64 tl = g_readlink(path, target, sizeof target);
    g_puts("path  "); g_putln(path);

    NomStat st;
    if (tl > 0) {
        g_putln("kind  symlink");
        g_puts("->    "); g_putln(target);
        if (g_stat(path, &st) != 0) {
            g_putln("      THE TARGET IS NOT THERE -- this link is dangling.");
            g_putln("      `ls` shows the link and says nothing about that;");
            g_putln("      whatever reads this path gets nothing.");
            return 1;
        }
        kindline("target it resolves to  ", &st);
        g_puts("target mode  "); g_putoct((unsigned)st.mode, 4);
        g_puts(st.mode & 0111 ? "  (executable)" : "  (not executable)");
        g_puts("\n");
        g_puts("target size  "); g_putn(st.size); g_puts("\n");
        return 0;
    }

    if (g_stat(path, &st) != 0) {
        g_puts("stat: "); g_puts(path); g_putln(": no such file");
        return 1;
    }
    kindline("kind  ", &st);
    g_puts("mode  "); g_putoct((unsigned)st.mode, 4);
    g_puts(st.mode & 0111 ? "  (executable)" : "  (not executable)");
    g_puts("\n");
    g_puts("size  "); g_putn(st.size); g_puts("\n");
    return 0;
}

void _start(void)
{
    g_getarg(arg, sizeof arg);
    char *v[GARGS];
    int n = g_argv(arg, v);
    if (n < 1) { g_putln("usage: stat <path>..."); g_exit(1); }
    g_argv_warn("stat");
    /* EVERY path, not the first one. `stat a b` used to describe a and drop b
     * without a word, which is the same silence this file exists to end. */
    int bad = 0;
    for (int i = 0; i < n; i++) {
        if (i) g_putln("");
        bad |= one(v[i]);
    }
    g_exit(bad);
}

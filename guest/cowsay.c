/* /usr/bin/cowsay — a cow says what you tell it to.
 *
 *   cowsay hello world
 *   cowsay -f tux the kernel is fine
 *   fortune | cowsay
 *
 * It reads stdin when given no words, which is the only reason it is worth
 * having: `fortune | cowsay` is the joke, and a pipeline stage on this machine
 * gets its stdin the same way grep and wc do.
 *
 * The balloon is MEASURED, not drawn. Text wraps at 40 columns, the box is as
 * wide as the longest line that came out, and a multi-line balloon uses the
 * /\ || \/ corners a single-line one does not -- because a cow with a
 * hand-drawn fixed-width box would only look right for one sentence, and the
 * first thing anybody does is type a long one.
 */
#include "gsys.h"

#define WRAP  40
#define MAXL  24

static char arg[1024];
static char text[2048];
static char wrapped[2048];
static char *line[MAXL];
static char face[32];

/* Copy the argument text, removing the quoting the shell would have removed
 * had we gone through g_argv -- which we cannot, because g_argv stops at eight
 * words and a cow should be able to say a whole sentence. */
static void dequote(const char *s, char *d, u64 cap)
{
    u64 j = 0;
    char q = 0;
    for (u64 i = 0; s[i] && j + 1 < cap; i++) {
        char c = s[i];
        if (q) { if (c == q) { q = 0; continue; } }
        else if (c == '\'' || c == '"') { q = c; continue; }
        else if (c == '\\' && s[i + 1]) { c = s[++i]; }
        d[j++] = c;
    }
    d[j] = 0;
}

static void repeat(const char *c, int n)
{
    for (int i = 0; i < n; i++) g_puts(c);
}

/* One balloon line: the left border, the text padded to the box width, the
 * right border. */
static void balloon_line(const char *l, const char *r, const char *s, int w)
{
    g_puts(l);
    g_puts(" ");
    g_puts(s);
    int pad = w - (int)g_strlen(s);
    for (int i = 0; i < pad; i++) g_puts(" ");
    g_puts(" ");
    g_putln(r);
}

void _start(void)
{
    g_getarg(arg, sizeof arg);
    char *p = arg;
    while (*p == ' ' || *p == '\t') p++;

    g_copy(face, "cow", sizeof face);
    /* -f NAME, or -fNAME. Any other flag is skipped: an unknown flag printing
     * as part of the cow's sentence looks like a bug in the cow. */
    while (*p == '-' && p[1]) {
        if (p[1] == 'f') {
            p += 2;
            while (*p == ' ' || *p == '\t') p++;
            u64 k = 0;
            while (*p && *p != ' ' && *p != '\t' && k + 1 < sizeof face)
                face[k++] = *p++;
            face[k] = 0;
        } else {
            while (*p && *p != ' ' && *p != '\t') p++;
        }
        while (*p == ' ' || *p == '\t') p++;
    }

    if (*p) {
        dequote(p, text, sizeof text);
    } else {
        /* No words: we are a pipeline stage. */
        i64 got = g_slurp_stdin(text, sizeof text);
        if (got <= 0) g_copy(text, "moo", sizeof text);
    }
    /* Newlines and tabs become spaces: the balloon does its own line breaks,
     * and a raw newline inside it would tear the box open. */
    for (u64 i = 0; text[i]; i++)
        if (text[i] == '\n' || text[i] == '\t' || text[i] == '\r') text[i] = ' ';

    /* Wrap. Words longer than the box are cut, because the alternative is a
     * balloon wider than the terminal. */
    int nl = 0, col = 0;
    u64 w = 0;
    const char *s = text;
    while (*s == ' ') s++;
    line[nl++] = &wrapped[w];
    while (*s && nl <= MAXL) {
        /* Room for the widest thing this iteration can write: a separating
         * space, the word, and the terminator. Guests own their buffers and
         * nothing here will stop us running off the end of one. */
        if (w + WRAP + 4 >= sizeof wrapped) break;
        u64 wl = 0;
        while (s[wl] && s[wl] != ' ') wl++;
        if (wl > WRAP) wl = WRAP;
        if (col && col + 1 + (int)wl > WRAP) {
            wrapped[w++] = 0;
            if (nl >= MAXL) break;
            line[nl++] = &wrapped[w];
            col = 0;
        }
        if (col) { wrapped[w++] = ' '; col++; }
        for (u64 i = 0; i < wl && w + 2 < sizeof wrapped; i++) wrapped[w++] = s[i];
        col += (int)wl;
        s += wl;
        while (*s == ' ') s++;
    }
    wrapped[w] = 0;

    int width = 0;
    for (int i = 0; i < nl; i++) {
        int l = (int)g_strlen(line[i]);
        if (l > width) width = l;
    }
    if (width < 1) { width = 3; g_copy(wrapped, "moo", sizeof wrapped); line[0] = wrapped; nl = 1; }

    g_puts(" ");
    repeat("_", width + 2);
    g_putln("");
    if (nl == 1) {
        balloon_line("<", ">", line[0], width);
    } else {
        for (int i = 0; i < nl; i++) {
            const char *l = i == 0 ? "/" : (i == nl - 1 ? "\\" : "|");
            const char *r = i == 0 ? "\\" : (i == nl - 1 ? "/" : "|");
            balloon_line(l, r, line[i], width);
        }
    }
    g_puts(" ");
    repeat("-", width + 2);
    g_putln("");

    /* The tail hangs off the bottom left of the balloon, as it does in the
     * original, so the animal is attached to what it said. */
    if (g_streq(face, "tux")) {
        g_putln("   \\");
        g_putln("    \\   .--.");
        g_putln("       |o_o |");
        g_putln("       |:_/ |");
        g_putln("      //   \\ \\");
        g_putln("     (|     | )");
        g_putln("    /'\\_   _/`\\");
        g_putln("    \\___)=(___/");
    } else if (g_streq(face, "dragon")) {
        g_putln("      \\                 / \\  //\\");
        g_putln("       \\    |\\___/|    /   \\//  \\\\");
        g_putln("            /0  0  \\__/    //  | \\ \\");
        g_putln("           /     /  \\/_/  //   |  \\  \\");
        g_putln("           @_^_@'/   \\/_ //    |   \\   \\");
        g_putln("           //_^_/     \\/_//    |    \\   |");
        g_putln("        ( //) |         \\//    |     \\  |");
        g_putln("      (/ /) _|_ /   )   //     |      \\  \\");
        g_putln("    (// /) '/,_ _ _/  ( ; -.   |     _ _\\.-~");
    } else if (g_streq(face, "daemon")) {
        g_putln("   \\         ,        ,");
        g_putln("    \\       /(        )`");
        g_putln("            \\ \\___   / |");
        g_putln("            /- _  `-/  '");
        g_putln("           (/\\/ \\ \\   /\\");
        g_putln("           / /   | `    \\");
        g_putln("           O O   ) /    |");
        g_putln("           `-^--'`<     '");
        g_putln("          (_.)  _  )   /");
        g_putln("           `.___/`    /");
    } else {
        g_putln("        \\   ^__^");
        g_putln("         \\  (oo)\\_______");
        g_putln("            (__)\\       )\\/\\");
        g_putln("                ||----w |");
        g_putln("                ||     ||");
    }
    g_exit(0);
}

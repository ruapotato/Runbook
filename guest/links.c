/* /usr/bin/links — the text browser.
 *
 * Resolution is done HERE, by reading the machine's own /etc/hosts and then
 * falling back to the nameserver in /etc/resolv.conf. That is deliberate: it
 * makes both files load-bearing, so "I can reach it by address but not by
 * name" is a real state of this machine and a real thing to diagnose.
 *
 * Pages are markup, not plain text. The subset is small enough to parse
 * honestly in one pass with no allocator, and it is the SAME subset the
 * desktop browser draws -- which is the point: there is one web, stored once,
 * and two renderers for it.
 *
 *   <h1> <h2>                 headings
 *   <p>                       a paragraph, wrapped to fit
 *   <ul> <li>                 a bulleted list
 *   <pre>                     verbatim: commands, logs, ASCII art
 *   <hr>                      a rule
 *   <b> <i>                   emphasis
 *   <a href="host/path">      a link; href may be "/path" for this host
 *   <img src=".." alt="..">   an image nobody can see
 *   &lt; &gt; &amp; &quot;    the four entities that matter
 *
 * Rendering here is what a text browser has always done: headings underlined,
 * paragraphs wrapped, list items bulleted, and every link followed by where
 * it actually goes, because a player with no desktop must be able to read the
 * whole web and type the next address themselves. `links --raw` prints the
 * markup untouched, which is how the desktop browser fetches: the page comes
 * out of the machine either way, and the window is only ever a view of it.
 */
#define NOM_NEEDS_LIBZ   /* gzips what it writes */
#include "gsys.h"

static char arg[256], hosts[4096], resolv[256], page[65536], ipbuf[64];
static char host[128], path[192];

/* Look a name up in /etc/hosts: "<address> <name> [alias...]" per line. */
static int hosts_lookup(const char *name, char *out, u64 cap)
{
    if (g_slurp("/etc/hosts", hosts, sizeof hosts) < 0) return 0;
    char *p = hosts;
    while (*p) {
        char *nl = p; while (*nl && *nl != '\n') nl++;
        char save = *nl; *nl = 0;
        static char line[256];
        g_copy(line, p, sizeof line);
        *nl = save; p = *nl ? nl + 1 : nl;
        char *t = g_trim(line);
        if (!*t || *t == '#') continue;
        char *v[GARGS];
        int n = g_argv(t, v);
        for (int i = 1; i < n; i++)
            if (g_streq(v[i], name)) { g_copy(out, v[0], cap); return 1; }
    }
    return 0;
}

static int have_nameserver(void)
{
    if (g_slurp("/etc/resolv.conf", resolv, sizeof resolv) < 0) return 0;
    char *p = resolv;
    while (*p) {
        char *nl = p; while (*nl && *nl != '\n') nl++;
        char save = *nl; *nl = 0;
        char *t = g_trim(p);
        int ok = (t[0]=='n'&&t[1]=='a'&&t[2]=='m'&&t[3]=='e'&&t[4]=='s'&&
                  t[5]=='e'&&t[6]=='r'&&t[7]=='v'&&t[8]=='e'&&t[9]=='r');
        *nl = save; p = *nl ? nl + 1 : nl;
        if (ok) return 1;
    }
    return 0;
}

/* --- the renderer ---------------------------------------------------- *
 *
 * One pass, three little buffers, no allocation. `word` collects characters
 * until whitespace ends them, `lbuf` collects words until the line is full,
 * and `out` collects lines until the page is done and gets written once.
 */

#define WRAP 76

static char out[98304];
static u64  outn;

static void o_ch(char c)            { if (outn + 1 < sizeof out) out[outn++] = c; }
static void o_rep(char c, int n)    { for (int i = 0; i < n; i++) o_ch(c); }

static char lbuf[512];
static int  llen, line_ind, have_line;
static int  cur_ind, cont_ind;   /* indent of a block's first / later lines */
static int  pending_blank;       /* a blank line owed before the next block */

static void line_open(int ind)
{
    llen = 0;
    for (int i = 0; i < ind && i < 400; i++) lbuf[llen++] = ' ';
    line_ind = llen;
    have_line = 1;
}

static void line_end(void)
{
    if (!have_line) return;
    while (llen > 0 && lbuf[llen - 1] == ' ') llen--;
    for (int i = 0; i < llen; i++) o_ch(lbuf[i]);
    o_ch('\n');
    llen = 0; have_line = 0;
}

/* The blank line between blocks is owed, not spent, until a block actually
 * produces a character. An empty <p></p> or a list of nothing therefore
 * leaves no hole in the page, and the page never starts with a blank line. */
static void blank_now(void)
{
    if (pending_blank && outn) o_ch('\n');
    pending_blank = 0;
}

static void wput(const char *w, int n)
{
    if (n <= 0) return;
    blank_now();
    if (!have_line) line_open(cur_ind);
    int sp = (llen > line_ind) ? 1 : 0;
    if (llen + sp + n > WRAP && llen > line_ind) {
        line_end();
        line_open(cont_ind);
        sp = 0;
    }
    if (sp) lbuf[llen++] = ' ';
    for (int i = 0; i < n && llen < (int)sizeof lbuf - 1; i++) lbuf[llen++] = w[i];
}

static char word[256];  static int wlen;
static char ltext[256]; static int ltlen;   /* the text inside the current <a> */
static char href[192];  static int in_a;

static void w_ch(char c)
{
    if (wlen < (int)sizeof word - 1) word[wlen++] = c;
    if (in_a && ltlen < (int)sizeof ltext - 1) ltext[ltlen++] = c;
}

static void w_end(void) { if (wlen) { wput(word, wlen); wlen = 0; } }

static void block_end(void) { w_end(); line_end(); pending_blank = 1; }

/* Pull an attribute out of the inside of a tag: attr("a href=\"x\"", "href"). */
static void attr(const char *tag, const char *name, char *dst, u64 cap)
{
    dst[0] = 0;
    for (u64 i = 0; tag[i]; i++) {
        u64 k = 0;
        while (name[k] && tag[i + k] == name[k]) k++;
        if (name[k]) continue;
        u64 j = i + k;
        while (tag[j] == ' ') j++;
        if (tag[j] != '=') continue;
        j++;
        while (tag[j] == ' ') j++;
        char q = 0;
        if (tag[j] == '"' || tag[j] == '\'') q = tag[j++];
        u64 n = 0;
        while (tag[j] && n + 1 < cap && (q ? tag[j] != q : tag[j] != ' '))
            dst[n++] = tag[j++];
        dst[n] = 0;
        return;
    }
}

/* Tag name comparison, ignoring whatever attributes follow it. */
static int tag_is(const char *tag, const char *name)
{
    u64 i = 0;
    while (name[i] && tag[i] == name[i]) i++;
    return !name[i] && (tag[i] == 0 || tag[i] == ' ' || tag[i] == '/');
}

/* &lt; and friends. Returns how many characters were consumed, 0 if this is
 * a bare ampersand -- which pages full of shell one-liners contain a lot of. */
static int entity(const char *p, char *c)
{
    if (p[0] != '&') return 0;
    if (p[1]=='l'&&p[2]=='t'&&p[3]==';')                { *c='<'; return 4; }
    if (p[1]=='g'&&p[2]=='t'&&p[3]==';')                { *c='>'; return 4; }
    if (p[1]=='a'&&p[2]=='m'&&p[3]=='p'&&p[4]==';')     { *c='&'; return 5; }
    if (p[1]=='q'&&p[2]=='u'&&p[3]=='o'&&p[4]=='t'&&p[5]==';') { *c='"'; return 6; }
    return 0;
}

/* Verbatim, up to </pre>. Indented two spaces so a code block reads as one,
 * and never wrapped: an ASCII-art page that gets reflowed is not a page. */
static char *do_pre(char *p)
{
    blank_now();
    int col = 0;
    if (*p == '\n') p++;      /* a newline right after <pre> is not a blank line */
    while (*p) {
        if (p[0]=='<' && p[1]=='/' && p[2]=='p' && p[3]=='r' && p[4]=='e') {
            while (*p && *p != '>') p++;
            if (*p) p++;
            break;
        }
        if (*p == '\n') {
            /* Blank lines inside a block are content: they separate stanzas
             * of a poem and rows of a table, and eating them ruined both. */
            o_ch('\n');
            col = 0; p++;
            continue;
        }
        char c;
        int n = entity(p, &c);
        if (n) { p += n; } else { c = *p++; }
        if (!col) { o_ch(' '); o_ch(' '); }
        o_ch(c);
        col++;
    }
    if (col) o_ch('\n');
    pending_blank = 1;
    return p;
}

static void render(char *p)
{
    outn = 0; llen = 0; have_line = 0; wlen = 0; ltlen = 0; in_a = 0;
    cur_ind = cont_ind = 0; pending_blank = 0;

    while (*p) {
        if (*p == '<') {
            static char tag[256];
            char *e = p + 1;
            while (*e && *e != '>') e++;
            int tl = 0;
            for (char *q = p + 1; q < e && tl < (int)sizeof tag - 1; q++) tag[tl++] = *q;
            tag[tl] = 0;
            p = *e ? e + 1 : e;

            if (tag_is(tag, "pre")) { block_end(); p = do_pre(p); continue; }

            if (tag_is(tag, "h1") || tag_is(tag, "h2")) {
                block_end();
                cur_ind = cont_ind = 0;
                continue;
            }
            if (tag_is(tag, "/h1") || tag_is(tag, "/h2")) {
                w_end();
                /* The rule is as wide as the text it belongs to, so it is the
                 * last line's width -- headings here are short enough that
                 * this is also the whole heading. */
                while (llen > 0 && lbuf[llen - 1] == ' ') llen--;
                int n = llen;
                line_end();
                if (n > 0) { o_rep(tag[2] == '1' ? '=' : '-', n); o_ch('\n'); }
                pending_blank = 1;
                continue;
            }
            if (tag_is(tag, "p") || tag_is(tag, "/p")) {
                block_end();
                cur_ind = cont_ind = 0;
                continue;
            }
            if (tag_is(tag, "ul") || tag_is(tag, "/ul")) {
                block_end();
                cur_ind = cont_ind = 0;
                continue;
            }
            if (tag_is(tag, "/li")) {
                /* A list is one block: items sit on consecutive lines. Ending
                 * an item owes no blank line, or every list on the network
                 * would be double spaced. */
                w_end(); line_end();
                cur_ind = cont_ind = 0;
                continue;
            }
            if (tag_is(tag, "li")) {
                w_end(); line_end();
                blank_now();
                line_open(2);
                lbuf[llen++] = '*'; lbuf[llen++] = ' ';
                line_ind = llen;               /* the bullet is not content */
                cur_ind = cont_ind = 4;        /* wrapped text hangs under it */
                continue;
            }
            if (tag_is(tag, "hr")) {
                block_end();
                blank_now();
                o_rep('-', 70); o_ch('\n');
                pending_blank = 1;
                continue;
            }
            if (tag_is(tag, "img")) {
                static char alt[160];
                attr(tag, "alt", alt, sizeof alt);
                w_end();
                wput("[image:", 7);
                if (!alt[0]) g_copy(alt, "no description", sizeof alt);
                for (int i = 0; alt[i]; ) {
                    int s = i;
                    while (alt[i] && alt[i] != ' ') i++;
                    int n2 = i - s;
                    while (alt[i] == ' ') i++;
                    if (n2 <= 0) continue;
                    if (alt[i]) { wput(alt + s, n2); continue; }
                    /* The bracket belongs to the last word, not beside it:
                     * "[image: a coffee pot]", never "... pot ]". */
                    static char last[176];
                    int k = 0;
                    while (k < n2 && k < (int)sizeof last - 2) { last[k] = alt[s + k]; k++; }
                    last[k++] = ']';
                    wput(last, k);
                }
                continue;
            }
            if (tag_is(tag, "a")) {
                w_end();
                attr(tag, "href", href, sizeof href);
                in_a = 1; ltlen = 0;
                continue;
            }
            if (tag_is(tag, "/a")) {
                w_end();
                in_a = 0;
                ltext[ltlen < (int)sizeof ltext ? ltlen : 0] = 0;
                if (href[0] && !g_streq(ltext, href)) {
                    /* Where it goes, spelled out, because in a terminal the
                     * only way to follow a link is to type it. A relative
                     * href is printed resolved for the same reason. */
                    static char shown[256];
                    shown[0] = '[';
                    shown[1] = 0;
                    if (href[0] == '/') g_cat(shown, host, sizeof shown);
                    g_cat(shown, href, sizeof shown);
                    g_cat(shown, "]", sizeof shown);
                    wput(shown, (int)g_strlen(shown));
                }
                href[0] = 0;
                continue;
            }
            if (tag_is(tag, "b") || tag_is(tag, "/b")) { w_ch('*'); continue; }
            if (tag_is(tag, "i") || tag_is(tag, "/i")) { w_ch('_'); continue; }
            continue;                            /* a tag we do not know: drop it */
        }

        char c;
        int n = entity(p, &c);
        if (n) { p += n; w_ch(c); continue; }
        c = *p++;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { w_end(); continue; }
        w_ch(c);
    }
    block_end();
    /* One trailing newline, never two: a page that ends in a heading and a
     * page that ends in a list should look the same at the prompt. */
    while (outn > 1 && out[outn - 1] == '\n' && out[outn - 2] == '\n') outn--;
}

void _start(void)
{
    g_getarg(arg, sizeof arg);
    char *v[GARGS];
    int n = g_argv(arg, v);

    int raw = 0, i = 0;
    while (i < n && v[i][0] == '-') {
        if (g_streq(v[i], "--raw")) raw = 1;
        i++;
    }
    if (i >= n) {
        g_putln("usage: links [--raw] <host>[/path]");
        g_putln("try:   links wiki.nomnix.org");
        g_putln("       --raw prints the page markup instead of rendering it");
        g_exit(1);
    }

    /* split host from path */
    char *u = v[i];
    if (u[0]=='h'&&u[1]=='t'&&u[2]=='t'&&u[3]=='p'&&u[4]==':'&&u[5]=='/'&&u[6]=='/') u += 7;
    u64 k = 0;
    while (u[k] && u[k] != '/') k++;
    for (u64 j = 0; j < k && j + 1 < sizeof host; j++) host[j] = u[j];
    host[k < sizeof host - 1 ? k : sizeof host - 1] = 0;
    g_copy(path, u[k] ? u + k : "/", sizeof path);

    /* an address needs no resolving */
    int numeric = (host[0] >= '0' && host[0] <= '9');
    if (numeric) {
        g_copy(ipbuf, host, sizeof ipbuf);
    } else if (hosts_lookup(host, ipbuf, sizeof ipbuf)) {
        /* found in /etc/hosts */
    } else if (have_nameserver() && g_dns(host, ipbuf, sizeof ipbuf) > 0) {
        /* found by the nameserver */
    } else {
        g_puts("links: cannot resolve ");
        g_putln(host);
        if (!have_nameserver())
            g_putln("       (no nameserver in /etc/resolv.conf, and it is not in /etc/hosts)");
        else
            g_putln("       (not in /etc/hosts and the nameserver does not know it)");
        g_exit(1);
    }

    i64 got = g_http(ipbuf, path, page);
    if (got < 0) {
        g_puts("links: nothing responded at ");
        g_putln(ipbuf);
        g_exit(1);
    }

    if (raw) {
        g_write(1, page, (u64)got);
        g_exit(0);
    }
    render(page);
    g_write(1, out, outn);
    g_exit(0);
}

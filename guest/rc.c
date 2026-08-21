/* /bin/rc — the script interpreter that runs the boot.
 *
 * A deliberately small command language, in the spirit of nomsh: one command
 * per line, '#' comments, blank lines ignored, and any failure stops the
 * script. That last rule is why a corrupted rc file takes the machine down
 * instead of quietly skipping a step.
 *
 *   echo <text>            print it
 *   mount <what> <where>   bring a filesystem online
 *   run <script>           run another rc script
 *   exec <program> [arg]   run a compiled program
 *   need <path>            fail unless path exists and is executable
 */
#include "gsys.h"

static char script[16384];
static char line[512];

static void die(const char *what, const char *why)
{
    g_puts("rc: ");
    g_puts(what);
    g_puts(": ");
    g_putln(why);
    g_exit(1);
}

/* Split `s` into up to 3 whitespace-separated words, in place. */
static int words(char *s, char **w, int max)
{
    int n = 0;
    while (*s && n < max) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;
        w[n++] = s;
        while (*s && *s != ' ' && *s != '\t') s++;
        if (*s) *s++ = 0;
    }
    return n;
}

void _start(void)
{
    static char path[256];
    if (g_getarg(path, sizeof path) <= 0) {
        g_putln("rc: no script given");
        g_exit(1);
    }
    if (g_slurp(path, script, sizeof script) < 0) {
        g_puts("rc: ");
        g_puts(path);
        g_putln(": cannot read");
        g_exit(1);
    }

    char *p = script;
    while (*p) {
        char *nl = p;
        while (*nl && *nl != '\n') nl++;
        char save = *nl;
        *nl = 0;
        g_copy(line, p, sizeof line);
        *nl = save;
        p = *nl ? nl + 1 : nl;

        char *t = g_trim(line);
        if (!*t || *t == '#') continue;

        /* echo takes the REST OF THE LINE verbatim, so it has to be handled
         * before words() chops the line up in place. */
        if (t[0] == 'e' && t[1] == 'c' && t[2] == 'h' && t[3] == 'o' &&
            (t[4] == ' ' || t[4] == '\t' || t[4] == 0)) {
            g_putln(t[4] ? g_trim(t + 5) : "");
            continue;
        }

        char *w[4];
        int n = words(t, w, 4);
        if (n == 0) continue;

        if (g_streq(w[0], "mount")) {
            if (n < 3) die(path, "mount needs a device and a mount point");
            NomStat st;
            /* A virtual filesystem has no device to find; a real one must
             * name something this machine actually has. */
            if (!g_streq(w[1], "none") && g_stat(w[1], &st) != 0) {
                g_puts("rc: mount: ");
                g_puts(w[1]);
                g_putln(": no such device");
                g_exit(1);
            }
            g_puts("rc: mounted ");
            g_puts(w[1]);
            g_puts(" on ");
            g_putln(w[2]);
        } else if (g_streq(w[0], "run")) {
            if (n < 2) die(path, "run needs a script");
            if (g_spawn("/bin/rc", w[1]) != 0) g_exit(1);
        } else if (g_streq(w[0], "exec")) {
            if (n < 2) die(path, "exec needs a program");
            /* Same rule as init: the program reported its own failure. Do not
             * talk over it. */
            if (g_spawn(w[1], n > 2 ? w[2] : "") != 0) g_exit(1);
        } else if (g_streq(w[0], "bind")) {
            /* Plan 9 bind, in the boot scripts, the way NomnixOS's own rc.boot
             * uses it. Every process started after this inherits the view,
             * which is what makes a bad bind such a quiet fault: nothing is
             * corrupt and everything downstream reads the wrong file. */
            if (n < 3) die(path, "bind needs a target and a mount point");
            if (g_bind(w[1], w[2]) != 0) {
                g_puts("rc: bind ");
                g_puts(w[1]);
                g_puts(" ");
                g_puts(w[2]);
                g_putln(": failed");
                g_exit(1);
            }
            g_puts("rc: bound ");
            g_puts(w[1]);
            g_puts(" over ");
            g_putln(w[2]);
        } else if (g_streq(w[0], "need")) {
            if (n < 2) die(path, "need needs a path");
            NomStat st;
            if (g_stat(w[1], &st) != 0) {
                g_puts("rc: ");
                g_puts(w[1]);
                g_putln(": not found");
                g_exit(1);
            }
            if (!(st.mode & 0111)) {
                g_puts("rc: ");
                g_puts(w[1]);
                g_putln(": not executable");
                g_exit(1);
            }
        } else {
            g_puts("rc: ");
            g_puts(path);
            g_puts(": unrecognised command: ");
            g_putln(w[0]);
            g_exit(1);
        }
    }
    g_exit(0);
}

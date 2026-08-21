/* /usr/sbin/syslogd — the logger.
 *
 * A real daemon: it reads its configuration, opens its log, writes that it
 * started, and then stays up. Everything after the setup is a wait loop,
 * which is what a daemon mostly is.
 *
 * It matters that this is a real program rather than a stub, because now
 * /var/log/messages has something in it, `grep` over the log is a diagnostic
 * rather than a toy, and a syslogd that cannot write its log fails in a way
 * you can see.
 */
#include "gsys.h"

static char conf[1024];

void _start(void)
{
    const char *logfile = "/var/log/messages";
    if (g_slurp("/etc/syslog.conf", conf, sizeof conf) < 0) {
        g_putln("syslogd: /etc/syslog.conf: cannot read");
        g_exit(1);
    }
    /* "*.info /var/log/messages" -- take the path off the first rule. */
    char *p = conf;
    while (*p) {
        char *nl = p; while (*nl && *nl != '\n') nl++;
        char save = *nl; *nl = 0;
        static char line[256];
        g_copy(line, p, sizeof line);
        *nl = save; p = *nl ? nl + 1 : nl;
        char *t = g_trim(line);
        if (!*t || *t == '#') continue;
        char *v[GARGS];
        if (g_argv(t, v) >= 2 && v[1][0] == '/') {
            static char lf[128];
            g_copy(lf, v[1], sizeof lf);
            logfile = lf;
            break;
        }
    }

    int fd = g_open(logfile, O_WRONLY | O_CREAT | O_APPEND);
    if (fd < 0) {
        g_puts("syslogd: ");
        g_puts(logfile);
        g_putln(": cannot open for writing");
        g_exit(1);
    }
    /* Check the write. A logger that cannot write its log is not running,
     * whatever the process table says -- and on a full disk this is the FIRST
     * thing to fail, which is almost never the interesting thing. What
     * actually filled the disk is a log that has been growing since March. */
    const char *banner = "syslogd: started, logging to ";
    i64 w = sysc(SYS_write, fd, (i64)banner, (i64)g_strlen(banner));
    g_close(fd);
    if (w < 0) {
        g_puts("syslogd: ");
        g_puts(logfile);
        g_putln(": cannot write -- is the disk full?");
        g_exit(1);
    }
    fd = g_open(logfile, O_WRONLY | O_APPEND);
    if (fd >= 0) {
        sysc(SYS_write, fd, (i64)logfile, (i64)g_strlen(logfile));
        sysc(SYS_write, fd, (i64)"\n", 1);
        g_close(fd);
    }

    /* Up. A daemon spends its life here. */
    for (;;) { }
}

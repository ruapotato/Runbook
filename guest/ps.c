/* /bin/ps — what has run on this machine, read out of /proc. */
#include "gsys.h"
static char name[64], path[128], body[512];
static void field(const char *b, const char *k, char *out, u64 cap)
{
    u64 kl = g_strlen(k);
    const char *p = b;
    g_copy(out, "", cap);
    while (*p) {
        const char *nl = p; while (*nl && *nl != '\n') nl++;
        u64 i = 0; while (i < kl && p + i < nl && p[i] == k[i]) i++;
        if (i == kl && p + i < nl && p[i] == ' ') {
            const char *v = p + i + 1; u64 j = 0;
            while (v + j < nl && j + 1 < cap) { out[j] = v[j]; j++; }
            out[j] = 0; return;
        }
        p = *nl ? nl + 1 : nl;
    }
}
void _start(void)
{
    g_putln("  PID  PPID STATE     EXIT  INSTRUCTIONS  COMMAND");
    for (int i = 0; i < 256; i++) {
        if (g_readdir("/proc", i, name) < 0) break;
        g_copy(path, "/proc/", sizeof path);
        g_cat(path, name, sizeof path);
        g_cat(path, "/status", sizeof path);
        if (g_slurp(path, body, sizeof body) < 0) continue;
        static char pid[16], ppid[16], state[16], ex[16], ic[24], nm[64];
        field(body, "pid", pid, sizeof pid);
        field(body, "ppid", ppid, sizeof ppid);
        field(body, "state", state, sizeof state);
        field(body, "exit", ex, sizeof ex);
        field(body, "instructions", ic, sizeof ic);
        field(body, "name", nm, sizeof nm);
        for (u64 k = g_strlen(pid); k < 5; k++) g_puts(" "); g_puts(pid);
        for (u64 k = g_strlen(ppid); k < 6; k++) g_puts(" "); g_puts(ppid);
        g_puts(" "); g_puts(state);
        for (u64 k = g_strlen(state); k < 10; k++) g_puts(" ");
        for (u64 k = g_strlen(ex); k < 4; k++) g_puts(" "); g_puts(ex);
        for (u64 k = g_strlen(ic); k < 14; k++) g_puts(" "); g_puts(ic);
        g_puts("  "); g_putln(nm);
    }
    g_exit(0);
}

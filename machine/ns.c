/* ns.c — namespace resolution. See ns.h for why this exists. */
#include <string.h>
#include <stdio.h>
#include "nom.h"
#include "ns.h"

void ns_init(Ns *ns) { ns->n = 0; }

void ns_copy(Ns *dst, const Ns *src)
{
    /* A child gets its parent's view and may then diverge from it. Copying
     * rather than sharing is what makes a namespace per-process. */
    *dst = *src;
}

static void norm(const char *in, char *out, size_t outsz)
{
    vfs_normalize("/", in, out, outsz);
    /* Strip a trailing slash so /etc and /etc/ are the same binding. */
    size_t n = strlen(out);
    while (n > 1 && out[n - 1] == '/') out[--n] = '\0';
}

bool ns_bind(Ns *ns, const char *target, const char *at, char *err, size_t errsz)
{
    char a[NOM_PATH_MAX * 2], t[NOM_PATH_MAX * 2];
    norm(at, a, sizeof a);
    norm(target, t, sizeof t);
    if (strlen(a) >= NOM_PATH_MAX || strlen(t) >= NOM_PATH_MAX) {
        if (err) snprintf(err, errsz, "path too long");
        return false;
    }
    /* Binding a path onto itself is how you would accidentally build an
     * infinite loop, so it is refused rather than resolved lazily. */
    if (strcmp(a, t) == 0) {
        if (err) snprintf(err, errsz, "%s: cannot bind a path onto itself", at);
        return false;
    }
    for (int i = 0; i < ns->n; i++) {
        if (strcmp(ns->b[i].at, a) == 0) {          /* rebind replaces */
            snprintf(ns->b[i].target, NOM_PATH_MAX, "%s", t);
            return true;
        }
    }
    if (ns->n >= NS_MAX) {
        if (err) snprintf(err, errsz, "namespace full (%d bindings)", NS_MAX);
        return false;
    }
    snprintf(ns->b[ns->n].at, NOM_PATH_MAX, "%s", a);
    snprintf(ns->b[ns->n].target, NOM_PATH_MAX, "%s", t);
    ns->n++;
    return true;
}

bool ns_unbind(Ns *ns, const char *at)
{
    char a[NOM_PATH_MAX * 2];
    norm(at, a, sizeof a);
    for (int i = 0; i < ns->n; i++) {
        if (strcmp(ns->b[i].at, a) != 0) continue;
        for (int j = i; j < ns->n - 1; j++) ns->b[j] = ns->b[j + 1];
        ns->n--;
        return true;
    }
    return false;
}

/* Does `path` sit at or under `at`? Only whole path elements count, so a
 * binding at /etc does not capture /etcetera. */
static bool under(const char *path, const char *at, size_t atlen)
{
    if (strncmp(path, at, atlen) != 0) return false;
    if (atlen == 1 && at[0] == '/') return true;      /* everything is under / */
    return path[atlen] == '\0' || path[atlen] == '/';
}

void ns_resolve(const Ns *ns, const char *path, char *out, size_t outsz)
{
    char p[NOM_PATH_MAX * 2];
    norm(path, p, sizeof p);

    /* Longest prefix wins, and resolution repeats so a binding can be made
     * through another binding. The hop limit is what stops a namespace the
     * player has tangled from hanging the machine. */
    for (int hop = 0; hop < 8; hop++) {
        int best = -1;
        size_t bestlen = 0;
        for (int i = 0; i < ns->n; i++) {
            size_t al = strlen(ns->b[i].at);
            if (!under(p, ns->b[i].at, al)) continue;
            if (best < 0 || al > bestlen) { best = i; bestlen = al; }
        }
        if (best < 0) break;
        char next[NOM_PATH_MAX * 2];
        snprintf(next, sizeof next, "%s%s", ns->b[best].target,
                 (bestlen == 1 && ns->b[best].at[0] == '/') ? p : p + bestlen);
        char n2[NOM_PATH_MAX * 2];
        norm(next, n2, sizeof n2);
        if (strcmp(n2, p) == 0) break;           /* fixed point */
        snprintf(p, sizeof p, "%s", n2);
    }
    snprintf(out, outsz, "%s", p);
}

void ns_print(const Ns *ns, Buf *out)
{
    for (int i = 0; i < ns->n; i++)
        buf_printf(out, "%s %s\n", ns->b[i].at, ns->b[i].target);
}

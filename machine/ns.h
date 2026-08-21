/* ns.h — per-process namespaces, Plan 9 style.
 *
 * A namespace is a list of bindings: "when you look up a name under `at`, you
 * are really looking at `target`". Every process has its own, inherited from
 * its parent at spawn, and a child may change its own view without disturbing
 * anyone else's. That is the whole Plan 9 idea and it is the thing that makes
 * `bind` a first-class administrative verb rather than a curiosity.
 *
 * Resolution is LONGEST PREFIX MATCH, which is what NomnixOS's own rc.boot
 * relies on: binding '#c' at /dev and '#b' at /dev/blk works because
 * /dev/blk/nvme0n1 matches the longer of the two.
 *
 * WHY THIS MATTERS FOR THE GAME: a wrong bind is a fault where *nothing is
 * corrupt*. Every file passes `pkg verify`, and the machine still reads the
 * wrong /etc. The only way to see it is to look at the namespace -- which is
 * exactly the class of problem that separates someone who knows the system
 * from someone who has memorised a checklist.
 */
#ifndef NOM_NS_H
#define NOM_NS_H

#define NS_MAX 24

typedef struct {
    char at[NOM_PATH_MAX];      /* the name a process looks up   */
    char target[NOM_PATH_MAX];  /* what it actually resolves to  */
} NsBind;

typedef struct {
    NsBind b[NS_MAX];
    int    n;
} Ns;

void ns_init(Ns *ns);
void ns_copy(Ns *dst, const Ns *src);

/* bind target at `at`. Later bindings win over earlier ones at equal length,
 * so re-binding a path replaces what was there, as a shell user expects. */
bool ns_bind(Ns *ns, const char *target, const char *at, char *err, size_t errsz);
bool ns_unbind(Ns *ns, const char *at);

/* Rewrite `path` through the namespace into `out`. Always succeeds: a path
 * with no matching binding resolves to itself. */
void ns_resolve(const Ns *ns, const char *path, char *out, size_t outsz);

/* One "at target" line per binding, in the order they were made -- which is
 * the order that matters when you are working out which one is shadowing
 * which. */
void ns_print(const Ns *ns, Buf *out);

#endif /* NOM_NS_H */

/* yaml.h — the subset of YAML the appliance specs are written in.
 *
 * A subset, and a small one, said plainly so nobody files a bug about
 * anchors. What is supported:
 *
 *   key: value                block mappings, nested by indentation
 *   - item                    block sequences, of scalars or of mappings
 *   [a, b, c]                 flow sequences
 *   {k: v, k2: v2}            flow mappings
 *   "quoted scalars"          and bare ones
 *   # comments                to end of line
 *
 * What is not: anchors, aliases, tags, multi-line scalars, multiple
 * documents, and every other thing that makes a YAML library a dependency.
 * Appliance specs are data written by us and by agents (handoff §9, M8), and
 * the day one of them needs an anchor is the day the spec format is wrong.
 *
 * Errors carry a line number and say what was expected. A spec that fails to
 * load must fail loudly at the gate, never silently produce an appliance with
 * three of its four endpoints.
 */
#ifndef RB_YAML_H
#define RB_YAML_H

#include "rb.h"

typedef enum { Y_SCALAR, Y_MAP, Y_SEQ } YKind;

typedef struct YNode YNode;

typedef struct {
    char   key[RB_NAME_MAX];
    YNode *val;
} YPair;

struct YNode {
    YKind   kind;
    int     line;
    char   *scalar;          /* Y_SCALAR */
    YPair  *pair;  size_t npair;   /* Y_MAP */
    YNode **item;  size_t nitem;   /* Y_SEQ */
};

/* Parse a whole document. Returns NULL and fills err on failure. */
YNode *yaml_parse(const char *text, char *err, size_t errcap);
void   yaml_free(YNode *n);

/* Lookups that do not crash on the wrong shape — a malformed spec is a
 * content bug, and content bugs must produce messages, not segfaults. */
const YNode *y_get(const YNode *map, const char *key);
const char  *y_str(const YNode *map, const char *key, const char *dflt);
int          y_int(const YNode *map, const char *key, int dflt);
bool         y_has(const YNode *map, const char *key);
/* Sequence access that treats a lone scalar as a one-element sequence, so
 * `requires: credential_valid` and `requires: [credential_valid]` mean the
 * same thing. Spec authors will write both. */
size_t       y_count(const YNode *seq);
const YNode *y_at(const YNode *seq, size_t i);

#endif

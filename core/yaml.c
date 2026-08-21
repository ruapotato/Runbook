/* yaml.c — the spec parser. See yaml.h for the subset and why it is one. */
#include "yaml.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

/* ------------------------------------------------------------------ lines */
typedef struct {
    const char *p;     /* first non-space character */
    size_t      len;   /* to end of line, comment already stripped */
    int         indent;
    int         no;    /* 1-based, for error messages */
} Line;

typedef struct {
    Line  *l;
    size_t n, cap;
    char  *err;
    size_t errcap;
    bool   failed;
} Doc;

static void fail(Doc *d, int line, const char *fmt, ...)
{
    if (d->failed) return;      /* keep the first error; the rest are noise */
    d->failed = true;
    char tmp[256];
    va_list ap; va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    snprintf(d->err, d->errcap, "line %d: %s", line, tmp);
}

/* Strip a trailing comment, honouring quotes: a '#' inside "..." is content.
 * Vendor manuals will contain them. */
static size_t strip_comment(const char *s, size_t len)
{
    bool q = false;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '"') q = !q;
        else if (s[i] == '#' && !q) return i;
    }
    return len;
}

static size_t rtrim(const char *s, size_t len)
{
    while (len && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r')) len--;
    return len;
}

static void doc_scan(Doc *d, const char *text)
{
    int no = 0;
    const char *p = text;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        no++;

        int indent = 0;
        size_t i = 0;
        while (i < len && (p[i] == ' ' || p[i] == '\t')) {
            /* Tabs are refused rather than guessed at. A tab that counts as
             * one column in the editor and eight in the parser is a spec that
             * loads differently than it reads. */
            if (p[i] == '\t') fail(d, no, "tab in indentation; specs use spaces");
            indent++; i++;
        }
        size_t body = strip_comment(p + i, len - i);
        body = rtrim(p + i, body);

        if (body > 0) {
            if (d->n == d->cap) {
                d->cap = d->cap ? d->cap * 2 : 64;
                d->l = rb_realloc(d->l, d->cap * sizeof *d->l);
            }
            d->l[d->n].p      = p + i;
            d->l[d->n].len    = body;
            d->l[d->n].indent = indent;
            d->l[d->n].no     = no;
            d->n++;
        }
        if (!eol) break;
        p = eol + 1;
    }
}

/* ------------------------------------------------------------------ nodes */
static YNode *node(YKind k, int line)
{
    YNode *n = rb_alloc(sizeof *n);
    n->kind = k;
    n->line = line;
    return n;
}

static YNode *scalar_node(const char *s, size_t len, int line)
{
    /* Unquote. Quoting exists so a value may contain ':' or '#'; it carries
     * no other meaning, and there are no escapes. */
    if (len >= 2 && s[0] == '"' && s[len - 1] == '"') { s++; len -= 2; }
    YNode *n = node(Y_SCALAR, line);
    n->scalar = rb_alloc(len + 1);
    memcpy(n->scalar, s, len);
    n->scalar[len] = 0;
    return n;
}

static void map_put(YNode *m, const char *key, size_t klen, YNode *v)
{
    m->pair = rb_realloc(m->pair, (m->npair + 1) * sizeof *m->pair);
    YPair *pr = &m->pair[m->npair++];
    memset(pr, 0, sizeof *pr);
    if (klen >= RB_NAME_MAX) klen = RB_NAME_MAX - 1;
    memcpy(pr->key, key, klen);
    pr->key[klen] = 0;
    pr->val = v;
}

static void seq_put(YNode *q, YNode *v)
{
    q->item = rb_realloc(q->item, (q->nitem + 1) * sizeof *q->item);
    q->item[q->nitem++] = v;
}

void yaml_free(YNode *n)
{
    if (!n) return;
    rb_free(n->scalar);
    for (size_t i = 0; i < n->npair; i++) yaml_free(n->pair[i].val);
    rb_free(n->pair);
    for (size_t i = 0; i < n->nitem; i++) yaml_free(n->item[i]);
    rb_free(n->item);
    rb_free(n);
}

/* ------------------------------------------------------------------- flow */
/* [a, b] and {k: v}. One level of nesting is supported and no more, which is
 * all the spec format uses; deeper structure goes in block form where it can
 * be read. */
static YNode *parse_flow(Doc *d, const char *s, size_t len, int line);

static size_t flow_span(const char *s, size_t len)
{
    /* Find the matching close, respecting one level of nesting and quotes. */
    char open = s[0], close = (open == '[') ? ']' : '}';
    int depth = 0; bool q = false;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '"') q = !q;
        else if (!q && s[i] == open) depth++;
        else if (!q && s[i] == close) { depth--; if (!depth) return i; }
    }
    return len;   /* unterminated; caller reports it */
}

/* NESTING, ONE LEVEL DEEPER THAN IT USED TO GO.
 *
 * `{ name: status, type: enum, values: [active, suspended] }` is one flow
 * mapping whose last value is a flow sequence -- and the entry splitter used
 * to break on the first comma it saw, including the ones INSIDE the brackets,
 * so it produced "values: [active" and then an entry called "suspended]" with
 * no colon in it. The error said "flow mapping entry without ':'", which is
 * true and unhelpful.
 *
 * Splitting now skips over any bracketed span, and a value that opens one is
 * parsed as what it is rather than kept as text. */
static size_t flow_entry_end(const char *s, size_t start, size_t end)
{
    int depth = 0;
    bool q = false;
    size_t i = start;
    while (i < end) {
        char c = s[i];
        if (c == '"') q = !q;
        else if (!q && (c == '[' || c == '{')) depth++;
        else if (!q && (c == ']' || c == '}')) depth--;
        else if (!q && c == ',' && depth == 0) break;
        i++;
    }
    return i;
}

static YNode *parse_flow(Doc *d, const char *s, size_t len, int line)
{
    char open = s[0];
    size_t end = flow_span(s, len);
    if (end >= len) { fail(d, line, "unterminated %c", open); return NULL; }

    YNode *n = node(open == '[' ? Y_SEQ : Y_MAP, line);
    size_t i = 1;
    while (i < end) {
        while (i < end && (s[i] == ' ' || s[i] == ',')) i++;
        if (i >= end) break;
        size_t start = i;
        i = flow_entry_end(s, start, end);
        size_t ilen = rtrim(s + start, i - start);
        if (!ilen) continue;

        if (open == '[') {
            const char *ip = s + start;
            if (ip[0] == '[' || ip[0] == '{') seq_put(n, parse_flow(d, ip, ilen, line));
            else                              seq_put(n, scalar_node(ip, ilen, line));
        } else {
            /* The colon that separates key from value is the first one at
             * depth zero -- not the first one in the entry, which may be
             * inside a nested mapping. */
            const char *ep = s + start;
            size_t ci = 0;
            int depth = 0;
            bool q = false, found = false;
            for (; ci < ilen; ci++) {
                char c = ep[ci];
                if (c == '"') q = !q;
                else if (!q && (c == '[' || c == '{')) depth++;
                else if (!q && (c == ']' || c == '}')) depth--;
                else if (!q && c == ':' && depth == 0) { found = true; break; }
            }
            if (!found) { fail(d, line, "flow mapping entry without ':'"); yaml_free(n); return NULL; }
            size_t klen = rtrim(ep, ci);
            const char *vp = ep + ci + 1;
            size_t vlen = ilen - ci - 1;
            while (vlen && *vp == ' ') { vp++; vlen--; }
            if (vlen && (vp[0] == '[' || vp[0] == '{')) map_put(n, ep, klen, parse_flow(d, vp, vlen, line));
            else                                        map_put(n, ep, klen, scalar_node(vp, vlen, line));
        }
    }
    return n;
}

/* ------------------------------------------------------------------ block */
static YNode *parse_block(Doc *d, size_t *idx, int indent);

/* The value that follows "key:" — inline on the same line, or the indented
 * block beneath it. */
static YNode *parse_value(Doc *d, size_t *idx, int indent, const char *v, size_t vlen, int line)
{
    if (vlen) {
        if (v[0] == '[' || v[0] == '{') return parse_flow(d, v, vlen, line);
        return scalar_node(v, vlen, line);
    }
    /* Nothing after the colon: look for a deeper block. An empty value is an
     * empty scalar, not an error — `forms:` with no forms is a legitimate
     * appliance. */
    if (*idx < d->n && d->l[*idx].indent > indent) return parse_block(d, idx, d->l[*idx].indent);
    return scalar_node("", 0, line);
}

static YNode *parse_block(Doc *d, size_t *idx, int indent)
{
    if (d->failed) return NULL;
    bool is_seq = d->l[*idx].p[0] == '-' &&
                  (d->l[*idx].len == 1 || d->l[*idx].p[1] == ' ');
    YNode *n = node(is_seq ? Y_SEQ : Y_MAP, d->l[*idx].no);

    while (*idx < d->n && !d->failed) {
        Line *ln = &d->l[*idx];
        if (ln->indent < indent) break;
        if (ln->indent > indent) { fail(d, ln->no, "unexpected indentation"); break; }

        bool item = ln->p[0] == '-' && (ln->len == 1 || ln->p[1] == ' ');
        if (item != is_seq) { fail(d, ln->no, "list item and mapping key at the same indent"); break; }

        if (is_seq) {
            const char *body = ln->p + 1;
            size_t blen = ln->len - 1;
            while (blen && *body == ' ') { body++; blen--; }
            int no = ln->no;
            (*idx)++;

            if (!blen) {
                /* "-" alone: the item is the block underneath it. */
                if (*idx < d->n && d->l[*idx].indent > indent)
                    seq_put(n, parse_block(d, idx, d->l[*idx].indent));
                else
                    seq_put(n, scalar_node("", 0, no));
                continue;
            }
            if (body[0] == '[' || body[0] == '{') { seq_put(n, parse_flow(d, body, blen, no)); continue; }

            /* "- key: value" opens a mapping whose remaining keys are
             * indented to line up under the key, which is how every example
             * in the handoff is written. */
            const char *colon = memchr(body, ':', blen);
            if (!colon) { seq_put(n, scalar_node(body, blen, no)); continue; }

            YNode *m = node(Y_MAP, no);
            size_t klen = rtrim(body, (size_t)(colon - body));
            const char *vp = colon + 1;
            size_t vlen = blen - (size_t)(vp - body);
            while (vlen && *vp == ' ') { vp++; vlen--; }
            int inner = indent + (int)(body - ln->p);
            map_put(m, body, klen, parse_value(d, idx, inner, vp, vlen, no));
            /* the rest of this item's keys */
            while (*idx < d->n && !d->failed && d->l[*idx].indent == inner &&
                   !(d->l[*idx].p[0] == '-' && (d->l[*idx].len == 1 || d->l[*idx].p[1] == ' '))) {
                Line *k = &d->l[*idx];
                const char *c2 = memchr(k->p, ':', k->len);
                if (!c2) { fail(d, k->no, "expected 'key:'"); break; }
                size_t kl = rtrim(k->p, (size_t)(c2 - k->p));
                const char *v2 = c2 + 1;
                size_t vl = k->len - (size_t)(v2 - k->p);
                while (vl && *v2 == ' ') { v2++; vl--; }
                int no2 = k->no;
                (*idx)++;
                map_put(m, k->p, kl, parse_value(d, idx, inner, v2, vl, no2));
            }
            seq_put(n, m);
        } else {
            const char *colon = memchr(ln->p, ':', ln->len);
            if (!colon) { fail(d, ln->no, "expected 'key:'"); break; }
            size_t klen = rtrim(ln->p, (size_t)(colon - ln->p));
            const char *vp = colon + 1;
            size_t vlen = ln->len - (size_t)(vp - ln->p);
            while (vlen && *vp == ' ') { vp++; vlen--; }
            int no = ln->no;
            (*idx)++;
            map_put(n, ln->p, klen, parse_value(d, idx, indent, vp, vlen, no));
        }
    }
    return n;
}

YNode *yaml_parse(const char *text, char *err, size_t errcap)
{
    Doc d;
    memset(&d, 0, sizeof d);
    d.err = err; d.errcap = errcap;
    err[0] = 0;
    doc_scan(&d, text);

    YNode *root = NULL;
    if (!d.failed) {
        if (d.n == 0) { snprintf(err, errcap, "empty document"); }
        else {
            size_t i = 0;
            root = parse_block(&d, &i, d.l[0].indent);
            if (!d.failed && i < d.n) fail(&d, d.l[i].no, "trailing content");
        }
    }
    rb_free(d.l);
    if (d.failed) { yaml_free(root); return NULL; }
    return root;
}

/* ---------------------------------------------------------------- lookups */
const YNode *y_get(const YNode *map, const char *key)
{
    if (!map || map->kind != Y_MAP) return NULL;
    for (size_t i = 0; i < map->npair; i++)
        if (strcmp(map->pair[i].key, key) == 0) return map->pair[i].val;
    return NULL;
}

const char *y_str(const YNode *map, const char *key, const char *dflt)
{
    const YNode *v = y_get(map, key);
    return (v && v->kind == Y_SCALAR && v->scalar[0]) ? v->scalar : dflt;
}

int y_int(const YNode *map, const char *key, int dflt)
{
    const YNode *v = y_get(map, key);
    if (!v || v->kind != Y_SCALAR || !v->scalar[0]) return dflt;
    return atoi(v->scalar);
}

bool y_has(const YNode *map, const char *key) { return y_get(map, key) != NULL; }

size_t y_count(const YNode *seq)
{
    if (!seq) return 0;
    if (seq->kind == Y_SEQ) return seq->nitem;
    if (seq->kind == Y_SCALAR && !seq->scalar[0]) return 0;
    return 1;      /* a lone scalar is a one-element sequence */
}

const YNode *y_at(const YNode *seq, size_t i)
{
    if (!seq) return NULL;
    if (seq->kind == Y_SEQ) return i < seq->nitem ? seq->item[i] : NULL;
    return i == 0 ? seq : NULL;
}

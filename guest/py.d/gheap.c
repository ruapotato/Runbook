/* gheap.c — a heap, buffers, strings and integer math for the guest.
 *
 * Everything nom.h promises and the machine does not provide. Written for
 * this job rather than lifted, because there was nothing to lift: the host's
 * versions are three lines each on top of libc, and there is no libc here.
 */
#include "nom.h"

/* ------------------------------------------------------------------ heap */
unsigned char g_heap[HEAP_BYTES];
Blk *g_free = 0;
u64 g_heap_used = 0, g_heap_peak = 0;

#define ALIGN(n) (((n) + 15u) & ~15u)

static void heap_init(void)
{
    g_free = (Blk *)g_heap;
    g_free->size = HEAP_BYTES - sizeof(Blk);
    g_free->next = 0;
    g_free->used = 0;
}

void *nom_alloc(size_t n)
{
    if (!g_free) heap_init();
    n = ALIGN(n ? n : 1);

    /* First fit, splitting what is left over. First fit rather than best fit
     * because the allocation pattern here is a VM's -- many small, short-
     * lived objects of similar size -- and best fit spends its time walking
     * a list to find a marginally tidier answer. */
    for (Blk *b = g_free; b; b = b->next) {
        if (b->used || b->size < n) continue;
        if (b->size >= n + sizeof(Blk) + 32u) {
            Blk *split = (Blk *)((unsigned char *)b + sizeof(Blk) + n);
            split->size = b->size - n - sizeof(Blk);
            split->used = 0;
            split->next = b->next;
            b->next = split;
            b->size = n;
        }
        b->used = 1;
        g_heap_used += b->size;
        if (g_heap_used > g_heap_peak) g_heap_peak = g_heap_used;
        unsigned char *p = (unsigned char *)b + sizeof(Blk);
        for (u64 i = 0; i < n; i++) p[i] = 0;
        return p;
    }
    return 0;      /* out of memory: every caller checks, because it must */
}

/* Coalesce forward on free. Without it a script that grows and drops a list
 * repeatedly fragments the arena into unusable slivers within a few thousand
 * iterations -- and a loop over four hundred tickets IS a few thousand
 * iterations. */
void nom_free(void *p)
{
    if (!p) return;
    Blk *b = (Blk *)((unsigned char *)p - sizeof(Blk));
    b->used = 0;
    if (g_heap_used >= b->size) g_heap_used -= b->size;
    while (b->next && !b->next->used) {
        b->size += sizeof(Blk) + b->next->size;
        b->next = b->next->next;
    }
}

void *nom_realloc(void *p, size_t n)
{
    if (!p) return nom_alloc(n);
    Blk *b = (Blk *)((unsigned char *)p - sizeof(Blk));
    if (b->size >= ALIGN(n)) return p;
    void *q = nom_alloc(n);
    if (!q) return 0;
    nom_memcpy(q, p, b->size);
    nom_free(p);
    return q;
}

char *nom_strdup(const char *s)
{
    size_t n = nom_strlen(s) + 1;
    char *d = nom_alloc(n);
    if (d) nom_memcpy(d, s, n);
    return d;
}

/* ----------------------------------------------------------------- meter */
static size_t meter_used, meter_cap;
static int    meter_on;

void nom_meter_begin(size_t cap) { meter_used = 0; meter_cap = cap; meter_on = 1; }
void nom_meter_end(void)         { meter_on = 0; }
bool nom_meter_over(void)        { return meter_on && meter_used > meter_cap; }
size_t nom_meter_used(void)      { return meter_used; }

/* --------------------------------------------------------------- strings */
size_t nom_strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

void *nom_memcpy(void *d, const void *s, size_t n)
{
    unsigned char *a = d; const unsigned char *b = s;
    for (size_t i = 0; i < n; i++) a[i] = b[i];
    return d;
}

int nom_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int nom_memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    for (size_t i = 0; i < n; i++) if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    return 0;
}

int nom_strncmp(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
        if (!a[i]) return 0;
    }
    return 0;
}

/* THE COMPILER EMITS CALLS TO THESE, whatever -fno-builtin says: zeroing a
 * struct or copying one becomes a memset/memcpy call once it is big enough,
 * and a freestanding program has to provide them by those names or the link
 * fails with no line number. The macros in nom.h are undefined around them so
 * these definitions keep the names the linker is looking for. */
#undef memset
#undef memcpy
#undef memmove
void *memset(void *d, int c, size_t n) { return nom_memset(d, c, n); }
void *memcpy(void *d, const void *s, size_t n) { return nom_memcpy(d, s, n); }
void *memmove(void *d, const void *s, size_t n) { return nom_memmove(d, s, n); }
#define memset  nom_memset
#define memcpy  nom_memcpy
#define memmove nom_memmove

void *nom_memset(void *d, int c, size_t n)
{
    unsigned char *p = d;
    for (size_t i = 0; i < n; i++) p[i] = (unsigned char)c;
    return d;
}

void *nom_memmove(void *d, const void *s, size_t n)
{
    unsigned char *a = d;
    const unsigned char *b = s;
    if (a == b) return d;
    if (a < b) { for (size_t i = 0; i < n; i++) a[i] = b[i]; }
    else       { for (size_t i = n; i > 0; i--) a[i - 1] = b[i - 1]; }
    return d;
}

/* THE NUMBER PARSER, AND THERE IS NO POINT IN IT.
 *
 * strtod's whole job is the fractional part, and this machine has no reals
 * (see nom.h). A literal like 1.5 lexes to 1, which is what integer division
 * does to it everywhere else in the language, so the behaviour is at least
 * consistent -- and a script that wanted 1.5 was going to be disappointed by
 * the arithmetic anyway. */
double nom_strtod(const char *s, char **end)
{
    i64 sign = 1, v = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    if (*s == '.') { s++; while (*s >= '0' && *s <= '9') s++; }
    if (end) *end = (char *)s;
    return sign * v;
}

/* Base 10 and base 16, which is every base a source literal can be in. */
int64_t nom_strtoll(const char *s, char **end, int base)
{
    i64 sign = 1, v = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;
    if (base == 16 || (base == 0 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))) {
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
        for (;;) {
            int d;
            if (*s >= '0' && *s <= '9') d = *s - '0';
            else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
            else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
            else break;
            v = v * 16 + d;
            s++;
        }
    } else {
        while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    }
    if (end) *end = (char *)s;
    return sign * v;
}

/* --------------------------------------------------------------- buffers */
void buf_init(Buf *b)  { b->p = 0; b->len = b->cap = 0; }
void buf_free(Buf *b)  { nom_free(b->p); buf_init(b); }
void buf_clear(Buf *b) { b->len = 0; if (b->p) b->p[0] = 0; }

static void buf_reserve(Buf *b, size_t extra)
{
    if (b->p && b->len + extra + 1 <= b->cap) return;
    size_t cap = b->cap ? b->cap : 64;
    while (cap < b->len + extra + 1) cap *= 2;
    char *q = nom_realloc(b->p, cap);
    if (!q) return;             /* out of memory: the write is dropped, and
                                 * the meter will trip on the next check */
    b->p = q;
    b->cap = cap;
}

void buf_put(Buf *b, const void *data, size_t n)
{
    if (!n) return;
    buf_reserve(b, n);
    if (!b->p || b->len + n + 1 > b->cap) return;
    nom_memcpy(b->p + b->len, data, n);
    b->len += n;
    b->p[b->len] = 0;
}

void buf_puts(Buf *b, const char *s) { if (s) buf_put(b, s, nom_strlen(s)); }
void buf_putc(Buf *b, char c) { buf_put(b, &c, 1); }

static void buf_putll(Buf *b, i64 v)
{
    char t[24];
    int k = 0;
    int neg = v < 0;
    u64 u = neg ? (u64)(-(v + 1)) + 1u : (u64)v;
    if (!u) t[k++] = '0';
    while (u) { t[k++] = (char)('0' + (u % 10u)); u /= 10u; }
    if (neg) buf_putc(b, '-');
    while (k) buf_putc(b, t[--k]);
}

/* %s %d %ld %lld %u %c %% -- every format the language actually uses. */
void buf_vprintf(Buf *b, const char *fmt, va_list ap)
{
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { buf_putc(b, *p); continue; }
        p++;
        /* %.*s -- a length and a pointer. The lexer and compiler use it for
         * every error message they produce, because their identifiers are
         * slices of the source rather than terminated strings. Without it
         * every syntax error read `undefined name '%.*s'`, which is a
         * compiler telling you it has found a problem and refusing to say
         * which. It cost twenty minutes and it was the second thing to fix. */
        int star = 0;
        if (*p == '.' && p[1] == '*') { star = 1; p += 2; }
        while (*p == 'l' || *p == 'z' || *p == 'u') p++;
        switch (*p) {
        case 's': {
            if (star) {
                int n = va_arg(ap, int);
                const char *s = va_arg(ap, const char *);
                if (s && n > 0) buf_put(b, s, (size_t)n);
                break;
            }
            const char *s = va_arg(ap, const char *); buf_puts(b, s ? s : "(null)"); break;
        }
        case 'd': case 'i': buf_putll(b, (i64)va_arg(ap, int)); break;
        case 'c': buf_putc(b, (char)va_arg(ap, int)); break;
        case '%': buf_putc(b, '%'); break;
        default:  buf_putc(b, '%'); if (*p) buf_putc(b, *p); break;
        }
        if (!*p) break;
    }
}

void buf_printf(Buf *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    buf_vprintf(b, fmt, ap);
    va_end(ap);
}

/* The language calls this to print a number. There are no reals here, so it
 * prints the integer and ignores the decimals -- which is honest, because
 * there is nothing after the point to print. */
void buf_putnum(Buf *b, double v, int decimals)
{
    (void)decimals;
    buf_putll(b, (i64)v);
}

/* A cut-down (v)snprintf over the same formatter the buffer uses, so there is
 * one implementation of "%s means this" in the program. */
int nom_vsnprintf(char *out, size_t cap, const char *fmt, va_list ap)
{
    Buf b;
    buf_init(&b);
    buf_vprintf(&b, fmt, ap);
    size_t n = b.len;
    if (cap) {
        if (n > cap - 1) n = cap - 1;
        if (b.p) nom_memcpy(out, b.p, n);
        out[n] = 0;
    }
    size_t full = b.len;
    buf_free(&b);
    return (int)full;
}

int nom_snprintf(char *out, size_t cap, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = nom_vsnprintf(out, cap, fmt, ap);
    va_end(ap);
    return r;
}

/* ------------------------------------------------------------------ math
 * `double` is a typedef for int64 in this build (see nom.h), so these are
 * the integer operations the names describe. */
double nom_floor(double x) { return x; }

double nom_pow(double b, double e)
{
    if (e < 0) return 0;
    double r = 1;
    for (double i = 0; i < e; i++) r *= b;
    return r;
}

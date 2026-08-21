/* util.c — allocation, growable buffers, the deterministic RNG and the world
 * state hash.
 *
 * Lifted from ~/NOMINAL/core/util.c, trimmed to what this game needs. What
 * did NOT come across is NOMINAL's replacement libm; see the note in rb.h for
 * when to lift it and which half of it never to lift.
 *
 * NOMINAL's allocation meter did not come across. It bounded scripts the
 * player wrote, and there is no interpreter here yet — it comes back at M4
 * with the Python subset, and it must, for the same reason it existed there:
 * `while True: l.append(l)` needs very few instructions per megabyte.
 */
#include "rb.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

/* ------------------------------------------------------------- allocation */
void *rb_alloc(size_t n)
{
    void *p = calloc(1, n ? n : 1);
    if (!p) { fprintf(stderr, "runbook: out of memory\n"); abort(); }
    return p;
}

void *rb_realloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) { fprintf(stderr, "runbook: out of memory\n"); abort(); }
    return q;
}

void rb_free(void *p) { free(p); }

char *rb_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = rb_alloc(n);
    memcpy(d, s, n);
    return d;
}

/* ---------------------------------------------------------------- buffers */
void buf_init(Buf *b)  { b->p = NULL; b->len = b->cap = 0; }
void buf_free(Buf *b)  { rb_free(b->p); buf_init(b); }
void buf_clear(Buf *b) { b->len = 0; if (b->p) b->p[0] = 0; }

static void buf_reserve(Buf *b, size_t extra)
{
    if (b->len + extra + 1 <= b->cap) return;
    size_t cap = b->cap ? b->cap : 64;
    while (cap < b->len + extra + 1) cap *= 2;
    b->p = rb_realloc(b->p, cap);
    b->cap = cap;
}

void buf_put(Buf *b, const void *data, size_t n)
{
    if (!n) return;
    buf_reserve(b, n);
    memcpy(b->p + b->len, data, n);
    b->len += n;
    b->p[b->len] = 0;
}

void buf_puts(Buf *b, const char *s) { if (s) buf_put(b, s, strlen(s)); }
void buf_putc(Buf *b, char c)        { buf_put(b, &c, 1); }

void buf_printf(Buf *b, const char *fmt, ...)
{
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n < sizeof tmp) { buf_put(b, tmp, (size_t)n); return; }
    char *big = rb_alloc((size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    buf_put(b, big, (size_t)n);
    rb_free(big);
}

/* Fixed-point decimal rendering. printf("%f") drags in locale and libc
 * rounding differences; a world dump must not depend on either, because the
 * determinism gate compares those dumps byte for byte. */
void buf_putnum(Buf *b, double v, int decimals)
{
    if (v != v)    { buf_puts(b, "nan");  return; }
    if (v >  1e18) { buf_puts(b, "inf");  return; }
    if (v < -1e18) { buf_puts(b, "-inf"); return; }
    if (decimals < 0) decimals = 0;
    if (decimals > 9) decimals = 9;

    int neg = v < 0;
    if (neg) v = -v;

    int64_t scale = 1;
    for (int i = 0; i < decimals; i++) scale *= 10;

    /* round half away from zero, in integer space */
    int64_t whole = (int64_t)v;
    double  frac  = v - (double)whole;
    int64_t fscaled = (int64_t)(frac * (double)scale + 0.5);
    if (fscaled >= scale) { whole += 1; fscaled -= scale; }

    if (neg && (whole || fscaled)) buf_putc(b, '-');
    buf_printf(b, "%lld", (long long)whole);
    if (decimals > 0) {
        buf_putc(b, '.');
        char digits[16];
        for (int i = decimals - 1; i >= 0; i--) {
            digits[i] = (char)('0' + (fscaled % 10));
            fscaled /= 10;
        }
        buf_put(b, digits, (size_t)decimals);
    }
}

/* -------------------------------------------------------------------- rng */
void rng_seed(Rng *r, uint64_t seed) { r->s = seed + 0x9E3779B97F4A7C15ULL; }

uint64_t rng_next(Rng *r)
{
    uint64_t z = (r->s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

double rng_unit(Rng *r)
{
    /* 53 bits into [0,1); exact in binary, so identical everywhere */
    return (double)(rng_next(r) >> 11) * (1.0 / 9007199254740992.0);
}

int32_t rng_range(Rng *r, int32_t lo, int32_t hi)
{
    if (hi <= lo) return lo;
    uint64_t span = (uint64_t)(hi - lo) + 1;
    return lo + (int32_t)(rng_next(r) % span);
}

/* ------------------------------------------------------------------- hash */
/* FNV-1a, 64-bit. Not a security hash and not asked to be one: it is the
 * fingerprint the determinism gate compares, so all it has to be is
 * order-sensitive, cheap, and identical on every platform. */
#define FNV_OFFSET 0xCBF29CE484222325ULL
#define FNV_PRIME  0x00000100000001B3ULL

uint64_t rb_hash_init(void) { return FNV_OFFSET; }

uint64_t rb_hash_bytes(uint64_t h, const void *p, size_t n)
{
    const unsigned char *b = p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= FNV_PRIME; }
    return h;
}

uint64_t rb_hash_str(uint64_t h, const char *s)
{
    return s ? rb_hash_bytes(h, s, strlen(s)) : rb_hash_bytes(h, "", 0);
}

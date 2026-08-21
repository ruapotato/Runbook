/* rb.h — shared declarations for the RUNBOOK core.
 *
 * Plain C11, no third-party dependencies, no knowledge of Godot. The core
 * owns the world: the org, its people and machines, and eventually the
 * appliances, tickets and provenance records. Godot is a view of this and
 * nothing more (handoff §13, model/view rule).
 *
 * Lifted from ~/NOMINAL/core/nom.h and util.c — allocation, buffers, the
 * splitmix64 generator and the fp-safe math. Lifted, not forked: this is a
 * new repo and NOMINAL's hull, decks and walking are actively wrong here
 * (handoff decision 2). Nothing spatial came across, and nothing spatial
 * should be added; there is no geometry in this game.
 */
#ifndef RB_H
#define RB_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* ---------------------------------------------------------------- limits */
/* Every one of these raises a named error on overflow. None truncates
 * silently — a world that quietly stops growing at the cap would show up as
 * a balance problem days later instead of as an error now. */
#define RB_ID_MAX     24
#define RB_NAME_MAX   48
#define RB_LINE_MAX   8192
#define RB_ERR_MAX    256

/* --------------------------------------------------------------- buffers */
typedef struct {
    char   *p;
    size_t  len, cap;
} Buf;

void  buf_init(Buf *b);
void  buf_free(Buf *b);
void  buf_clear(Buf *b);
void  buf_put(Buf *b, const void *data, size_t n);
void  buf_puts(Buf *b, const char *s);
void  buf_putc(Buf *b, char c);
void  buf_printf(Buf *b, const char *fmt, ...);
/* Deterministic double formatting: fixed decimals, no locale, no %g. */
void  buf_putnum(Buf *b, double v, int decimals);

void *rb_alloc(size_t n);
void *rb_realloc(void *p, size_t n);
void  rb_free(void *p);
char *rb_strdup(const char *s);

/* ------------------------------------------------------------------- rng */
/* splitmix64. The only source of randomness in the program.
 *
 * Handoff decision 16: failure is deterministic per seed. That is not a
 * nicety — the balance harness (--play, --naive) grades numbers, and numbers
 * that move under you cannot be graded. Anything that wants a random value
 * takes an Rng*; nothing calls rand(), time(), or reads the clock. */
typedef struct { uint64_t s; } Rng;
void     rng_seed(Rng *r, uint64_t seed);
uint64_t rng_next(Rng *r);
/* uniform in [0,1) with 53 bits, deterministic across platforms */
double   rng_unit(Rng *r);
int32_t  rng_range(Rng *r, int32_t lo, int32_t hi);

/* ------------------------------------------------------------------ hash */
/* FNV-1a 64. The world state hash the determinism gate compares. */
uint64_t rb_hash_init(void);
uint64_t rb_hash_bytes(uint64_t h, const void *p, size_t n);
uint64_t rb_hash_str(uint64_t h, const char *s);

/* ----------------------------------------------------------------- fmath */
/* NOMINAL's fp-safe math (floor, sqrt, exp, log, pow, trig) is deliberately
 * NOT here. Nothing in the world model uses floating point yet — growth,
 * attrition and the clock are integer arithmetic in thousandths, so the
 * cross-platform determinism claim costs nothing to keep.
 *
 * When load and latency arrive and fp becomes unavoidable, lift floor/exp/
 * log/pow from ~/NOMINAL/core/util.c rather than calling libm, and lift the
 * trig NOT AT ALL: there is no geometry in this game (handoff §2). The
 * Makefile already pins -ffp-contract=off, so the flags will be right the
 * day the first double lands.
 */

#endif /* RB_H */

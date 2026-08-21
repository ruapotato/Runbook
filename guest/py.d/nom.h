/* nom.h — what NomScript needs, provided by the guest instead of the host.
 *
 * THE LANGUAGE RUNS ON THE EMULATED MACHINE. Handoff decision 13: "player
 * scripts run on the emulated machine. The moat. No other game in this space
 * has a real interpreter on a real machine." That is only true if the
 * interpreter is a RISC-V program on the disk -- and NomScript's lexer,
 * compiler and VM are host C that assumes a host: malloc, snprintf, doubles,
 * a filesystem.
 *
 * This header is the whole adaptation. It gives those files a heap, a
 * growable buffer, the value system and the handful of libc calls they make,
 * all in freestanding rv64 with nothing underneath but syscalls. The four
 * language files are then compiled UNCHANGED except where floating point had
 * to go, and every one of those places is marked.
 *
 * NO FLOATING POINT, and that is the CPU's decision rather than mine.
 * cpu.h: "no floating point -- the F/D extensions are the single largest
 * source of cross-platform result divergence. Integer-only means determinism
 * is structural rather than maintained." A language with doubles in it would
 * need soft-float from compiler-rt, which a -nostdlib program does not have,
 * and would put the determinism guarantee back into the hands of whoever
 * compiled it.
 *
 * It costs nothing worth having. This is a language for provisioning users
 * and parsing API responses: integers, strings, lists and dicts. Nobody
 * onboarding four hundred people needs a cosine.
 */
#ifndef NOM_GUEST_LANG_H
#define NOM_GUEST_LANG_H

#include "gsys.h"

typedef i64 int64_t;
typedef u64 uint64_t;
typedef unsigned int uint32_t;
typedef int int32_t;
typedef unsigned char uint8_t;
typedef u64 size_t;
#ifndef bool
typedef int _lang_bool;
#define bool _lang_bool
#define true 1
#define false 0
#endif
#define NULL ((void *)0)

/* ---------------------------------------------------------------- limits */
#define NOM_PATH_MAX      256
#define NOM_NAME_MAX      64
#define NOM_ARG_MAX       GARG_MAX
#define NOM_STACK_MAX     256
#define NOM_FRAMES_MAX    32
#define NOM_LOCALS_MAX    64
#define NOM_GLOBALS_MAX   128
#define NOM_CONSTS_MAX    512
#define NOM_CODE_MAX      16384
#define NOM_FUNCS_MAX     32
#define NOM_ARGS_MAX      8
#define NOM_LIST_MAX      4096
#define NOM_SRC_MAX       32768
#define NOM_TOK_MAX       8192
#define NOM_ERR_MAX       256

/* ------------------------------------------------------------------ heap
 *
 * A first-fit free list over one static arena. The machine gives a process a
 * megabyte (CPU_MEM_BYTES) and the program itself is about forty kilobytes,
 * so this is most of what is left.
 *
 * It is a real allocator rather than a bump pointer because the VM refcounts
 * and frees: a script that builds a list in a loop and drops it every
 * iteration would exhaust a bump arena in seconds, and a script doing exactly
 * that -- for each ticket, fetch, parse, discard -- is the FIRST thing
 * anybody writes here.
 */
#define HEAP_BYTES (640u * 1024u)

typedef struct Blk { u64 size; struct Blk *next; int used; } Blk;
extern unsigned char g_heap[HEAP_BYTES];
extern Blk *g_free;
extern u64 g_heap_used, g_heap_peak;

void *nom_alloc(size_t n);
void *nom_realloc(void *p, size_t n);
void  nom_free(void *p);
char *nom_strdup(const char *s);

/* The allocation meter, kept because the VM checks it once per instruction
 * and because the reason it exists is even more true here than on the host:
 * `while True: l.append(l)` needs very few instructions per kilobyte, and
 * this heap is 640 of them. */
void   nom_meter_begin(size_t cap);
void   nom_meter_end(void);
bool   nom_meter_over(void);
size_t nom_meter_used(void);

/* --------------------------------------------------------------- strings */
size_t nom_strlen(const char *s);
void  *nom_memcpy(void *d, const void *s, size_t n);
int    nom_strcmp(const char *a, const char *b);
int    nom_strncmp(const char *a, const char *b, size_t n);
int    nom_memcmp(const void *a, const void *b, size_t n);
#define strlen  nom_strlen
#define memcpy  nom_memcpy
#define strcmp  nom_strcmp
#define strncmp nom_strncmp
#define memcmp  nom_memcmp

/* ------------------------------------------------------------------ math
 * THERE IS NO `double` ON THIS MACHINE, so it is a name for an integer here.
 * That one macro is what lets the four language files compile unchanged:
 * every `double` in them becomes an int64, VAL_NUM becomes VAL_INT, and
 * arithmetic that would have produced 2.5 produces 2. A language for
 * provisioning users does not miss the difference, and cpu.h explains at
 * length why the alternative -- soft-float from a compiler-rt that a
 * -nostdlib program does not have -- is worse.
 *
 * A MACRO rather than a typedef, because `double` is a keyword and you
 * cannot typedef one. There are no system headers here for it to collide
 * with; that is what -nostdlib means.
 */
#define double int64_t

double nom_floor(double x);
double nom_pow(double b, double e);

/* --------------------------------------------------------------- buffers */
typedef __builtin_va_list va_list;
#define va_start __builtin_va_start
#define va_end   __builtin_va_end
#define va_arg   __builtin_va_arg

typedef struct { char *p; size_t len, cap; } Buf;

void buf_init(Buf *b);
void buf_free(Buf *b);
void buf_clear(Buf *b);
void buf_put(Buf *b, const void *data, size_t n);
void buf_puts(Buf *b, const char *s);
void buf_putc(Buf *b, char c);
/* A cut-down printf: %s %d %ld %lld %c %% and nothing else, because that is
 * every format string the language uses and a full one is a kilobyte of
 * program for no gain. */
void buf_printf(Buf *b, const char *fmt, ...);
void buf_vprintf(Buf *b, const char *fmt, va_list ap);
void buf_putnum(Buf *b, double v, int decimals);

/* ------------------------------------------------------------- more libc
 * The rest of what the lifted files reach for. snprintf and vsnprintf are
 * the cut-down ones: %s %d %c %% and nothing else, which is every format
 * string in the language. */
void *nom_memset(void *d, int c, size_t n);
void *nom_memmove(void *d, const void *s, size_t n);
int   nom_snprintf(char *out, size_t cap, const char *fmt, ...);
int   nom_vsnprintf(char *out, size_t cap, const char *fmt, va_list ap);
double nom_strtod(const char *s, char **end);
int64_t nom_strtoll(const char *s, char **end, int base);
#define memset    nom_memset
#define memmove   nom_memmove
#define snprintf  nom_snprintf
#define vsnprintf nom_vsnprintf
#define strtod    nom_strtod
#define strtoll   nom_strtoll

typedef struct Obj    Obj;
typedef struct VM     VM;
/* The language's own forward declarations, which NOMINAL's nom.h made and
 * lang.h relies on. */
typedef struct Chunk  Chunk;
typedef struct Func   Func;
typedef struct Prog   Prog;
/* Verbatim from NOMINAL's nom.h. BLOCKED and SLEEP are for a script waiting
 * on a device or a tick, which this machine's scripts do not do -- but the VM
 * mentions them, and inventing a shorter enum would mean editing it. */
typedef enum {
    VM_OK,        /* program ran to completion */
    VM_YIELD,     /* instruction budget exhausted this tick */
    VM_BLOCKED,   /* suspended on a blocking device read */
    VM_SLEEP,     /* explicitly waiting until vm->wake_tick */
    VM_ERROR      /* runtime fault; vm->err set */
} VmStatus;
typedef struct Vfs    Vfs;     /* never instantiated here; the VM holds NULL */
typedef struct Sim    Sim;
typedef struct Ship   Ship;

typedef enum { V_NIL, V_BOOL, V_INT, V_NUM, V_OBJ } VKind;
typedef enum { O_STR, O_LIST, O_DICT, O_FUNC, O_NATIVE } OKind;

typedef struct {
    uint8_t k;
    /* NO DOUBLE IN THE UNION. V_NUM survives as a kind so the lifted files
     * still compile, and nothing ever produces one: every number in this
     * language is an int64. */
    /* `d` and `i` are the same field under two names: `double` is int64_t in
     * this build (see above), and value.c refers to both. Naming it twice
     * costs nothing and saves editing a lifted file. */
    union { int64_t i; double d; Obj *o; } as;
} Value;

struct Obj { uint8_t kind; uint32_t rc; };

typedef struct { Obj o; uint32_t len; char s[]; } Str;
typedef struct { Obj o; uint32_t len, cap; Value *v; } List;
typedef struct { Obj o; uint32_t len, cap; Value *k; Value *v; } Dict;

#define VAL_NIL      ((Value){ .k = V_NIL,  .as.i = 0 })
#define VAL_BOOL(b)  ((Value){ .k = V_BOOL, .as.i = (b) ? 1 : 0 })
#define VAL_INT(n)   ((Value){ .k = V_INT,  .as.i = (n) })
#define VAL_NUM(x)   ((Value){ .k = V_INT,  .as.i = (int64_t)(x) })
#define VAL_OBJ(p)   ((Value){ .k = V_OBJ,  .as.o = (Obj *)(p) })

#define IS_STR(v)   ((v).k == V_OBJ && (v).as.o->kind == O_STR)
#define IS_LIST(v)  ((v).k == V_OBJ && (v).as.o->kind == O_LIST)
#define IS_DICT(v)  ((v).k == V_OBJ && (v).as.o->kind == O_DICT)
#define AS_STR(v)   ((Str  *)(v).as.o)
#define AS_LIST(v)  ((List *)(v).as.o)
#define AS_DICT(v)  ((Dict *)(v).as.o)

Value  str_new(const char *s, size_t len);
Value  str_newz(const char *s);
Value  list_new(void);
void   list_push(List *l, Value v);
Value  dict_new(void);
void   dict_set(Dict *d, Value key, Value v);
bool   dict_get(Dict *d, Value key, Value *out);

Value  val_retain(Value v);
void   val_release(Value v);
bool   val_truthy(Value v);
bool   val_equal(Value a, Value b);
void   val_repr(Buf *b, Value v);
void   val_tostr(Buf *b, Value v);
double val_num(Value v);
bool   nom_parse_number(const char *s, size_t len, Value *out);
int64_t val_int(Value v);

/* The compiler and the VM, as NOMINAL's nom.h declares them. */
Prog *prog_compile(const char *src, const char *name, char *err, size_t errsz);
void  prog_free(Prog *p);
VM   *vm_new(Prog *p, Vfs *fs, Sim *sim);
void  vm_free(VM *v);
VmStatus vm_run(VM *v, int budget);
const char *vm_err(VM *v);
void  vm_set_ship(VM *v, Ship *ship);

Value func_obj_new(int index, bool is_native);
int   func_obj_index(Value v);

#endif

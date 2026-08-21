/* /bin/py — the scripting language, on the machine.
 *
 * A Python subset: indentation, if/elif/else, while, for/in, def/return,
 * and/or/not, True/False/nil, integers, strings, lists and dicts. Handoff
 * decision 14 asked for exactly this -- "the audience knows Python" -- and
 * NOMINAL already had a lexer, compiler and bytecode VM for it, so what this
 * program does is put them on a RISC-V disk.
 *
 *     py script.py        run a file
 *     py -c 'print(1+1)'  run a line
 *
 * WHY IT IS NOT THE SHELL. /bin/sh has loops and pipes and gets a long way,
 * and the first automation anybody writes here will be a shell for loop. What
 * a shell cannot do is hold a parsed answer: `rb ticket.get TCK-1` comes back
 * as a line of JSON, and picking two fields out of it and branching on them
 * is where sh runs out and a language begins. Hence dicts, and hence json().
 */
#include "gsys.h"
#include "nom.h"
#include "lang.h"

/* The language files are compiled as SEPARATE translation units and linked,
 * not #included here. They were included at first, which is the obvious thing
 * when the build takes one .c per program -- and lex.c and compile.c both
 * have a static `emit`, so the first attempt failed to compile in a way that
 * looked like a type error in somebody else's file. Separate units also mean
 * they stay byte-identical to NOMINAL's, which is the whole point. */

static char src[NOM_SRC_MAX];
static char argbuf[GARG_MAX];
static char apibuf[65536];

/* ------------------------------------------------------------- natives --
 *
 * The whole library, and it is short on purpose. Everything the game can do
 * is one call away through api(), so this does not need a function per verb
 * -- it needs the handful of things you cannot write in the language itself.
 */

/* print(...) -- to stdout, which on this machine is the terminal. */
static VmStatus n_print(VM *v, Value *a, int n, Value *out)
{
    (void)v;
    Buf b;
    buf_init(&b);
    for (int i = 0; i < n; i++) {
        if (i) buf_putc(&b, ' ');
        val_tostr(&b, a[i]);
    }
    buf_putc(&b, '\n');
    if (b.p) g_write(1, b.p, b.len);
    buf_free(&b);
    *out = VAL_NIL;
    return VM_OK;
}

/* api(line) -- THE ONE THAT MATTERS. The same protocol the desktop's forms
 * send, from a script, on this machine. Returns the response as a string. */
static VmStatus n_api(VM *v, Value *a, int n, Value *out)
{
    (void)v; (void)n;
    if (!IS_STR(a[0])) { *out = VAL_NIL; return VM_OK; }
    i64 got = sysc(SYS_rbapi, (i64)AS_STR(a[0])->s, (i64)apibuf, (i64)sizeof apibuf);
    if (got < 0) { *out = str_newz("-ERR no world attached\n"); return VM_OK; }
    *out = str_new(apibuf, (size_t)got);
    return VM_OK;
}

/* len, str, int -- the three conversions every script needs in its first
 * five lines. */
static VmStatus n_len(VM *v, Value *a, int n, Value *out)
{
    (void)v; (void)n;
    if (IS_STR(a[0]))  { *out = VAL_INT(AS_STR(a[0])->len);  return VM_OK; }
    if (IS_LIST(a[0])) { *out = VAL_INT(AS_LIST(a[0])->len); return VM_OK; }
    if (IS_DICT(a[0])) { *out = VAL_INT(AS_DICT(a[0])->len); return VM_OK; }
    *out = VAL_INT(0);
    return VM_OK;
}

static VmStatus n_str(VM *v, Value *a, int n, Value *out)
{
    (void)v; (void)n;
    Buf b;
    buf_init(&b);
    val_tostr(&b, a[0]);
    *out = str_new(b.p ? b.p : "", b.len);
    buf_free(&b);
    return VM_OK;
}

static VmStatus n_int(VM *v, Value *a, int n, Value *out)
{
    (void)v; (void)n;
    if (IS_STR(a[0])) {
        const char *s = AS_STR(a[0])->s;
        i64 sign = 1, val = 0;
        while (*s == ' ') s++;
        if (*s == '-') { sign = -1; s++; }
        while (*s >= '0' && *s <= '9') { val = val * 10 + (*s - '0'); s++; }
        *out = VAL_INT(sign * val);
        return VM_OK;
    }
    *out = VAL_INT(val_int(a[0]));
    return VM_OK;
}

/* split(s, sep) -- a list of pieces. The workhorse of reading anything back
 * from the API, and the thing sh can only fake. */
static VmStatus n_split(VM *v, Value *a, int n, Value *out)
{
    (void)v;
    if (!IS_STR(a[0])) { *out = list_new(); return VM_OK; }
    const char *s = AS_STR(a[0])->s;
    u32 slen = AS_STR(a[0])->len;
    const char *sep = (n > 1 && IS_STR(a[1])) ? AS_STR(a[1])->s : "\n";
    size_t seplen = nom_strlen(sep);
    if (!seplen) seplen = 1;

    Value list = list_new();
    size_t start = 0;
    for (size_t i = 0; i + seplen <= slen; ) {
        if (nom_strncmp(s + i, sep, seplen) == 0) {
            list_push(AS_LIST(list), str_new(s + start, i - start));
            i += seplen;
            start = i;
        } else i++;
    }
    list_push(AS_LIST(list), str_new(s + start, slen - start));
    *out = list;
    return VM_OK;
}

/* find(haystack, needle) -- the index, or -1. */
static VmStatus n_find(VM *v, Value *a, int n, Value *out)
{
    (void)v; (void)n;
    *out = VAL_INT(-1);
    if (!IS_STR(a[0]) || !IS_STR(a[1])) return VM_OK;
    const char *h = AS_STR(a[0])->s, *nd = AS_STR(a[1])->s;
    u32 hl = AS_STR(a[0])->len, nl = AS_STR(a[1])->len;
    if (!nl || nl > hl) return VM_OK;
    for (u32 i = 0; i + nl <= hl; i++)
        if (nom_strncmp(h + i, nd, nl) == 0) { *out = VAL_INT((i64)i); return VM_OK; }
    return VM_OK;
}

/* json(text) -- every "key":"value" pair in one flat dict.
 *
 * FLAT, AND NESTING IS WALKED INTO, which is the same decision the desktop's
 * reader made and for the same reason: a ticket carries
 * `"subject":{"ref":"u_00041"}` and what a script wants from that is `ref`.
 * Arrays are kept as their raw text, because walking into one turns its
 * second element into a key.
 *
 * It is not a JSON parser and does not pretend to be. It is the thing that
 * turns one line of an API answer into something you can index.
 */
static VmStatus n_json(VM *v, Value *a, int n, Value *out)
{
    (void)v; (void)n;
    Value d = dict_new();
    *out = d;
    if (!IS_STR(a[0])) return VM_OK;
    const char *s = AS_STR(a[0])->s;
    u32 len = AS_STR(a[0])->len;

    u32 i = 0;
    while (i < len) {
        while (i < len && s[i] != '"') i++;
        if (i >= len) break;
        u32 k0 = ++i;
        while (i < len && s[i] != '"') i++;
        if (i >= len) break;
        u32 k1 = i++;
        while (i < len && (s[i] == ' ' || s[i] == '\t')) i++;
        if (i >= len || s[i] != ':') continue;      /* a string, not a key */
        i++;
        while (i < len && s[i] == ' ') i++;
        if (i >= len) break;

        Value key = str_new(s + k0, k1 - k0);
        if (s[i] == '"') {
            u32 v0 = ++i;
            while (i < len && s[i] != '"') i++;
            dict_set(AS_DICT(d), key, str_new(s + v0, i - v0));
            if (i < len) i++;
        } else if (s[i] == '[') {
            u32 v0 = i;
            int depth = 0;
            while (i < len) {
                if (s[i] == '[') depth++;
                else if (s[i] == ']') { depth--; if (!depth) { i++; break; } }
                i++;
            }
            dict_set(AS_DICT(d), key, str_new(s + v0, i - v0));
        } else if (s[i] == '{') {
            /* walked into: the keys inside land in this same dict */
            i++;
        } else {
            u32 v0 = i;
            while (i < len && s[i] != ',' && s[i] != '}' && s[i] != ']') i++;
            u32 v1 = i;
            while (v1 > v0 && (s[v1 - 1] == ' ')) v1--;
            dict_set(AS_DICT(d), key, str_new(s + v0, v1 - v0));
        }
        /* NO val_release(key) HERE. dict_set TAKES OWNERSHIP of both the key
         * and the value -- it stores them without retaining, and releases the
         * key itself when it overwrites an existing entry. Releasing it here
         * as well freed the string the dict was still pointing at, and the
         * script then failed with `key "id" not present` about a dict that
         * had just been given one. */
    }
    return VM_OK;
}

/* lines(text) -- split on newline, dropping empties. Half of every script
 * starts with it, so it is not worth making everybody write it. */
static VmStatus n_lines(VM *v, Value *a, int n, Value *out)
{
    (void)n;
    Value sep = str_newz("\n");
    Value args[2] = { a[0], sep };
    Value all;
    n_split(v, args, 2, &all);
    val_release(sep);

    Value keep = list_new();
    List *src_l = AS_LIST(all);
    for (u32 i = 0; i < src_l->len; i++) {
        if (IS_STR(src_l->v[i]) && AS_STR(src_l->v[i])->len > 0)
            list_push(AS_LIST(keep), val_retain(src_l->v[i]));
    }
    val_release(all);
    *out = keep;
    return VM_OK;
}

/* lower(s), upper(s) -- the org's naming convention is lowercase, and every
 * script here starts by applying it. */
static VmStatus n_case(VM *v, Value *a, int n, Value *out, int up)
{
    (void)v; (void)n;
    if (!IS_STR(a[0])) { *out = val_retain(a[0]); return VM_OK; }
    Str *s = AS_STR(a[0]);
    Buf b;
    buf_init(&b);
    for (u32 i = 0; i < s->len; i++) {
        char c = s->s[i];
        if (up  && c >= 'a' && c <= 'z') c = (char)(c - 32);
        if (!up && c >= 'A' && c <= 'Z') c = (char)(c + 32);
        buf_putc(&b, c);
    }
    *out = str_new(b.p ? b.p : "", b.len);
    buf_free(&b);
    return VM_OK;
}

static VmStatus n_lower(VM *v, Value *a, int n, Value *out) { return n_case(v, a, n, out, 0); }
static VmStatus n_upper(VM *v, Value *a, int n, Value *out) { return n_case(v, a, n, out, 1); }

/* sub(s, from, to) -- a slice, spelled as a call.
 *
 * The language has no slice syntax and adding one would mean editing the
 * compiler, which is lifted. A function does the same work and reads no
 * worse, and `sub(family, 0, 1)` says what it does. */
static VmStatus n_sub(VM *v, Value *a, int n, Value *out)
{
    (void)v;
    *out = str_newz("");
    if (!IS_STR(a[0])) return VM_OK;
    Str *s = AS_STR(a[0]);
    i64 from = val_int(a[1]);
    i64 to = (n > 2) ? val_int(a[2]) : (i64)s->len;
    if (from < 0) from += (i64)s->len;
    if (to < 0)   to += (i64)s->len;
    if (from < 0) from = 0;
    if (to > (i64)s->len) to = (i64)s->len;
    if (to <= from) return VM_OK;
    val_release(*out);
    *out = str_new(s->s + from, (size_t)(to - from));
    return VM_OK;
}

/* exit(code) -- because a script that has decided it is finished should be
 * able to say so. */
static VmStatus n_exit(VM *v, Value *a, int n, Value *out)
{
    (void)v; (void)out;
    g_exit(n > 0 ? (int)val_int(a[0]) : 0);
    return VM_OK;
}

static const Native NATIVES[] = {
    { "print", 0, 8, n_print },
    { "api",   1, 1, n_api   },
    { "len",   1, 1, n_len   },
    { "str",   1, 1, n_str   },
    { "int",   1, 1, n_int   },
    { "split", 1, 2, n_split },
    { "find",  2, 2, n_find  },
    { "json",  1, 1, n_json  },
    { "lines", 1, 1, n_lines },
    { "lower", 1, 1, n_lower },
    { "upper", 1, 1, n_upper },
    { "sub",   2, 3, n_sub   },
    { "exit",  0, 1, n_exit  },
};

const Native *native_table(int *count)
{
    *count = (int)(sizeof NATIVES / sizeof NATIVES[0]);
    return NATIVES;
}

int native_find(const char *name, int len)
{
    int n = (int)(sizeof NATIVES / sizeof NATIVES[0]);
    for (int i = 0; i < n; i++) {
        if ((int)nom_strlen(NATIVES[i].name) != len) continue;
        if (nom_strncmp(NATIVES[i].name, name, (size_t)len) == 0) return i;
    }
    return -1;
}

/* ------------------------------------------------------------------ main */
static void die(const char *what, const char *detail)
{
    g_puts("py: ");
    g_puts(what);
    if (detail && detail[0]) { g_puts(": "); g_puts(detail); }
    g_puts("\n");
    g_exit(1);
}

void _start(void)
{
    if (g_getarg(argbuf, sizeof argbuf) <= 0) {
        g_puts("usage: py <script>          run a file\n"
               "       py -c '<source>'     run a line\n"
               "\n"
               "A Python subset on this machine: if/elif/else, while, for/in,\n"
               "def/return, integers, strings, lists and dicts.\n"
               "\n"
               "  api(line)     one line of the company's API, as a string\n"
               "  json(text)    one answer, as a dict you can index\n"
               "  lines(text)   an answer, as a list of lines\n"
               "  split, find, sub, lower, upper, len, str, int, print, exit\n"
               "\n"
               "  r = api(\"ticket.list open 5\")\n"
               "  for l in lines(r):\n"
               "      t = json(l)\n"
               "      print(t[\"id\"], t[\"ref\"])\n");
        g_exit(2);
    }

    const char *text = argbuf;
    if (argbuf[0] == '-' && argbuf[1] == 'c') {
        /* A NEWLINE ON THE END, and it is not cosmetic: this is an
         * indentation-sensitive language, and its lexer ends a statement at a
         * NEWLINE token. `py -c 'print(1)'` compiled to nothing at all and
         * exited silently -- no output, no error -- until the line was
         * terminated the way a line in a file always is. */
        const char *from = argbuf + 2;
        while (*from == ' ') from++;
        /* AND ITS QUOTES TAKEN OFF. The shell hands the argument through with
         * them still on, so `py -c "print(9)"` arrived as source reading
         * "print(9)" -- a perfectly valid string literal, evaluated and
         * discarded, printing nothing and reporting no error. Twenty minutes
         * of a program that did exactly what it was told. */
        size_t flen = 0;
        while (from[flen]) flen++;
        if (flen >= 2 && (from[0] == '"' || from[0] == '\'') && from[flen - 1] == from[0]) {
            from++;
            flen -= 2;
        }
        size_t n = 0;
        while (n < flen && n < sizeof src - 2) { src[n] = from[n]; n++; }
        src[n++] = '\n';
        src[n] = 0;
        text = src;
    } else {
        i64 n = g_slurp(argbuf, src, sizeof src);
        if (n < 0) die("cannot read", argbuf);
        src[n] = 0;
        text = src;
    }

    char err[NOM_ERR_MAX] = "";
    Prog *prog = prog_compile(text, "script", err, sizeof err);
    if (!prog) die("syntax", err);

    /* The meter: 512 KB of the 640 the heap has. A script that runs away is
     * stopped by the VM rather than by the machine falling over, and the
     * message says which. */
    nom_meter_begin(512u * 1024u);
    VM *vm = vm_new(prog, 0, 0);
    if (!vm) die("out of memory", "");

    /* Instructions, not seconds: this machine has no clock a program can read
     * and that is deliberate (cpu.h). Ten million is a long script and a very
     * short wait. */
    VmStatus st = vm_run(vm, 10000000);
    if (st == VM_ERROR)    die("error", vm_err(vm));
    if (st == VM_YIELD)    die("ran too long -- ten million instructions", "");

    int code = 0;
    vm_free(vm);
    prog_free(prog);
    g_exit(code);
}

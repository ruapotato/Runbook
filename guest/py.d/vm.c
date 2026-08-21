/* vm.c — the NomScript bytecode interpreter.
 *
 * Two properties matter more than speed here:
 *
 *  1. `vm_run(v, budget)` executes at most `budget` instructions and returns
 *     VM_YIELD. That is the per-tick CPU budget: a script cannot hang the
 *     simulation and cannot win by burning more CPU than its opponent.
 *
 *  2. A native call that would block does NOT consume its operands. It rewinds
 *     `ip` to the OP_CALL and returns VM_BLOCKED, so resuming re-executes the
 *     identical call. Retry is idempotent by construction. See D4.
 */
#include "lang.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

void vm_runtime_error(VM *v, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(v->err, sizeof v->err, fmt, ap);
    va_end(ap);
}

VM *vm_new(Prog *p, Vfs *fs, Sim *sim)
{
    VM *v = nom_alloc(sizeof(VM));
    v->prog = p;
    v->fs = fs;
    v->sim = sim;
    v->nframes = 1;
    v->frame[0].fn = &p->func[0];
    v->frame[0].ip = 0;
    v->frame[0].base = 0;
    v->sp = p->func[0].nlocals;   /* top-level locals are unused but reserved */
    for (int i = 0; i < v->sp; i++) v->stack[i] = VAL_NIL;
    return v;
}

void vm_free(VM *v)
{
    if (!v) return;
    for (int i = 0; i < v->sp; i++) val_release(v->stack[i]);
    for (int i = 0; i < NOM_GLOBALS_MAX; i++)
        if (v->gdef[i]) val_release(v->global[i]);
    nom_free(v);
}

void vm_set_ship(VM *v, Ship *ship) { v->ship = ship; }

const char *vm_err(VM *v)        { return v->err; }
uint64_t    vm_steps(VM *v)      { return v->steps; }
uint64_t    vm_wake_tick(VM *v)  { return v->wake_tick; }
const char *vm_blocked_on(VM *v) { return v->blocked_on; }
int         vm_line(VM *v)       { return v->line; }

/* ------------------------------------------------------------ stack ops */
static bool push(VM *v, Value val)
{
    if (v->sp >= NOM_STACK_MAX) {
        vm_runtime_error(v, "value stack overflow (STACK_MAX=%d)", NOM_STACK_MAX);
        val_release(val);
        return false;
    }
    v->stack[v->sp++] = val;
    return true;
}

static Value pop(VM *v)  { return v->stack[--v->sp]; }
static Value peek(VM *v, int d) { return v->stack[v->sp - 1 - d]; }

/* --------------------------------------------------------------- helpers */
static const char *type_name(Value v)
{
    switch (v.k) {
    case V_NIL:  return "nil";
    case V_BOOL: return "bool";
    case V_INT:  return "int";
    case V_NUM:  return "num";
    case V_OBJ:
        switch (v.as.o->kind) {
        case O_STR:  return "str";
        case O_LIST: return "list";
        case O_DICT: return "dict";
        default:     return "function";
        }
    }
    return "?";
}

static bool is_number(Value v) { return v.k == V_INT || v.k == V_NUM || v.k == V_BOOL; }

/* Arithmetic keeps ints as ints (so tick counters stay exact) and promotes to
 * double only when an operand already is one. `/` is always true division. */
static bool arith(VM *v, OpCode op, Value a, Value b, Value *out)
{
    if (IS_STR(a) && IS_STR(b) && op == OP_ADD) {
        Str *x = AS_STR(a), *y = AS_STR(b);
        Buf t; buf_init(&t);
        buf_put(&t, x->s, x->len);
        buf_put(&t, y->s, y->len);
        *out = str_new(t.p ? t.p : "", t.len);
        buf_free(&t);
        return true;
    }
    if (IS_LIST(a) && IS_LIST(b) && op == OP_ADD) {
        Value r = list_new();
        List *x = AS_LIST(a), *y = AS_LIST(b), *z = AS_LIST(r);
        for (uint32_t i = 0; i < x->len; i++) list_push(z, val_retain(x->v[i]));
        for (uint32_t i = 0; i < y->len; i++) list_push(z, val_retain(y->v[i]));
        *out = r;
        return true;
    }
    if (IS_STR(a) && b.k == V_INT && op == OP_MUL) {
        Str *x = AS_STR(a);
        int64_t n = b.as.i;
        Buf t; buf_init(&t);
        for (int64_t i = 0; i < n && t.len < 65536; i++) buf_put(&t, x->s, x->len);
        *out = str_new(t.p ? t.p : "", t.len);
        buf_free(&t);
        return true;
    }
    if (!is_number(a) || !is_number(b)) {
        vm_runtime_error(v, "unsupported operand types for arithmetic: %s and %s",
                         type_name(a), type_name(b));
        return false;
    }

    bool ints = (a.k != V_NUM) && (b.k != V_NUM);
    if (ints) {
        int64_t x = val_int(a), y = val_int(b);
        switch (op) {
        case OP_ADD: *out = VAL_INT(x + y); return true;
        case OP_SUB: *out = VAL_INT(x - y); return true;
        case OP_MUL: *out = VAL_INT(x * y); return true;
        case OP_DIV:
            if (y == 0) { vm_runtime_error(v, "division by zero"); return false; }
            *out = VAL_NUM((double)x / (double)y);   /* Python-3 true division */
            return true;
        case OP_FDIV:
            if (y == 0) { vm_runtime_error(v, "floor division by zero"); return false; }
            { int64_t q = x / y; if ((x % y != 0) && ((x < 0) != (y < 0))) q--; *out = VAL_INT(q); }
            return true;
        case OP_MOD:
            if (y == 0) { vm_runtime_error(v, "modulo by zero"); return false; }
            { int64_t r = x % y; if (r != 0 && ((r < 0) != (y < 0))) r += y; *out = VAL_INT(r); }
            return true;
        case OP_POW:
            if (y >= 0 && y < 63) {
                int64_t r = 1;
                for (int64_t i = 0; i < y; i++) r *= x;
                *out = VAL_INT(r);
            } else {
                *out = VAL_NUM(nom_pow((double)x, (double)y));
            }
            return true;
        default: break;
        }
    }

    double x = val_num(a), y = val_num(b);
    switch (op) {
    case OP_ADD:  *out = VAL_NUM(x + y); return true;
    case OP_SUB:  *out = VAL_NUM(x - y); return true;
    case OP_MUL:  *out = VAL_NUM(x * y); return true;
    case OP_DIV:
        if (y == 0.0) { vm_runtime_error(v, "division by zero"); return false; }
        *out = VAL_NUM(x / y);
        return true;
    case OP_FDIV:
        if (y == 0.0) { vm_runtime_error(v, "floor division by zero"); return false; }
        *out = VAL_NUM(nom_floor(x / y));
        return true;
    case OP_MOD:
        if (y == 0.0) { vm_runtime_error(v, "modulo by zero"); return false; }
        { double r = x - y * nom_floor(x / y); *out = VAL_NUM(r); }
        return true;
    case OP_POW:  *out = VAL_NUM(nom_pow(x, y)); return true;
    default: break;
    }
    vm_runtime_error(v, "bad arithmetic opcode");
    return false;
}

static bool compare(VM *v, OpCode op, Value a, Value b, bool *out)
{
    if (op == OP_EQ) { *out = val_equal(a, b); return true; }
    if (op == OP_NE) { *out = !val_equal(a, b); return true; }
    if (IS_STR(a) && IS_STR(b)) {
        Str *x = AS_STR(a), *y = AS_STR(b);
        uint32_t n = x->len < y->len ? x->len : y->len;
        int c = memcmp(x->s, y->s, n);
        if (c == 0) c = (x->len < y->len) ? -1 : (x->len > y->len) ? 1 : 0;
        switch (op) {
        case OP_LT: *out = c <  0; return true;
        case OP_LE: *out = c <= 0; return true;
        case OP_GT: *out = c >  0; return true;
        case OP_GE: *out = c >= 0; return true;
        default: break;
        }
    }
    if (!is_number(a) || !is_number(b)) {
        vm_runtime_error(v, "cannot order %s against %s", type_name(a), type_name(b));
        return false;
    }
    double x = val_num(a), y = val_num(b);
    switch (op) {
    case OP_LT: *out = x <  y; return true;
    case OP_LE: *out = x <= y; return true;
    case OP_GT: *out = x >  y; return true;
    case OP_GE: *out = x >= y; return true;
    default: break;
    }
    return false;
}

static bool index_get(VM *v, Value box, Value key, Value *out)
{
    if (IS_LIST(box)) {
        if (key.k != V_INT) { vm_runtime_error(v, "list index must be an int, got %s", type_name(key)); return false; }
        List *l = AS_LIST(box);
        int64_t i = key.as.i;
        if (i < 0) i += l->len;
        if (i < 0 || i >= (int64_t)l->len) {
            vm_runtime_error(v, "list index %lld out of range (len %u)", (long long)key.as.i, l->len);
            return false;
        }
        *out = val_retain(l->v[i]);
        return true;
    }
    if (IS_DICT(box)) {
        Value got;
        if (!dict_get(AS_DICT(box), key, &got)) {
            Buf b; buf_init(&b);
            val_repr(&b, key);
            vm_runtime_error(v, "key %s not present", b.p ? b.p : "?");
            buf_free(&b);
            return false;
        }
        *out = val_retain(got);
        return true;
    }
    if (IS_STR(box)) {
        if (key.k != V_INT) { vm_runtime_error(v, "string index must be an int"); return false; }
        Str *s = AS_STR(box);
        int64_t i = key.as.i;
        if (i < 0) i += s->len;
        if (i < 0 || i >= (int64_t)s->len) { vm_runtime_error(v, "string index out of range"); return false; }
        *out = str_new(s->s + i, 1);
        return true;
    }
    vm_runtime_error(v, "cannot index a %s", type_name(box));
    return false;
}

/* -------------------------------------------------------------- the loop */
#define READ_BYTE()  (chunk->code[f->ip++])
#define READ_U16()   (f->ip += 2, (int)((chunk->code[f->ip - 2] << 8) | chunk->code[f->ip - 1]))
#define READ_S16()   ((int16_t)READ_U16())

VmStatus vm_run(VM *v, int budget)
{
    if (v->done) return VM_OK;
    if (v->err[0]) return VM_ERROR;

    struct Frame *f = &v->frame[v->nframes - 1];
    Chunk *chunk = &f->fn->chunk;

    while (budget-- > 0) {
        if (f->ip < 0 || f->ip >= chunk->len) {
            vm_runtime_error(v, "instruction pointer ran off the end of %s", f->fn->name);
            return VM_ERROR;
        }
        v->line = chunk->line[f->ip];
        /* THE MEMORY BOUND, CHECKED WHERE TIME IS CHECKED.
         *
         * The instruction budget above stops a script that spins. It does not
         * stop one that allocates: `while true: l = append(l, l)` runs four
         * instructions per iteration and eats the machine. So the allocation
         * meter is read once per instruction, which caps the overshoot past
         * the limit at whatever ONE opcode can allocate -- and every opcode
         * that allocates is itself bounded (a string repeat stops at 64 KiB,
         * a list literal at its own length).
         *
         * It is a runtime error and not a yield, because a script that has
         * hit the wall must not be resumable into hitting it again. */
        if (nom_meter_over()) {
            vm_runtime_error(v, "memory budget exhausted (%zu bytes allocated)",
                             nom_meter_used());
            return VM_ERROR;
        }
        v->steps++;
        int call_site = f->ip;
        uint8_t op = READ_BYTE();

        switch (op) {
        case OP_CONST: { int k = READ_U16(); if (!push(v, val_retain(v->prog->consts[k]))) return VM_ERROR; break; }
        case OP_NIL:   if (!push(v, VAL_NIL)) return VM_ERROR; break;
        case OP_TRUE:  if (!push(v, VAL_BOOL(true))) return VM_ERROR; break;
        case OP_FALSE: if (!push(v, VAL_BOOL(false))) return VM_ERROR; break;
        case OP_POP:   val_release(pop(v)); break;
        case OP_DUP:   if (!push(v, val_retain(peek(v, 0)))) return VM_ERROR; break;

        case OP_GETLOCAL: {
            int s = READ_BYTE();
            if (!push(v, val_retain(v->stack[f->base + s]))) return VM_ERROR;
            break;
        }
        case OP_SETLOCAL: {
            int s = READ_BYTE();
            val_release(v->stack[f->base + s]);
            v->stack[f->base + s] = pop(v);
            break;
        }
        case OP_GETGLOBAL: {
            int g = READ_U16();
            if (!v->gdef[g]) {
                vm_runtime_error(v, "'%s' used before it was assigned", v->prog->global[g]);
                return VM_ERROR;
            }
            if (!push(v, val_retain(v->global[g]))) return VM_ERROR;
            break;
        }
        case OP_SETGLOBAL: {
            int g = READ_U16();
            if (v->gdef[g]) val_release(v->global[g]);
            v->global[g] = pop(v);
            v->gdef[g] = true;
            break;
        }
        case OP_GETNATIVE: { int n = READ_BYTE(); if (!push(v, func_obj_new(n, true))) return VM_ERROR; break; }
        case OP_GETFUNC:   { int n = READ_BYTE(); if (!push(v, func_obj_new(n, false))) return VM_ERROR; break; }

        case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV:
        case OP_FDIV: case OP_MOD: case OP_POW: {
            Value b = pop(v), a = pop(v), r;
            bool ok = arith(v, (OpCode)op, a, b, &r);
            val_release(a); val_release(b);
            if (!ok) return VM_ERROR;
            if (!push(v, r)) return VM_ERROR;
            break;
        }
        case OP_NEG: {
            Value a = pop(v);
            if (!is_number(a)) { vm_runtime_error(v, "cannot negate a %s", type_name(a)); val_release(a); return VM_ERROR; }
            Value r = (a.k == V_NUM) ? VAL_NUM(-a.as.d) : VAL_INT(-val_int(a));
            val_release(a);
            if (!push(v, r)) return VM_ERROR;
            break;
        }
        case OP_EQ: case OP_NE: case OP_LT: case OP_LE: case OP_GT: case OP_GE: {
            Value b = pop(v), a = pop(v);
            bool r = false;
            bool ok = compare(v, (OpCode)op, a, b, &r);
            val_release(a); val_release(b);
            if (!ok) return VM_ERROR;
            if (!push(v, VAL_BOOL(r))) return VM_ERROR;
            break;
        }
        case OP_IN: {
            Value box = pop(v), needle = pop(v);
            bool found = false;
            if (IS_LIST(box)) {
                List *l = AS_LIST(box);
                for (uint32_t i = 0; i < l->len && !found; i++) found = val_equal(l->v[i], needle);
            } else if (IS_DICT(box)) {
                Value dummy;
                found = dict_get(AS_DICT(box), needle, &dummy);
            } else if (IS_STR(box) && IS_STR(needle)) {
                Str *h = AS_STR(box), *n = AS_STR(needle);
                if (n->len == 0) found = true;
                else for (uint32_t i = 0; i + n->len <= h->len && !found; i++)
                    found = memcmp(h->s + i, n->s, n->len) == 0;
            } else {
                vm_runtime_error(v, "'in' needs a list, dict or string, got %s", type_name(box));
                val_release(box); val_release(needle);
                return VM_ERROR;
            }
            val_release(box); val_release(needle);
            if (!push(v, VAL_BOOL(found))) return VM_ERROR;
            break;
        }
        case OP_NOT: { Value a = pop(v); bool t = val_truthy(a); val_release(a); if (!push(v, VAL_BOOL(!t))) return VM_ERROR; break; }

        case OP_JMP:  { int off = READ_S16(); f->ip += off; break; }
        case OP_JMPF: { int off = READ_S16(); Value c = pop(v); if (!val_truthy(c)) f->ip += off; val_release(c); break; }
        case OP_JMPF_KEEP: { int off = READ_S16(); if (!val_truthy(peek(v, 0))) f->ip += off; break; }
        case OP_JMPT_KEEP: { int off = READ_S16(); if ( val_truthy(peek(v, 0))) f->ip += off; break; }

        case OP_LIST: {
            int n = READ_BYTE();
            Value l = list_new();
            for (int i = 0; i < n; i++) list_push(AS_LIST(l), v->stack[v->sp - n + i]);
            v->sp -= n;
            if (!push(v, l)) return VM_ERROR;
            break;
        }
        case OP_DICT: {
            int n = READ_BYTE();
            Value d = dict_new();
            for (int i = 0; i < n; i++) {
                Value k = v->stack[v->sp - 2 * n + 2 * i];
                Value val = v->stack[v->sp - 2 * n + 2 * i + 1];
                dict_set(AS_DICT(d), k, val);
            }
            v->sp -= 2 * n;
            if (!push(v, d)) return VM_ERROR;
            break;
        }
        case OP_INDEX: {
            Value key = pop(v), box = pop(v), r;
            bool ok = index_get(v, box, key, &r);
            val_release(key); val_release(box);
            if (!ok) return VM_ERROR;
            if (!push(v, r)) return VM_ERROR;
            break;
        }
        case OP_SETINDEX: {
            Value val = pop(v), key = pop(v), box = pop(v);
            if (IS_LIST(box)) {
                if (key.k != V_INT) { vm_runtime_error(v, "list index must be an int"); goto setindex_fail; }
                List *l = AS_LIST(box);
                int64_t i = key.as.i;
                if (i < 0) i += l->len;
                if (i < 0 || i >= (int64_t)l->len) { vm_runtime_error(v, "list index out of range"); goto setindex_fail; }
                val_release(l->v[i]);
                l->v[i] = val;
                val_release(key); val_release(box);
            } else if (IS_DICT(box)) {
                dict_set(AS_DICT(box), key, val);   /* takes both */
                val_release(box);
            } else {
                vm_runtime_error(v, "cannot assign into a %s", type_name(box));
                goto setindex_fail;
            }
            break;
        setindex_fail:
            val_release(val); val_release(key); val_release(box);
            return VM_ERROR;
        }

        case OP_FORPREP: {
            Value seq = peek(v, 0);
            if (!IS_LIST(seq) && !IS_STR(seq) && !IS_DICT(seq)) {
                vm_runtime_error(v, "cannot iterate over a %s", type_name(seq));
                return VM_ERROR;
            }
            if (!push(v, VAL_INT(0))) return VM_ERROR;
            break;
        }
        case OP_FORNEXT: {
            int off = READ_S16();
            Value seq = peek(v, 1);
            int64_t i = peek(v, 0).as.i;
            uint32_t n = IS_LIST(seq) ? AS_LIST(seq)->len
                       : IS_DICT(seq) ? AS_DICT(seq)->len
                       : AS_STR(seq)->len;
            if (i >= (int64_t)n) { f->ip += off; break; }
            v->stack[v->sp - 1] = VAL_INT(i + 1);
            Value elem = IS_LIST(seq) ? val_retain(AS_LIST(seq)->v[i])
                       : IS_DICT(seq) ? val_retain(AS_DICT(seq)->k[i])
                       : str_new(AS_STR(seq)->s + i, 1);
            if (!push(v, elem)) return VM_ERROR;
            break;
        }

        case OP_CALL: {
            int nargs = READ_BYTE();
            Value callee = v->stack[v->sp - nargs - 1];
            if (callee.k != V_OBJ ||
                (callee.as.o->kind != O_NATIVE && callee.as.o->kind != O_FUNC)) {
                vm_runtime_error(v, "cannot call a %s", type_name(callee));
                return VM_ERROR;
            }
            int idx = func_obj_index(callee);

            if (callee.as.o->kind == O_NATIVE) {
                int ncount = 0;
                const Native *tbl = native_table(&ncount);
                const Native *nat = &tbl[idx];
                if (nargs < nat->minargs || nargs > nat->maxargs) {
                    vm_runtime_error(v, "%s() takes %d..%d arguments, got %d",
                                     nat->name, nat->minargs, nat->maxargs, nargs);
                    return VM_ERROR;
                }
                Value out = VAL_NIL;
                VmStatus st = nat->fn(v, &v->stack[v->sp - nargs], nargs, &out);
                if (st == VM_BLOCKED || st == VM_SLEEP) {
                    /* Leave the operands exactly as they are and re-run this
                     * very instruction when we resume. */
                    f->ip = call_site;
                    return st;
                }
                if (st == VM_ERROR) return VM_ERROR;
                for (int i = 0; i < nargs; i++) val_release(v->stack[v->sp - 1 - i]);
                v->sp -= nargs;
                val_release(pop(v));            /* the callee */
                if (!push(v, out)) return VM_ERROR;
                break;
            }

            Func *fn = &v->prog->func[idx];
            if (nargs != fn->arity) {
                vm_runtime_error(v, "%s() takes %d arguments, got %d", fn->name, fn->arity, nargs);
                return VM_ERROR;
            }
            if (v->nframes >= NOM_FRAMES_MAX) {
                vm_runtime_error(v, "call depth exceeded (FRAMES_MAX=%d)", NOM_FRAMES_MAX);
                return VM_ERROR;
            }
            /* args are already in place; reserve the rest of the locals */
            int base = v->sp - nargs;
            for (int i = nargs; i < fn->nlocals; i++)
                if (!push(v, VAL_NIL)) return VM_ERROR;
            struct Frame *nf = &v->frame[v->nframes++];
            nf->fn = fn;
            nf->ip = 0;
            nf->base = base;
            f = nf;
            chunk = &fn->chunk;
            break;
        }
        case OP_RET: {
            Value r = pop(v);
            int base = f->base;
            while (v->sp > base) val_release(pop(v));
            val_release(pop(v));                /* the callee object */
            v->nframes--;
            if (v->nframes == 0) { v->done = true; val_release(r); return VM_OK; }
            f = &v->frame[v->nframes - 1];
            chunk = &f->fn->chunk;
            if (!push(v, r)) return VM_ERROR;
            break;
        }
        case OP_HALT:
            v->done = true;
            return VM_OK;

        default:
            vm_runtime_error(v, "bad opcode %d", op);
            return VM_ERROR;
        }
    }
    return VM_YIELD;
}

/* value.c — NomScript's dynamically-typed value model.
 *
 * Reference counted. Dicts are insertion-ordered arrays, not hash tables:
 * iteration order is part of observable program behaviour and therefore part
 * of the replay. See D3.
 */
#include "nom.h"
#include <string.h>
#include <stdio.h>

typedef struct { Obj o; int index; } RefObj;  /* O_FUNC / O_NATIVE */

Value str_new(const char *s, size_t len)
{
    Str *o = nom_alloc(sizeof(Str) + len + 1);
    o->o.kind = O_STR;
    o->o.rc = 1;
    o->len = (uint32_t)len;
    if (len) memcpy(o->s, s, len);
    o->s[len] = 0;
    return VAL_OBJ(o);
}

Value str_newz(const char *s) { return str_new(s, s ? strlen(s) : 0); }

Value list_new(void)
{
    List *l = nom_alloc(sizeof(List));
    l->o.kind = O_LIST;
    l->o.rc = 1;
    return VAL_OBJ(l);
}

void list_push(List *l, Value v)
{
    if (l->len == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 8;
        l->v = nom_realloc(l->v, l->cap * sizeof(Value));
    }
    l->v[l->len++] = v;
}

Value dict_new(void)
{
    Dict *d = nom_alloc(sizeof(Dict));
    d->o.kind = O_DICT;
    d->o.rc = 1;
    return VAL_OBJ(d);
}

void dict_set(Dict *d, Value key, Value v)
{
    for (uint32_t i = 0; i < d->len; i++) {
        if (val_equal(d->k[i], key)) {
            val_release(d->v[i]);
            d->v[i] = v;
            val_release(key);
            return;
        }
    }
    if (d->len == d->cap) {
        d->cap = d->cap ? d->cap * 2 : 8;
        d->k = nom_realloc(d->k, d->cap * sizeof(Value));
        d->v = nom_realloc(d->v, d->cap * sizeof(Value));
    }
    d->k[d->len] = key;
    d->v[d->len] = v;
    d->len++;
}

bool dict_get(Dict *d, Value key, Value *out)
{
    for (uint32_t i = 0; i < d->len; i++)
        if (val_equal(d->k[i], key)) { *out = d->v[i]; return true; }
    return false;
}

Value func_obj_new(int index, bool native)
{
    RefObj *r = nom_alloc(sizeof(RefObj));
    r->o.kind = native ? O_NATIVE : O_FUNC;
    r->o.rc = 1;
    r->index = index;
    return VAL_OBJ(r);
}

int func_obj_index(Value v) { return ((RefObj *)v.as.o)->index; }

Value val_retain(Value v)
{
    if (v.k == V_OBJ) v.as.o->rc++;
    return v;
}

void val_release(Value v)
{
    if (v.k != V_OBJ) return;
    Obj *o = v.as.o;
    if (--o->rc) return;
    switch (o->kind) {
    case O_LIST: {
        List *l = (List *)o;
        for (uint32_t i = 0; i < l->len; i++) val_release(l->v[i]);
        nom_free(l->v);
        break;
    }
    case O_DICT: {
        Dict *d = (Dict *)o;
        for (uint32_t i = 0; i < d->len; i++) { val_release(d->k[i]); val_release(d->v[i]); }
        nom_free(d->k);
        nom_free(d->v);
        break;
    }
    default: break;
    }
    nom_free(o);
}

bool val_truthy(Value v)
{
    switch (v.k) {
    case V_NIL:  return false;
    case V_BOOL: return v.as.i != 0;
    case V_INT:  return v.as.i != 0;
    case V_NUM:  return v.as.d != 0.0;
    case V_OBJ:
        switch (v.as.o->kind) {
        case O_STR:  return AS_STR(v)->len != 0;
        case O_LIST: return AS_LIST(v)->len != 0;
        case O_DICT: return AS_DICT(v)->len != 0;
        default:     return true;
        }
    }
    return false;
}

bool val_equal(Value a, Value b)
{
    if (a.k == V_OBJ && b.k == V_OBJ) {
        if (a.as.o->kind != b.as.o->kind) return false;
        if (a.as.o->kind == O_STR) {
            Str *x = AS_STR(a), *y = AS_STR(b);
            return x->len == y->len && memcmp(x->s, y->s, x->len) == 0;
        }
        if (a.as.o->kind == O_LIST) {
            List *x = AS_LIST(a), *y = AS_LIST(b);
            if (x->len != y->len) return false;
            for (uint32_t i = 0; i < x->len; i++)
                if (!val_equal(x->v[i], y->v[i])) return false;
            return true;
        }
        return a.as.o == b.as.o;
    }
    if (a.k == V_OBJ || b.k == V_OBJ) return false;
    if (a.k == V_NIL || b.k == V_NIL) return a.k == b.k;
    if (a.k == V_NUM || b.k == V_NUM) return val_num(a) == val_num(b);
    return val_int(a) == val_int(b);
}

double val_num(Value v)
{
    switch (v.k) {
    case V_INT:  return (double)v.as.i;
    case V_NUM:  return v.as.d;
    case V_BOOL: return v.as.i ? 1.0 : 0.0;
    default:     return 0.0;
    }
}

int64_t val_int(Value v)
{
    switch (v.k) {
    case V_INT:  return v.as.i;
    case V_NUM:  return (int64_t)v.as.d;
    case V_BOOL: return v.as.i ? 1 : 0;
    default:     return 0;
    }
}

static void emit(Buf *b, Value v, bool quoted)
{
    switch (v.k) {
    case V_NIL:  buf_puts(b, "nil"); return;
    case V_BOOL: buf_puts(b, v.as.i ? "true" : "false"); return;
    case V_INT:  buf_printf(b, "%lld", (long long)v.as.i); return;
    case V_NUM:  buf_putnum(b, v.as.d, 4); return;
    case V_OBJ: break;
    }
    switch (v.as.o->kind) {
    case O_STR:
        if (quoted) {
            buf_putc(b, '"');
            Str *s = AS_STR(v);
            for (uint32_t i = 0; i < s->len; i++) {
                char c = s->s[i];
                if (c == '"' || c == '\\') { buf_putc(b, '\\'); buf_putc(b, c); }
                else if (c == '\n') buf_puts(b, "\\n");
                else buf_putc(b, c);
            }
            buf_putc(b, '"');
        } else {
            buf_put(b, AS_STR(v)->s, AS_STR(v)->len);
        }
        return;
    case O_LIST: {
        List *l = AS_LIST(v);
        buf_putc(b, '[');
        for (uint32_t i = 0; i < l->len; i++) {
            if (i) buf_puts(b, ", ");
            emit(b, l->v[i], true);
        }
        buf_putc(b, ']');
        return;
    }
    case O_DICT: {
        Dict *d = AS_DICT(v);
        buf_putc(b, '{');
        for (uint32_t i = 0; i < d->len; i++) {
            if (i) buf_puts(b, ", ");
            emit(b, d->k[i], true);
            buf_puts(b, ": ");
            emit(b, d->v[i], true);
        }
        buf_putc(b, '}');
        return;
    }
    default:
        buf_puts(b, "<function>");
        return;
    }
}

void val_repr(Buf *b, Value v)  { emit(b, v, true);  }
void val_tostr(Buf *b, Value v) { emit(b, v, false); }

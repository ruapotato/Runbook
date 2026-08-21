/* compile.c — NomScript parser and code generator, single pass.
 *
 * Statement dispatch is deterministic in the spirit of HAMSH_SPEC 2: a
 * statement is a control construct if it starts with a reserved word, an
 * assignment if it matches TARGET (= | += | ...) , and otherwise an
 * expression. There is no heuristic "is this code or a command" step.
 */
#include "lang.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

typedef struct Compiler {
    Func   *fn;
    char    local[NOM_LOCALS_MAX][NOM_NAME_MAX];
    int     nlocals;
    bool    toplevel;
    struct Compiler *enclosing;
} Compiler;

typedef struct Loop {
    int  start;            /* continue target */
    int  exitpatch[32];
    int  nexit;
    struct Loop *prev;
} Loop;

typedef struct {
    TokenList tl;
    int       cur;
    Prog     *prog;
    Compiler *comp;
    Loop     *loop;
    char     *err;
    size_t    errsz;
    bool      failed;
} P;

static void expression(P *p);
static void statement(P *p);
static void block(P *p);

/* ------------------------------------------------------------- utilities */
static void perr(P *p, const char *fmt, ...)
{
    if (p->failed) return;
    p->failed = true;
    char msg[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    int line = p->cur < p->tl.ntok ? p->tl.tok[p->cur].line : 0;
    snprintf(p->err, p->errsz, "parse error [line %d]: %s", line, msg);
}

static Token *peek(P *p)  { return &p->tl.tok[p->cur]; }
static Token *prev(P *p)  { return &p->tl.tok[p->cur - 1]; }
static bool check(P *p, TokType t) { return peek(p)->type == t; }
static Token *advance(P *p)
{
    if (p->cur < p->tl.ntok - 1) p->cur++;
    return prev(p);
}
static bool match(P *p, TokType t) { if (check(p, t)) { advance(p); return true; } return false; }
static void expect(P *p, TokType t, const char *what)
{
    if (!match(p, t)) perr(p, "expected %s", what);
}
/* Newlines and stray INDENT/DEDENT are separators, never syntax. */
static void skip_seps(P *p)
{
    while (check(p, T_NEWLINE) || check(p, T_INDENT) || check(p, T_DEDENT)) advance(p);
}

/* ----------------------------------------------------------------- emit */
static Chunk *cur_chunk(P *p) { return &p->comp->fn->chunk; }

static void emit(P *p, uint8_t b)
{
    Chunk *c = cur_chunk(p);
    if (c->len >= NOM_CODE_MAX) { perr(p, "script too large (CODE_MAX)"); return; }
    if (c->len == c->cap) {
        c->cap = c->cap ? c->cap * 2 : 128;
        c->code = nom_realloc(c->code, (size_t)c->cap);
        c->line = nom_realloc(c->line, (size_t)c->cap * sizeof(int));
    }
    c->line[c->len] = p->cur ? prev(p)->line : 1;
    c->code[c->len++] = b;
}

static void emit2(P *p, uint8_t a, uint8_t b) { emit(p, a); emit(p, b); }
static void emit_u16(P *p, int v) { emit(p, (uint8_t)((v >> 8) & 0xff)); emit(p, (uint8_t)(v & 0xff)); }

static int emit_jump(P *p, uint8_t op)
{
    emit(p, op);
    emit(p, 0xff);
    emit(p, 0xff);
    return cur_chunk(p)->len - 2;
}

static void patch_jump(P *p, int pos)
{
    Chunk *c = cur_chunk(p);
    int off = c->len - (pos + 2);
    if (off > 32767 || off < -32768) { perr(p, "jump too far"); return; }
    c->code[pos]     = (uint8_t)((off >> 8) & 0xff);
    c->code[pos + 1] = (uint8_t)(off & 0xff);
}

static void emit_loop(P *p, int start)
{
    emit(p, OP_JMP);
    int off = start - (cur_chunk(p)->len + 2);
    if (off < -32768) { perr(p, "loop too large"); return; }
    emit_u16(p, off & 0xffff);
}

static int add_const(P *p, Value v)
{
    Prog *pr = p->prog;
    for (int i = 0; i < pr->nconsts; i++)
        if (pr->consts[i].k == v.k && val_equal(pr->consts[i], v)) {
            val_release(v);
            return i;
        }
    if (pr->nconsts >= NOM_CONSTS_MAX) { perr(p, "too many constants"); val_release(v); return 0; }
    pr->consts[pr->nconsts] = v;
    return pr->nconsts++;
}

static void emit_const(P *p, Value v)
{
    int k = add_const(p, v);
    emit(p, OP_CONST);
    emit_u16(p, k);
}

/* ----------------------------------------------------------- name lookup */
static int local_slot(Compiler *c, const char *name, int len)
{
    for (int i = c->nlocals - 1; i >= 0; i--)
        if ((int)strlen(c->local[i]) == len && memcmp(c->local[i], name, (size_t)len) == 0)
            return i;
    return -1;
}

static int add_local(P *p, const char *name, int len)
{
    Compiler *c = p->comp;
    int slot = local_slot(c, name, len);
    if (slot >= 0) return slot;
    if (c->nlocals >= NOM_LOCALS_MAX) { perr(p, "too many local names"); return 0; }
    if (len >= NOM_NAME_MAX) { perr(p, "name too long"); return 0; }
    memcpy(c->local[c->nlocals], name, (size_t)len);
    c->local[c->nlocals][len] = 0;
    if (c->nlocals + 1 > c->fn->nlocals) c->fn->nlocals = c->nlocals + 1;
    return c->nlocals++;
}

static int global_slot(P *p, const char *name, int len, bool create)
{
    Prog *pr = p->prog;
    for (int i = 0; i < pr->nglobals; i++)
        if ((int)strlen(pr->global[i]) == len && memcmp(pr->global[i], name, (size_t)len) == 0)
            return i;
    if (!create) return -1;
    if (pr->nglobals >= NOM_GLOBALS_MAX) { perr(p, "too many global names"); return 0; }
    if (len >= NOM_NAME_MAX) { perr(p, "name too long"); return 0; }
    memcpy(pr->global[pr->nglobals], name, (size_t)len);
    pr->global[pr->nglobals][len] = 0;
    return pr->nglobals++;
}

/* Resolution order: local, then an already-declared global, then a native.
 * An unresolvable name is a compile error rather than a silent nil — the
 * never-a-wrong-answer rule (HAMSH_SPEC 16a). */
static void emit_name_load(P *p, const char *name, int len)
{
    if (!p->comp->toplevel) {
        int s = local_slot(p->comp, name, len);
        if (s >= 0) { emit2(p, OP_GETLOCAL, (uint8_t)s); return; }
    }
    int g = global_slot(p, name, len, false);
    if (g >= 0) { emit(p, OP_GETGLOBAL); emit_u16(p, g); return; }
    int n = native_find(name, len);
    if (n >= 0) { emit2(p, OP_GETNATIVE, (uint8_t)n); return; }
    perr(p, "undefined name '%.*s'", len, name);
}

static void emit_name_store(P *p, const char *name, int len)
{
    if (!p->comp->toplevel) {
        int s = add_local(p, name, len);
        emit2(p, OP_SETLOCAL, (uint8_t)s);
        return;
    }
    int g = global_slot(p, name, len, true);
    emit(p, OP_SETGLOBAL);
    emit_u16(p, g);
}

/* ---------------------------------------------------------- expressions */
static int arg_list(P *p, TokType closer)
{
    int n = 0;
    if (!check(p, closer)) {
        do {
            skip_seps(p);
            expression(p);
            n++;
            if (n > NOM_ARGS_MAX) { perr(p, "too many arguments (max %d)", NOM_ARGS_MAX); return n; }
            skip_seps(p);
        } while (match(p, T_COMMA) && !p->failed);
    }
    expect(p, closer, closer == T_RPAREN ? "')'" : "']'");
    return n;
}

static void primary(P *p)
{
    Token *t = advance(p);
    switch (t->type) {
    case T_INT:   emit_const(p, VAL_INT(t->ival)); return;
    case T_NUM:   emit_const(p, VAL_NUM(t->dval)); return;
    case T_STR:   emit_const(p, str_new(t->sval, (size_t)t->len)); return;
    case T_TRUE:  emit(p, OP_TRUE);  return;
    case T_FALSE: emit(p, OP_FALSE); return;
    case T_NIL:   emit(p, OP_NIL);   return;
    case T_IDENT: emit_name_load(p, t->start, t->len); return;
    case T_LPAREN:
        expression(p);
        expect(p, T_RPAREN, "')'");
        return;
    case T_LBRACKET: {
        int n = 0;
        if (!check(p, T_RBRACKET)) {
            do {
                skip_seps(p);
                if (check(p, T_RBRACKET)) break;
                expression(p);
                n++;
                if (n > 255) { perr(p, "list literal too long"); return; }
                skip_seps(p);
            } while (match(p, T_COMMA) && !p->failed);
        }
        expect(p, T_RBRACKET, "']'");
        emit2(p, OP_LIST, (uint8_t)n);
        return;
    }
    case T_LBRACE: {
        int n = 0;
        if (!check(p, T_RBRACE)) {
            do {
                skip_seps(p);
                if (check(p, T_RBRACE)) break;
                expression(p);
                expect(p, T_COLON, "':' in dict literal");
                expression(p);
                n++;
                if (n > 255) { perr(p, "dict literal too long"); return; }
                skip_seps(p);
            } while (match(p, T_COMMA) && !p->failed);
        }
        expect(p, T_RBRACE, "'}'");
        emit2(p, OP_DICT, (uint8_t)n);
        return;
    }
    default:
        perr(p, "expected an expression");
        return;
    }
}

static void postfix(P *p)
{
    primary(p);
    for (;;) {
        if (match(p, T_LPAREN)) {
            int n = arg_list(p, T_RPAREN);
            emit2(p, OP_CALL, (uint8_t)n);
        } else if (match(p, T_LBRACKET)) {
            expression(p);
            expect(p, T_RBRACKET, "']'");
            emit(p, OP_INDEX);
        } else {
            return;
        }
        if (p->failed) return;
    }
}

static void unary(P *p);

static void power(P *p)
{
    postfix(p);
    if (match(p, T_DSTAR)) { unary(p); emit(p, OP_POW); }   /* right associative */
}

static void unary(P *p)
{
    if (match(p, T_MINUS)) { unary(p); emit(p, OP_NEG); return; }
    if (match(p, T_PLUS))  { unary(p); return; }
    power(p);
}

static void term(P *p)
{
    unary(p);
    for (;;) {
        if      (match(p, T_STAR))    { unary(p); emit(p, OP_MUL); }
        else if (match(p, T_SLASH))   { unary(p); emit(p, OP_DIV); }
        else if (match(p, T_DSLASH))  { unary(p); emit(p, OP_FDIV); }
        else if (match(p, T_PERCENT)) { unary(p); emit(p, OP_MOD); }
        else return;
        if (p->failed) return;
    }
}

static void sum(P *p)
{
    term(p);
    for (;;) {
        if      (match(p, T_PLUS))  { term(p); emit(p, OP_ADD); }
        else if (match(p, T_MINUS)) { term(p); emit(p, OP_SUB); }
        else return;
        if (p->failed) return;
    }
}

static void comparison(P *p)
{
    sum(p);
    for (;;) {
        if      (match(p, T_EQ)) { sum(p); emit(p, OP_EQ); }
        else if (match(p, T_NE)) { sum(p); emit(p, OP_NE); }
        else if (match(p, T_LT)) { sum(p); emit(p, OP_LT); }
        else if (match(p, T_LE)) { sum(p); emit(p, OP_LE); }
        else if (match(p, T_GT)) { sum(p); emit(p, OP_GT); }
        else if (match(p, T_GE)) { sum(p); emit(p, OP_GE); }
        else if (match(p, T_IN)) { sum(p); emit(p, OP_IN); }
        else if (check(p, T_NOT) && p->tl.tok[p->cur + 1].type == T_IN) {
            advance(p); advance(p);
            sum(p);
            emit(p, OP_IN);
            emit(p, OP_NOT);
        }
        else return;
        if (p->failed) return;
    }
}

static void not_expr(P *p)
{
    if (match(p, T_NOT)) { not_expr(p); emit(p, OP_NOT); return; }
    comparison(p);
}

static void and_expr(P *p)
{
    not_expr(p);
    while (match(p, T_AND)) {
        int j = emit_jump(p, OP_JMPF_KEEP);
        emit(p, OP_POP);
        not_expr(p);
        patch_jump(p, j);
        if (p->failed) return;
    }
}

static void or_expr(P *p)
{
    and_expr(p);
    while (match(p, T_OR)) {
        int j = emit_jump(p, OP_JMPT_KEEP);
        emit(p, OP_POP);
        and_expr(p);
        patch_jump(p, j);
        if (p->failed) return;
    }
}

static void expression(P *p) { or_expr(p); }

/* ------------------------------------------------------------ statements */

/* A block is either `{ ... }` or `: NEWLINE INDENT ... DEDENT` or an inline
 * `: statement`. Chosen per block, freely mixable — HAMSH_SPEC 5. */
static void block(P *p)
{
    if (match(p, T_LBRACE)) {
        skip_seps(p);
        while (!check(p, T_RBRACE) && !check(p, T_EOF) && !p->failed) {
            statement(p);
            skip_seps(p);
        }
        expect(p, T_RBRACE, "'}'");
        return;
    }
    expect(p, T_COLON, "':' or '{' to open a block");
    if (p->failed) return;
    if (match(p, T_NEWLINE)) {
        if (!match(p, T_INDENT)) { perr(p, "expected an indented block"); return; }
        while (!check(p, T_DEDENT) && !check(p, T_EOF) && !p->failed) {
            statement(p);
            while (check(p, T_NEWLINE)) advance(p);
        }
        expect(p, T_DEDENT, "end of indented block");
    } else {
        statement(p);      /* inline single-statement form: `if x: echo big` */
    }
}

static void begin_loop(P *p, Loop *l, int start)
{
    l->start = start;
    l->nexit = 0;
    l->prev = p->loop;
    p->loop = l;
}

static void end_loop(P *p, Loop *l)
{
    for (int i = 0; i < l->nexit; i++) patch_jump(p, l->exitpatch[i]);
    p->loop = l->prev;
}

static void if_stmt(P *p)
{
    expression(p);
    int else_jump = emit_jump(p, OP_JMPF);
    block(p);
    int end_jumps[32], nend = 0;
    for (;;) {
        /* Look past newlines for a continuation keyword, but never past a
         * DEDENT: that one belongs to an enclosing block. */
        int save = p->cur;
        while (check(p, T_NEWLINE)) advance(p);
        if (check(p, T_ELIF)) {
            advance(p);
            if (nend < 32) end_jumps[nend++] = emit_jump(p, OP_JMP);
            patch_jump(p, else_jump);
            expression(p);
            else_jump = emit_jump(p, OP_JMPF);
            block(p);
            continue;
        }
        if (check(p, T_ELSE)) {
            advance(p);
            if (nend < 32) end_jumps[nend++] = emit_jump(p, OP_JMP);
            patch_jump(p, else_jump);
            else_jump = -1;
            block(p);
            break;
        }
        p->cur = save;
        break;
    }
    if (else_jump >= 0) patch_jump(p, else_jump);
    for (int i = 0; i < nend; i++) patch_jump(p, end_jumps[i]);
}

static void while_stmt(P *p)
{
    int start = cur_chunk(p)->len;
    expression(p);
    int exit_jump = emit_jump(p, OP_JMPF);
    Loop l;
    begin_loop(p, &l, start);
    block(p);
    emit_loop(p, start);
    patch_jump(p, exit_jump);
    end_loop(p, &l);
}

/* for NAME in EXPR: body
 *
 * Stack layout during the loop is [seq, index]; OP_FORNEXT pushes the next
 * element or jumps past the two cleanup POPs. `break` therefore jumps to the
 * cleanup, which is why it does not need to know the loop's stack depth. */
static void for_stmt(P *p)
{
    if (!check(p, T_IDENT)) { perr(p, "expected a loop variable name"); return; }
    Token nameTok = *advance(p);
    expect(p, T_IN, "'in'");
    expression(p);
    emit(p, OP_FORPREP);                 /* pushes the index 0 */

    int start = cur_chunk(p)->len;
    int exit_jump = emit_jump(p, OP_FORNEXT);
    emit_name_store(p, nameTok.start, nameTok.len);

    Loop l;
    begin_loop(p, &l, start);
    block(p);
    emit_loop(p, start);
    patch_jump(p, exit_jump);
    end_loop(p, &l);
    emit(p, OP_POP);                     /* index */
    emit(p, OP_POP);                     /* seq */
}

static void def_stmt(P *p)
{
    if (!p->comp->toplevel) { perr(p, "nested 'def' is not supported"); return; }
    if (!check(p, T_IDENT)) { perr(p, "expected a function name"); return; }
    Token nameTok = *advance(p);
    if (p->prog->nfuncs >= NOM_FUNCS_MAX) { perr(p, "too many functions"); return; }

    int fi = p->prog->nfuncs++;
    Func *fn = &p->prog->func[fi];
    memset(fn, 0, sizeof *fn);
    int nlen = nameTok.len < NOM_NAME_MAX - 1 ? nameTok.len : NOM_NAME_MAX - 1;
    memcpy(fn->name, nameTok.start, (size_t)nlen);

    Compiler c;
    memset(&c, 0, sizeof c);
    c.fn = fn;
    c.toplevel = false;
    c.enclosing = p->comp;

    /* bind the name before compiling the body so recursion resolves */
    int g = global_slot(p, nameTok.start, nameTok.len, true);

    p->comp = &c;
    expect(p, T_LPAREN, "'(' after function name");
    if (!check(p, T_RPAREN)) {
        do {
            if (!check(p, T_IDENT)) { perr(p, "expected a parameter name"); break; }
            Token *pt = advance(p);
            add_local(p, pt->start, pt->len);
            fn->arity++;
            if (fn->arity > NOM_ARGS_MAX) { perr(p, "too many parameters"); break; }
        } while (match(p, T_COMMA) && !p->failed);
    }
    expect(p, T_RPAREN, "')'");
    block(p);
    emit(p, OP_NIL);
    emit(p, OP_RET);
    p->comp = c.enclosing;

    emit2(p, OP_GETFUNC, (uint8_t)fi);
    emit(p, OP_SETGLOBAL);
    emit_u16(p, g);
}

/* Is the upcoming token run an assignment target? Looks ahead without
 * emitting, then rewinds. */
static bool looks_like_assignment(P *p)
{
    int save = p->cur;
    bool yes = false;
    if (check(p, T_IDENT)) {
        p->cur++;
        int depth = 0;
        for (;;) {
            TokType t = peek(p)->type;
            if (t == T_LBRACKET) { depth++; p->cur++; continue; }
            if (depth > 0) {
                if (t == T_RBRACKET) depth--;
                else if (t == T_NEWLINE || t == T_EOF) break;
                p->cur++;
                continue;
            }
            yes = (t == T_ASSIGN || t == T_PLUSEQ || t == T_MINUSEQ ||
                   t == T_STAREQ || t == T_SLASHEQ);
            break;
        }
    }
    p->cur = save;
    return yes;
}

static void assignment(P *p)
{
    Token nameTok = *advance(p);
    bool indexed = check(p, T_LBRACKET);

    if (indexed) {
        /* NAME[expr] = value — plain assignment only */
        emit_name_load(p, nameTok.start, nameTok.len);
        advance(p);                      /* '[' */
        expression(p);
        expect(p, T_RBRACKET, "']'");
        if (check(p, T_LBRACKET)) { perr(p, "chained index assignment is not supported"); return; }
        if (!match(p, T_ASSIGN)) { perr(p, "augmented assignment to an element is not supported"); return; }
        expression(p);
        emit(p, OP_SETINDEX);
        return;
    }

    TokType op = advance(p)->type;
    if (op != T_ASSIGN) {
        emit_name_load(p, nameTok.start, nameTok.len);
        expression(p);
        switch (op) {
        case T_PLUSEQ:  emit(p, OP_ADD); break;
        case T_MINUSEQ: emit(p, OP_SUB); break;
        case T_STAREQ:  emit(p, OP_MUL); break;
        case T_SLASHEQ: emit(p, OP_DIV); break;
        default: perr(p, "bad assignment operator"); return;
        }
    } else {
        expression(p);
    }
    emit_name_store(p, nameTok.start, nameTok.len);
}

static void statement(P *p)
{
    /* Callers own separator handling; consuming a DEDENT here would steal the
     * one that terminates an enclosing indented block. */
    while (check(p, T_NEWLINE)) advance(p);
    if (check(p, T_EOF) || check(p, T_RBRACE) || check(p, T_DEDENT)) return;

    if (match(p, T_IF))    { if_stmt(p);    return; }
    if (match(p, T_WHILE)) { while_stmt(p); return; }
    if (match(p, T_FOR))   { for_stmt(p);   return; }
    if (match(p, T_DEF))   { def_stmt(p);   return; }
    if (match(p, T_PASS))  { return; }

    if (match(p, T_BREAK)) {
        if (!p->loop) { perr(p, "'break' outside a loop"); return; }
        if (p->loop->nexit < 32) p->loop->exitpatch[p->loop->nexit++] = emit_jump(p, OP_JMP);
        return;
    }
    if (match(p, T_CONTINUE)) {
        if (!p->loop) { perr(p, "'continue' outside a loop"); return; }
        emit_loop(p, p->loop->start);
        return;
    }
    if (match(p, T_RETURN)) {
        if (check(p, T_NEWLINE) || check(p, T_RBRACE) || check(p, T_EOF)) emit(p, OP_NIL);
        else expression(p);
        emit(p, OP_RET);
        return;
    }

    if (looks_like_assignment(p)) { assignment(p); return; }

    expression(p);
    emit(p, OP_POP);
}

/* -------------------------------------------------------------- entry pt */
Prog *prog_compile(const char *src, const char *name, char *err, size_t errsz)
{
    P p;
    memset(&p, 0, sizeof p);
    p.err = err;
    p.errsz = errsz;
    if (err) err[0] = 0;

    if (!lex_source(src, &p.tl, err, errsz)) return NULL;

    Prog *pr = nom_alloc(sizeof(Prog));
    snprintf(pr->name, sizeof pr->name, "%s", name ? name : "script");
    pr->nfuncs = 1;
    snprintf(pr->func[0].name, NOM_NAME_MAX, "%s", "<main>");
    p.prog = pr;

    Compiler c;
    memset(&c, 0, sizeof c);
    c.fn = &pr->func[0];
    c.toplevel = true;
    p.comp = &c;

    skip_seps(&p);
    while (!check(&p, T_EOF) && !p.failed) {
        statement(&p);
        skip_seps(&p);
    }
    emit(&p, OP_HALT);

    lex_free(&p.tl);
    if (p.failed) { prog_free(pr); return NULL; }
    return pr;
}

void prog_free(Prog *p)
{
    if (!p) return;
    for (int i = 0; i < p->nfuncs; i++) {
        nom_free(p->func[i].chunk.code);
        nom_free(p->func[i].chunk.line);
    }
    for (int i = 0; i < p->nconsts; i++) val_release(p->consts[i]);
    nom_free(p);
}

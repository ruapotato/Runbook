/* lex.c — tokenizer for NomScript.
 *
 * Tracks indentation the way HAMSH_SPEC 5 describes: an INDENT when a logical
 * line's column exceeds the enclosing level, one DEDENT per level closed.
 * INDENT/DEDENT are suppressed inside ( ) and [ ] so expressions may wrap, and
 * a brace block simply ignores stray ones — which is what lets the colon form
 * and the brace form nest inside each other.
 */
#include "lang.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

typedef struct {
    const char *src, *p;
    int    line;
    int    depth;              /* bracket nesting: suppresses INDENT/DEDENT */
    int    indent[32], nindent;
    bool   at_line_start;
    TokenList *out;
    char  *err;
    size_t errsz;
    bool   failed;
} Lexer;

static void lx_err(Lexer *lx, const char *fmt, ...)
{
    if (lx->failed) return;
    lx->failed = true;
    char msg[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    snprintf(lx->err, lx->errsz, "parse error [line %d]: %s", lx->line, msg);
}

static Token *push(Lexer *lx, TokType t)
{
    TokenList *tl = lx->out;
    if (tl->ntok >= NOM_TOK_MAX) { lx_err(lx, "script too long (TOK_MAX)"); return NULL; }
    Token *tk = &tl->tok[tl->ntok++];
    memset(tk, 0, sizeof *tk);
    tk->type = t;
    tk->line = lx->line;
    return tk;
}

static bool is_ident_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static bool is_digit(char c)       { return c >= '0' && c <= '9'; }
static bool is_ident(char c)       { return is_ident_start(c) || is_digit(c); }

static struct { const char *word; TokType t; } KEYWORDS[] = {
    { "if", T_IF }, { "elif", T_ELIF }, { "else", T_ELSE },
    { "while", T_WHILE }, { "for", T_FOR }, { "in", T_IN },
    { "break", T_BREAK }, { "continue", T_CONTINUE },
    { "def", T_DEF }, { "return", T_RETURN },
    { "and", T_AND }, { "or", T_OR }, { "not", T_NOT },
    { "true", T_TRUE }, { "false", T_FALSE }, { "nil", T_NIL },
    /* Python spellings accepted too — muscle memory is not a bug */
    { "True", T_TRUE }, { "False", T_FALSE }, { "None", T_NIL },
    { "pass", T_PASS },
    { NULL, T_EOF }
};

static TokType keyword_of(const char *s, int len)
{
    for (int i = 0; KEYWORDS[i].word; i++)
        if ((int)strlen(KEYWORDS[i].word) == len && memcmp(KEYWORDS[i].word, s, (size_t)len) == 0)
            return KEYWORDS[i].t;
    return T_IDENT;
}

/* Handle the start of a logical line: measure indentation and emit
 * INDENT/DEDENT. Returns false if the line turned out to be blank/comment. */
static bool handle_indent(Lexer *lx)
{
    int col = 0;
    const char *p = lx->p;
    for (;;) {
        if (*p == ' ') { col++; p++; }
        else if (*p == '\t') { col = (col / 4 + 1) * 4; p++; }
        else break;
    }
    if (*p == '\n' || *p == '\r' || *p == '#' || *p == 0) {
        lx->p = p;                 /* blank or comment-only: carries no indent */
        return false;
    }
    lx->p = p;

    int cur = lx->indent[lx->nindent - 1];
    if (col > cur) {
        if (lx->nindent >= 32) { lx_err(lx, "indentation too deep"); return true; }
        lx->indent[lx->nindent++] = col;
        push(lx, T_INDENT);
    } else {
        while (lx->nindent > 1 && col < lx->indent[lx->nindent - 1]) {
            lx->nindent--;
            push(lx, T_DEDENT);
        }
        if (col != lx->indent[lx->nindent - 1])
            lx_err(lx, "inconsistent indentation (column %d)", col);
    }
    return true;
}

static void lex_string(Lexer *lx, char quote)
{
    Buf b;
    buf_init(&b);
    while (*lx->p && *lx->p != quote) {
        char c = *lx->p++;
        if (c == '\n') { lx_err(lx, "unterminated string"); buf_free(&b); return; }
        if (c == '\\' && quote == '"') {
            char e = *lx->p++;
            switch (e) {
            case 'n':  buf_putc(&b, '\n'); break;
            case 't':  buf_putc(&b, '\t'); break;
            case 'r':  buf_putc(&b, '\r'); break;
            case '0':  buf_putc(&b, '\0'); break;
            case '\\': buf_putc(&b, '\\'); break;
            case '"':  buf_putc(&b, '"');  break;
            case '\'': buf_putc(&b, '\''); break;
            default:   buf_putc(&b, '\\'); buf_putc(&b, e); break;
            }
        } else {
            buf_putc(&b, c);
        }
    }
    if (*lx->p != quote) { lx_err(lx, "unterminated string"); buf_free(&b); return; }
    lx->p++;
    Token *tk = push(lx, T_STR);
    if (tk) {
        tk->sval = b.p ? b.p : nom_strdup("");
        tk->len  = (int)b.len;
        if (!b.p) buf_free(&b);
    } else {
        buf_free(&b);
    }
}

static void lex_number(Lexer *lx)
{
    const char *start = lx->p;
    while (is_digit(*lx->p)) lx->p++;
    bool isfloat = false;
    if (*lx->p == '.' && is_digit(lx->p[1])) {
        isfloat = true;
        lx->p++;
        while (is_digit(*lx->p)) lx->p++;
    }
    if (*lx->p == 'e' || *lx->p == 'E') {
        const char *save = lx->p;
        lx->p++;
        if (*lx->p == '+' || *lx->p == '-') lx->p++;
        if (is_digit(*lx->p)) { isfloat = true; while (is_digit(*lx->p)) lx->p++; }
        else lx->p = save;
    }
    char tmp[64];
    int n = (int)(lx->p - start);
    if (n >= (int)sizeof tmp) { lx_err(lx, "number literal too long"); return; }
    memcpy(tmp, start, (size_t)n);
    tmp[n] = 0;
    Token *tk = push(lx, isfloat ? T_NUM : T_INT);
    if (!tk) return;
    if (isfloat) tk->dval = strtod(tmp, NULL);
    else         tk->ival = strtoll(tmp, NULL, 10);
    tk->start = start;
    tk->len = n;
}

bool lex_source(const char *src, TokenList *out, char *err, size_t errsz)
{
    Lexer lx;
    memset(&lx, 0, sizeof lx);
    lx.src = lx.p = src;
    lx.line = 1;
    lx.indent[0] = 0;
    lx.nindent = 1;
    lx.at_line_start = true;
    lx.out = out;
    lx.err = err;
    lx.errsz = errsz;

    out->tok = nom_alloc(sizeof(Token) * NOM_TOK_MAX);
    out->ntok = 0;

    while (*lx.p && !lx.failed) {
        if (lx.at_line_start && lx.depth == 0) {
            lx.at_line_start = false;
            if (!handle_indent(&lx)) {
                /* skip the blank/comment line entirely */
                while (*lx.p && *lx.p != '\n') lx.p++;
                if (*lx.p == '\n') { lx.p++; lx.line++; lx.at_line_start = true; }
                continue;
            }
        }

        char c = *lx.p;
        if (c == ' ' || c == '\t' || c == '\r') { lx.p++; continue; }
        if (c == '#') { while (*lx.p && *lx.p != '\n') lx.p++; continue; }
        if (c == '\\' && lx.p[1] == '\n') { lx.p += 2; lx.line++; continue; }  /* explicit continuation */
        if (c == '\n') {
            lx.p++;
            lx.line++;
            if (lx.depth == 0) {
                /* collapse runs of newlines into one separator */
                int n = out->ntok;
                if (n && out->tok[n - 1].type != T_NEWLINE && out->tok[n - 1].type != T_INDENT)
                    push(&lx, T_NEWLINE);
                lx.at_line_start = true;
            }
            continue;
        }

        if (is_digit(c)) { lex_number(&lx); continue; }
        if (c == '"' || c == '\'') { lx.p++; lex_string(&lx, c); continue; }
        if (is_ident_start(c)) {
            const char *start = lx.p;
            while (is_ident(*lx.p)) lx.p++;
            int len = (int)(lx.p - start);
            Token *tk = push(&lx, keyword_of(start, len));
            if (tk) { tk->start = start; tk->len = len; }
            continue;
        }

        lx.p++;
        TokType t = T_EOF;
        switch (c) {
        case '(': t = T_LPAREN;   lx.depth++; break;
        case ')': t = T_RPAREN;   if (lx.depth) lx.depth--; break;
        case '[': t = T_LBRACKET; lx.depth++; break;
        case ']': t = T_RBRACKET; if (lx.depth) lx.depth--; break;
        case '{': t = T_LBRACE;   break;
        case '}': t = T_RBRACE;   break;
        case ',': t = T_COMMA;  break;
        case ':': t = T_COLON;  break;
        case '%': t = T_PERCENT; break;
        case '+': t = (*lx.p == '=') ? (lx.p++, T_PLUSEQ)  : T_PLUS;  break;
        case '-': t = (*lx.p == '=') ? (lx.p++, T_MINUSEQ) : T_MINUS; break;
        case '*':
            if (*lx.p == '*') { lx.p++; t = T_DSTAR; }
            else if (*lx.p == '=') { lx.p++; t = T_STAREQ; }
            else t = T_STAR;
            break;
        case '/':
            if (*lx.p == '/') { lx.p++; t = T_DSLASH; }
            else if (*lx.p == '=') { lx.p++; t = T_SLASHEQ; }
            else t = T_SLASH;
            break;
        case '=': t = (*lx.p == '=') ? (lx.p++, T_EQ) : T_ASSIGN; break;
        case '!':
            if (*lx.p == '=') { lx.p++; t = T_NE; }
            else { lx_err(&lx, "unexpected '!' (did you mean 'not'?)"); }
            break;
        case '<': t = (*lx.p == '=') ? (lx.p++, T_LE) : T_LT; break;
        case '>': t = (*lx.p == '=') ? (lx.p++, T_GE) : T_GT; break;
        default:
            lx_err(&lx, "unexpected character '%c'", c);
            break;
        }
        if (lx.failed) break;
        Token *tk = push(&lx, t);
        if (tk) { tk->start = lx.p - 1; tk->len = 1; }
    }

    if (!lx.failed) {
        int n = out->ntok;
        if (n && out->tok[n - 1].type != T_NEWLINE) push(&lx, T_NEWLINE);
        while (lx.nindent > 1) { lx.nindent--; push(&lx, T_DEDENT); }
        push(&lx, T_EOF);
    }
    if (lx.failed) { lex_free(out); return false; }
    return true;
}

void lex_free(TokenList *tl)
{
    if (!tl->tok) return;
    for (int i = 0; i < tl->ntok; i++)
        if (tl->tok[i].type == T_STR) nom_free(tl->tok[i].sval);
    nom_free(tl->tok);
    tl->tok = NULL;
    tl->ntok = 0;
}

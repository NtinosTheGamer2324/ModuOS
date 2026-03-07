/* parser.c - NTCC recursive-descent C parser */
#include "parser.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* =========================================================
 * Built-in types
 * ========================================================= */
Type *ty_void, *ty_char, *ty_short, *ty_int, *ty_long;
Type *ty_uchar, *ty_ushort, *ty_uint, *ty_ulong;

static Type *mk_ty(TypeKind k, int sz, int al) {
    Type *t = calloc(1, sizeof(Type));
    t->kind = k; t->size = sz; t->align = al;
    return t;
}

Type *make_ptr(Type *base) {
    Type *t = mk_ty(TY_PTR, 8, 8); t->base = base; return t;
}
Type *make_array(Type *base, int len) {
    Type *t = mk_ty(TY_ARRAY, base->size * len, base->align);
    t->base = base; t->arr_len = len; return t;
}

static void init_types(void) {
    ty_void  = mk_ty(TY_VOID,  0, 1);
    ty_char  = mk_ty(TY_CHAR,  1, 1);
    ty_short = mk_ty(TY_SHORT, 2, 2);
    ty_int   = mk_ty(TY_INT,   4, 4);
    ty_long  = mk_ty(TY_LONG,  8, 8);
    ty_uchar  = mk_ty(TY_UCHAR,  1, 1);
    ty_ushort = mk_ty(TY_USHORT, 2, 2);
    ty_uint   = mk_ty(TY_UINT,   4, 4);
    ty_ulong  = mk_ty(TK_ULONG,  8, 8);
}

/* =========================================================
 * Node allocation
 * ========================================================= */
static Node *nd(NodeKind k, int line) {
    Node *n = calloc(1, sizeof(Node)); n->kind = k; n->line = line; return n;
}

static Node *nd_int(long long v, int line) {
    Node *n = nd(ND_INT, line); n->ival = v; n->ty = ty_int; return n;
}

static Node *nd_binop(NodeKind k, Node *l, Node *r, int line) {
    Node *n = nd(k, line); n->lhs = l; n->rhs = r; return n;
}

/* =========================================================
 * Parser infrastructure
 * ========================================================= */
static char *strdup2(const char *s) {
    if (!s) return NULL;
    char *p = malloc(strlen(s)+1); strcpy(p, s); return p;
}

Parser *parser_new(Lexer *l) {
    init_types();
    Parser *p = calloc(1, sizeof(Parser));
    p->lexer = l;
    p->tok   = lexer_next(l);
    p->scope = calloc(1, sizeof(Scope));
    p->func_locals = NULL;
    return p;
}

void parser_free(Parser *p) { free(p); }

static int   is(Parser *p, TokenKind k) { return p->tok.kind == k; }

static Token advance(Parser *p) {
    Token t = p->tok;
    p->tok = lexer_next(p->lexer);
    return t;
}

static Token expect(Parser *p, TokenKind k) {
    if (!is(p, k)) {
        fprintf(stderr, "line %d: expected '%s', got '%s'\n",
                p->tok.line, token_kind_name(k), token_kind_name(p->tok.kind));
        p->errors++;
    }
    return advance(p);
}

static int consume(Parser *p, TokenKind k) {
    if (is(p, k)) { advance(p); return 1; }
    return 0;
}

/* =========================================================
 * Scope / variable lookup
 * ========================================================= */
static void scope_push(Parser *p) {
    Scope *s = calloc(1, sizeof(Scope));
    s->parent = p->scope;
    p->scope = s;
}

static void scope_pop(Parser *p) {
    p->scope = p->scope->parent;
}

static Var *find_var(Parser *p, const char *name) {
    for (Scope *s = p->scope; s; s = s->parent)
        for (Var *v = s->vars; v; v = v->next)
            if (strcmp(v->name, name) == 0) return v;
    for (Var *v = p->globals; v; v = v->next)
        if (strcmp(v->name, name) == 0) return v;
    return NULL;
}

static Var *new_lvar(Parser *p, const char *name, Type *ty) {
    Var *v = calloc(1, sizeof(Var));
    v->name     = strdup2(name);
    v->ty       = ty;
    v->is_local = 1;
    v->is_param = 0;
    v->param_index = -1;
    v->next     = p->scope->vars;
    p->scope->vars = v;
    v->local_next = p->func_locals;
    p->func_locals = v;
    return v;
}

static Var *new_gvar(Parser *p, const char *name, Type *ty) {
    Var *v = calloc(1, sizeof(Var));
    v->name     = strdup2(name);
    v->ty       = ty;
    v->is_local = 0;
    v->is_param = 0;
    v->param_index = -1;
    char buf[128]; snprintf(buf, sizeof(buf), "_%s", name);
    v->label    = strdup2(buf);
    v->next     = p->globals;
    p->globals  = v;
    return v;
}

static char *new_str_label(Parser *p) {
    char buf[32];
    snprintf(buf, sizeof(buf), ".LC%d", p->label_cnt++);
    return strdup2(buf);
}

/* Find or intern a string literal */
static Var *intern_string(Parser *p, const char *s, int len) {
    char *lbl = new_str_label(p);
    /* Store in strings table */
    p->strings[p->nstrings].val   = strdup2(s);
    p->strings[p->nstrings].len   = len;
    p->strings[p->nstrings].label = lbl;
    p->nstrings++;
    /* Create a global var of type char[] pointing to it */
    Var *v = calloc(1, sizeof(Var));
    v->name     = lbl;
    v->ty       = make_array(ty_char, len + 1);
    v->is_local = 0;
    v->label    = lbl;
    v->next     = p->globals;
    p->globals  = v;
    return v;
}

/* Type lookup */
static Type *find_typedef(Parser *p, const char *name) {
    for (int i = 0; i < p->ntypedefs; i++)
        if (strcmp(p->typedefs[i].name, name) == 0)
            return p->typedefs[i].ty;
    return NULL;
}

static Type *find_struct_tag(Parser *p, const char *tag) {
    for (int i = 0; i < p->nstructs; i++)
        if (strcmp(p->structs[i].tag, tag) == 0)
            return p->structs[i].ty;
    return NULL;
}

/* =========================================================
 * Forward declarations
 * ========================================================= */
static Node *parse_expr(Parser *p);
static Node *parse_stmt(Parser *p);
static Node *parse_block(Parser *p);
static Type *parse_declspec(Parser *p, int *is_typedef, int *is_static, int *is_extern);
static Type *parse_declarator(Parser *p, Type *base, char **name_out);

static Member *parser_find_member(Type *t, const char *name) {
    if (!t || t->kind != TY_STRUCT) return NULL;
    for (Member *m = t->members; m; m = m->next)
        if (m->name && strcmp(m->name, name) == 0) return m;
    return NULL;
}

/* =========================================================
 * Type parsing
 * ========================================================= */
static int is_type_token(Parser *p) {
    TokenKind k = p->tok.kind;
    if (k == TK_INT || k == TK_CHAR || k == TK_LONG || k == TK_SHORT ||
        k == TK_VOID || k == TK_UNSIGNED || k == TK_SIGNED ||
        k == TK_FLOAT || k == TK_DOUBLE ||
        k == TK_STRUCT || k == TK_UNION || k == TK_CONST ||
        k == TK_VOLATILE || k == TK_STATIC || k == TK_EXTERN ||
        k == TK_TYPEDEF || k == TK_INLINE || k == TK_AUTO ||
        k == TK_REGISTER)
        return 1;
    if (k == TK_IDENT && p->tok.val && find_typedef(p, p->tok.val))
        return 1;
    return 0;
}

static Type *parse_struct_or_union(Parser *p) {
    advance(p); /* consume 'struct' */
    char *tag = NULL;
    if (is(p, TK_IDENT)) { tag = strdup2(p->tok.val); advance(p); }

    if (!is(p, TK_LBRACE)) {
        /* Reference to existing struct */
        Type *t = tag ? find_struct_tag(p, tag) : NULL;
        if (!t) {
            t = mk_ty(TY_STRUCT, 0, 1);
            t->tag = tag;
            if (tag && p->nstructs < 255) {
                p->structs[p->nstructs].tag = tag;
                p->structs[p->nstructs].ty  = t;
                p->nstructs++;
            }
        }
        return t;
    }
    advance(p); /* consume '{' */

    Type *ty = mk_ty(TY_STRUCT, 0, 1);
    ty->tag  = tag;
    Member *tail = NULL;
    int offset = 0;

    while (!is(p, TK_RBRACE) && !is(p, TK_EOF)) {
        Type *base = parse_declspec(p, NULL, NULL, NULL);
        while (!is(p, TK_SEMI) && !is(p, TK_EOF)) {
            char *mname = NULL;
            Type *mty   = parse_declarator(p, base, &mname);
            Member *m   = calloc(1, sizeof(Member));
            m->name     = mname;
            m->ty       = mty;
            /* Align */
            int al = mty->align ? mty->align : 1;
            offset = (offset + al - 1) & ~(al - 1);
            m->offset   = offset;
            offset     += mty->size;
            if (mty->align > ty->align) ty->align = mty->align;
            if (tail) tail->next = m; else ty->members = m;
            tail = m;
            if (!consume(p, TK_COMMA)) break;
        }
        consume(p, TK_SEMI);
    }
    expect(p, TK_RBRACE);
    ty->size = (offset + ty->align - 1) & ~(ty->align - 1);
    if (tag && p->nstructs < 255) {
        p->structs[p->nstructs].tag = tag;
        p->structs[p->nstructs].ty  = ty;
        p->nstructs++;
    }
    return ty;
}

static Type *parse_declspec(Parser *p, int *is_typedef, int *is_static, int *is_extern) {
    if (is_typedef) *is_typedef = 0;
    if (is_static)  *is_static  = 0;
    if (is_extern)  *is_extern  = 0;

    Type *base = ty_int;
    int seen_unsigned = 0, seen_long = 0, seen_short = 0, seen_char = 0;

    for (;;) {
        switch (p->tok.kind) {
        case TK_TYPEDEF:  if (is_typedef) *is_typedef = 1; advance(p); continue;
        case TK_STATIC:   if (is_static)  *is_static  = 1; advance(p); continue;
        case TK_EXTERN:   if (is_extern)  *is_extern  = 1; advance(p); continue;
        case TK_CONST: case TK_VOLATILE: case TK_INLINE:
        case TK_AUTO: case TK_REGISTER:  advance(p); continue;
        case TK_VOID:     base = ty_void;  advance(p); break;
        case TK_CHAR:     seen_char = 1;   advance(p); break;
        case TK_SHORT:    seen_short = 1;  advance(p); break;
        case TK_INT:      advance(p); break;
        case TK_LONG:     seen_long++;     advance(p); break;
        case TK_UNSIGNED: seen_unsigned=1; advance(p); break;
        case TK_SIGNED:   advance(p); break;
        case TK_FLOAT: case TK_DOUBLE: advance(p); break;
        case TK_STRUCT: case TK_UNION: base = parse_struct_or_union(p); break;
        case TK_IDENT: {
            Type *td = find_typedef(p, p->tok.val);
            if (!td) goto done;
            base = td; advance(p); break;
        }
        default: goto done;
        }
        continue;
done:   break;
    }

    if (seen_char)       base = seen_unsigned ? ty_uchar  : ty_char;
    else if (seen_short) base = seen_unsigned ? ty_ushort : ty_short;
    else if (seen_long)  base = seen_unsigned ? ty_ulong  : ty_long;
    else if (seen_unsigned && base == ty_int) base = ty_uint;
    return base;
}

/* Parse pointer prefixes, then the name, then array/function suffixes */
static Type *parse_declarator(Parser *p, Type *base, char **name_out) {
    /* Pointer prefix */
    while (consume(p, TK_STAR)) {
        while (is(p, TK_CONST) || is(p, TK_VOLATILE)) advance(p);
        base = make_ptr(base);
    }
    /* Grouped declarator: (*name) */
    if (is(p, TK_LPAREN)) {
        advance(p);
        Type *placeholder = calloc(1, sizeof(Type));
        char *inner_name  = NULL;
        parse_declarator(p, placeholder, &inner_name);
        expect(p, TK_RPAREN);
        Type *suffix = base;
        /* Handle array suffix */
        while (is(p, TK_LBRACKET)) {
            advance(p);
            int len = 0;
            if (!is(p, TK_RBRACKET)) { len = (int)p->tok.ival; advance(p); }
            expect(p, TK_RBRACKET);
            suffix = make_array(suffix, len);
        }
        *placeholder = *suffix;
        if (name_out) *name_out = inner_name;
        return base;
    }
    /* Direct name */
    if (name_out && is(p, TK_IDENT)) {
        *name_out = strdup2(p->tok.val);
        advance(p);
    }
    /* Array suffix */
    while (is(p, TK_LBRACKET)) {
        advance(p);
        int len = 0;
        if (!is(p, TK_RBRACKET)) { len = (int)p->tok.ival; advance(p); }
        expect(p, TK_RBRACKET);
        base = make_array(base, len);
    }
    return base;
}
/* =========================================================
 * Expression parsing
 * ========================================================= */
static int is_assign_op(TokenKind k) {
    return k == TK_ASSIGN || k == TK_PLUS_ASSIGN || k == TK_MINUS_ASSIGN ||
           k == TK_STAR_ASSIGN || k == TK_SLASH_ASSIGN || k == TK_PERCENT_ASSIGN ||
           k == TK_AMP_ASSIGN || k == TK_PIPE_ASSIGN || k == TK_CARET_ASSIGN ||
           k == TK_LSHIFT_ASSIGN || k == TK_RSHIFT_ASSIGN;
}

static NodeKind assign_op_kind(TokenKind k) {
    switch(k){
    case TK_ASSIGN:         return ND_ASSIGN;
    case TK_PLUS_ASSIGN:    return ND_PLUS_ASSIGN;
    case TK_MINUS_ASSIGN:   return ND_MINUS_ASSIGN;
    case TK_STAR_ASSIGN:    return ND_MUL_ASSIGN;
    case TK_SLASH_ASSIGN:   return ND_DIV_ASSIGN;
    case TK_PERCENT_ASSIGN: return ND_MOD_ASSIGN;
    case TK_AMP_ASSIGN:     return ND_AND_ASSIGN;
    case TK_PIPE_ASSIGN:    return ND_OR_ASSIGN;
    case TK_CARET_ASSIGN:   return ND_XOR_ASSIGN;
    case TK_LSHIFT_ASSIGN:  return ND_SHL_ASSIGN;
    case TK_RSHIFT_ASSIGN:  return ND_SHR_ASSIGN;
    default: return ND_ASSIGN;
    }
}

static Node *parse_expr(Parser *p);
static Node *parse_assign(Parser *p);
static Node *parse_unary(Parser *p);

static Node *parse_primary(Parser *p) {
    int ln = p->tok.line;
    if (is(p, TK_INT_LIT)) {
        Node *n = nd_int(p->tok.ival, ln); advance(p); return n;
    }
    if (is(p, TK_CHAR_LIT)) {
        Node *n = nd_int(p->tok.ival, ln); n->ty = ty_char; advance(p); return n;
    }
    if (is(p, TK_STR_LIT)) {
        char *sv = strdup2(p->tok.val); int slen = (int)strlen(sv);
        advance(p);
        Var *v = intern_string(p, sv, slen); free(sv);
        Node *n = nd(ND_VAR, ln); n->var = v; n->ty = v->ty; return n;
    }
    if (is(p, TK_IDENT)) {
        char *name = strdup2(p->tok.val); advance(p);
        if (is(p, TK_LPAREN)) {
            advance(p);
            Node *c = nd(ND_CALL, ln); c->fname = name; c->ty = ty_int;
            Node *atail = NULL;
            while (!is(p, TK_RPAREN) && !is(p, TK_EOF)) {
                Node *a = parse_assign(p); a->next_arg = NULL;
                if (!c->args) c->args = a; else atail->next_arg = a; atail = a;
                if (!consume(p, TK_COMMA)) break;
            }
            expect(p, TK_RPAREN); return c;
        }
        Var *v = find_var(p, name);
        if (!v) {
            fprintf(stderr, "line %d: undeclared '%s'\n", ln, name);
            p->errors++; v = new_lvar(p, name, ty_int);
        }
        free(name);
        Node *n = nd(ND_VAR, ln); n->var = v; n->ty = v->ty; return n;
    }
    if (is(p, TK_LPAREN)) {
        advance(p);
        if (is_type_token(p)) {
            Type *ty = parse_declspec(p, NULL, NULL, NULL);
            ty = parse_declarator(p, ty, NULL);
            expect(p, TK_RPAREN);
            Node *n = nd(ND_CAST, ln); n->lhs = parse_unary(p);
            n->cast_ty = ty; n->ty = ty; return n;
        }
        Node *n = parse_expr(p); expect(p, TK_RPAREN); return n;
    }
    if (is(p, TK_SIZEOF)) {
        advance(p);
        if (is(p, TK_LPAREN) && is_type_token(p)) {
            advance(p);
            Type *ty = parse_declspec(p, NULL, NULL, NULL);
            ty = parse_declarator(p, ty, NULL);
            expect(p, TK_RPAREN); return nd_int(ty->size, ln);
        }
        Node *e = parse_unary(p);
        return nd_int(e->ty ? e->ty->size : 8, ln);
    }
    fprintf(stderr, "line %d: unexpected '%s'\n", ln, token_kind_name(p->tok.kind));
    p->errors++; advance(p); return nd_int(0, ln);
}

static Node *parse_postfix(Parser *p) {
    Node *n = parse_primary(p);
    for (;;) {
        int ln = p->tok.line;
        if (is(p, TK_LBRACKET)) {
            advance(p); Node *i = parse_expr(p); expect(p, TK_RBRACKET);
            Node *add = nd_binop(ND_ADD, n, i, ln);
            n = nd(ND_DEREF, ln); n->lhs = add;
            if (add->lhs && add->lhs->ty && add->lhs->ty->base) n->ty = add->lhs->ty->base;
            continue;
        }
        if (is(p, TK_DOT) || is(p, TK_ARROW)) {
            int arr = is(p, TK_ARROW); advance(p);
            if (arr) { Node *d = nd(ND_DEREF, ln); d->lhs = n; n = d; }
            Node *m = nd(ND_MEMBER, ln); m->lhs = n;
            if (is(p, TK_IDENT)) {
                m->member_name = strdup2(p->tok.val);
                /* Resolve member type if possible */
                Type *bt = NULL;
                if (n->ty && n->ty->kind == TY_STRUCT) bt = n->ty;
                if (!bt && n->kind == ND_VAR && n->var) bt = n->var->ty;
                if (!bt && n->kind == ND_DEREF && n->lhs && n->lhs->kind == ND_VAR && n->lhs->var) {
                    Type *pt = n->lhs->var->ty;
                    if (pt && pt->base) bt = pt->base;
                }
                if (bt && bt->kind == TY_STRUCT) {
                    Member *mem = parser_find_member(bt, m->member_name);
                    m->member = mem;
                    m->ty = mem ? mem->ty : ty_long;
                }
                advance(p);
            }
            n = m; continue;
        }
        if (is(p, TK_INC)) { advance(p); Node *x=nd(ND_POSTINC,ln); x->lhs=n; n=x; continue; }
        if (is(p, TK_DEC)) { advance(p); Node *x=nd(ND_POSTDEC,ln); x->lhs=n; n=x; continue; }
        break;
    }
    return n;
}

static Node *parse_unary(Parser *p) {
    int ln = p->tok.line;
    if (is(p,TK_INC))   { advance(p); Node *x=nd(ND_PREINC,ln);  x->lhs=parse_unary(p); return x; }
    if (is(p,TK_DEC))   { advance(p); Node *x=nd(ND_PREDEC,ln);  x->lhs=parse_unary(p); return x; }
    if (is(p,TK_AMP))   { advance(p); Node *x=nd(ND_ADDR,ln);    x->lhs=parse_unary(p); return x; }
    if (is(p,TK_STAR))  { advance(p); Node *x=nd(ND_DEREF,ln);   x->lhs=parse_unary(p); return x; }
    if (is(p,TK_BANG))  { advance(p); Node *x=nd(ND_NOT,ln);     x->lhs=parse_unary(p); return x; }
    if (is(p,TK_TILDE)) { advance(p); Node *x=nd(ND_BITNOT,ln);  x->lhs=parse_unary(p); return x; }
    if (is(p,TK_MINUS)) { advance(p); Node *x=nd(ND_NEG,ln);     x->lhs=parse_unary(p); return x; }
    if (is(p,TK_PLUS))  { advance(p); return parse_unary(p); }
    return parse_postfix(p);
}

static int get_prec(TokenKind k) {
    switch(k){
    case TK_STAR: case TK_SLASH: case TK_PERCENT: return 12;
    case TK_PLUS: case TK_MINUS: return 11;
    case TK_LSHIFT: case TK_RSHIFT: return 10;
    case TK_LT: case TK_LE: case TK_GT: case TK_GE: return 9;
    case TK_EQ: case TK_NEQ: return 8;
    case TK_AMP: return 7;
    case TK_CARET: return 6;
    case TK_PIPE: return 5;
    case TK_AND: return 4;
    case TK_OR:  return 3;
    default: return 0;
    }
}

static NodeKind token_to_nd(TokenKind k) {
    switch(k){
    case TK_STAR: return ND_MUL; case TK_SLASH: return ND_DIV;
    case TK_PERCENT: return ND_MOD; case TK_PLUS: return ND_ADD;
    case TK_MINUS: return ND_SUB; case TK_LSHIFT: return ND_SHL;
    case TK_RSHIFT: return ND_SHR; case TK_LT: return ND_LT;
    case TK_LE: return ND_LE; case TK_GT: return ND_LT; /* reversed */
    case TK_GE: return ND_LE; /* reversed */
    case TK_EQ: return ND_EQ; case TK_NEQ: return ND_NEQ;
    case TK_AMP: return ND_AND; case TK_CARET: return ND_XOR;
    case TK_PIPE: return ND_OR; case TK_AND: return ND_LAND;
    case TK_OR: return ND_LOR;
    default: return ND_ADD;
    }
}

static Node *parse_binop_prec(Parser *p, int min_prec) {
    Node *lhs = parse_unary(p);
    for (;;) {
        int ln = p->tok.line;
        TokenKind tk = p->tok.kind;
        int prec = get_prec(tk);
        if (!prec || prec < min_prec) break;
        advance(p);
        int reversed = (tk == TK_GT || tk == TK_GE);
        NodeKind nk  = token_to_nd(tk);
        Node *rhs = parse_binop_prec(p, prec + 1);
        if (reversed) { Node *t=lhs; lhs=rhs; rhs=t; }
        lhs = nd_binop(nk, lhs, rhs, ln);
    }
    return lhs;
}

static Node *parse_ternary(Parser *p) {
    Node *n = parse_binop_prec(p, 1);
    if (!is(p, TK_QUESTION)) return n;
    int ln = p->tok.line; advance(p);
    Node *t = nd(ND_TERNARY, ln);
    t->cond = n; t->then = parse_expr(p);
    expect(p, TK_COLON); t->els = parse_ternary(p);
    return t;
}

static Node *parse_assign(Parser *p) {
    Node *lhs = parse_ternary(p);
    if (!is_assign_op(p->tok.kind)) return lhs;
    int ln = p->tok.line; NodeKind ak = assign_op_kind(p->tok.kind); advance(p);
    return nd_binop(ak, lhs, parse_assign(p), ln);
}

static Node *parse_expr(Parser *p) {
    Node *n = parse_assign(p);
    while (is(p, TK_COMMA)) {
        int ln = p->tok.line; advance(p);
        n = nd_binop(ND_COMMA_EXPR, n, parse_assign(p), ln);
    }
    return n;
}

/* =========================================================
 * Statement and top-level parsing
 * ========================================================= */
static Node *parse_block(Parser *p);

static Node *parse_decl_stmt(Parser *p, Type *base) {
    Node *head = NULL, *tail = NULL;
    while (!is(p,TK_SEMI) && !is(p,TK_EOF)) {
        char *name = NULL;
        Type *ty = parse_declarator(p, base, &name);
        if (!name) { consume(p,TK_COMMA); continue; }
        Var *v = new_lvar(p, name, ty); free(name);
        Node *decl = nd(ND_DECL, p->tok.line); decl->var = v;
        if (consume(p, TK_ASSIGN)) {
            if (is(p, TK_LBRACE)) {
                advance(p); int idx = 0;
                while (!is(p,TK_RBRACE) && !is(p,TK_EOF)) {
                    Node *el = parse_assign(p);
                    Node *vr = nd(ND_VAR, p->tok.line); vr->var = v; vr->ty = v->ty;
                    Node *ix = nd_int(idx++, p->tok.line);
                    Node *ad = nd_binop(ND_ADD, vr, ix, p->tok.line);
                    Node *dr = nd(ND_DEREF, p->tok.line); dr->lhs = ad;
                    Node *as = nd_binop(ND_ASSIGN, dr, el, p->tok.line);
                    Node *es = nd(ND_EXPR_STMT, p->tok.line); es->lhs = as;
                    if (!head) head = decl; else tail->next = decl; tail = decl;
                    decl = es;
                    if (!consume(p,TK_COMMA)) break;
                }
                consume(p, TK_RBRACE);
            } else { decl->rhs = parse_assign(p); }
        }
        if (!head) head = decl; else tail->next = decl; tail = decl;
        if (!consume(p,TK_COMMA)) break;
    }
    consume(p, TK_SEMI); return head;
}

static Node *parse_stmt(Parser *p) {
    int ln = p->tok.line;
    if (is(p,TK_LBRACE))   return parse_block(p);
    if (is(p,TK_RETURN)) {
        advance(p); Node *n = nd(ND_RETURN, ln);
        if (!is(p,TK_SEMI)) n->lhs = parse_expr(p);
        expect(p,TK_SEMI); return n;
    }
    if (is(p,TK_IF)) {
        advance(p); expect(p,TK_LPAREN);
        Node *n = nd(ND_IF, ln); n->cond = parse_expr(p);
        expect(p,TK_RPAREN); n->then = parse_stmt(p);
        if (consume(p,TK_ELSE)) n->els = parse_stmt(p);
        return n;
    }
    if (is(p,TK_WHILE)) {
        advance(p); expect(p,TK_LPAREN);
        Node *n = nd(ND_WHILE, ln); n->cond = parse_expr(p);
        expect(p,TK_RPAREN); n->body = parse_stmt(p); return n;
    }
    if (is(p,TK_DO)) {
        advance(p); Node *n = nd(ND_DO, ln); n->body = parse_stmt(p);
        expect(p,TK_WHILE); expect(p,TK_LPAREN); n->cond = parse_expr(p);
        expect(p,TK_RPAREN); expect(p,TK_SEMI); return n;
    }
    if (is(p,TK_FOR)) {
        advance(p); expect(p,TK_LPAREN);
        Node *n = nd(ND_FOR, ln); scope_push(p);
        if (!is(p,TK_SEMI)) {
            if (is_type_token(p)) {
                Type *b = parse_declspec(p,NULL,NULL,NULL);
                n->init = parse_decl_stmt(p,b);
            } else { n->init = nd(ND_EXPR_STMT,ln); n->init->lhs = parse_expr(p); consume(p,TK_SEMI); }
        } else consume(p,TK_SEMI);
        if (!is(p,TK_SEMI))
            n->cond = parse_expr(p);
        consume(p,TK_SEMI);
        if (!is(p,TK_RPAREN)) n->step = parse_expr(p);
        expect(p,TK_RPAREN); n->body = parse_stmt(p); scope_pop(p); return n;
    }
    if (is(p,TK_BREAK))    { advance(p); expect(p,TK_SEMI); return nd(ND_BREAK,ln); }
    if (is(p,TK_CONTINUE)) { advance(p); expect(p,TK_SEMI); return nd(ND_CONTINUE,ln); }
    if (is(p,TK_SWITCH)) {
        advance(p); expect(p,TK_LPAREN); Node *n = nd(ND_SWITCH,ln);
        n->cond = parse_expr(p); expect(p,TK_RPAREN); n->body = parse_stmt(p); return n;
    }
    if (is(p,TK_CASE)) {
        advance(p); Node *n = nd(ND_CASE,ln); n->case_val = p->tok.ival;
        advance(p); expect(p,TK_COLON); n->body = parse_stmt(p); return n;
    }
    if (is(p,TK_DEFAULT)) {
        advance(p); expect(p,TK_COLON); Node *n = nd(ND_CASE,ln);
        n->is_default = 1; n->body = parse_stmt(p); return n;
    }
    if (is_type_token(p)) {
        Type *b = parse_declspec(p,NULL,NULL,NULL); return parse_decl_stmt(p,b);
    }
    if (is(p,TK_SEMI)) { advance(p); return nd(ND_BLOCK,ln); }
    Node *n = nd(ND_EXPR_STMT,ln); n->lhs = parse_expr(p); expect(p,TK_SEMI); return n;
}

static Node *parse_block(Parser *p) {
    int ln = p->tok.line; expect(p,TK_LBRACE); scope_push(p);
    Node *head = NULL, *tail = NULL;
    while (!is(p,TK_RBRACE) && !is(p,TK_EOF)) {
        size_t pos_before = p->lexer->pos;
        Node *s = parse_stmt(p);
        if ((size_t)p->lexer->pos == pos_before && !is(p, TK_RBRACE)) {
            fprintf(stderr, "line %d: block parser made no progress\n", p->tok.line);
            advance(p);
            continue;
        }
        while (s) { Node *nx=s->next; s->next=NULL;
            if (!head) head=s; else tail->next=s; tail=s; s=nx; }
    }
    expect(p,TK_RBRACE); scope_pop(p);
    Node *blk = nd(ND_BLOCK,ln); blk->body = head; return blk;
}

static Node *parse_function(Parser *p, Type *ret, char *name, int is_static_fn) {
    Node *fn = nd(ND_FUNC, p->tok.line);
    fn->func_name = name; fn->is_static = is_static_fn; fn->ty = ret;
    p->func_locals = NULL;
    scope_push(p); expect(p,TK_LPAREN);
    Var *ph = NULL, *pt = NULL;
    int param_idx = 0;
    while (!is(p,TK_RPAREN) && !is(p,TK_EOF)) {
        if (is(p,TK_ELLIPSIS)) { advance(p); break; }
        Type *ptype = parse_declspec(p,NULL,NULL,NULL);
        char *pname = NULL; ptype = parse_declarator(p,ptype,&pname);
        Var *pv = new_lvar(p, pname?pname:"_", ptype); free(pname);
        pv->is_param = 1;
        pv->param_index = param_idx++;
        pv->param_next = NULL;
        if (!ph) ph=pv; else pt->param_next=pv; pt=pv;
        if (!consume(p,TK_COMMA)) break;
    }
    expect(p,TK_RPAREN); fn->params = ph;
    if (is(p,TK_SEMI)) { advance(p); scope_pop(p); return NULL; }
    fn->body   = parse_block(p);
    fn->locals = p->func_locals;
    scope_pop(p);
    p->func_locals = NULL;
    return fn;
}

Node *parser_parse(Parser *p) {
    Node *prog = nd(ND_PROGRAM, 0);
    Node *ftail = NULL, *gtail = NULL;
    while (!is(p,TK_EOF)) {
        Token before = p->tok;
        size_t before_pos = p->lexer->pos;
        int is_td=0, is_st=0, is_ex=0;
        if (!is_type_token(p)) {
            if (is(p, TK_EOF)) break;
            fprintf(stderr, "line %d: expected declaration, got '%s'\n",
                    p->tok.line, token_kind_name(p->tok.kind));
            advance(p);
            continue;
        }
        Type *base = parse_declspec(p,&is_td,&is_st,&is_ex);
        if (is(p,TK_EOF)) break;
        if (is(p,TK_SEMI)) { advance(p); continue; }
        char *name = NULL;
        Type *ty   = parse_declarator(p,base,&name);
        if (p->tok.kind == before.kind && p->tok.val == before.val && p->tok.ival == before.ival &&
            (size_t)p->lexer->pos == before_pos) {
            fprintf(stderr, "line %d: parser made no progress at token '%s'\n",
                    p->tok.line, token_kind_name(p->tok.kind));
            advance(p);
            continue;
        }
        if (is_td && name) {
            if (p->ntypedefs < 255) {
                p->typedefs[p->ntypedefs].name = strdup2(name);
                p->typedefs[p->ntypedefs].ty   = ty; p->ntypedefs++;
            }
            free(name); consume(p,TK_SEMI); continue;
        }
        if (!name) {
            if (is(p, TK_EOF)) break;
            consume(p, TK_SEMI);
            continue;
        }
        if (is(p,TK_LPAREN)) {
            Node *fn = parse_function(p,ty,strdup2(name),is_st); free(name);
            if (fn) { if (!prog->funcs) prog->funcs=fn; else ftail->next=fn; ftail=fn; }
            if (is(p, TK_EOF)) break;
        } else {
            Var *gv = new_gvar(p,name,ty); free(name);
            (void)is_ex;
            Node *gvn = nd(ND_GVAR,0); gvn->var = gv;
            if (consume(p, TK_ASSIGN)) {
                if (is(p, TK_STR_LIT)) {
                    gvn->init_str = strdup2(p->tok.val);
                    gvn->has_init = 1;
                    advance(p);
                } else if (is(p, TK_INT_LIT) || is(p, TK_CHAR_LIT)) {
                    gvn->init_ival = p->tok.ival;
                    gvn->has_init = 1;
                    advance(p);
                }
            }
            if (!prog->gvars) prog->gvars=gvn; else gtail->next=gvn; gtail=gvn;
            consume(p,TK_SEMI);
            if (is(p, TK_EOF)) break;
        }
    }
    /* Attach interned string globals */
    for (int i = 0; i < p->nstrings; i++) {
        Var *sv = calloc(1,sizeof(Var));
        sv->name     = p->strings[i].label;
        sv->label    = p->strings[i].label;
        sv->ty       = make_array(ty_char, p->strings[i].len + 1);
        sv->is_local = 0;
        Node *gvn = nd(ND_GVAR,0); gvn->var = sv;
        gvn->init_str = p->strings[i].val;
        if (!prog->gvars) prog->gvars=gvn; else gtail->next=gvn; gtail=gvn;
    }
    return prog;
}

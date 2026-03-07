/* lexer.c - NTCC C tokenizer */
#include "lexer.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>

static const struct { const char *kw; TokenKind kind; } keywords[] = {
    {"int",      TK_INT},      {"char",     TK_CHAR},
    {"long",     TK_LONG},     {"short",    TK_SHORT},
    {"void",     TK_VOID},     {"unsigned", TK_UNSIGNED},
    {"signed",   TK_SIGNED},   {"float",    TK_FLOAT},
    {"double",   TK_DOUBLE},   {"if",       TK_IF},
    {"else",     TK_ELSE},     {"while",    TK_WHILE},
    {"for",      TK_FOR},      {"do",       TK_DO},
    {"return",   TK_RETURN},   {"break",    TK_BREAK},
    {"continue", TK_CONTINUE}, {"struct",   TK_STRUCT},
    {"union",    TK_UNION},    {"enum",     TK_ENUM},
    {"typedef",  TK_TYPEDEF},  {"static",   TK_STATIC},
    {"extern",   TK_EXTERN},   {"const",    TK_CONST},
    {"volatile", TK_VOLATILE}, {"sizeof",   TK_SIZEOF},
    {"switch",   TK_SWITCH},   {"case",     TK_CASE},
    {"default",  TK_DEFAULT},  {"goto",     TK_GOTO},
    {"inline",   TK_INLINE},   {"auto",     TK_AUTO},
    {"register", TK_REGISTER}, {NULL, TK_EOF}
};

Lexer *lexer_new(const char *src) {
    Lexer *l = malloc(sizeof(Lexer));
    l->src      = src;
    l->pos      = 0;
    l->line     = 1;
    l->has_peek = 0;
    memset(&l->cur,  0, sizeof(Token));
    memset(&l->peek, 0, sizeof(Token));
    return l;
}

void lexer_free(Lexer *l) { free(l); }

void token_free(Token *t) {
    if (t->val) { free(t->val); t->val = NULL; }
}

static char cur(Lexer *l)  { return l->src[l->pos]; }
static char next_c(Lexer *l) { return l->src[l->pos + 1]; }
static void adv(Lexer *l) {
    if (l->src[l->pos] == '\n') l->line++;
    l->pos++;
}

static Token make_tok(TokenKind k, int line) {
    Token t; memset(&t, 0, sizeof(t));
    t.kind = k; t.line = line;
    return t;
}

static Token read_token(Lexer *l) {
    /* Skip whitespace and comments */
    for (;;) {
        while (cur(l) && isspace((unsigned char)cur(l))) adv(l);
        if (!cur(l)) return make_tok(TK_EOF, l->line);
        /* Line comment */
        if (cur(l) == '/' && next_c(l) == '/') {
            while (cur(l) && cur(l) != '\n') adv(l);
            continue;
        }
        /* Block comment */
        if (cur(l) == '/' && next_c(l) == '*') {
            adv(l); adv(l);
            while (cur(l) && !(cur(l) == '*' && next_c(l) == '/')) adv(l);
            if (cur(l)) { adv(l); adv(l); }
            continue;
        }
        /* Preprocessor line: skip entire line */
        if (cur(l) == '#') {
            while (cur(l) && cur(l) != '\n') adv(l);
            continue;
        }
        break;
    }

    int line = l->line;
    char c = cur(l);

    /* String literal */
    if (c == '"') {
        adv(l);
        int start = l->pos;
        char buf[4096]; int bi = 0;
        while (cur(l) && cur(l) != '"') {
            if (cur(l) == '\\') {
                adv(l);
                switch (cur(l)) {
                case 'n':  buf[bi++] = '\n'; break;
                case 't':  buf[bi++] = '\t'; break;
                case 'r':  buf[bi++] = '\r'; break;
                case '\\': buf[bi++] = '\\'; break;
                case '"':  buf[bi++] = '"';  break;
                case '0':  buf[bi++] = '\0'; break;
                default:   buf[bi++] = cur(l); break;
                }
            } else {
                buf[bi++] = cur(l);
            }
            adv(l);
            (void)start;
        }
        if (cur(l) == '"') adv(l);
        buf[bi] = '\0';
        Token t = make_tok(TK_STR_LIT, line);
        t.val = malloc(bi + 1);
        memcpy(t.val, buf, bi + 1);
        return t;
    }

    /* Char literal */
    if (c == '\'') {
        adv(l);
        char ch = cur(l);
        if (ch == '\\') {
            adv(l);
            switch (cur(l)) {
            case 'n': ch = '\n'; break; case 't': ch = '\t'; break;
            case 'r': ch = '\r'; break; case '\\': ch = '\\'; break;
            case '\'': ch = '\''; break; case '0': ch = '\0'; break;
            default:  ch = cur(l); break;
            }
        }
        adv(l);
        if (cur(l) == '\'') adv(l);
        Token t = make_tok(TK_CHAR_LIT, line);
        t.ival = (unsigned char)ch;
        return t;
    }

    /* Number */
    if (isdigit((unsigned char)c) || (c == '0' && (next_c(l) == 'x' || next_c(l) == 'X'))) {
        char buf[64]; int bi = 0;
        int base = 10;
        if (c == '0' && (next_c(l) == 'x' || next_c(l) == 'X')) {
            buf[bi++] = cur(l); adv(l);
            buf[bi++] = cur(l); adv(l);
            base = 16;
        } else if (c == '0' && isdigit((unsigned char)next_c(l))) {
            base = 8;
        }
        while (cur(l) && (isxdigit((unsigned char)cur(l)) || cur(l) == 'x' || cur(l) == 'X'))
            buf[bi++] = cur(l), adv(l);
        /* Skip suffixes: u, l, ul, ll, etc. */
        while (cur(l) == 'u' || cur(l) == 'U' || cur(l) == 'l' || cur(l) == 'L') adv(l);
        buf[bi] = '\0';
        Token t = make_tok(TK_INT_LIT, line);
        t.ival = (long long)strtoull(buf, NULL, base);
        return t;
    }

    /* Identifier or keyword */
    if (isalpha((unsigned char)c) || c == '_') {
        char buf[256]; int bi = 0;
        while (cur(l) && (isalnum((unsigned char)cur(l)) || cur(l) == '_'))
            buf[bi++] = cur(l), adv(l);
        buf[bi] = '\0';
        /* Check keywords */
        for (int i = 0; keywords[i].kw; i++) {
            if (strcmp(buf, keywords[i].kw) == 0)
                return make_tok(keywords[i].kind, line);
        }
        Token t = make_tok(TK_IDENT, line);
        t.val = malloc(bi + 1);
        memcpy(t.val, buf, bi + 1);
        return t;
    }

    /* Operators and punctuation */
    adv(l);
    switch (c) {
    case '(':  return make_tok(TK_LPAREN,   line);
    case ')':  return make_tok(TK_RPAREN,   line);
    case '{':  return make_tok(TK_LBRACE,   line);
    case '}':  return make_tok(TK_RBRACE,   line);
    case '[':  return make_tok(TK_LBRACKET, line);
    case ']':  return make_tok(TK_RBRACKET, line);
    case ';':  return make_tok(TK_SEMI,     line);
    case ':':  return make_tok(TK_COLON,    line);
    case ',':  return make_tok(TK_COMMA,    line);
    case '~':  return make_tok(TK_TILDE,    line);
    case '?':  return make_tok(TK_QUESTION, line);
    case '#':  return make_tok(TK_HASH,     line);
    case '.':
        if (cur(l) == '.' && next_c(l) == '.') { adv(l); adv(l); return make_tok(TK_ELLIPSIS, line); }
        return make_tok(TK_DOT, line);
    case '+':
        if (cur(l) == '+') { adv(l); return make_tok(TK_INC, line); }
        if (cur(l) == '=') { adv(l); return make_tok(TK_PLUS_ASSIGN, line); }
        return make_tok(TK_PLUS, line);
    case '-':
        if (cur(l) == '-') { adv(l); return make_tok(TK_DEC, line); }
        if (cur(l) == '>') { adv(l); return make_tok(TK_ARROW, line); }
        if (cur(l) == '=') { adv(l); return make_tok(TK_MINUS_ASSIGN, line); }
        return make_tok(TK_MINUS, line);
    case '*':
        if (cur(l) == '=') { adv(l); return make_tok(TK_STAR_ASSIGN, line); }
        return make_tok(TK_STAR, line);
    case '/':
        if (cur(l) == '=') { adv(l); return make_tok(TK_SLASH_ASSIGN, line); }
        return make_tok(TK_SLASH, line);
    case '%':
        if (cur(l) == '=') { adv(l); return make_tok(TK_PERCENT_ASSIGN, line); }
        return make_tok(TK_PERCENT, line);
    case '&':
        if (cur(l) == '&') { adv(l); return make_tok(TK_AND, line); }
        if (cur(l) == '=') { adv(l); return make_tok(TK_AMP_ASSIGN, line); }
        return make_tok(TK_AMP, line);
    case '|':
        if (cur(l) == '|') { adv(l); return make_tok(TK_OR, line); }
        if (cur(l) == '=') { adv(l); return make_tok(TK_PIPE_ASSIGN, line); }
        return make_tok(TK_PIPE, line);
    case '^':
        if (cur(l) == '=') { adv(l); return make_tok(TK_CARET_ASSIGN, line); }
        return make_tok(TK_CARET, line);
    case '!':
        if (cur(l) == '=') { adv(l); return make_tok(TK_NEQ, line); }
        return make_tok(TK_BANG, line);
    case '=':
        if (cur(l) == '=') { adv(l); return make_tok(TK_EQ, line); }
        return make_tok(TK_ASSIGN, line);
    case '<':
        if (cur(l) == '<') {
            adv(l);
            if (cur(l) == '=') { adv(l); return make_tok(TK_LSHIFT_ASSIGN, line); }
            return make_tok(TK_LSHIFT, line);
        }
        if (cur(l) == '=') { adv(l); return make_tok(TK_LE, line); }
        return make_tok(TK_LT, line);
    case '>':
        if (cur(l) == '>') {
            adv(l);
            if (cur(l) == '=') { adv(l); return make_tok(TK_RSHIFT_ASSIGN, line); }
            return make_tok(TK_RSHIFT, line);
        }
        if (cur(l) == '=') { adv(l); return make_tok(TK_GE, line); }
        return make_tok(TK_GT, line);
    default: {
        /* Unknown char: emit a placeholder and continue */
        fprintf(stderr, "lexer: unexpected char '%c' at line %d\n", c, line);
        return read_token(l); /* skip and retry */
    }
    }
}

Token lexer_next(Lexer *l) {
    if (l->has_peek) {
        l->has_peek = 0;
        l->cur = l->peek;
        return l->cur;
    }
    l->cur = read_token(l);
    return l->cur;
}

Token lexer_peek(Lexer *l) {
    if (!l->has_peek) {
        l->peek     = read_token(l);
        l->has_peek = 1;
    }
    return l->peek;
}

const char *token_kind_name(TokenKind k) {
    switch (k) {
    case TK_INT_LIT: return "INT_LIT"; case TK_STR_LIT: return "STR_LIT";
    case TK_CHAR_LIT: return "CHAR_LIT"; case TK_FLOAT_LIT: return "FLOAT_LIT";
    case TK_IDENT: return "IDENT";
    case TK_INT: return "int"; case TK_CHAR: return "char";
    case TK_LONG: return "long"; case TK_SHORT: return "short";
    case TK_VOID: return "void"; case TK_UNSIGNED: return "unsigned";
    case TK_SIGNED: return "signed"; case TK_RETURN: return "return";
    case TK_IF: return "if"; case TK_ELSE: return "else";
    case TK_WHILE: return "while"; case TK_FOR: return "for";
    case TK_DO: return "do"; case TK_BREAK: return "break";
    case TK_CONTINUE: return "continue"; case TK_STRUCT: return "struct";
    case TK_TYPEDEF: return "typedef"; case TK_STATIC: return "static";
    case TK_EXTERN: return "extern"; case TK_SIZEOF: return "sizeof";
    case TK_SWITCH: return "switch"; case TK_CASE: return "case";
    case TK_DEFAULT: return "default";
    case TK_SEMI: return ";"; case TK_COLON: return ":";
    case TK_COMMA: return ","; case TK_DOT: return ".";
    case TK_LPAREN: return "("; case TK_RPAREN: return ")";
    case TK_LBRACE: return "{"; case TK_RBRACE: return "}";
    case TK_LBRACKET: return "["; case TK_RBRACKET: return "]";
    case TK_PLUS: return "+"; case TK_MINUS: return "-";
    case TK_STAR: return "*"; case TK_SLASH: return "/";
    case TK_PERCENT: return "%"; case TK_AMP: return "&";
    case TK_PIPE: return "|"; case TK_CARET: return "^";
    case TK_TILDE: return "~"; case TK_BANG: return "!";
    case TK_EQ: return "=="; case TK_NEQ: return "!=";
    case TK_LT: return "<"; case TK_LE: return "<=";
    case TK_GT: return ">"; case TK_GE: return ">=";
    case TK_AND: return "&&"; case TK_OR: return "||";
    case TK_ASSIGN: return "="; case TK_INC: return "++";
    case TK_DEC: return "--"; case TK_ARROW: return "->";
    case TK_EOF: return "EOF";
    default: return "?";
    }
}

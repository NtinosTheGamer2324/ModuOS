#ifndef NTCC_LEXER_H
#define NTCC_LEXER_H

/* =========================================================
 * NTCC Lexer - C tokenizer
 * ========================================================= */

typedef enum {
    /* Literals */
    TK_INT_LIT, TK_FLOAT_LIT, TK_STR_LIT, TK_CHAR_LIT,
    /* Identifiers and keywords */
    TK_IDENT,
    TK_INT, TK_CHAR, TK_LONG, TK_SHORT, TK_VOID,
    TK_UNSIGNED, TK_SIGNED, TK_FLOAT, TK_DOUBLE,
    TK_IF, TK_ELSE, TK_WHILE, TK_FOR, TK_DO,
    TK_RETURN, TK_BREAK, TK_CONTINUE,
    TK_STRUCT, TK_UNION, TK_ENUM, TK_TYPEDEF,
    TK_STATIC, TK_EXTERN, TK_CONST, TK_VOLATILE,
    TK_SIZEOF, TK_SWITCH, TK_CASE, TK_DEFAULT,
    TK_GOTO, TK_INLINE, TK_AUTO, TK_REGISTER,
    /* Punctuation */
    TK_LPAREN, TK_RPAREN,     /* ( ) */
    TK_LBRACE, TK_RBRACE,     /* { } */
    TK_LBRACKET, TK_RBRACKET, /* [ ] */
    TK_SEMI, TK_COLON, TK_COMMA, TK_DOT,
    TK_ARROW,                 /* -> */
    TK_ELLIPSIS,              /* ... */
    /* Operators */
    TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH, TK_PERCENT,
    TK_AMP, TK_PIPE, TK_CARET, TK_TILDE,
    TK_LSHIFT, TK_RSHIFT,
    TK_EQ, TK_NEQ, TK_LT, TK_LE, TK_GT, TK_GE,
    TK_AND, TK_OR, TK_BANG,
    TK_ASSIGN,
    TK_PLUS_ASSIGN, TK_MINUS_ASSIGN, TK_STAR_ASSIGN,
    TK_SLASH_ASSIGN, TK_PERCENT_ASSIGN,
    TK_AMP_ASSIGN, TK_PIPE_ASSIGN, TK_CARET_ASSIGN,
    TK_LSHIFT_ASSIGN, TK_RSHIFT_ASSIGN,
    TK_INC, TK_DEC,          /* ++ -- */
    TK_QUESTION, TK_HASH,
    /* Special */
    TK_EOF,
} TokenKind;

typedef struct {
    TokenKind   kind;
    int         line;
    char       *val;    /* heap-allocated text (for literals/idents) */
    long long   ival;   /* integer literal value */
    double      fval;   /* float literal value */
} Token;

typedef struct {
    const char *src;
    int         pos;
    int         line;
    Token       cur;
    Token       peek;
    int         has_peek;
} Lexer;

Lexer *lexer_new(const char *src);
void   lexer_free(Lexer *l);
Token  lexer_next(Lexer *l);
Token  lexer_peek(Lexer *l);
void   token_free(Token *t);
const char *token_kind_name(TokenKind k);

#endif /* NTCC_LEXER_H */

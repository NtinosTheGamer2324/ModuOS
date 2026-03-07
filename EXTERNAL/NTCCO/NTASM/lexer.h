#ifndef LEXER_H
#define LEXER_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

typedef enum {
    TOK_EOF,
    TOK_LABEL,              // label:
    TOK_INSTRUCTION,        // mov, add, push, etc
    TOK_REGISTER,           // rax, rbx, eax, al, etc
    TOK_NUMBER,             // decimal, hex (0x...), octal (0...)
    TOK_STRING,             // "string"
    TOK_IDENTIFIER,         // variable names
    TOK_LPAREN,             // (
    TOK_RPAREN,             // )
    TOK_LBRACKET,           // [
    TOK_RBRACKET,           // ]
    TOK_COMMA,              // ,
    TOK_COLON,              // :
    TOK_SEMICOLON,          // ;
    TOK_DOLLAR,             // $ (immediate prefix)
    TOK_PERCENT,            // % (register prefix in AT&T)
    TOK_DIRECTIVE,          // .section, .global, .bytes, etc
    TOK_PLUS,               // +
    TOK_MINUS,              // -
    TOK_STAR,               // *
    TOK_SLASH,              // /
    TOK_BITS,               // bits (mode directive)
    TOK_NEWLINE,            // \n
} TokenType;

typedef struct {
    TokenType type;
    char value[512];
    int line;
    int column;
} Token;

typedef struct {
    char *source;
    int pos;
    int line;
    int column;
    int len;
} Lexer;

Lexer *lexer_create(const char *source);
void lexer_free(Lexer *lexer);
Token lexer_next_token(Lexer *lexer);

#endif

#include "lexer.h"

Lexer *lexer_create(const char *source) {
    Lexer *lexer = (Lexer *)malloc(sizeof(Lexer));
    lexer->source = (char *)malloc(strlen(source) + 1);
    strcpy(lexer->source, source);
    lexer->pos = 0;
    lexer->line = 1;
    lexer->column = 1;
    lexer->len = strlen(source);
    return lexer;
}

void lexer_free(Lexer *lexer) {
    free(lexer->source);
    free(lexer);
}

static char current(Lexer *lexer) {
    if (lexer->pos >= lexer->len) return '\0';
    return lexer->source[lexer->pos];
}

static char peek(Lexer *lexer, int offset) {
    int pos = lexer->pos + offset;
    if (pos >= lexer->len) return '\0';
    return lexer->source[pos];
}

static void advance(Lexer *lexer) {
    if (lexer->pos < lexer->len) {
        if (lexer->source[lexer->pos] == '\n') {
            lexer->line++;
            lexer->column = 1;
        } else {
            lexer->column++;
        }
        lexer->pos++;
    }
}

static void skip_whitespace(Lexer *lexer) {
    while (current(lexer) != '\0' && isspace(current(lexer)) && current(lexer) != '\n') {
        advance(lexer);
    }
}

static void skip_comment(Lexer *lexer) {
    if (current(lexer) == ';' || (current(lexer) == '#' && peek(lexer, 1) != '0')) {
        while (current(lexer) != '\0' && current(lexer) != '\n') {
            advance(lexer);
        }
    }
}

// List of x86/x86-64 instructions
static int is_instruction(const char *str) {
    static const char *instructions[] = {
        // Data movement
        "mov", "movsx", "movzx", "movsxd", "movabs",
        // Arithmetic
        "add", "sub", "imul", "mul", "idiv", "div", "inc", "dec", "neg",
        // Bitwise
        "and", "or", "xor", "not", "shl", "shr", "sal", "sar", "rol", "ror",
        // Comparison
        "cmp", "test",
        // Branching
        "jmp", "je", "jne", "jl", "jle", "jg", "jge", "jo", "jno", "js", "jns",
        "ja", "jbe", "jz", "jnz", "ljmp", "lcall",
        // Call/Return
        "call", "ret", "nop",
        // Stack
        "push", "pop",
        // String operations
        "movs", "stos", "lods", "cmps", "scas",
        // System
        "syscall", "sysret", "int",
        NULL
    };
    
    for (int i = 0; instructions[i] != NULL; i++) {
        if (strcasecmp(str, instructions[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

// List of x86/x86-64 registers
static int is_register(const char *str) {
    static const char *registers[] = {
        // 64-bit
        "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp",
        "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
        // 32-bit
        "eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp",
        "r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d",
        // 16-bit
        "ax", "bx", "cx", "dx", "si", "di", "bp", "sp",
        "r8w", "r9w", "r10w", "r11w", "r12w", "r13w", "r14w", "r15w",
        // 8-bit
        "al", "bl", "cl", "dl", "sil", "dil", "bpl", "spl",
        "ah", "bh", "ch", "dh",
        "r8b", "r9b", "r10b", "r11b", "r12b", "r13b", "r14b", "r15b",
        // Segment registers
        "cs", "ds", "es", "fs", "gs", "ss",
        NULL
    };
    
    for (int i = 0; registers[i] != NULL; i++) {
        if (strcasecmp(str, registers[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

Token lexer_next_token(Lexer *lexer) {
    Token tok;
    tok.line = lexer->line;
    tok.column = lexer->column;
    tok.value[0] = '\0';

    skip_whitespace(lexer);
    skip_comment(lexer);
    skip_whitespace(lexer);

    if (current(lexer) == '\0') {
        tok.type = TOK_EOF;
        return tok;
    }

    char c = current(lexer);

    // Newline
    if (c == '\n') {
        tok.type = TOK_NEWLINE;
        strcpy(tok.value, "\\n");
        advance(lexer);
        return tok;
    }

    // Numbers (hex, octal, decimal)
    if (isdigit(c) || (c == '0' && peek(lexer, 1) == 'x')) {
        int i = 0;
        if (c == '0' && peek(lexer, 1) == 'x') {
            tok.value[i++] = current(lexer);
            advance(lexer);
            tok.value[i++] = current(lexer);
            advance(lexer);
            while (isxdigit(current(lexer))) {
                tok.value[i++] = current(lexer);
                advance(lexer);
            }
        } else {
            while (isdigit(current(lexer))) {
                tok.value[i++] = current(lexer);
                advance(lexer);
            }
        }
        tok.value[i] = '\0';
        tok.type = TOK_NUMBER;
        return tok;
    }

    // Strings
    if (c == '"') {
        advance(lexer);
        int i = 0;
        while (current(lexer) != '"' && current(lexer) != '\0') {
            tok.value[i++] = current(lexer);
            advance(lexer);
        }
        tok.value[i] = '\0';
        if (current(lexer) == '"') advance(lexer);
        tok.type = TOK_STRING;
        return tok;
    }

    // Identifiers, instructions, registers, labels
    if (isalpha(c) || c == '_' || c == '.') {
        int i = 0;
        while (isalnum(current(lexer)) || current(lexer) == '_' || current(lexer) == '.') {
            tok.value[i++] = current(lexer);
            advance(lexer);
        }
        tok.value[i] = '\0';

        // Check if it's a "bits" keyword
        if (strcasecmp(tok.value, "bits") == 0) {
            tok.type = TOK_BITS;
        }
        // Check if it's a directive
        else if (tok.value[0] == '.') {
            tok.type = TOK_DIRECTIVE;
        }
        // Check if it's an instruction
        else if (is_instruction(tok.value)) {
            tok.type = TOK_INSTRUCTION;
        }
        // Check if it's a register
        else if (is_register(tok.value)) {
            tok.type = TOK_REGISTER;
        }
        // It's a label or identifier
        else {
            tok.type = TOK_IDENTIFIER;
        }
        return tok;
    }

    // Single character tokens
    if (c == '(') { tok.type = TOK_LPAREN; strcpy(tok.value, "("); advance(lexer); return tok; }
    if (c == ')') { tok.type = TOK_RPAREN; strcpy(tok.value, ")"); advance(lexer); return tok; }
    if (c == '[') { tok.type = TOK_LBRACKET; strcpy(tok.value, "["); advance(lexer); return tok; }
    if (c == ']') { tok.type = TOK_RBRACKET; strcpy(tok.value, "]"); advance(lexer); return tok; }
    if (c == ',') { tok.type = TOK_COMMA; strcpy(tok.value, ","); advance(lexer); return tok; }
    if (c == ':') { tok.type = TOK_COLON; strcpy(tok.value, ":"); advance(lexer); return tok; }
    if (c == ';') { tok.type = TOK_SEMICOLON; strcpy(tok.value, ";"); advance(lexer); return tok; }
    if (c == '$') { tok.type = TOK_DOLLAR; strcpy(tok.value, "$"); advance(lexer); return tok; }
    if (c == '%') { tok.type = TOK_PERCENT; strcpy(tok.value, "%"); advance(lexer); return tok; }
    if (c == '+') { tok.type = TOK_PLUS; strcpy(tok.value, "+"); advance(lexer); return tok; }
    if (c == '-') { tok.type = TOK_MINUS; strcpy(tok.value, "-"); advance(lexer); return tok; }
    if (c == '*') { tok.type = TOK_STAR; strcpy(tok.value, "*"); advance(lexer); return tok; }
    if (c == '/') { tok.type = TOK_SLASH; strcpy(tok.value, "/"); advance(lexer); return tok; }

    // Unknown character - skip it
    advance(lexer);
    return lexer_next_token(lexer);
}

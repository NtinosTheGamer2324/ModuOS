/* ntcc.c - NTCC compiler driver */
#include "lexer.h"
#include "parser.h"
#include "codegen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "ntcc: cannot open %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f); rewind(f);
    char *buf = malloc(sz + 1);
    size_t read = fread(buf, 1, sz, f);
    buf[read] = '\0';
    fclose(f);
    return buf;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "NTCC - ModuOS C Compiler\n");
        fprintf(stderr, "Usage: %s <input.c> [-S] [-o output] [-c]\n", argv[0]);
        return 1;
    }

    const char *in_path = argv[1];
    const char *out_path = "out.s";
    int emit_asm = 1;
    int assemble = 1;
    int dump_tokens = 0;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-S") == 0) { emit_asm = 1; assemble = 0; }
        else if (strcmp(argv[i], "-c") == 0) { assemble = 1; }
        else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) { out_path = argv[++i]; }
        else if (strcmp(argv[i], "-tokens") == 0) { dump_tokens = 1; }
    }

    char *src = read_file(in_path);
    if (!src) return 1;

    Lexer *lex = lexer_new(src);
    if (dump_tokens) {
        Token t;
        do {
            t = lexer_next(lex);
            fprintf(stderr, "tok %s line %d", token_kind_name(t.kind), t.line);
            if (t.val) fprintf(stderr, " val='%s'", t.val);
            if (t.kind == TK_INT_LIT || t.kind == TK_CHAR_LIT)
                fprintf(stderr, " ival=%lld", (long long)t.ival);
            fprintf(stderr, "\n");
            token_free(&t);
        } while (t.kind != TK_EOF);
        return 0;
    }
    Parser *parser = parser_new(lex);
    Node *prog = parser_parse(parser);

    if (parser->errors) {
        fprintf(stderr, "ntcc: parse failed (%d errors)\n", parser->errors);
        return 1;
    }

    /* Emit assembly */
    const char *asm_path = emit_asm ? out_path : "out.s";
    codegen(prog, asm_path);

    if (assemble) {
        /* Invoke NTASM to produce ELF64 object */
        char cmd[512];
        const char *obj = out_path;
        if (emit_asm) {
            /* If output was .s, produce .o */
            snprintf(cmd, sizeof(cmd), "ntasm %s -o %s -f elf64", asm_path, out_path);
        } else {
            snprintf(cmd, sizeof(cmd), "ntasm out.s -o %s -f elf64", obj);
        }
        int rc = system(cmd);
        if (rc != 0) {
            fprintf(stderr, "ntcc: ntasm failed (%d)\n", rc);
            return 1;
        }
    }

    free(src);
    return 0;
}

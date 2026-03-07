#ifndef SYMBOLS_H
#define SYMBOLS_H

#include <stdint.h>

typedef enum {
    SYM_LABEL,      /* code label */
    SYM_DATA,       /* data label */
    SYM_EXTERN,     /* external (undefined) */
    SYM_GLOBAL,     /* exported global */
    SYM_CONST,      /* constant (.equ / %define) */
} SymbolType;

typedef struct Symbol {
    char        name[256];
    SymbolType  type;
    uint64_t    value;      /* address / offset */
    int         section;    /* 0=text, 1=data, -1=undef */
    int         is_global;  /* exported in ELF symtab */
    int         defined;    /* 0 if extern/forward ref only */
    struct Symbol *next;
} Symbol;

typedef struct {
    Symbol *head;
    int     count;
} SymbolTable;

SymbolTable *symtab_create(void);
void         symtab_free(SymbolTable *st);
Symbol      *symtab_add(SymbolTable *st, const char *name, SymbolType type,
                        uint64_t value, int section);
Symbol      *symtab_find(SymbolTable *st, const char *name);
void         symtab_mark_global(SymbolTable *st, const char *name);
void         symtab_mark_extern(SymbolTable *st, const char *name);
void         symtab_remove(SymbolTable *st, const char *name);

#endif /* SYMBOLS_H */

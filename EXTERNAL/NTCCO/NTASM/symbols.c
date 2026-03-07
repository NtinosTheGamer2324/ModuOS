#include "symbols.h"
#include <stdlib.h>
#include <string.h>

SymbolTable *symtab_create(void) {
    SymbolTable *st = malloc(sizeof(SymbolTable));
    st->head  = NULL;
    st->count = 0;
    return st;
}

void symtab_free(SymbolTable *st) {
    Symbol *s = st->head;
    while (s) { Symbol *n = s->next; free(s); s = n; }
    free(st);
}

Symbol *symtab_find(SymbolTable *st, const char *name) {
    for (Symbol *s = st->head; s; s = s->next)
        if (strcmp(s->name, name) == 0) return s;
    return NULL;
}

Symbol *symtab_add(SymbolTable *st, const char *name, SymbolType type,
                   uint64_t value, int section) {
    Symbol *s = symtab_find(st, name);
    if (s) {
        /* Update existing forward reference */
        if (!s->defined) {
            s->type    = type;
            s->value   = value;
            s->section = section;
            s->defined = 1;
        }
        return s;
    }
    s = malloc(sizeof(Symbol));
    memset(s, 0, sizeof(*s));
    strncpy(s->name, name, 255);
    s->type    = type;
    s->value   = value;
    s->section = section;
    s->defined = 1;
    s->next    = st->head;
    st->head   = s;
    st->count++;
    return s;
}

void symtab_mark_global(SymbolTable *st, const char *name) {
    Symbol *s = symtab_find(st, name);
    if (!s) {
        s = symtab_add(st, name, SYM_GLOBAL, 0, -1);
        s->defined = 0;
    }
    s->is_global = 1;
}

void symtab_mark_extern(SymbolTable *st, const char *name) {
    Symbol *s = symtab_find(st, name);
    if (!s) {
        s = symtab_add(st, name, SYM_EXTERN, 0, -1);
        s->defined = 0;
    }
    s->type    = SYM_EXTERN;
    s->is_global = 1;
}

void symtab_remove(SymbolTable *st, const char *name) {
    Symbol *prev = NULL;
    for (Symbol *s = st->head; s; s = s->next) {
        if (strcmp(s->name, name) == 0) {
            if (prev) prev->next = s->next;
            else st->head = s->next;
            free(s);
            st->count--;
            return;
        }
        prev = s;
    }
}

#ifndef NTCC_PARSER_H
#define NTCC_PARSER_H

#include "lexer.h"

/* =========================================================
 * Type system
 * ========================================================= */
typedef enum {
    TY_VOID, TY_CHAR, TY_SHORT, TY_INT, TY_LONG,
    TY_UCHAR, TY_USHORT, TY_UINT, TK_ULONG,
    TY_PTR, TY_ARRAY, TY_FUNC, TY_STRUCT,
} TypeKind;

typedef struct Type Type;
typedef struct Member Member;

struct Type {
    TypeKind  kind;
    int       size;     /* sizeof in bytes */
    int       align;    /* alignment */
    Type     *base;     /* for ptr/array */
    int       arr_len;  /* for TY_ARRAY */
    /* For TY_STRUCT */
    char     *tag;
    Member   *members;
    /* For TY_FUNC */
    Type     *ret;
};

struct Member {
    char    *name;
    Type    *ty;
    int      offset;
    Member  *next;
};

/* =========================================================
 * AST node kinds
 * ========================================================= */
typedef enum {
    /* Expressions */
    ND_INT,        /* integer literal */
    ND_STR,        /* string literal */
    ND_VAR,        /* variable reference */
    ND_ADDR,       /* &expr */
    ND_DEREF,      /* *expr */
    ND_NEG,        /* -expr */
    ND_NOT,        /* !expr */
    ND_BITNOT,     /* ~expr */
    ND_CAST,       /* (type)expr */
    ND_SIZEOF,     /* sizeof(type or expr) */
    ND_ADD,        /* + */
    ND_SUB,        /* - */
    ND_MUL,        /* * */
    ND_DIV,        /* / */
    ND_MOD,        /* % */
    ND_AND,        /* & */
    ND_OR,         /* | */
    ND_XOR,        /* ^ */
    ND_SHL,        /* << */
    ND_SHR,        /* >> */
    ND_EQ,         /* == */
    ND_NEQ,        /* != */
    ND_LT,         /* < */
    ND_LE,         /* <= */
    ND_LAND,       /* && */
    ND_LOR,        /* || */
    ND_ASSIGN,     /* = */
    ND_PLUS_ASSIGN,/* += */
    ND_MINUS_ASSIGN,/* -= */
    ND_MUL_ASSIGN, /* *= */
    ND_DIV_ASSIGN, /* /= */
    ND_MOD_ASSIGN, /* %= */
    ND_AND_ASSIGN, /* &= */
    ND_OR_ASSIGN,  /* |= */
    ND_XOR_ASSIGN, /* ^= */
    ND_SHL_ASSIGN, /* <<= */
    ND_SHR_ASSIGN, /* >>= */
    ND_PREINC,     /* ++x */
    ND_PREDEC,     /* --x */
    ND_POSTINC,    /* x++ */
    ND_POSTDEC,    /* x-- */
    ND_CALL,       /* f(args) */
    ND_INDEX,      /* a[i] */
    ND_MEMBER,     /* s.m or s->m */
    ND_TERNARY,    /* a ? b : c */
    ND_COMMA_EXPR, /* a, b */
    /* Statements */
    ND_BLOCK,      /* { stmts } */
    ND_IF,         /* if/else */
    ND_WHILE,      /* while */
    ND_FOR,        /* for */
    ND_DO,         /* do/while */
    ND_RETURN,     /* return */
    ND_BREAK,      /* break */
    ND_CONTINUE,   /* continue */
    ND_SWITCH,     /* switch */
    ND_CASE,       /* case / default */
    ND_EXPR_STMT,  /* expression statement */
    ND_DECL,       /* local variable declaration */
    /* Top-level */
    ND_FUNC,       /* function definition */
    ND_GVAR,       /* global variable */
    ND_PROGRAM,    /* whole translation unit */
} NodeKind;

/* Variable (local or global) */
typedef struct Var Var;
struct Var {
    char   *name;
    Type   *ty;
    int     is_local;
    int     is_param;
    int     param_index;
    int     offset;     /* rbp-relative for locals */
    char   *label;      /* asm label for globals/strings */
    Var    *next;        /* globals list */
    Var    *param_next;  /* function parameter list */
    Var    *local_next;  /* function local list */
};

/* AST node */
typedef struct Node Node;
struct Node {
    NodeKind  kind;
    int       line;
    Type     *ty;       /* inferred type (set during codegen) */

    /* Binary / unary */
    Node     *lhs;
    Node     *rhs;

    /* Literals */
    long long ival;
    char     *sval;     /* string literal content */
    int       slen;

    /* Variable */
    Var      *var;

    /* Function call */
    char     *fname;
    Node     *args;     /* linked list via next_arg */
    Node     *next_arg;

    /* Member access */
    char     *member_name;
    Member   *member;

    /* Cast / sizeof type */
    Type     *cast_ty;

    /* Block / sequence */
    Node     *body;     /* block body: linked list via next */
    Node     *next;     /* next statement in block */

    /* if/while/for/do */
    Node     *cond;
    Node     *then;
    Node     *els;
    Node     *init;
    Node     *step;

    /* switch/case */
    long long case_val;
    int       is_default;

    /* Function definition */
    char     *func_name;
    Var      *params;    /* linked list via param_next */
    Var      *locals;    /* function locals (including params) */
    int       stack_size;
    int       is_static;

    /* Global variable */
    char     *init_str;  /* for string-initialized globals */
    long long init_ival; /* integer initializer */
    int       has_init;

    /* Program */
    Node     *funcs;     /* linked list via next */
    Node     *gvars;     /* linked list via next */
};

/* =========================================================
 * Parser context
 * ========================================================= */
typedef struct Scope Scope;
struct Scope {
    Var    *vars;
    Scope  *parent;
};

typedef struct {
    Lexer  *lexer;
    Token   tok;

    /* Symbol tables */
    Scope  *scope;
    Var    *globals;
    Var    *func_locals; /* current function local list */

    /* Struct tag table */
    struct { char *tag; Type *ty; } structs[256];
    int nstructs;

    /* String literals */
    struct { char *val; int len; char *label; } strings[1024];
    int nstrings;

    /* Typedef table */
    struct { char *name; Type *ty; } typedefs[256];
    int ntypedefs;

    /* Label counters */
    int label_cnt;

    int errors;
} Parser;

/* Built-in types */
extern Type *ty_void, *ty_char, *ty_short, *ty_int, *ty_long;
extern Type *ty_uchar, *ty_ushort, *ty_uint, *ty_ulong;

Type *make_ptr(Type *base);
Type *make_array(Type *base, int len);

Parser *parser_new(Lexer *l);
void    parser_free(Parser *p);
Node   *parser_parse(Parser *p);

#endif /* NTCC_PARSER_H */

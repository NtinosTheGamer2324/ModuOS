// expr.c — evaluate an expression and print the result
// Part of the ModuOS coreutils package.
//
// Supports: + - * / % (integer arithmetic)
//           = != < > <= >= (string/integer comparison, returns 0/1)
//           : (basic regex match length — NYI, reserved)

#include "libc.h"

static void usage(void)
{
    puts("Usage: expr VAL OP VAL");
    puts("Ops:   + - * / %  (arithmetic)");
    puts("       = != < > <= >=  (comparison, prints 1/0)");
}

int md_main(long argc, char **argv)
{
    if (argc != 4) {
        usage();
        return 1;
    }

    const char *a  = argv[1];
    const char *op = argv[2];
    const char *b  = argv[3];
    char buf[32];

    // Arithmetic operators require numeric operands.
    if (!strcmp(op, "+") || !strcmp(op, "-") || !strcmp(op, "*") ||
        !strcmp(op, "/") || !strcmp(op, "%")) {
        long la = atoi(a), lb = atoi(b), res = 0;
        if (!strcmp(op, "+")) res = la + lb;
        else if (!strcmp(op, "-")) res = la - lb;
        else if (!strcmp(op, "*")) res = la * lb;
        else if (!strcmp(op, "/")) {
            if (!lb) { puts("expr: division by zero"); return 1; }
            res = la / lb;
        } else if (!strcmp(op, "%")) {
            if (!lb) { puts("expr: division by zero"); return 1; }
            res = la % lb;
        }
        sprintf(buf, "%ld", res);
        puts(buf);
        return res == 0 ? 1 : 0; // POSIX: exit 1 if result is zero
    }

    // Comparison operators: try numeric first, fall back to lexicographic.
    if (!strcmp(op, "=")  || !strcmp(op, "!=") || !strcmp(op, "<") ||
        !strcmp(op, ">")  || !strcmp(op, "<=") || !strcmp(op, ">=")) {
        int res;
        // Determine if both operands look numeric.
        char *ea, *eb;
        long la = strtol(a, &ea, 10);
        long lb = strtol(b, &eb, 10);
        int numeric = (*ea == '\0' && *eb == '\0');

        if (numeric) {
            if      (!strcmp(op, "="))  res = la == lb;
            else if (!strcmp(op, "!=")) res = la != lb;
            else if (!strcmp(op, "<"))  res = la <  lb;
            else if (!strcmp(op, ">"))  res = la >  lb;
            else if (!strcmp(op, "<=")) res = la <= lb;
            else                        res = la >= lb;
        } else {
            int cmp = strcmp(a, b);
            if      (!strcmp(op, "="))  res = cmp == 0;
            else if (!strcmp(op, "!=")) res = cmp != 0;
            else if (!strcmp(op, "<"))  res = cmp <  0;
            else if (!strcmp(op, ">"))  res = cmp >  0;
            else if (!strcmp(op, "<=")) res = cmp <= 0;
            else                        res = cmp >= 0;
        }

        puts(res ? "1" : "0");
        return res ? 0 : 1;
    }

    printf("expr: unknown operator: %s\n", op);
    return 1;
}
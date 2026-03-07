/* codegen.c - NTCC x86-64 code generator (NTASM) */
#include "parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

static FILE *out;
static int label_id = 0;
static const char *current_func = "fn";

static void emit(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vfprintf(out, fmt, ap); va_end(ap);
}

static int align_to(int n, int align) { return (n + align - 1) & ~(align - 1); }

static void gen_expr(Node *n);
static void gen_stmt(Node *n);
static void gen_addr(Node *n);

static const char *arg_regs[] = {"rdi","rsi","rdx","rcx","r8","r9"};

static int is_ptr(Type *t) { return t && (t->kind == TY_PTR || t->kind == TY_ARRAY); }

static Member *find_member(Type *t, const char *name) {
    if (!t || t->kind != TY_STRUCT) return NULL;
    for (Member *m = t->members; m; m = m->next)
        if (m->name && strcmp(m->name, name) == 0) return m;
    return NULL;
}

static Type *type_of(Node *n) { if (!n) return ty_long;
    if (!n) return ty_long;
    if (n->ty) return n->ty;
    switch (n->kind) {
    case ND_INT: n->ty = ty_int; return n->ty;
    case ND_VAR: n->ty = n->var ? n->var->ty : ty_long; return n->ty;
    case ND_ADDR: n->ty = make_ptr(type_of(n->lhs)); return n->ty;
    case ND_DEREF: {
        Type *bt = type_of(n->lhs);
        n->ty = bt && bt->base ? bt->base : ty_long; return n->ty;
    }
    case ND_MEMBER: {
        Type *bt = type_of(n->lhs);
        Member *m = find_member(bt, n->member_name);
        n->member = m;
        n->ty = m ? m->ty : ty_long;
        return n->ty;
    }
    case ND_ADD:
    case ND_SUB: {
        Type *lt = type_of(n->lhs);
        Type *rt = type_of(n->rhs);
        if (is_ptr(lt) && !is_ptr(rt)) { n->ty = lt; return n->ty; }
        if (!is_ptr(lt) && is_ptr(rt)) { n->ty = rt; return n->ty; }
        if (is_ptr(lt) && is_ptr(rt))  { n->ty = ty_long; return n->ty; }
        n->ty = ty_long; return n->ty;
    }
    default: n->ty = ty_long; return n->ty;
    }
}

static void gen_addr(Node *n) {
    switch (n->kind) {
    case ND_VAR:
        if (n->var->is_local) {
            emit("    lea rax, [rbp-%d]\n", n->var->offset);
        } else {
            emit("    lea rax, [%s]\n", n->var->label);
        }
        return;
    case ND_DEREF:
        gen_expr(n->lhs);
        return;
    case ND_MEMBER:
        gen_addr(n->lhs);
        if (!n->member && n->member_name) {
            Type *bt = NULL;
            if (n->lhs && n->lhs->kind == ND_VAR && n->lhs->var)
                bt = n->lhs->var->ty;
            if (!bt) bt = type_of(n->lhs);
            n->member = find_member(bt, n->member_name);
        }
        if (n->member) emit("    add rax, %d\n", n->member->offset);
        return;
    case ND_INDEX:
        /* handled as deref of add in parser */
        gen_addr(n->lhs);
        return;
    default:
        fprintf(stderr, "codegen: invalid lvalue\n");
        return;
    }
}

static void load(Type *ty) {
    if (!ty) return;
    if (ty->kind == TY_ARRAY) return; /* arrays decay to pointer */
    switch (ty->size) {
    case 1: emit("    movsx rax, byte ptr [rax]\n"); return;
    case 2: emit("    movsx rax, word ptr [rax]\n"); return;
    case 4: emit("    movsxd rax, dword ptr [rax]\n"); return;
    case 8: emit("    mov rax, qword ptr [rax]\n"); return;
    default: emit("    mov rax, qword ptr [rax]\n"); return;
    }
}

static void store(Type *ty) {
    if (!ty) ty = ty_long;
    switch (ty->size) {
    case 1: emit("    mov byte ptr [rdi], al\n"); return;
    case 2: emit("    mov word ptr [rdi], ax\n"); return;
    case 4: emit("    mov dword ptr [rdi], eax\n"); return;
    case 8: emit("    mov qword ptr [rdi], rax\n"); return;
    default: emit("    mov qword ptr [rdi], rax\n"); return;
    }
}

static void gen_expr(Node *n) {
    switch (n->kind) {
    case ND_INT:
        emit("    mov rax, %lld\n", n->ival);
        return;
    case ND_STR:
        /* String literal should be lowered to ND_VAR, but handle safely */
        if (n->sval)
            emit("    lea rax, [%s]\n", n->sval);
        else
            emit("    mov rax, 0\n");
        return;
    case ND_VAR:
        gen_addr(n);
        load(n->var->ty);
        return;
    case ND_MEMBER:
        gen_addr(n);
        n->ty = type_of(n);
        load(n->ty ? n->ty : ty_long);
        return;
    case ND_ADDR:
        gen_addr(n->lhs);
        n->ty = type_of(n);
        return;
    case ND_DEREF:
        gen_expr(n->lhs);
        n->ty = type_of(n);
        load(n->ty ? n->ty : ty_long);
        return;
    case ND_NEG:
        gen_expr(n->lhs);
        emit("    neg rax\n");
        return;
    case ND_NOT:
        gen_expr(n->lhs);
        emit("    cmp rax, 0\n");
        emit("    sete al\n");
        emit("    movzx rax, al\n");
        return;
    case ND_BITNOT:
        gen_expr(n->lhs);
        emit("    not rax\n");
        return;
    case ND_PREINC:
    case ND_PREDEC: {
        gen_addr(n->lhs);
        emit("    mov rdi, rax\n");
        load(n->lhs->ty);
        emit(n->kind == ND_PREINC ? "    add rax, 1\n" : "    sub rax, 1\n");
        store(n->lhs->ty);
        return;
    }
    case ND_POSTINC:
    case ND_POSTDEC: {
        gen_addr(n->lhs);
        emit("    mov rdi, rax\n");
        load(n->lhs->ty);
        emit("    mov rcx, rax\n");
        emit(n->kind == ND_POSTINC ? "    add rax, 1\n" : "    sub rax, 1\n");
        store(n->lhs->ty);
        emit("    mov rax, rcx\n");
        return;
    }
    case ND_ASSIGN: {
        gen_addr(n->lhs);
        emit("    push rax\n");
        gen_expr(n->rhs);
        emit("    pop rdi\n");
        Type *lt = type_of(n->lhs);
        store(lt ? lt : ty_long);
        return;
    }
    case ND_PLUS_ASSIGN:
    case ND_MINUS_ASSIGN:
    case ND_MUL_ASSIGN:
    case ND_DIV_ASSIGN: {
        gen_addr(n->lhs);
        emit("    push rax\n");
        load(n->lhs->ty);
        emit("    push rax\n");
        gen_expr(n->rhs);
        emit("    pop rdi\n");
        if (n->kind == ND_PLUS_ASSIGN) emit("    add rax, rdi\n");
        else if (n->kind == ND_MINUS_ASSIGN) emit("    sub rdi, rax\n    mov rax, rdi\n");
        else if (n->kind == ND_MUL_ASSIGN) emit("    imul rax, rdi\n");
        else if (n->kind == ND_DIV_ASSIGN) {
            emit("    mov rcx, rax\n    mov rax, rdi\n    cqo\n    idiv rcx\n");
        }
        emit("    pop rdi\n");
        store(n->lhs->ty);
        return;
    }
    case ND_ADD: case ND_SUB: case ND_MUL: case ND_DIV: case ND_MOD:
    case ND_AND: case ND_OR: case ND_XOR: case ND_SHL: case ND_SHR:
    case ND_EQ: case ND_NEQ: case ND_LT: case ND_LE:
    case ND_LAND: case ND_LOR: {
        Type *lt = type_of(n->lhs);
        Type *rt = type_of(n->rhs);
        gen_expr(n->lhs); emit("    push rax\n");
        gen_expr(n->rhs); emit("    mov rdi, rax\n");
        emit("    pop rax\n");
        switch (n->kind) {
        case ND_ADD:
            if (is_ptr(lt) && !is_ptr(rt)) emit("    imul rdi, %d\n", lt->base ? lt->base->size : 8);
            if (!is_ptr(lt) && is_ptr(rt)) emit("    imul rax, %d\n", rt->base ? rt->base->size : 8);
            emit("    add rax, rdi\n");
            break;
        case ND_SUB:
            if (is_ptr(lt) && !is_ptr(rt)) emit("    imul rdi, %d\n", lt->base ? lt->base->size : 8);
            emit("    sub rax, rdi\n");
            break;
        case ND_MUL: emit("    imul rax, rdi\n"); break;
        case ND_DIV:
            emit("    cqo\n    idiv rdi\n");
            break;
        case ND_MOD:
            emit("    cqo\n    idiv rdi\n    mov rax, rdx\n");
            break;
        case ND_AND: emit("    and rax, rdi\n"); break;
        case ND_OR:  emit("    or rax, rdi\n"); break;
        case ND_XOR: emit("    xor rax, rdi\n"); break;
        case ND_SHL: emit("    mov rcx, rdi\n    shl rax, cl\n"); break;
        case ND_SHR: emit("    mov rcx, rdi\n    shr rax, cl\n"); break;
        case ND_EQ:
            emit("    cmp rax, rdi\n    sete al\n    movzx rax, al\n"); break;
        case ND_NEQ:
            emit("    cmp rax, rdi\n    setne al\n    movzx rax, al\n"); break;
        case ND_LT:
            emit("    cmp rax, rdi\n    setl al\n    movzx rax, al\n"); break;
        case ND_LE:
            emit("    cmp rax, rdi\n    setle al\n    movzx rax, al\n"); break;
        case ND_LAND: {
            int l1 = label_id++, l2 = label_id++;
            emit("    cmp rax, 0\n    je .Lfalse%d\n", l1);
            emit("    cmp rdi, 0\n    je .Lfalse%d\n", l1);
            emit("    mov rax, 1\n    jmp .Lend%d\n", l2);
            emit(".Lfalse%d:\n    mov rax, 0\n.Lend%d:\n", l1, l2);
            break;
        }
        case ND_LOR: {
            int l1 = label_id++, l2 = label_id++;
            emit("    cmp rax, 0\n    jne .Ltrue%d\n", l1);
            emit("    cmp rdi, 0\n    jne .Ltrue%d\n", l1);
            emit("    mov rax, 0\n    jmp .Lend%d\n", l2);
            emit(".Ltrue%d:\n    mov rax, 1\n.Lend%d:\n", l1, l2);
            break;
        }
        default: break;
        }
        return;
    }
    case ND_CALL: {
        int argc = 0;
        for (Node *a = n->args; a; a = a->next_arg) argc++;
        int stack_args = argc > 6 ? argc - 6 : 0;
        /* push args right-to-left */
        Node *args[32]; int i = 0;
        for (Node *a = n->args; a; a = a->next_arg) args[i++] = a;
        for (int j = argc - 1; j >= 0; j--) {
            gen_expr(args[j]);
            if (j >= 6) emit("    push rax\n");
        }
        for (int j = 0; j < argc && j < 6; j++) {
            gen_expr(args[j]);
            emit("    mov %s, rax\n", arg_regs[j]);
        }
        emit("    call %s\n", n->fname);
        if (stack_args) emit("    add rsp, %d\n", stack_args * 8);
        return;
    }
    case ND_COMMA_EXPR:
        gen_expr(n->lhs); gen_expr(n->rhs); return;
    default:
        fprintf(stderr, "codegen: unhandled expr kind %d\n", n->kind);
        return;
    }
}

static void gen_stmt(Node *n) {
    switch (n->kind) {
    case ND_BLOCK:
        for (Node *s = n->body; s; s = s->next) gen_stmt(s);
        return;
    case ND_EXPR_STMT:
        if (n->lhs) gen_expr(n->lhs);
        return;
    case ND_RETURN:
        if (n->lhs) gen_expr(n->lhs);
        emit("    jmp .Lret_%s\n", current_func);
        return;
    case ND_IF: {
        int id = label_id++;
        gen_expr(n->cond);
        emit("    cmp rax, 0\n    je .Lelse%d\n", id);
        gen_stmt(n->then);
        emit("    jmp .Lend%d\n", id);
        emit(".Lelse%d:\n", id);
        if (n->els) gen_stmt(n->els);
        emit(".Lend%d:\n", id);
        return;
    }
    case ND_WHILE: {
        int id = label_id++;
        emit(".Lbegin%d:\n", id);
        gen_expr(n->cond);
        emit("    cmp rax, 0\n    je .Lend%d\n", id);
        gen_stmt(n->body);
        emit("    jmp .Lbegin%d\n.Lend%d:\n", id, id);
        return;
    }
    case ND_FOR: {
        int id = label_id++;
        if (n->init) gen_stmt(n->init);
        emit(".Lbegin%d:\n", id);
        if (n->cond) { gen_expr(n->cond); emit("    cmp rax, 0\n    je .Lend%d\n", id); }
        gen_stmt(n->body);
        if (n->step) gen_expr(n->step);
        emit("    jmp .Lbegin%d\n.Lend%d:\n", id, id);
        return;
    }
    case ND_DO: {
        int id = label_id++;
        emit(".Lbegin%d:\n", id);
        gen_stmt(n->body);
        gen_expr(n->cond);
        emit("    cmp rax, 0\n    jne .Lbegin%d\n", id);
        return;
    }
    case ND_DECL:
        if (n->rhs) {
            Node tmp = {0};
            tmp.kind = ND_VAR;
            tmp.var  = n->var;
            gen_addr(&tmp);
            emit("    push rax\n");
            gen_expr(n->rhs);
            emit("    pop rdi\n");
            store(n->var->ty);
        }
        return;
    default:
        fprintf(stderr, "codegen: unhandled stmt kind %d\n", n->kind);
        return;
    }
}

void codegen(Node *prog, const char *out_path) {
    out = fopen(out_path, "w");
    if (!out) { fprintf(stderr, "cannot open %s\n", out_path); exit(1); }

    emit("bits 64\n");
    emit(".text\n");

    /* Emit function definitions */
    for (Node *fn = prog->funcs; fn; fn = fn->next) {
        current_func = fn->func_name;
        emit(".global %s\n", fn->func_name);
        emit("%s:\n", fn->func_name);
        emit("    push rbp\n    mov rbp, rsp\n");

        /* Allocate locals */
        int offset = 0;
        for (Var *v = fn->locals; v; v = v->local_next) {
            offset += align_to(v->ty->size, 8);
            v->offset = offset;
        }
        fn->stack_size = align_to(offset, 16);
        if (fn->stack_size) emit("    sub rsp, %d\n", fn->stack_size);

        /* Move params from registers to stack slots */
        for (Var *v = fn->params; v; v = v->param_next) {
            if (v->param_index >= 0 && v->param_index < 6) {
                emit("    mov [rbp-%d], %s\n", v->offset, arg_regs[v->param_index]);
            }
        }

        /* Body */
        fn->body->func_name = fn->func_name;
        gen_stmt(fn->body);

        emit(".Lret_%s:\n", fn->func_name);
        emit("    mov rsp, rbp\n    pop rbp\n    ret\n");
    }

    /* Globals and strings */
    emit(".data\n");
    for (Node *gv = prog->gvars; gv; gv = gv->next) {
        emit("%s:\n", gv->var->label);
        if (gv->init_str) {
            emit("    db \"%s\", 0\n", gv->init_str);
        } else if (gv->has_init) {
            emit("    dq %lld\n", gv->init_ival);
        } else {
            emit("    dq 0\n");
        }
    }

    fclose(out);
}

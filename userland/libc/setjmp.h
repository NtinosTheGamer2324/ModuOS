#ifndef _SETJMP_H
#define _SETJMP_H

/* x86-64: rbx, rbp, r12-r15, rsp, rip */
typedef unsigned long jmp_buf[8];

int  setjmp(jmp_buf env) __attribute__((returns_twice));
void longjmp(jmp_buf env, int val) __attribute__((noreturn));

#define setjmp(env) setjmp(env)

#endif /* _SETJMP_H */

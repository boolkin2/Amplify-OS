#ifndef _SETJMP_H
#define _SETJMP_H

#ifdef __cplusplus
extern "C" {
#endif

// jmp_buf хранит callee-saved регистры x86_64 SysV:
//   [0] = rbx, [1] = rbp, [2] = r12, [3] = r13, [4] = r14, [5] = r15,
//   [6] = rsp (после восстановления), [7] = rip
typedef long jmp_buf[8];
typedef long sigjmp_buf[8 + 2];   // +mask (не поддерживается, padding)

int  setjmp   (jmp_buf env);
void longjmp  (jmp_buf env, int val) __attribute__((noreturn));
int  sigsetjmp(sigjmp_buf env, int savesigs);
void siglongjmp(sigjmp_buf env, int val) __attribute__((noreturn));

#define _setjmp setjmp
#define _longjmp longjmp

#ifdef __cplusplus
}
#endif
#endif

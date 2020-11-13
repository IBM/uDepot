/*
 * Copyright(c) 2012-2015, ETH Zurich. All rights reserved.
 *
 * Released under the BSD 3-clause license. When using or
 * redistributing this file, you may do so under either license.
 *
 * Kornilios Kourtis <akourtis@inf.ethz.ch>, <kornilios@gmail.com>.
 */

/**
 * jctx: user-space context switching
 *
 * It uses some assembly to set up a stack and gcc's setjmp/longjmp intrinsics.
 */

#ifndef JCTX_H__
#define JCTX_H__

#include <stdlib.h>

// an array of 5  words is needed according to gcc/builtins.c
struct jctx {
	void *stk;
	unsigned long stk_size;
	void *ctx_[5];
};
typedef struct jctx jctx_t;

// ref: libcilkrts/runtime/jmpbuff.h + code generation
#define jctx_fp(jctx) ((jctx)->ctx_[0])
#define jctx_ip(jctx) ((jctx)->ctx_[1])
#define jctx_sp(jctx) ((jctx)->ctx_[2])

void jctx_init__(jctx_t *jctx, void *stk, size_t stk_size, void *fn, void *fn_arg);
void jctx_init(jctx_t *jctx, size_t stk_size, void *fn, void *fn_arg);
void jctx_reset(jctx_t *jctx, void *fn, void *fn_arg);
void jctx_init2(jctx_t *jctx, size_t stk_size, void *fn, void *fn_arg1, void *fn_arg2);
void jctx_destroy(jctx_t *jctx);

/*
static inline int
jctx_setjmp(struct jctx *jctx)
{
	return __builtin_setjmp(&jctx->ctx_);
}

static inline void
jctx_longjmp(struct jctx *jctx)
{
	__builtin_longjmp(&jctx->ctx_, 1);
}
*/


#define jctx_setjmp(x)  __builtin_setjmp(&(x)->ctx_)
#define jctx_longjmp(x) __builtin_longjmp(&(x)->ctx_, 1) // XXX: This is actually unsafe with newer GCC versions!

void jctx_switch(jctx_t *from, jctx_t *to);
void jctx_jump(jctx_t *to);

#endif /* JCTX_H__ */

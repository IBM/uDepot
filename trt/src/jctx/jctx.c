/*
 * Copyright(c) 2012-2015, ETH Zurich. All rights reserved.
 *
 * Released under the BSD 3-clause license. When using or
 * redistributing this file, you may do so under either license.
 *
 * Kornilios Kourtis <akourtis@inf.ethz.ch>, <kornilios@gmail.com>.
 */

#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include <sys/mman.h>
#include <stdbool.h>

#define PAGE_SIZE 4096
#define NPAGES(s) ((s + PAGE_SIZE - 1) / PAGE_SIZE)

#include "jctx.h"

//#define JCTX_USE_MMAP
#if defined(JCTX_USE_MMAP)
static const bool stack_protect = false;
#endif


static void *
alloc_stack(size_t size)
{
	#if defined(JCTX_USE_MMAP)
	const int prot = PROT_READ | PROT_WRITE;

	size_t npages = NPAGES(size);
	if (stack_protect)
		npages++;

	void *ret = mmap(NULL, npages*PAGE_SIZE, prot, MAP_PRIVATE | MAP_ANON, -1, 0);
	if (ret == MAP_FAILED) {
		perror("mmap");
		exit(1);
	}

	if (stack_protect) {
		void *guard = ret + (npages-1)*PAGE_SIZE;
		int r = mprotect(guard, PAGE_SIZE, PROT_NONE);
		if (r < 0) {
			perror("mprotect");
			exit(1);
		}
	}

	return ret;
	#else
	void *ret = malloc(size);
	if (ret == NULL) {
		perror("malloc");
		exit(1);
	}
	return ret;
	#endif
}


void jctx_destroy(jctx_t *jctx)
{
	#if defined(JCTX_USE_MMAP)
	size_t npages = NPAGES(jctx->stk_size);
	if (stack_protect)
		npages++;
	int ret = munmap(jctx->stk, npages*PAGE_SIZE);
	if (ret < 0) {
		perror("munmap");
		exit(1);
	}
	#else
	free(jctx->stk);
	#endif
}

// this function will be called from jctx_begin__, after @fn in jctx_init()
// returns
__attribute__((weak))
void jctx_end(void *ret)
{
	printf("%s: The end, my only friend the end (exiting)\n", __FUNCTION__);
	printf("ret = %p\n", ret);
	exit(1);
}

void jctx_init(jctx_t *jctx, size_t stk_size, void *fn, void *fn_arg)
{
	void *stk = alloc_stack(stk_size);
	jctx_init__(jctx, stk, stk_size, fn, fn_arg);
}


/* ->stk and ->stk_size are valid, we just want to reset fn and fn arg */
void jctx_reset(jctx_t *jctx, void *fn, void *fn_arg)
{
	void jctx_begin__(void);

	// push some arguments to the stack for jctx_begin__
	#if 1
	uintptr_t *sp = (uintptr_t *)(jctx->stk + jctx->stk_size) - 8;
	sp[0] = (uintptr_t)fn_arg;
	sp[1] = (uintptr_t)fn;
	sp[2] = 0x1a1a1a1a1a1a1a1a;
	sp[3] = 0x1a1a1a1a1a1a1a1a;
	sp[4] = 0x1a1a1a1a1a1a1a1a;
	sp[5] = 0x1a1a1a1a1a1a1a1a;
	sp[6] = 0x1a1a1a1a1a1a1a1a;
	sp[7] = 0x1a1a1a1a1a1a1a1a;
	#else
	uintptr_t *sp = (uintptr_t *)(jctx->stk + jctx->stk_size) - 2;
	sp[0] = (uintptr_t)fn_arg;
	sp[1] = (uintptr_t)fn;
	#endif

	jctx_ip(jctx) = (void *)jctx_begin__;
	jctx_sp(jctx) = sp;

	#if defined(__PPC64__)
	/*
	 * NB: My initial approach did not work, so I examined the
	 * jctx_longjmp() assembly code and wrote something that seems to work.
	 * Need to understand the ABI better to ensure that it will work
	 * correctly.
	 *
	 * This is the code for longjmp in PPC64:
	 * jctx_longjmp:
	 *    0x0000000010000e9c <+0>:     std     r31,-8(r1)
	 *    0x0000000010000ea0 <+4>:     stdu    r1,-64(r1)
	 *    0x0000000010000ea4 <+8>:     mr      r31,r1
	 *    0x0000000010000ea8 <+12>:    std     r3,40(r31)
	 *     STORES first argument (jctx) to 40(r31) -- somewhere in the stack
	 * => 0x0000000010000eac <+16>:    ld      r9,40(r31)
	 *      loads the value from the stack to r9
	 *    0x0000000010000eb0 <+20>:    addi    r9,r9,16
	 *      r9 now has the address of the jump buffer
	 *    0x0000000010000eb4 <+24>:    ld      r10,8(r9)
	 *      r10 gets ctx[1], which is the IP
	 *    0x0000000010000eb8 <+28>:    ld      r31,0(r9)
	 *      r31 gets ctx[0] which is FP
	 *    0x0000000010000ebc <+32>:    ld      r8,16(r9)
	 *      r8 gets ctx[2] which is the new SP
	 *    0x0000000010000ec0 <+36>:    ld      r9,24(r9)
	 *      r9 is reloaded from ctx[3] which is ???
	 *    0x0000000010000ec4 <+40>:    std     r8,0(r9)
	 *      store r8 (the new SP) to *(ctx[3])
	 *    0x0000000010000ec8 <+44>:    mr      r1,r9
	 *      store r9 into r1 (r1 should contain the new the stack pointer)
	 *    0x0000000010000ecc <+48>:    mtctr   r10
	 *      set counter to r10 (which is the IP)
	 *    0x0000000010000ed0 <+52>:    bctr
	 *      jump to counter
	 *
	 */

	// make sure that the stack pointer is loaded correctly
	jctx->ctx_[3] = jctx_sp(jctx);

	// sp[0] will be overwritten by the above code.
	// Store the argument into sp[2]
	sp[2] = (uintptr_t)fn_arg;
	#endif

	/*
	for (unsigned i=0; i<5; i++)
		printf("jctx->ctx_[%d] = 0x%p\n", i, jctx->ctx_[i]);
	*/

	return;
}

void jctx_init__(jctx_t *jctx, void *stk, size_t stk_size, void *fn, void *fn_arg)
{
	jctx->stk = stk;
	jctx->stk_size = stk_size;
	//printf("stack size: %p-%p\n", jctx->stk, jctx->stk + jctx->stk_size);
	jctx_reset(jctx, fn, fn_arg);
}

void
jctx_init2(jctx_t *jctx, size_t stk_size, void *fn, void *fn_arg1, void *fn_arg2)
{
	jctx->stk = alloc_stack(stk_size);
	jctx->stk_size = stk_size;
	//printf("stack size: %p-%p\n", jctx->stk, jctx->stk + jctx->stk_size);

	void jctx_begin2__(void);

	// push some arguments to the stack for jctx_begin__
	uintptr_t *sp = (uintptr_t *)(jctx->stk + stk_size) - 8;
	sp[0] = (uintptr_t)fn_arg2;
	sp[1] = (uintptr_t)fn_arg1;
	sp[2] = (uintptr_t)fn;
	sp[3] = 0x1a1a1a1a1a1a1a1a;
	sp[4] = 0x1a1a1a1a1a1a1a1a;
	sp[5] = 0x1a1a1a1a1a1a1a1a;
	sp[6] = 0x1a1a1a1a1a1a1a1a;
	sp[7] = 0x1a1a1a1a1a1a1a1a;

	jctx_ip(jctx) = (void *)jctx_begin2__;
	jctx_sp(jctx) = sp;

	/*
	for (unsigned i=0; i<5; i++)
		printf("jctx->ctx_[%d] = 0x%p\n", i, jctx->ctx_[i]);
	*/

	return;
}

// XXX: copied from gcc's cilk code. Kept the name for future reference.
//
// GCC doesn't allow us to call __builtin_longjmp in the same function that
// calls __builtin_setjmp, so create a new function to house the call to
// __builtin_longjmp
static void __attribute__((noinline))
do_cilk_longjmp(jctx_t *jmpbuf)
{
    jctx_longjmp(jmpbuf);
}

void jctx_switch(jctx_t *from, jctx_t *to)
{
	if (jctx_setjmp(from) == 0) {
		do_cilk_longjmp(to);
		fprintf(stderr, "%s: after longjmp", __FUNCTION__);
		abort();
	}
}

void jctx_jump(jctx_t *to)
{
	do_cilk_longjmp(to);
}


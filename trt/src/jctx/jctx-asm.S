/*
 * Copyright(c) 2012-2015, ETH Zurich. All rights reserved.
 *
 * Released under the BSD 3-clause license. When using or
 * redistributing this file, you may do so under either license.
 *
 * Kornilios Kourtis <akourtis@inf.ethz.ch>, <kornilios@gmail.com>.
 */

#if defined(__x86_64__)

#if defined(__linux__)
	/* Define an entry point visible from C.  */
	#define ENTRY(name)      \
	  .globl name;           \
	  .type name,@function;  \
	  .align 16;             \
	  name ##:

	#define END(name)        \
	  .size name, .-name;
#elif defined(__APPLE__)
	/* Define an entry point visible from C.  */
	#define ENTRY(name)      \
	  .globl name;           \
	  .align 8;             \
	  name ##:

	#define END(name)
#endif


/*
 * jctx_begin: initial function
 */
ENTRY(jctx_begin__)
	popq %rdi       // fn_arg
	popq %r13       // fn
	call *%r13      // call fn
	movq %rax, %rdi // first argument: fn's return
	call jctx_end@PLT   // call jctx_end
	movq $1, 0      // segfault
END(jctx_begin__)


/*
 * jctx_begin2: initial function (with three arguments)
 */
ENTRY(jctx_begin2__)
	popq %rsi       // fn_arg2
	popq %rdi       // fn_arg1
	popq %r13       // fn
	call *%r13      // call fn
	movq %rax, %rdi // first argument: fn's return
	call jctx_end@PLT   // call jctx_end
	movq $1, 0      // segfault
END(jctx_begin__)

#elif defined(__PPC64__)

	.globl jctx_begin__
	.type  jctx_begin__, @function
jctx_begin__:
	ld 4,8(1)          // function
	mtctr 4            // move contents of r4 to the counter register
	ld 3,16(1)         // arg (see: jctx_init())
	stdu 1,-32(1)      // grow stack (not sure if this is needed)
	bctrl              // "call" (branch to the) counter register
	bl jctx_end        // "call" jctx_end
	// segfault
	li 9,0
	ld 9,0(9)
	trap
	.size	jctx_begin__,.-jctx_begin__


// NYI!
	.globl jctx_begin2__
	.type  jctx_begin2__, @function
jctx_begin2__:
	li 9,0
	ld 9,0(9)
	trap
	.size	jctx_begin2__,.-jctx_begin2__

#endif // END PPC64

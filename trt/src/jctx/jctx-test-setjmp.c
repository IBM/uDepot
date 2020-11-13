/*
 * Copyright(c) 2012-2015, ETH Zurich. All rights reserved.
 *
 * Released under the BSD 3-clause license. When using or
 * redistributing this file, you may do so under either license.
 *
 * Kornilios Kourtis <akourtis@inf.ethz.ch>, <kornilios@gmail.com>.
 */

#include <stdio.h>
#include "jctx.h"

int main(int argc, char *argv[])
{
	struct jctx jctx;

	printf("=> Initializing jump\n");
	if (jctx_setjmp(&jctx) != 0) {
		printf("Jump completed\n");
		return 0;
	}

	printf("FP=%p\n", (void *)jctx_fp(&jctx));
	printf("IP=%p\n", (void *)jctx_ip(&jctx));
	printf("SP=%p\n", (void *)jctx_sp(&jctx));
	printf("Jumping!\n");
	jctx_jump(&jctx);

	return 0;
}

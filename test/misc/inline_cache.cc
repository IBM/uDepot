/*
 *  Copyright (c) 2020 International Business Machines
 *  All rights reserved.
 *
 *  SPDX-License-Identifier: BSD-3-Clause
 *
 *  Authors: Nikolas Ioannou (nio@zurich.ibm.com),
 *           Kornilios Kourtis (kou@zurich.ibm.com, kornilios@gmail.com)
 *
 */

#include <deque>
#include "util/inline-cache.hh"

struct Obj {
	uint64_t a[8];
};

int main(int argc, char *argv[]) {

	InlineObjCache<Obj, 8> obj_cache;
	std::deque<Obj *> objs;

	for (size_t i=0; i < 16; i++) {
		Obj *o = obj_cache.get();
		//printf("Pushing o=%p\n", o);
		objs.push_back(o);
	}

	for (size_t i=0; i < 16; i++) {
		Obj *o = objs.front();
		//printf("Popped o=%p\n", o);
		objs.pop_front();
		obj_cache.put(o);
	}
}

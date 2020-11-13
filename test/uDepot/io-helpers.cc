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

#include <stdlib.h>
#include <cassert>

#include "uDepot/mbuff.hh"
#include "uDepot/io.hh"
#include "uDepot/io/helpers.hh"

using IO = udepot::FileIO;

// move this so that it can be used elsewhere
template<typename Ptr>
struct IoPtrSlice : public Ptr {
	Ptr  orig_ptr;

	IoPtrSlice(Ptr &&parent_ptr) {
		Ptr::ptr_m = parent_ptr.ptr_m;
		Ptr::size_m = parent_ptr.size_m;
		orig_ptr.ptr_m = parent_ptr.ptr_m;
		orig_ptr.size_m = parent_ptr.size_m;
	}

	IoPtrSlice() = delete;
	IoPtrSlice(IoPtrSlice const&) = delete;
	void operator=(IoPtrSlice const &) = delete;

	void assert_valid_slice() {
		assert(Ptr::ptr_m >= orig_ptr.ptr_m);
		assert((char *)Ptr::ptr_m + Ptr::size_m <= (char *)orig_ptr.ptr_m + orig_ptr.size_m);
	}

	// move the start of the buffer by @off and set buffer length to @len
	void reslice(ssize_t off, size_t len) {
		Ptr::ptr_m = ((char *)Ptr::ptr_m + off);
		Ptr::size_m = len;
		assert_valid_slice();
	}

	// reslice from the original pointer
	void reslice_from_orig(size_t off, size_t len) {
		Ptr::ptr_m = orig_ptr.ptr_m + off;
		Ptr::size_m = len;
		assert_valid_slice();
	}

	// reset to the original pointer
	void reslice_orig(void) {
		Ptr::ptr_m  = orig_ptr.ptr_m;
		Ptr::size_m = orig_ptr.size_m;
		assert_valid_slice();
	}
};

// Builds an Mbuff of @buffs_nr buffers, each of @buff_size
template<typename IO>
static udepot::Mbuff
create_mbuff(size_t buff_size, size_t buffs_nr)
{
	udepot::Mbuff mbuff(std::type_index(typeid(typename IO::Ptr)));
	for (size_t i=0; i < buffs_nr; i++) {
		auto ioptr = IO::alloc_iobuff(buff_size);
		if (!ioptr.ptr_m) {
			perror("alloc_iobuff");
			abort();
		}
		mbuff.add_iobuff(ioptr, buff_size);
	}
	assert(mbuff.get_free_size() == buff_size*buffs_nr);
	return mbuff;
}

int main(int argc, char *argv[])
{
	const size_t buff_size = 10;
	const size_t nbuffs = 20;
	const size_t total_size = nbuffs*buff_size;

	// temporary file
	char fname[] = "/tmp/udepot-io-test-XXXXXX";
	int fd = mkstemp(fname);
	if (fd == -1) {
		perror("creating random file");
		exit(1);
	}

	// buffer with random data
	IO::Ptr buff = IO::alloc_iobuff(total_size);
	if (!buff.ptr_m) {
		perror("malloc");
		exit(1);
	}
	for (size_t i=0; i < total_size; i++) {
		*((char *)buff.ptr_m + i)= rand();
	}

	IO io(fd);
	assert(buff.size_m == total_size);
	size_t off = 0, rem = total_size;
	while (rem > 0) {
		IO::Ptr slice((char *)buff.ptr_m + off, rem);
		ssize_t ret = io.pwrite_native(std::move(slice), rem, off);
		if (ret < 0) {
			perror("pwrite_native");
			exit(1);
		} else if (!ret) {
			fprintf(stderr, "pwrite_native: nothing was written\n");
			exit(1);
		}

		assert(rem >= (size_t)ret);
		off += ret;
		rem -= ret;
	}

	udepot::Mbuff mb = create_mbuff<IO>(buff_size, nbuffs);

	int err;
	size_t bytes_read;
	std::tie(err, bytes_read) = udepot::io_pread_mbuff_append_full(io, mb, total_size, 0);
	if (err) {
		fprintf(stderr, "io_pread_mbuff_append_full() returned err: %s (%d)\n", strerror(err), err);
		exit(1);
	}
	assert(bytes_read == total_size);

	int check = mb.mem_compare_buffer(0, (char *)buff.ptr_m, total_size);
	if (check != 0) {
		fprintf(stderr, "ERROR: mem_compare failed\n");
		exit(1);
	}

	mb.reset(
		[] (void *ptr) { IO::free_iobuff(typename IO::Ptr(ptr, 0)); }
	);

	unlink(fname);

	printf("%s: Everything looks OK!\n", __FILE__);
	return 0;
}

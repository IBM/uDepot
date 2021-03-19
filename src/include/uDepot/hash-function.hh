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

#ifndef _UDEPOT_HASH_FUNCTION_HH_
#define _UDEPOT_HASH_FUNCTION_HH_

#include <cinttypes>

#include "uDepot/mbuff.hh"

#include "cityhash/src/city.h"
#include "util/debug.h"

#include "util/wyhash.h"
#include "util/xxhash.h"

//#define	_UDEPOT_HASH_FN(key, len)	CityHash64(key, len)
// #include "MurmurHash2.hh"
// #define	_UDEPOT_HASH_FN(key, len)	MurmurHash64A((void *) key, (int) len, 0x1F0D3804)
// #define	_UDEPOT_HASH_FN(key, len)	(*((u64 *) (key)))

namespace udepot {

class HashFnCity;
class HashFnWyhash;
class HashFnXXH;
//class HashFnMurmur2;

using HashFn = HashFnCity;
  //using HashFn = HashFnXXH;
  //using HashFn = HashFnWyhash;

struct HashFnCity {

	static inline uint64_t hash(const char *s, size_t len) {
		return CityHash64(s, len);
	}

	static inline int hash_copy(uint64_t &ret, Mbuff const& mb, size_t off, size_t len) {
		char *buff = (char *)std::malloc(len);
		if (!buff)
			return ENOMEM;

		int err = 0;
		size_t copied = mb.copy_to_buffer(off, buff, len);
		if (copied != len) {
			UDEPOT_ERR("copy_to_buffer() copied: %zd instead of %zd. This should not happen\n", copied, len);
			err = EINVAL;
		} else {
			ret = hash(buff, len);
		}

		std::free(buff);
		return err;
	}

	static inline int hash(uint64_t &ret, Mbuff const& mb, size_t off, size_t len) {

		Mbuff::Iterator iter;
		mb.iterate_valid_init(iter, off, len);
		if (!iter.is_valid()) {
			ret = CityHash64(NULL, 0);
			return 0;
		}

		if (iter.is_next_valid()) {
			// city hash does not support incremental hash computation, so if we
			// want to use it we will need to do a copy. MurmurHash also does
			// not seem to support incremental hash computation. Spooky does,
			// however, so this might be our best bet if we want to avoid
			// copying.
			UDEPOT_ERR("Cannot compute city hash on mbuff with >1 buffers.  Copying mbuff\n");
			iter.stop();
			return hash_copy(ret, mb, off, len);
		}

		Mbuff::Chunk const& c = iter.get_chunk();
		assert(c.is_valid());
		ret = hash((const char *)c.getConstPtr(), c.len);
		iter.stop();
		return 0;
	}

	static inline int hash(uint64_t &ret, Mbuff const& mb) {
		return hash(ret, mb, 0, mb.get_valid_size());
	}
};

struct HashFnWyhash {

	static inline uint64_t hash(const char *s, size_t len) {
		return wyhash(s, len, 0, _wyp);
	}

	static inline int hash_copy(uint64_t &ret, Mbuff const& mb, size_t off, size_t len) {
		char *buff = (char *)std::malloc(len);
		if (!buff)
			return ENOMEM;

		int err = 0;
		size_t copied = mb.copy_to_buffer(off, buff, len);
		if (copied != len) {
			UDEPOT_ERR("copy_to_buffer() copied: %zd instead of %zd. This should not happen\n", copied, len);
			err = EINVAL;
		} else {
			ret = hash(buff, len);
		}

		std::free(buff);
		return err;
	}

	static inline int hash(uint64_t &ret, Mbuff const& mb, size_t off, size_t len) {

		Mbuff::Iterator iter;
		mb.iterate_valid_init(iter, off, len);
		if (!iter.is_valid()) {
			ret = CityHash64(NULL, 0);
			return 0;
		}

		if (iter.is_next_valid()) {
			// city hash does not support incremental hash computation, so if we
			// want to use it we will need to do a copy. MurmurHash also does
			// not seem to support incremental hash computation. Spooky does,
			// however, so this might be our best bet if we want to avoid
			// copying.
			UDEPOT_ERR("Cannot compute city hash on mbuff with >1 buffers.  Copying mbuff\n");
			iter.stop();
			return hash_copy(ret, mb, off, len);
		}

		Mbuff::Chunk const& c = iter.get_chunk();
		assert(c.is_valid());
		ret = hash((const char *)c.getConstPtr(), c.len);
		iter.stop();
		return 0;
	}

	static inline int hash(uint64_t &ret, Mbuff const& mb) {
		return hash(ret, mb, 0, mb.get_valid_size());
	}
};

  struct HashFnXXH {

	static inline uint64_t hash(const char *s, size_t len) {
	  return XXH3_64bits(s, len);
		     //return wyhash(s, len, 0, _wyp);
	}

	static inline int hash_copy(uint64_t &ret, Mbuff const& mb, size_t off, size_t len) {
		char *buff = (char *)std::malloc(len);
		if (!buff)
			return ENOMEM;

		int err = 0;
		size_t copied = mb.copy_to_buffer(off, buff, len);
		if (copied != len) {
			UDEPOT_ERR("copy_to_buffer() copied: %zd instead of %zd. This should not happen\n", copied, len);
			err = EINVAL;
		} else {
			ret = hash(buff, len);
		}

		std::free(buff);
		return err;
	}

	static inline int hash(uint64_t &ret, Mbuff const& mb, size_t off, size_t len) {

		Mbuff::Iterator iter;
		mb.iterate_valid_init(iter, off, len);
		if (!iter.is_valid()) {
			ret = CityHash64(NULL, 0);
			return 0;
		}

		if (iter.is_next_valid()) {
			// city hash does not support incremental hash computation, so if we
			// want to use it we will need to do a copy. MurmurHash also does
			// not seem to support incremental hash computation. Spooky does,
			// however, so this might be our best bet if we want to avoid
			// copying.
			UDEPOT_ERR("Cannot compute city hash on mbuff with >1 buffers.  Copying mbuff\n");
			iter.stop();
			return hash_copy(ret, mb, off, len);
		}

		Mbuff::Chunk const& c = iter.get_chunk();
		assert(c.is_valid());
		ret = hash((const char *)c.getConstPtr(), c.len);
		iter.stop();
		return 0;
	}

	static inline int hash(uint64_t &ret, Mbuff const& mb) {
		return hash(ret, mb, 0, mb.get_valid_size());
	}
};

} // end namespace udepot

#endif /* _UDEPOT_HASH_FUNCTION_HH_ */

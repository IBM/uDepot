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

 #ifndef _INLINE_CACHE_HH_
 #define _INLINE_CACHE_HH_

#include <boost/intrusive/list.hpp>
namespace bi = boost::intrusive;

struct FreeObj {
	bi::list_member_hook<>  lnode_;
};


template<typename T, size_t I>
class InlineObjCache {
public:
	InlineObjCache(): inline_allocated_(0) , heap_allocated_(0) {}

	// allocate a new object
	T *get() {
		// objects exist in the free list, use them
		if (!free_nodes_.empty()) {
			FreeObj *o = &free_nodes_.front();
			free_nodes_.pop_front();
			return static_cast<T *>(static_cast<void *>(o));
		}
		// objects exist in the inline cache, use them
		if (inline_allocated_ < I) {
			return &inline_cache_[inline_allocated_++];
		}
		// allocate a new object
		T *ret = new T;
		heap_allocated_++;
		return ret;
	}

	// put object back into cache
	void put(T *t) {
		FreeObj *o = static_cast<FreeObj *>(static_cast<void *>(t));
		new (o) FreeObj; // call constructor to initialize lndoe
		free_nodes_.push_front(*o);
	}

	InlineObjCache(InlineObjCache const&) = delete;
	void operator=(InlineObjCache const&) = delete;

	void reset(void) {
		while (free_nodes_.size() != 0) {
			FreeObj *o = &free_nodes_.front();
			free_nodes_.pop_front();
			T *obj = static_cast<T *>(static_cast<void *>(o));
			if (!is_inline_obj(obj)) {
				delete obj;
				heap_allocated_--;
			} else {
				inline_allocated_--;
			}
		}
		assert(heap_allocated_ == 0 && "Not all objects were returned to the cache");
		assert(inline_allocated_ == 0 && "Not all objects were returned to the cache");
	}

	~InlineObjCache() {
		reset();
	}

private:
    using FreeList = bi::list<FreeObj, bi::member_hook<FreeObj, bi::list_member_hook<>, &FreeObj::lnode_>>;
	static_assert(sizeof(T) >= sizeof(FreeObj), "Too small object");

	size_t            inline_allocated_;
	FreeList          free_nodes_;
	size_t            heap_allocated_;
	T                 inline_cache_[I];

public:
	#if 0
	InlineObjCache(InlineObjCache &&o)
		: inline_allocated_(std::move(o.inline_allocated_))
		, free_nodes_(std::move(o.free_nodes_))
		, heap_allocated_(std::move(o.heap_allocated_))
		, inline_cache_(std::move(o.inline_cache_))
		{}
	#endif

private:

	bool is_inline_obj(T *o) {
		if (o < &inline_cache_[0] || o > &inline_cache_[I-1])
			return false;
		else
			return true;
	}
};


 #endif /* _INLINE_CACHE_HH_ */

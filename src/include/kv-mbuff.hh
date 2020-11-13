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

#ifndef _KV_MBUFF_HH_
#define _KV_MBUFF_HH_

#include "uDepot/mbuff.hh"
#include "uDepot/mbuff-alloc.hh"
#include "util/debug.h"

#include "kv.hh"

using Mbuff = udepot::Mbuff;

// Interface for avoiding copies, between users and the KV store.
//
// It inherits from MbuffAllocIface for providing the caller with functions for
// allocating Mbuffs.
class KV_MbuffInterface : public virtual udepot::MbuffAllocIface {
public:
	enum PutOp { NORMAL, REPLACE, CREATE };

	virtual int get(Mbuff const& key, Mbuff &val_out)  = 0;
	// NB: keyval cannot be const& because put() needs to modify it to insert
	// a metadata prefix and/or suffix.
	virtual int put(Mbuff &keyval, size_t key_len, PutOp op = NORMAL)  = 0;
	virtual int del(Mbuff const& key)  = 0;

	virtual size_t get_kvmbuff_size() = 0;
	// Optimization to avoid copy for PUTs when using IO operations that have
	// buffer restrictions (e.g., direct IO, SPDK).
	//
	// We assume (as is the case for uDepotSalsa) that the each key value pair
	// is stored on the IO device as:
	//   [MD PREFIX][KEY][VALUE][MD SUFFIX]
	// The two functions below provide hints to the network server to properly
	// align/arrange the passed Mbuff so that no copy is required.
	virtual size_t putKeyvalPrefixSize() const = 0;
	virtual size_t putKeyvalSuffixSize() const = 0;

	virtual ~KV_MbuffInterface() {};
};


// KV_MbuffInterface dummy implementation using a KV (will perform copies)
//  This implementation is inneficient. It is meant to act as transition
//  wrapper. All stores should eventually implement the Mbuff interface.
class KV_Mbuff : KV_MbuffInterface {
public:
	KV_Mbuff(KV &kv) : kv_m(kv) {}

	virtual int get(Mbuff const& key_mbuff, Mbuff &val_out_mbuff) override {
		int err;
		char *key = nullptr;
		char *val = nullptr;
		size_t val_buff_size = 1024*1024;
		size_t key_size, val_size, key_cp __attribute__((unused)),
			x __attribute__((unused));

		// val_out should not have any valid data
		if (val_out_mbuff.get_valid_size() != 0) {
			err = EINVAL;
			goto end;
		}

		key_size = key_mbuff.get_valid_size();
		key = (char *)std::malloc(key_size);
		if (!key) {
			err = ENOMEM;
			goto end;
		}

		key_cp = key_mbuff.copy_to_buffer(0, key, key_size);
		assert(key_cp == key_size);

		val = (char *)std::malloc(val_buff_size);
		if (!val) {
			err = ENOMEM;
			goto end;
		}

		err = get_realloc(key, key_size, &val, val_buff_size, val_size);
		if (err)
			goto end;
		assert(val_size <= val_buff_size);

		mbuff_add_buffers(val_out_mbuff, val_size);
		if (val_out_mbuff.get_free_size() < val_size) {
			err = ENOMEM;
			goto end;
		}

		x = val_out_mbuff.append_from_buffer(val, val_size);
		assert(x == val_size);
		err = 0;

	end:
		if (val) std::free(val);
		if (key) std::free(key);
		return err;
	}

	virtual size_t putKeyvalPrefixSize() const override { return 0; }
	virtual size_t putKeyvalSuffixSize() const override { return 0; }
	virtual int put(Mbuff &keyval, size_t key_len, PutOp op) override {
		char *key = nullptr;
		char *val = nullptr;
		int err;

		assert(keyval.get_valid_size() >= key_len);
		size_t val_len = keyval.get_valid_size() - key_len;
		key = static_cast<char *>(std::malloc(key_len));
		val = static_cast<char *>(std::malloc(val_len));
		if (!key || !val) {
			err = ENOMEM;
			goto end;
		}

		size_t copied;
		copied = keyval.copy_to_buffer(0, key, key_len);
		if (copied != key_len) {
			UDEPOT_ERR("copied (%zd) =/= key_len (%zd)\n",  copied, key_len);
			abort();
		}

		copied = keyval.copy_to_buffer(key_len, val, val_len);
		if (copied != val_len) {
			UDEPOT_ERR("copied (%zd) =/= val_len (%zd)\n",  copied, val_len);
			abort();
		}

		err = kv_m.put(key, key_len, val, val_len);

	end:
		if (val) std::free(val);
		if (key) std::free(key);
		return err;
	}

	virtual int del(Mbuff const& key) override {
		UDEPOT_ERR("%s:%d: NYI!", __PRETTY_FUNCTION__, __LINE__);
		abort();
	}

	std::type_index mbuff_type_index(void) override final {
		return std::type_index(typeid(unsigned char *));
	}

	virtual Mbuff mbuff_alloc(size_t s) override {
		Mbuff mb(mbuff_type_index());
		unsigned char *b = static_cast<unsigned char *>(std::malloc(s));
		if (b == nullptr) {
			assert(mb.get_free_size() == 0);
			return mb;
		}
		mb.add_buff(b, s);
		assert(mb.get_free_size() == s);
		return mb;
	}

	virtual void mbuff_free_buffers(Mbuff &mb) override {
		mb.reset(std::free);
		assert(mb.get_free_size() == 0);
	}

	virtual void mbuff_add_buffers(Mbuff &mb, size_t new_free_size) override {
		size_t free_size = mb.get_free_size();
		if (free_size >= new_free_size)
			return;

		size_t b_len = new_free_size - free_size;
		unsigned char *b = static_cast<unsigned char *>(std::malloc(b_len));
		if (b == nullptr)
				return;

		mb.add_buff(b, b_len);
		assert(mb.get_free_size() == new_free_size);
		return;
	}
private:
	int get_realloc(const char *key, size_t key_size,
	                char **val_buff_ptr, size_t &val_buff_size,
	                size_t &val_size) {
		int err;
		size_t val_size_read;

		err = kv_m.get(key, key_size, *val_buff_ptr, val_buff_size, val_size_read, val_size);
		if (err)
			return err;
		if (val_size <= val_buff_size)
			return 0;

		*val_buff_ptr = (char *)std::realloc((void *)(*val_buff_ptr), val_size);
		if (*val_buff_ptr == nullptr)
			return ENOMEM;
		val_buff_size = val_size;

		err = kv_m.get(key, key_size, *val_buff_ptr, val_buff_size, val_size_read, val_size);
		if (err) {
			UDEPOT_MSG("get() returned: %s (%d)", strerror(err), err);
		}
		return err;
	}

	KV_Mbuff() = delete;
	KV_Mbuff(KV_Mbuff const&) = delete;
	void operator=(KV_Mbuff const&) = delete;

	KV &kv_m;
};

#endif /* ifndef _KV_MBUFF_HH_ */

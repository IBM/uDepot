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

#include <arpa/inet.h>
#include <cassert>
#include <cstdio>
#include <iostream>
#include <pthread.h> // pthread_barrier_t
#include <stdlib.h>
#include <string.h>
#include <string>
#include <system_error>
#include <thread>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <unordered_set>
#include <vector>
#if defined(UDEPOT_TRT_SPDK)
#include <rte_config.h>
#include <rte_lcore.h>
#endif

#include "util/debug.h"
#include "kv.hh"
#include "timer.h"
#include "util/types.h"
#include "uDepot/backend.hh"
#include "uDepot/kv-conf.hh"
#include "uDepot/kv-factory.hh"
#include "uDepot/net/socket.hh"
#include "uDepot/uDepot.hh"
#include "uDepot/udepot-lsa.hh"

#include "trt/uapi/trt.hh"
#include "trt_backends/trt_aio.hh"
#include "trt_backends/trt_uring.hh"
#include "trt_util/arg_pool.hh"

using namespace udepot;
namespace udepot {
const static u32 thread_max_g = std::thread::hardware_concurrency() * 16;

static pthread_barrier_t barrier_g;

struct thread_arg;
template<typename RT>
using test_fn = std::function<int (uDepotSalsa<RT> *, u32, u64, u64)>;

constexpr u64 prime_g = 2654435761UL;
struct udepot_test_conf : public KV_conf {

    // default values
    const size_t TRT_NTASKS = 32;

    udepot_test_conf()
	: KV_conf(),
	  read_nr_m(1),
	  write_nr_m(1),
	  local_ip_m("127.0.0.1"),
	  thin_test_m(false),
	  del_test_m(false),
	  gc_test_m(false),
	  zerocopy_m(false),
	  run_unit_test_m(false),
	  run_crash_unit_test_m(false),
	  seed_m(1),
	  kv_val_size_m(3072),
	  trt_ntasks_m(TRT_NTASKS) {}

	~udepot_test_conf() {};
	u64 read_nr_m, write_nr_m;
	std::string local_ip_m;
	bool thin_test_m, del_test_m, gc_test_m, zerocopy_m, run_unit_test_m, run_crash_unit_test_m;
	u64 seed_m;
	u32 kv_val_size_m;
	size_t trt_ntasks_m;

	int parse_args(ParseArgs &args) override final;
	void print_usage(const char []) override final;
	void validate_and_sanitize_parameters() override final;
	bool is_server_mode(void);
};

static std::function<void (thread_arg *)> threadfn();
template<typename RT>
static test_fn<RT> putfn();
template<typename RT>
static test_fn<RT> getfn();
template<typename RT>
static test_fn<RT> delfn();
template<typename RT>
static test_fn<RT> putrndfn();

static udepot_test_conf conf_g;
std::atomic<bool> test_failure_g(false);

struct test_key_value {
	union {
		u64 key;
		char keyb[32];
	};
	char val[];
};

static test_key_value* kvs_g;

__attribute__((optimize("unroll-loops")))
static int create_test_data()
{
	sockaddr_in addr;
	int rc = inet_pton(AF_INET, conf_g.local_ip_m.c_str(), &addr.sin_addr);
	if (1 != rc) {
		UDEPOT_ERR("inet_pton failed with %d ip=%s\n", rc, conf_g.local_ip_m.c_str());
		return EINVAL;
	}
	UDEPOT_DBG("my ip=%u", addr.sin_addr.s_addr);
	if (conf_g.thin_test_m) {
		conf_g.seed_m = static_cast<u32>(addr.sin_addr.s_addr);
		return 0;
	}

	const u64 tkv_size = std::max(conf_g.write_nr_m, conf_g.read_nr_m);
	const u64 kvs_size = sizeof(*kvs_g) + conf_g.kv_val_size_m;
	kvs_g = static_cast<test_key_value *>(malloc(kvs_size * tkv_size));
	if (NULL == kvs_g)
		return ENOMEM;
	const u64 key_high = ((static_cast<u64>(addr.sin_addr.s_addr)) << 32);
	std::unordered_set<u64> keys(tkv_size);
	for (u64 i = 0; i < tkv_size; ++i) {
		std::unordered_set<u64>::const_iterator exists;
		test_key_value *const tkv = &kvs_g[i];
		do {
			const u32 key_low = lrand48();
			tkv->key = key_high | key_low;
			UDEPOT_DBG("high|low=0x%lx high=0x%lx low=0x%lx.\n", key_high | key_low, key_high, (u64) key_low);
			exists = keys.find(tkv->key);
		} while ((tkv->key && exists != keys.end()));
		keys.insert(tkv->key);
		u32 *val = reinterpret_cast<u32 *>(tkv->val);
		u32 *const val_end = reinterpret_cast<u32 *>(tkv->val) + conf_g.kv_val_size_m / sizeof(*val);
		for (; val < val_end; ++val)
			*val = lrand48();
	}
	return 0;
}

static void destroy_test_data()
{
	assert(nullptr != kvs_g);
	free(kvs_g);
}

template<typename RT>
__attribute__((optimize("unroll-loops")))
static int put_test(uDepotSalsa<RT> *const KV, const u32 tid, const u64 start, const u64 end)
{
	const test_key_value *const tkv_start = &kvs_g[start];
	const test_key_value *const tkv_end   = &kvs_g[end];
	UDEPOT_DBG("start=%p (%llu) end=%p (%llu)", tkv_start, (unsigned long long)start, tkv_end, (unsigned long long)end);
	for (const test_key_value *tkv = tkv_start; tkv < tkv_end; ++tkv) {
		const int err = KV->put(reinterpret_cast<const char *>(&tkv->key), sizeof(tkv->key), tkv->val, conf_g.kv_val_size_m);
		if (0 != err && EEXIST != err) {
			UDEPOT_ERR("put returned %d tid=%u key=0x%16lx.", err, tid, tkv->key);
			return err;
		}
	}
	return 0;
}

template<typename RT>
__attribute__((optimize("unroll-loops")))
static int put_rnd_test(uDepotSalsa<RT> *const KV, const u32 tid, const u64 start, const u64 end)
{
	for (u64 i = start; i < end; ++i) {
		const u32 idx = lrand48() % (end - start);
		const test_key_value *tkv = &kvs_g[idx];
		const int err = KV->put(reinterpret_cast<const char *>(&tkv->key), sizeof(tkv->key), tkv->val, conf_g.kv_val_size_m);
		if (0 != err && EEXIST != err) {
			UDEPOT_ERR("put returned %d tid=%u key=0x%16lx.", err, tid, tkv->key);
			return err;
		}
	}
	return 0;
}

template<typename RT>
__attribute__((optimize("unroll-loops")))
static int get_test(uDepotSalsa<RT> *const KV, const u32 tid, const u64 start, const u64 end)
{
	const test_key_value *const tkv_start = &kvs_g[start];
	const test_key_value *const tkv_end   = &kvs_g[end];
	char *const val_out = (char *) malloc(conf_g.kv_val_size_m);
	if (nullptr == val_out)
		return ENOMEM;
	for (const test_key_value *tkv = tkv_start; tkv < tkv_end; ++tkv) {
		size_t val_size_read, val_size;
		const int err = KV->get(
			reinterpret_cast<const char *>(&tkv->key),sizeof(tkv->key),
			val_out, conf_g.kv_val_size_m,
			val_size_read, val_size);
		if (0 != err) {
			UDEPOT_ERR("get returned %d (%s).", err, strerror(err));
			abort();
		}
		assert(val_size_read == conf_g.kv_val_size_m);
		assert(val_size == conf_g.kv_val_size_m);
		assert(0 == memcmp(val_out, tkv->val, conf_g.kv_val_size_m));
	}
	free(val_out);
	return 0;
}

template<typename RT>
__attribute__((optimize("unroll-loops")))
static int del_test(uDepotSalsa<RT> *const KV, const u32 tid, const u64 start, const u64 end)
{
	const test_key_value *const tkv_start = &kvs_g[start];
	const test_key_value *const tkv_end   = &kvs_g[end];
	for (const test_key_value *tkv = tkv_start; tkv < tkv_end; ++tkv) {
		const int err = KV->del(reinterpret_cast<const char *>(&tkv->key), sizeof(tkv->key));
		if (0 != err) {
			UDEPOT_ERR("put returned %d tid=%u key=0x%16lx.", err, tid, tkv->key);
			return err;
		}
	}
	for (const test_key_value *tkv = tkv_start; tkv < tkv_end && tkv < tkv_start + 1; ++tkv) {
		const int err = KV->del(reinterpret_cast<const char *>(&tkv->key), sizeof(tkv->key));
		if (ENODATA != err) {
			UDEPOT_ERR("put returned %d tid=%u key=0x%16lx.", err, tid, tkv->key);
			return err;
		}
	}
	// assert(0 == static_cast<::KV*>(KV)->get_size());
	return 0;
}

template<typename RT>
__attribute__((optimize("unroll-loops")))
static int put_test_thin(uDepotSalsa<RT> *const KV, const u32 tid, const u64 start, const u64 end)
{
	//printf("tid: %lu start: %lu end: %lu\n", (unsigned long)tid, (unsigned long)start, (unsigned long) end);
	char keyb[32] = { 0 };
	// char val[conf_g.kv_val_size_m];
	char *const val = (char *) malloc(conf_g.kv_val_size_m);
	if (nullptr == val)
		return ENOMEM;
	memset(val, 0, conf_g.kv_val_size_m);
	for (u64 i = start; i < end; ++i) {
		const u64 key = ((conf_g.seed_m + i) * prime_g);
		const u64 valu = key * prime_g;
		const u64 key_size = 8 + (key % 24);
		const u64 val_size = conf_g.kv_val_size_m; //8 + (valu % (conf_g.kv_val_size_m - 8 + 1));
		memcpy(val, &valu, sizeof(key));
		memcpy(keyb, &key, sizeof(key));
		const int err = KV->put(keyb, key_size, val, val_size);
		if (0 != err && EEXIST != err) {
			UDEPOT_ERR("put returned %d tid=%u key=0x%16lx.", err, tid, key);
			assert(0);
			return err;
		}
	}
	free(val);
	return 0;
}

template<typename RT>
__attribute__((optimize("unroll-loops")))
static int get_test_thin(uDepotSalsa<RT> *const KV, const u32 tid, const u64 start, const u64 end)
{
	char keyb[32] = { 0 };
	// char val_out[conf_g.kv_val_size_m];
	char *const val_out = (char *) malloc(conf_g.kv_val_size_m);
	if (nullptr == val_out)
		return ENOMEM;
	// const u64 diff = 0 < end - start ? end - start : 1;
	// u64 q = prime_g % diff;
	// for (u64 i = start; i < end; ++i, q = (q + prime_g) % (diff)) {
	// 	const u64 key = (start + conf_g.seed_m + q) * prime_g;
	for (u64 i = start; i < end; ++i) {
		const u64 key = ((conf_g.seed_m + i) * prime_g);
		const u64 valu = key * prime_g;
		const u64 key_size = 8 + (key % 24);
		memcpy(keyb, &key, sizeof(key));
		size_t val_size_read, val_size;
		const int err = KV->get(keyb, key_size, val_out, conf_g.kv_val_size_m, val_size_read, val_size);
		u64 val_ret;
		memcpy(&val_ret, val_out, sizeof(val_ret));

		if (0 != err || val_ret != valu) {
			UDEPOT_ERR("get returned %d vale=%lu valret=%lu.", err, valu, val_ret);
			assert(0); // error
			return err;
		}
		// const u64 val_size = 8 + (valu % (conf_g.kv_val_size_m - 8 + 1));
		// assert(val_size == 8 + (valu % (conf_g.kv_val_size_m - 8 + 1)));
		assert(val_size_read == val_size);
	}
	free(val_out);
	return 0;
}

template<typename RT>
__attribute__((optimize("unroll-loops"), unused))
static int put_test_thin_mbuff(uDepotSalsa<RT> *const KV, const u32 tid, const u64 start, const u64 end)
{
	UDEPOT_DBG("tid: %lu start: %lu end: %lu\n", (unsigned long)tid, (unsigned long)start, (unsigned long) end);
	const size_t prefix_size = KV->putKeyvalPrefixSize();
	const size_t suffix_size = KV->putKeyvalSuffixSize();
	Mbuff mb = KV->mbuff_alloc(32 + conf_g.kv_val_size_m + prefix_size + suffix_size);

	for (u64 i = start; i < end; ++i) {
		const u64 key = ((conf_g.seed_m + i) * prime_g);
		const u64 valu = key * prime_g;
		const u64 key_size = 8 + (key % 24);
		const u64 val_size = conf_g.kv_val_size_m; //8 + (valu % (conf_g.kv_val_size_m - 8 + 1));
		UDEPOT_DBG("i=%lx key=%lx key_size=%lu", i, key, key_size);
		mb.reslice(0);
		auto append_fn =
			[&prefix_size, &key, &key_size, &valu, &val_size]
			(unsigned char *b, size_t b_len) -> size_t {
				assert(b_len > 32 + conf_g.kv_val_size_m + prefix_size); // assume a single buffer
				// skip prefix
				b += prefix_size;
				// write key
				*((u64 *)b) = key;
				b += 8;
				for (size_t j=0; j < key_size - 8; j++) {
					*b = 0;
					b++;
				}
				// write val (only the first 8 bytes)
				*((u64 *)b) = valu;

				return prefix_size + key_size + val_size;
			};

		mb.append(std::ref(append_fn));
		assert(mb.get_valid_size() == prefix_size + key_size + val_size);
		mb.reslice(key_size + val_size /* len */, prefix_size /* offset */);
		const int err = KV->put(mb, key_size);
		if (0 != err && EEXIST != err) {
			UDEPOT_ERR("put returned %d tid=%u key=0x%16lx.", err, tid, key);
			assert(0);
			return err;
		}
		// assert(mb.get_free_size() + mb.get_valid_size() == align_up(conf_g.kv_val_size_m + 32 + prefix_size, 4096));
		// assert(mb.get_valid_size() == align_up(val_size + key_size + prefix_size, 4096));
	}
	// assert(mb.get_free_size() + mb.get_valid_size() == align_up(conf_g.kv_val_size_m + 32 + prefix_size, 4096));
	KV->mbuff_free_buffers(mb);
	return 0;
}

template<typename RT>
__attribute__((optimize("unroll-loops"), unused))
static int get_test_thin_mbuff(uDepotSalsa<RT> *const KV, const u32 tid, const u64 start, const u64 end)
{
	int err = 0;
	const size_t prefix_size = KV->putKeyvalPrefixSize();
	Mbuff mb = KV->mbuff_alloc(32 + conf_g.kv_val_size_m + prefix_size);
	assert(mb.get_free_size() == align_up(32 + conf_g.kv_val_size_m + prefix_size, 4096));
	Mbuff keymb = KV->mbuff_alloc(32);
	assert(keymb.get_free_size() == align_up(32, 4096));
	keymb.append_zero(32);
	RT::Sched::yield(); // This targets trt, so that all tasks allocate their buffer first and then issue their operations
	const u64 diff = 0 < end - start ? end - start : 1;
	u64 q = prime_g % diff;
	for (u64 i = start; i < end; ++i, q = (q + prime_g) % diff) {
		const u64 key = (start + conf_g.seed_m + q) * prime_g;
	// for (u64 i = start; i < end; ++i) {
	// 	const u64 key = ((conf_g.seed_m + i) * prime_g);
		const u64 valu = key * prime_g;
		const u64 key_size = 8 + (key % 24);
		// const u64 val_size = conf_g.kv_val_size_m;
		UDEPOT_DBG("i=%lx key=%lx key_size=%lu", i, key, key_size);
		keymb.reslice(0);
		keymb.append(
			[&key, &key_size]
			(unsigned char *b, size_t b_len) -> size_t {
				assert(b_len > 32); // assume a single buffer
				// write key
				*((u64 *)b) = key;
				b += 8;
				for (size_t j = 0; j < key_size - 8; j++) {
					*b = 0;
					b++;
				}
				return key_size;
			}
		);
		assert(keymb.get_valid_size() == key_size);

		mb.reslice(0);
		err = KV->get(keymb, mb);
		const u64 val_ret = 0 == err ? mb.read_val<u64>(0) : 0;
		if (0 != err || val_ret != valu) {
			UDEPOT_ERR("get returned %d vale=%lu valret=%lu.", err, valu, val_ret);
			assert(0); // error
			break;
		}
		assert(mb.get_valid_size() == conf_g.kv_val_size_m);
	}
	mb.reslice(0);
	assert(mb.get_free_size() + mb.get_valid_size() == align_up(conf_g.kv_val_size_m + 32 + prefix_size, 4096));
	KV->mbuff_free_buffers(mb);
	keymb.reslice(0);
	assert(keymb.get_free_size() + keymb.get_valid_size() == align_up(32, 4096));
	KV->mbuff_free_buffers(keymb);
	return 0;
}

template<typename RT>
__attribute__((optimize("unroll-loops")))
static int del_test_thin(uDepotSalsa<RT> *const KV, const u32 tid, const u64 start, const u64 end)
{
	char keyb[32] = { 0 };
	for (u64 i = start; i < end; ++i) {
		const u64 key = ((conf_g.seed_m + i) * prime_g);
		const u64 key_size = 8 + (key % 24);
		memcpy(keyb, &key, sizeof(key));
		const int err = KV->del(keyb, key_size);
		if (0 != err) {
			UDEPOT_ERR("get returned %d.", err);
			assert(0); // error
			return err;
		}
	}
	// make sure that enodata is returned on invalid keys
	for (u64 i = start; i < end && i < start + 1; ++i) {
		const u64 key = ((conf_g.seed_m + i) * prime_g);
		const u64 key_size = 8 + (key % 24);
		memcpy(keyb, &key, sizeof(key));
		const int err = KV->del(keyb, key_size);
		if (ENODATA != err) {
			UDEPOT_ERR("get returned %d.", err);
			assert(0); // error
			return err;
		}
	}
	// assert(0 == static_cast<::KV*>(KV)->get_size());
	return 0;
}

template<typename RT>
__attribute__((optimize("unroll-loops")))
static int del_rnd_test_thin(uDepotSalsa<RT> *const KV, const u32 tid, const u64 start, const u64 end)
{
	char keyb[32] = { 0 };
	u64 q = prime_g % (end - start);
	for (u64 i = start + conf_g.seed_m; i < end + conf_g.seed_m; ++i) {
		const u64 key = (start + conf_g.seed_m + q) * prime_g;
		const u64 key_size = 8 + (key % 24);
		memcpy(keyb, &key, sizeof(key));
		const int err = KV->del(keyb, key_size);
		if (0 != err) {
			UDEPOT_ERR("del returned %d.", err);
			assert(0); // error
			return err;
		}
		q = (q + prime_g) % (end - start);
	}
	for (u64 i = start + conf_g.seed_m; i < end + conf_g.seed_m && i < start + conf_g.seed_m + 1; ++i) {
		const u64 key = (start + conf_g.seed_m + q) * prime_g;
		const u64 key_size = 8 + (key % 24);
		memcpy(keyb, &key, sizeof(key));
		const int err = KV->del(keyb, key_size);
		if (ENODATA != err) {
			UDEPOT_ERR("del returned %d.", err);
			assert(0); // error
			return err;
		}
		q = (q + prime_g) % (end - start);
	}
	return 0;
}

struct workload_part {
	u64 start;
	u64 len;

	workload_part() : start(-1), len(-1) {}

	workload_part(u64 total, u32 nthreads, u32 thread_id) {
		len = total / nthreads;
		start = thread_id*len;
		if (thread_id == nthreads - 1)
			len += total % nthreads;
	}

	workload_part(workload_part const& wp, u32 nthreads, u32 thread_id) {
		len = wp.len / nthreads;
		start = wp.start + (thread_id*len);
		if (thread_id == nthreads - 1)
			len += wp.len % nthreads;
	}
};

struct thread_arg {
	::KV *KV;
	u32 tid;
	workload_part read_p, write_p;

	struct timespec wr_start;
	struct timespec wr_finish;
	struct timespec rd_start;
	struct timespec rd_finish;
};

// sync test start
constexpr u64 BIL = 1000000000LL;
template<typename RT>
static void test_thread(thread_arg *const arg)
{
	struct timespec before;
	struct timespec after;
	s64 __attribute__((unused)) nsecs_reads, __attribute__((unused)) nsecs_writes;
	int err;
	const u32 tid = arg->tid;
	uDepotSalsa<RT> *const KV = (uDepotSalsa<RT> *) arg->KV;
	const test_fn<RT> put_fn = putfn<RT>();
	const test_fn<RT> get_fn = getfn<RT>();
	const test_fn<RT> del_fn = delfn<RT>();

	KV->thread_local_entry();

	pthread_barrier_wait(&barrier_g); // PUTs start
	UDEPOT_DBG("Thread %u starting test roffset=%lu reads=%lu woffset=%lu writes=%lu.\n",
		tid, arg->read_p.start, arg->read_p.len, arg->write_p.start, arg->write_p.len);
	clock_gettime(CLOCK_MONOTONIC, &before);
	err = put_fn(KV, tid, arg->write_p.start, arg->write_p.start + arg->write_p.len);
	clock_gettime(CLOCK_MONOTONIC, &after);
	if (0 != err) {
		test_failure_g = true;
		UDEPOT_ERR("udepot put test ret w/%d.", err);
		pthread_barrier_wait(&barrier_g); // PUTs end
		pthread_barrier_wait(&barrier_g); // GETs start
		pthread_barrier_wait(&barrier_g); // GETs end
		goto fail0;
	}
	nsecs_writes = BIL * (after.tv_sec - before.tv_sec) + after.tv_nsec - before.tv_nsec;
	UDEPOT_DBG("Writes thread=%u secs before=%lu secs after=%lu nsecs=%lu.", tid, before.tv_sec, after.tv_sec, nsecs_writes);
	pthread_barrier_wait(&barrier_g); // PUTs end

	pthread_barrier_wait(&barrier_g); // GETs start
	arg->wr_start = before;
	arg->wr_finish = after;
	clock_gettime(CLOCK_MONOTONIC, &before);
	err = get_fn(KV, tid, arg->read_p.start, arg->read_p.start + arg->read_p.len);
	clock_gettime(CLOCK_MONOTONIC, &after);
	if (0 != err) {
		test_failure_g = true;
		UDEPOT_ERR("udepot get test ret w/%d.", err);
		pthread_barrier_wait(&barrier_g); // GETs end
		goto fail0;
	}
	pthread_barrier_wait(&barrier_g); // GETs end

	if (conf_g.del_test_m) {
		pthread_barrier_wait(&barrier_g); // DELs start
		err = del_fn(KV, tid, arg->write_p.start, arg->write_p.start + arg->write_p.len);
		if (0 != err) {
			test_failure_g = true;
			UDEPOT_ERR("udepot del test ret w/%d.", err);
			pthread_barrier_wait(&barrier_g); // DELs end
			goto fail0;
		}
		pthread_barrier_wait(&barrier_g); // DELs end
	}

	if (0 == arg->read_p.len) arg->read_p.len = 1;
	if (0 == arg->write_p.len) arg->write_p.len = 1;
	nsecs_reads = BIL * (after.tv_sec - before.tv_sec) + after.tv_nsec - before.tv_nsec;
	UDEPOT_DBG("Test success. Thread %u Put Time=%lds.%06ldus. Latency per "
	       "request=%lds.%09ldus",
	       tid, nsecs_writes / BIL, (nsecs_writes % BIL) / 1000L,
	       (nsecs_writes / arg->write_p.len) / BIL,
	       ((nsecs_writes / arg->write_p.len) % BIL));
	UDEPOT_DBG("Test success. Thread %u Get Time=%lds.%06ldus. Latency per "
	       "request=%lds.%09ldus\n",
	       tid, nsecs_reads / BIL, (nsecs_reads % BIL) / 1000L,
	       (nsecs_reads / arg->read_p.len) / BIL,
	       ((nsecs_reads / arg->read_p.len) % BIL));
	arg->rd_start = before;
	arg->rd_finish = after;

fail0:
	KV->thread_local_exit();
	return;
}

template<typename RT>
static void gc_test_thread(thread_arg *const arg)
{
	int err;
	const u32 tid = arg->tid;
	uDepotSalsa<RT> *const KV = (uDepotSalsa<RT> *) arg->KV;
	const test_fn<RT> put_fn = putfn<RT>();
	const test_fn<RT> put_rnd_fn = putrndfn<RT>();
	const test_fn<RT> get_fn = getfn<RT>();
	const test_fn<RT> del_fn = delfn<RT>();

	KV->thread_local_entry();

	pthread_barrier_wait(&barrier_g);

	printf("GC test Thread %u starting test offset=%lu reads=%lu writes=%lu.\n",
	        tid, arg->read_p.start, arg->read_p.len, arg->write_p.len);
	err = put_fn(KV, tid, arg->write_p.start, arg->write_p.start + arg->write_p.len);
	if (0 != err) {
		UDEPOT_ERR("udepot put test ret w/%d.", err);
		goto fail0;
	}
	for (int i = 0; i < 10; ++i) {
		err = put_rnd_fn(KV, tid, arg->write_p.start, arg->write_p.start + arg->write_p.len);
		if (0 != err) {
			UDEPOT_ERR("udepot put test ret w/%d.", err);
			goto fail0;
		}
		// err = del_fn(KV, tid, arg->write_p.start, arg->write_p.start + arg->write_p.len);
		// if (0 != err) {
		// 	UDEPOT_ERR("udepot del test ret w/%d.", err);
		// 	goto fail0;
		// }
		UDEPOT_DBG("Wrote %lu KV pairs.", arg->write_p.len * (i + 1));
	}
	err = put_fn(KV, tid, arg->write_p.start, arg->write_p.start + arg->write_p.len);
	if (0 != err) {
		UDEPOT_ERR("udepot put test ret w/%d.", err);
		goto fail0;
	}
	err = get_fn(KV, tid, arg->read_p.start, arg->read_p.start + arg->read_p.len);
	if (0 != err) {
		UDEPOT_ERR("udepot get test ret w/%d.", err);
		goto fail0;
	}

	if (conf_g.del_test_m) {
		err = del_fn(KV, tid, arg->read_p.start, arg->read_p.start + arg->read_p.len);
		if (0 != err) {
			UDEPOT_ERR("udepot del test ret w/%d.", err);
			goto fail0;
		}
	}
	if (0 == arg->read_p.len) arg->read_p.len = 1;

	printf("Test success. GC Thread %u\n", tid);
 fail0:
	KV->thread_local_exit();
	return;
}

static std::function<void (thread_arg *)>
threadfn()
{
	switch(conf_g.type_m) {
		case KV_conf::KV_UDEPOT_SALSA:
			if (conf_g.gc_test_m) return &gc_test_thread<RuntimePosix>;
			return test_thread<RuntimePosix>;
		case KV_conf::KV_UDEPOT_SALSA_O_DIRECT:
			if (conf_g.gc_test_m) return gc_test_thread<RuntimePosixODirect>;
			return test_thread<RuntimePosixODirect>;
		case KV_conf::KV_UDEPOT_SALSA_TRT_AIO:
			if (conf_g.gc_test_m) return gc_test_thread<RuntimeTrt>;
			return test_thread<RuntimeTrt>;
		case KV_conf::KV_UDEPOT_SALSA_TRT_URING:
			if (conf_g.gc_test_m) return gc_test_thread<RuntimeTrtUring>;
			return test_thread<RuntimeTrtUring>;
#if defined(UDEPOT_TRT_SPDK)
		case KV_conf::KV_UDEPOT_SALSA_SPDK:
			if (conf_g.gc_test_m) return gc_test_thread<RuntimePosixSpdk>;
			return test_thread<RuntimePosixSpdk>;
		case KV_conf::KV_UDEPOT_SALSA_TRT_SPDK:
			if (conf_g.gc_test_m) return gc_test_thread<RuntimeTrtSpdk>;
			return test_thread<RuntimeTrtSpdk>;
		case KV_conf::KV_UDEPOT_SALSA_TRT_SPDK_ARRAY:
			if (conf_g.gc_test_m) return gc_test_thread<RuntimeTrtSpdkArray>;
			return test_thread<RuntimeTrtSpdkArray>;
#endif
		default:
			UDEPOT_ERR("Unsupported backend: %u\n", static_cast<unsigned>(conf_g.type_m));
			exit(1);
	}
	return nullptr;
}

template<typename RT>
static test_fn<RT> putfn()
{
	if (!conf_g.thin_test_m)
		return put_test<RT>;
	if (conf_g.zerocopy_m)
		return put_test_thin_mbuff<RT>;
	return put_test_thin<RT>;
}

template<typename RT>
static test_fn<RT> putrndfn()
{
	if (!conf_g.thin_test_m)
		return put_rnd_test<RT>;
	return put_test_thin<RT>;
}

template<typename RT>
static test_fn<RT> getfn()
{
	if (!conf_g.thin_test_m)
		return get_test<RT>;
	if (conf_g.zerocopy_m)
		return get_test_thin_mbuff<RT>;
	return get_test_thin<RT>;
}

template<typename RT>
static test_fn<RT> delfn()
{
	if (!conf_g.thin_test_m)
		return del_test<RT>;
	return del_test_thin<RT>;
}

template<typename RT>
struct t_worker_arg {
	KV                    *kv;
	ArgPool<t_worker_arg> *ap;
	size_t                *ops_completed;
	test_fn<RT>           op_fn;
	workload_part         task_part;
};

template<typename RT>
void *
t_worker(void *arg_) {
	t_worker_arg<RT> *arg = static_cast<t_worker_arg<RT> *>(arg_);
	size_t start = arg->task_part.start;
	size_t end   = arg->task_part.start + arg->task_part.len;
	arg->op_fn((uDepotSalsa<RT> *)arg->kv, 0, start, end);
	*(arg->ops_completed) += arg->task_part.len;
	arg->ap->put_arg(arg);
	return nullptr;
}

// We can run trt (within a thread) in two ways:
//  1. spawn a number of tasks at the begining, and distribute the load to them
//  2. dynamically spawn tasks every N operations (or similar)
//
//  We use to do (2), but now we do (1).
template<typename RT>
static double
do_trt_run(KV *kv, test_fn<RT> fn, workload_part thread_part)
{
	trt::Task::List tl;
	size_t ntasks = conf_g.trt_ntasks_m;
	size_t ops_completed = 0;

	ArgPool<t_worker_arg<RT>> task_args(ntasks);
	for (size_t i=0; i<ntasks; i++) {
		t_worker_arg<RT> *warg = task_args.get_arg();
		assert(warg);
		warg->kv = kv;
		warg->ap = &task_args;
		warg->ops_completed = &ops_completed;
		warg->op_fn = fn;
		warg->task_part = workload_part(thread_part, ntasks, i);
		trt::Task *t = trt::T::alloc_task(t_worker<RT>, warg, nullptr, false);
		tl.push_back(*t);
	}

	xtimer_t t; timer_init(&t); timer_start(&t);
	trt::T::spawn_many(tl);
	for (size_t i=0; i<ntasks; i++) {
		trt::T::task_wait();
	}
	timer_pause(&t);

	return timer_secs(&t);
}

struct t_main_arg {
	std::shared_ptr<KV> kv;
	unsigned            thread_id;
	unsigned            nthreads;
	pthread_barrier_t  *barrier;
	workload_part       read_p, write_p;
};

// main trt task for benchmaring
template<typename RT>
void *t_main_bench(void *arg__) {

	trt_dmsg("main task: enter\n");
	t_main_arg *arg = static_cast<t_main_arg *>(arg__);

	if (conf_g.gc_test_m) {
		UDEPOT_ERR("GC test not yet supported on TRT\n");
		abort();
	}

	const test_fn<RT> put_fn = putfn<RT>();
	const test_fn<RT> get_fn = getfn<RT>();
	const test_fn<RT> del_fn = delfn<RT>();

	if (arg->thread_id == 0) {
		// initialize KV
		int err = ENOMEM;
		arg->kv = std::shared_ptr<KV>(KV_factory::KV_new(conf_g));
		if (!arg->kv || (err = arg->kv->init()) != 0) {
			UDEPOT_ERR("KV init failed with %d (%s)\n", err, strerror(err));
			exit(1);
		}
		trt_dmsg("KV store ready\n");

		// copy KV to other tasks
		for (unsigned i=1; i<arg->nthreads; i++) {
			assert(arg[i].kv == nullptr);
			arg[i].kv = arg->kv;
		}
	}

	// initialize other threads's IO
	pthread_barrier_wait(arg->barrier);
	assert(arg->kv != nullptr);
	if (arg->thread_id != 0) {
		arg->kv->thread_local_entry();
	}

	double secs;
	xtimer_t t0;

	pthread_barrier_wait(arg->barrier);

	if (arg->thread_id == 0) {
		timer_init(&t0);
		timer_start(&t0);
	}
	secs = do_trt_run(arg->kv.get(), put_fn, arg->write_p);
	printf("TRT thread %4u PUTs time=%lfs Mops/sec=%lf ops=%lu\n", arg->thread_id, secs, arg->write_p.len/(secs*1000*1000), arg->write_p.len);
	pthread_barrier_wait(arg->barrier);
	if (arg->thread_id == 0) {
		timer_pause(&t0);
		double s = timer_secs(&t0);
		printf("TRT PUTs aggregate   time=%lfs Mops/sec=%lf ops=%lu\n", s, conf_g.write_nr_m/(s*1000*1000), conf_g.write_nr_m);
	}

	pthread_barrier_wait(arg->barrier);
	if (arg->thread_id == 0) {
#if defined(UDEPOT_TRT_SPDK)
		// rte_malloc_dump_stats(stdout, nullptr);
#endif
		timer_init(&t0);
		timer_start(&t0);
	}
	secs = do_trt_run(arg->kv.get(), get_fn, arg->read_p);
	printf("TRT GETs thread %4u time=%lfs Mops/sec=%lf ops=%lu\n", arg->thread_id, secs, arg->read_p.len/(secs*1000*1000), arg->read_p.len);
	pthread_barrier_wait(arg->barrier);
	if (arg->thread_id == 0) {
		timer_pause(&t0);
		double s = timer_secs(&t0);
		printf("TRT GETs aggregate   time=%lfs Mops/sec=%lf ops=%lu\n", s, conf_g.read_nr_m/(s*1000*1000), conf_g.read_nr_m);
	}

	pthread_barrier_wait(arg->barrier);
	if (conf_g.del_test_m && conf_g.thin_test_m) {
		do_trt_run(arg->kv.get(), del_fn, arg->read_p);
	}

	pthread_barrier_wait(arg->barrier);
	if (arg->thread_id != 0) {
		arg->kv->thread_local_exit();
	}

	// trt::T::yield();  // give a chance for everybody to run once
	pthread_barrier_wait(arg->barrier);
	if (arg->thread_id == 0) {
		trt_dmsg("%s: Shutting down KV\n", __FUNCTION__);
		arg->kv->shutdown();
	}

	pthread_barrier_wait(arg->barrier);

	if (arg->thread_id == 0) {
		trt_dmsg("%s: Triggering TRT exit\n", __FUNCTION__);
		trt::T::set_exit_all(); // notify trt schedulers that we are done
		trt_dmsg("%s: DONE\n", __FUNCTION__);
	}

	return nullptr;
}

// main trt task for benchmaring
template<typename RT>
void *t_main_server(void *arg__) {

	trt_dmsg("main task: server mode: enter\n");
	t_main_arg *arg = static_cast<t_main_arg *>(arg__);

	if (conf_g.gc_test_m) {
		UDEPOT_ERR("GC test not yet supported on TRT\n");
		abort();
	}

	const test_fn<RT> put_fn = putfn<RT>();
	const test_fn<RT> get_fn = getfn<RT>();
	const test_fn<RT> del_fn = delfn<RT>();

	if (arg->thread_id == 0) {
		// initialize KV
		arg->kv = std::shared_ptr<KV>(KV_factory::KV_new(conf_g));
		if (!arg->kv || arg->kv->init() != 0) {
			UDEPOT_ERR("KV init failed\n");
			abort();
		}
		trt_dmsg("KV store ready\n");

		// copy KV to other tasks
		for (unsigned i=1; i<arg->nthreads; i++) {
			assert(arg[i].kv == nullptr);
			arg[i].kv = arg->kv;
		}
	}

	// initialize other threads's state
	pthread_barrier_wait(arg->barrier);
	assert(arg->kv != nullptr);
	if (arg->thread_id != 0) {
		arg->kv->thread_local_entry();
	}

	#if 0
	pthread_barrier_wait(arg->barrier);
	if (arg->thread_id != 0) {
		arg->kv->thread_local_exit();
	}

	// trt::T::yield();  // give a chance for everybody to run once
	pthread_barrier_wait(arg->barrier);
	if (arg->thread_id == 0) {
		trt_dmsg("%s: Shutting down KV\n", __FUNCTION__);
		arg->kv->shutdown();
	}

	pthread_barrier_wait(arg->barrier);

	if (arg->thread_id == 0) {
		trt_dmsg("%s: Triggering TRT exit\n", __FUNCTION__);
		trt::T::set_exit_all(); // notify trt schedulers that we are done
		trt_dmsg("%s: DONE\n", __FUNCTION__);
	}
	#endif

	return nullptr;
}

template<typename RT>
int main_trt()
{
	pthread_barrier_t barrier;
	t_main_arg args[conf_g.thread_nr_m];

	auto prepare_args = [&barrier, &args](void) {
		pthread_barrier_init(&barrier, NULL, conf_g.thread_nr_m);
		for (unsigned i=0; i < conf_g.thread_nr_m; i++) {
			auto a = &args[i];
			a->kv = nullptr;
			a->thread_id = i;
			a->nthreads  = conf_g.thread_nr_m;
			a->barrier   = &barrier;
			a->read_p    = workload_part(conf_g.read_nr_m,  conf_g.thread_nr_m, i);
			a->write_p   = workload_part(conf_g.write_nr_m, conf_g.thread_nr_m, i);
		}
	};


	auto t_main = conf_g.is_server_mode() ? t_main_server<RT> : t_main_bench<RT>;
	switch(conf_g.type_m) {
#if defined(UDEPOT_TRT_SPDK)
		case KV_conf::KV_UDEPOT_SALSA_TRT_SPDK:
		case KV_conf::KV_UDEPOT_SALSA_TRT_SPDK_ARRAY:
		{
			RT::IO::global_init();
			trt::RteController c;
			const unsigned lcores_nr = rte_lcore_count();
			unsigned lcore;
			if (lcores_nr == 0) {
				UDEPOT_ERR("Did you initialize RTE?\n");
				abort();
			}
			const unsigned lcores_slaves_nr = lcores_nr - 1;
			#if !defined(NDEBUG)
			unsigned lcores_slaves_nr__ = 0;
			RTE_LCORE_FOREACH_SLAVE(lcore) {
				lcores_slaves_nr__++;
			}
			assert(lcores_slaves_nr__ == lcores_slaves_nr);
			#endif
			if (lcores_slaves_nr == 0) {
				UDEPOT_ERR("lcores=%u. No scheduler(s) will be spawned.", lcores_nr);
			}

			conf_g.thread_nr_m = std::min(conf_g.thread_nr_m, lcores_slaves_nr);
			UDEPOT_MSG("lcores=%u lcores_slaves=%u. Spawning %u schedulers.", lcores_nr, lcores_slaves_nr, conf_g.thread_nr_m);
			prepare_args();
			int i = -1;
			RTE_LCORE_FOREACH_SLAVE(lcore) {
				if (conf_g.thread_nr_m <= (u32) ++i)
					break;
				printf("spawning scheduler on lcore=%u i=%d\n", lcore, i);
				c.spawn_scheduler(t_main, &args[i], trt::TaskType::TASK, lcore);
			}
			c.wait_for_all();
			break;
		}
#endif
		case KV_conf::KV_UDEPOT_SALSA_TRT_AIO:
		case KV_conf::KV_UDEPOT_SALSA_TRT_URING:
		{
			trt::Controller c;
			prepare_args();
			for (unsigned i=0; i < conf_g.thread_nr_m; i++) {
				c.spawn_scheduler(t_main, &args[i], trt::TaskType::TASK);
			}
			c.wait_for_all();
			break;
		}

		default: {
			std::string ty_str = conf_g.type_to_string(conf_g.type_m);
			fprintf(stderr, "Cannot handle KV type: %d (%s)", (int)conf_g.type_m, ty_str.c_str());
			exit(1);
		}
	}

	printf("Controller done waiting\n");

	return 0;
}

int main_threads()
{
	KV *KV;
	int err;
	u32 tid;
	std::thread threads[conf_g.thread_nr_m];
	thread_arg args[conf_g.thread_nr_m];
	auto test_thread_fn = threadfn();
	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	err = sched_getaffinity(0, sizeof(cpu_set_t), &cpuset);
	if (err)
		return err;

	printf("Initializing: %s\n", conf_g.type_to_string(conf_g.type_m).c_str());
	KV = KV_factory::KV_new(conf_g);
	if (NULL == KV) {
		printf("failed to allocate KV.\n");
		err = ENOMEM;
		return err;
	}

	for (u32 tid = 0; tid<conf_g.thread_nr_m; tid++) {
		args[tid].KV      = KV;
		args[tid].tid     = tid,
		args[tid].read_p  = workload_part(conf_g.read_nr_m,  conf_g.thread_nr_m, tid);
		args[tid].write_p = workload_part(conf_g.write_nr_m, conf_g.thread_nr_m, tid);
	}

	err = KV->init();
	if (0 != err) {
		UDEPOT_ERR("udepot init failed w/%d.", err);
		goto fail;
	}

	if (conf_g.is_server_mode()) {
		// server mode
		printf("Entering server mode...\n");
		while (1)
			std::this_thread::sleep_for(std::chrono::milliseconds(100000));
		goto fail;
	}

	pthread_barrier_init(&barrier_g, NULL, conf_g.thread_nr_m + 1);

	err = sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
	if (err)
		return err;
	tid = 0;
	for (auto &thread : threads) {
		try {
			thread = std::thread(test_thread_fn, &args[tid]);
		} catch (...) {
			UDEPOT_ERR("thread %u failed to be created, aborting.", tid);
			exit(-1);
		}
		tid++;
	}

	printf("Starting test with write_nr=%lu read_nr=%lu size=%luKiB.\n",
		conf_g.write_nr_m, conf_g.read_nr_m, KV->get_size() >> 10);

	if (!conf_g.gc_test_m) {
		xtimer_t t;
		double s_put, s_get;

		timer_init(&t); timer_start(&t);
		pthread_barrier_wait(&barrier_g); // PUTs start
		// threads doing PUTs
		pthread_barrier_wait(&barrier_g); // PUTs end
		timer_pause(&t);
		s_put = timer_secs(&t);

		timer_init(&t); timer_start(&t);
		pthread_barrier_wait(&barrier_g); // GETs start
		// threads doing GETs
		pthread_barrier_wait(&barrier_g); // GETs end
		timer_pause(&t);
		s_get = timer_secs(&t);

		if (conf_g.del_test_m) {
			pthread_barrier_wait(&barrier_g); // DELs start
			pthread_barrier_wait(&barrier_g); // DELs end
		}

		std::string err;
		if (test_failure_g) {
			err = "*** TEST FAILURE: Number might not be sensible *** ";
		}

		printf("%sPUTs Aggregate time=%lfs Mops/sec=%lf\n", err.c_str(), s_put, conf_g.write_nr_m/(s_put*1000*1000));
		printf("%sGETs Aggregate time=%lfs Mops/sec=%lf\n", err.c_str(), s_get, conf_g.read_nr_m/(s_get*1000*1000));

	} else {
		pthread_barrier_wait(&barrier_g);
	}

	for (auto &thread : threads)
		thread.join();

	printf("Finished test with write_nr=%lu read_nr=%lu size=%luKiB.\n",
		conf_g.write_nr_m, conf_g.read_nr_m, KV->get_size() >> 10);

	err = KV->shutdown();
	if (0 != err) {
		UDEPOT_ERR("udepot init failed w/%d.", err);
	}

fail:
	delete KV;
	return err;
}



void udepot_test_conf::print_usage(const char name[])
{
	KV_conf::print_usage(name);
	printf("udepot-test specific parameters.\n");
	printf("-w, --writes nr: Number of writes to be performed by the KV clients\n");
	printf("-r, --reads nr: Number of reads to be performed by the KV clients\n");
	printf("--val-size bytes: Bytes of value part of KV pair (default:%uB)\n", kv_val_size_m);
	printf("--del: If set a number of deletes equal to the number of puts will be performed.\n");
	printf("--gc: If set a GC stress test will be performed instead of the default performance \n"\
		"and verification test (default: false).\n");
	printf("--thin: If set the keys and values will be generated using a deterministic generator\n"\
		" instead of being generated and stored stored in memory (default: false).\n");
	printf("--zero-copy: Use the zero copy KV interface (KV_MbuffInterface) (default:false).\n");
	printf("--unit-test: Run a uDepot unit test at the end of execution (default:false).\n");
	printf("--crash-unit-test: Run a uDepot crash recovery unit test at the end of execution (default:false).\n");
	printf("--trt-ntasks: How many tasks to spawn per thread on trt. (default:%zd)\n", udepot_test_conf::TRT_NTASKS);
}

bool udepot_test_conf::is_server_mode()
{
	return (0 != conf_g.self_server_m.size() &&
	        0 == conf_g.write_nr_m &&
	        0 == conf_g.read_nr_m);
}

void udepot_test_conf::validate_and_sanitize_parameters()
{
#define	KV_VAL_MAX_SIZE	(1U << 24) // 16MiB
	KV_conf::validate_and_sanitize_parameters();
	// arg sanitation
	if (force_destroy_m) {
		write_nr_m = std::max(read_nr_m, write_nr_m);
		if (write_nr_m)
			size_m = align_up(std::max(size_m, 5 * write_nr_m * sizeof(*kvs_g)), 4096);
	}
	if (thread_max_g < thread_nr_m)
		thread_nr_m = thread_max_g;
	kv_val_size_m = std::min(kv_val_size_m, KV_VAL_MAX_SIZE);
	kv_val_size_m = std::min(static_cast<u64>(kv_val_size_m), grain_size_m * segment_size_m);
}

int udepot_test_conf::parse_args(ParseArgs &args)
{
	int err;
	err = KV_conf::parse_args(args);
	if (err)
		return err;

	int argc      = args.argc_;
	const char *const *const argv   = args.argv_;

	for (int i = 1; i < argc; ++i) {
		std::string arg = std::string(argv[i]);
		// all arguments take at least one value, except help
		std::string val = i < argc - 1 ? std::string(argv[i + 1]) : "";
		UDEPOT_DBG("arg given %s.\n", arg.c_str());
		if ("-r" == arg || "--reads" == arg) {
			read_nr_m = std::stoul(val);
			args.used_arg_and_val(i);
			++i;
		} else if ("-w" == arg || "--writes" == arg) {
			write_nr_m = std::stoul(val);
			args.used_arg_and_val(i);
			++i;
		} else if ("--val-size" == arg) {
			kv_val_size_m = std::stoul(val);
			args.used_arg_and_val(i);
			++i;
		} else if ("--local_ip" == arg) {
			local_ip_m = val;
			args.used_arg_and_val(i);
			++i;
		} else if ("--thin" == arg) {
			thin_test_m = true;
			args.used_arg(i);
		} else if ("--zero-copy" == arg) {
			zerocopy_m = true;
			args.used_arg(i);
		} else if ("--unit-test" == arg) {
			run_unit_test_m = true;
			args.used_arg(i);
		} else if ("--crash-unit-test" == arg) {
			run_crash_unit_test_m = true;
			args.used_arg(i);
		} else if ("--del" == arg) {
			del_test_m = true;
			args.used_arg(i);
		} else if ("--gc" == arg) {
			gc_test_m = true;
			args.used_arg(i);
		} else if ("--trt-ntasks" == arg) {
			trt_ntasks_m = std::stoul(val);
			args.used_arg_and_val(i);
			++i;
		}
	}
	return 0;
}

};				// udepot namespace

void test_build ();
int test_crash_recovery_minimal ();

int main(const int argc, char *argv[])
{
	int err = 0;

	ParseArgs args(argc, argv);
	err = conf_g.parse_args(args);
	if (0 != err || conf_g.help_m) {
		conf_g.print_usage(argv[0]);
		goto fail0;
	} else if (args.has_unused_args()) {
		args.print_unused_args(std::cerr);
		conf_g.print_usage(argv[0]);
		goto fail0;
	}

	conf_g.validate_and_sanitize_parameters();

	err = create_test_data();
	if (0 != err) {
		printf("create_test_data failed w/%d.\n", err);
	}

	if (conf_g.type_m == KV_conf::KV_UDEPOT_SALSA_TRT_AIO) {
		err = main_trt<RuntimeTrt>();
	} else if (conf_g.type_m == KV_conf::KV_UDEPOT_SALSA_TRT_URING) {
		err = main_trt<RuntimeTrtUring>();
#if defined(UDEPOT_TRT_SPDK)
	} else if (conf_g.type_m == KV_conf::KV_UDEPOT_SALSA_TRT_SPDK) {
		err = main_trt<RuntimeTrtSpdk>();
	} else if (conf_g.type_m == KV_conf::KV_UDEPOT_SALSA_TRT_SPDK_ARRAY) {
		err = main_trt<RuntimeTrtSpdkArray>();
#endif
	} else {
		err = main_threads();
	}

	if (!conf_g.thin_test_m)
		destroy_test_data();
	if (conf_g.run_unit_test_m)
		test_build();
	if (conf_g.run_crash_unit_test_m)
		return test_crash_recovery_minimal();
fail0:
	return err;
}

__attribute__((unused))
void test_build ()
{
	udepot::KV_conf conf (
		"/tmp/udepot-test-file-small-tiny",
		(1UL << 30) + 20UL, /* file size, if file doesn't exist it will create one */
		true, /* do not try to restore data (force destroy) */
		32, /* grain_size, in bytes */
		4096 /*segment size */ );
	udepot::KV_conf conf2 (
		"/tmp/udepot-test-file-small-tiny_2",
		(1UL << 30) + 20UL, /* file size, if file doesn't exist it will create one */
		true, /* do not try to restore data (force destroy) */
		32, /* grain_size, in bytes */
		4096 /*segment size */ );

	::KV *const KV = udepot::KV_factory::KV_new(conf);
	if (nullptr == KV)
		abort();
	::KV *const KV2 = udepot::KV_factory::KV_new(conf2);
	if (nullptr == KV2)
		abort();

	int rc __attribute__((unused)) = KV->init();
	assert(0 == rc);
	rc = KV2->init();
	assert(0 == rc);

	char key[31] = { 0 };
	key[0] = 0xBE;
	key[1] = 0xEF;
	char val[3078] = { 0 };
	rc = KV->put(key, sizeof(key), val, sizeof(val));
	assert(0 == rc);
	key[0] = 0xDE;
	key[1] = 0xAD;
	rc = KV2->put(key, sizeof(key), val, sizeof(val));
	assert(0 == rc);

	char val_out[3078];
	size_t val_size_read, val_size;
	key[0] = 0xBE;
	key[1] = 0xEF;
	rc = KV->get(key, sizeof(key), val_out, sizeof(val_out), val_size_read, val_size);
	assert(0 == rc);
	assert(0 == memcmp(val, val_out, sizeof(val)));
	assert(val_size_read == val_size);
	key[0] = 0xDE;
	key[1] = 0xAD;
	rc = KV2->get(key, sizeof(key), val_out, sizeof(val_out), val_size_read, val_size);
	assert(0 == rc);
	assert(0 == memcmp(val, val_out, sizeof(val)));
	assert(val_size_read == val_size);

	rc = KV->shutdown();
	assert(0 == rc);
	rc = KV2->shutdown();
	assert(0 == rc);
	UDEPOT_MSG("test_build succces.");
	delete KV;
	delete KV2;
}

__attribute__((unused))
int test_crash_recovery_minimal ()
{
	const pid_t pid = fork();
	size_t val_size_read, val_size;
	udepot::KV_conf conf (
		"/tmp/udepot-test-file-small-tiny",
		(1UL << 20) + 20UL, /* file size, if file doesn't exist it will create one */
		false, /* do not try to restore data (force destroy) */
		512, /* grain_size, in bytes */
		64 /*segment size */ );
	conf.type_m = udepot::KV_conf::KV_UDEPOT_SALSA_O_DIRECT;
	assert(-1 != pid);
	if (0 == pid) {

		::KV *const KV = udepot::KV_factory::KV_new(conf);
		if (nullptr == KV)
			return ENOMEM;

		int rc __attribute__((unused)) = KV->init();
		assert(0 == rc);

		char key[31] = { 0 };
		key[0] = 0xBE;
		key[1] = 0xEF;
		char val[3078] = { 0 };
		rc = KV->put(key, sizeof(key), val, sizeof(val));
		assert(0 == rc);
		key[0] = 0xDE;
		key[1] = 0xAD;
		rc = KV->put(key, sizeof(key), val, sizeof(val));
		assert(0 == rc);

		char val_out[3078];
		key[0] = 0xBE;
		key[1] = 0xEF;
		rc = KV->get(key, sizeof(key), val_out, sizeof(val_out), val_size_read, val_size);
		assert(0 == rc);
		assert(0 == memcmp(val, val_out, sizeof(val)));
		assert(val_size_read == val_size);
		key[0] = 0xDE;
		key[1] = 0xAD;
		rc = KV->get(key, sizeof(key), val_out, sizeof(val_out), val_size_read, val_size);
		assert(0 == rc);
		assert(0 == memcmp(val, val_out, sizeof(val)));
		assert(val_size_read == val_size);
		// no shutdown(), simulate crash with destructor
		delete KV;

	} else {
		int status;
		const pid_t pid_ret = waitpid(pid, &status, 0);
		if (pid != pid_ret) {
			fprintf(stderr, "\n\n\n\n\n\nwaitpid failed with err=%s for pid=%d pid_ret=%d status=%d\n\n\n\n\n\n",
				strerror(errno), pid, pid_ret, status);
			abort();
		}
		int rc = kill(pid, SIGKILL);
		if (0 != rc)
			fprintf(stderr, "kill failed with %s\n", strerror(errno));
		std::this_thread::sleep_for(std::chrono::milliseconds(1000));
		::KV *const KVrestored = udepot::KV_factory::KV_new(conf);
		if (nullptr == KVrestored)
			return ENOMEM;

		// verify data were restored correctly
		char key[31] = { 0 };
		char __attribute__((unused)) val[3078] = { 0 };
		char val_out[3078];
		rc = KVrestored->init();
		assert(0 == rc);
		key[0] = 0xBE;
		key[1] = 0xEF;
		rc = KVrestored->get(key, sizeof(key), val_out, sizeof(val_out), val_size_read, val_size);
		assert(0 == rc);
		assert(0 == memcmp(val, val_out, sizeof(val)));
		assert(val_size_read == val_size);
		key[0] = 0xDE;
		key[1] = 0xAD;
		rc = KVrestored->get(key, sizeof(key), val_out, sizeof(val_out), val_size_read, val_size);
		assert(0 == rc);
		assert(0 == memcmp(val, val_out, sizeof(val)));
		assert(val_size_read == val_size);
	}
	return 0;
}

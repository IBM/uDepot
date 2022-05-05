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

#include "uDepot/udepot-lsa.hh"

#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <array>
#include <iostream>

#include "util/debug.h"
#include "frontends/usalsa++/SalsaMD.hh"
#include "util/types.h"
#include "uDepot/backend.hh"
#include "uDepot/kv-conf.hh"
#include "uDepot/hash-function.hh"
#include "uDepot/io.hh"
#include "uDepot/io/helpers.hh"
#include "uDepot/lsa/hash-entry.hh"
#include "uDepot/lsa/udepot-directory-map.hh"
#include "uDepot/lsa/metadata.hh"
#include "uDepot/lsa/store.hh"
#include "uDepot/thread-id.hh"
#include "uDepot/stimer.hh"
#include "util/profiling.hh"
// #include "uDepot/stats.hh"

namespace udepot {

template<typename RT>
uDepotSalsa<RT>::uDepotSalsa(const u32 thread_nr,
			std::vector<typename RT::Net::Client::Conf> self_clients,
			const std::string &fname, const size_t size,
			typename RT::Net::Server::Conf self_server,
			const bool force_destroy,
			const u64 grain_size, const u64 segment_size,
			const u32 overprovision)
	: uDepot<RT>(thread_nr, fname, size, force_destroy),
	  salsa::SalsaCtlr(),
	  type_m(uDepotSalsaType::KV),
	  scm_m(nullptr),
	  exit_gc_m(0),
	  grain_size_m(grain_size),
	  segment_size_m(segment_size),
	  overprovision_m(overprovision),
	  read_io_b_m(0), write_io_b_m(0),
	  false_positive_lookups_m(0),
	  reloc_b_m(0),
	  // We pass a reference of our udepot_io_m to md_m and map_m to use
	  md_m(size_m, grain_size_m, udepot_io_m),
	  map_m(md_m, udepot_io_m),
	  salsa_streams_nr_m(thread_nr),
	  local_get_mbuff_m(std::mem_fn(&uDepotSalsa::local_get_mbuff)),
	  local_put_mbuff_m(std::mem_fn(&uDepotSalsa::local_put_mbuff)),
	  local_del_mbuff_m(std::mem_fn(&uDepotSalsa::local_del_mbuff)),
	  net_m(thread_nr_m, self_clients, self_server, *this),
	  mb_cache_m(*this)
	  {}

template<typename RT>
uDepotSalsa<RT>::~uDepotSalsa()
{
	UDEPOT_MSG("Read I/O=%luKiB Write I/O=%luKiB, False positive lookups=%lu.",
		read_io_b_m.load() >> 10, write_io_b_m.load() >> 10, false_positive_lookups_m.load());
	UDEPOT_MSG("Relocation traffic=%luKiB\n", (reloc_b_m.load()) >> 10);
}

template<typename RT>
int uDepotSalsa<RT>::register_local_region()
{
	int rc = 0, err __attribute__((unused));
	const char *const fname = fname_m.c_str();
	u64 dev_size;
	rc = udepot_io_m.open(fname, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
	if (0 != rc) {
		UDEPOT_ERR("failed to open %s rc=%d errno=%s", fname, rc, strerror(errno));
		goto out;
	}

	dev_size = udepot_io_m.get_size();
	if ((u64)-1 == dev_size) {
		rc = errno;
		UDEPOT_ERR("get_size() failed with %d", errno);
		goto close;
	}

	if (dev_size < size_m) {
		UDEPOT_MSG("dev=%s size=%ld less than requested size=%lu, trying to truncate.",
			fname_m.c_str(), dev_size, size_m);
		rc = udepot_io_m.ftruncate(size_m);
		if (0 != rc) {
			rc = errno;
			UDEPOT_ERR("ftruncate failed with %d.\n", errno);
			goto close;
		}
		dev_size = udepot_io_m.get_size();
		if ((u64)-1 == dev_size) {
			rc = errno;
			UDEPOT_ERR("get_size() failed with %d\n", errno);
			goto close;
		}
	}
	size_m = udepot_io_m.get_size();
	assert(0 == rc);
	return rc;
close:
	err = udepot_io_m.close();
	assert(0 == err);
out:
	return rc;
}

template<typename RT>
int uDepotSalsa<RT>::unregister_local_region()
{
	int rc = udepot_io_m.close();
	assert(0 == rc);

	return rc;
}

template<typename RT>
void uDepotSalsa<RT>::thread_local_entry()
{
	uDepot<RT>::thread_local_entry();
	net_m.thread_init();
}

template<typename RT>
void uDepotSalsa<RT>::thread_local_exit()
{
	uDepot<RT>::thread_local_exit();
	net_m.thread_exit();
}

template<typename RT>
__attribute__((warn_unused_result))
int uDepotSalsa<RT>::init_local(const u32 gc_type, const u32 gc_low_wm, const u32 gc_high_wm)
{
	int rc = 0;
	salsa::salsa_dev_md dev_md = { 0 };
	bool restored = false;
	// init SalsaCtlr
	char argv_base[256];
	char *argv[16], *token;
	int argc;

	RT::IO::global_init();
	net_m.global_init();
	// TODO: add a matching thread_local_exit or only initialize
	// the parts of thread_local_entry needed and make sure that
	// matching exits are called
	thread_local_entry();

	UDEPOT_DBG("registering local socket region.\n");
	rc = register_local_region();
	if (0 != rc) {
		UDEPOT_ERR("register_local_region failed with %d.\n", rc);
		goto fail0;
	}
	do {
		// try to restore from persistent metadata
		dev_md = md_m.default_dev_md(grain_size_m, segment_size_m);
		if (!force_destroy_m)
			restored = md_m.validate_dev_md(&dev_md);
		md_m.init(dev_md);
		segment_size_m = dev_md.segment_size;
		grain_size_m = dev_md.grain_size;

		rc = md_m.check_dev_size(segment_size_m);
		if (0 != rc) {
			if (!restored) {
				// try a different segment size before we give up
				u64 new_seg_size = segment_size_m - 1;
				const u64 new_seg_min = segment_size_m / 2;
				u64 min_waste = std::numeric_limits<u64>::max();
				if (0U == new_seg_size) {
					UDEPOT_ERR("failed to find a valid segment size.");
					goto fail1;
				}
				new_seg_size = KV_conf::sanitize_segment_size(grain_size_m, new_seg_size);
				for (u64 ns = new_seg_size;
				     new_seg_min < ns;
				     ns = KV_conf::sanitize_segment_size(grain_size_m, ns - 1)) {
					const u64 seg_sizeb = ns * grain_size_m;
					const u64 net_size = size_m - grain_size_m;
					const u64 waste = net_size - align_down(net_size, seg_sizeb);
					if (waste < min_waste) {
						assert( 0 == (seg_sizeb % 4096U));
						min_waste = waste;
						new_seg_size = ns;
					}
				}
				segment_size_m = new_seg_size;
				continue; // try again
			}
			UDEPOT_ERR("check_dev_size failed with rc=%d.", rc);
			goto fail1;
		}

		size_m = md_m.get_dev_size(); // adjusted for metadata

		if (restored) {
			UDEPOT_MSG("Found valid md header size=%luMiB seg=%lu grain=%lu",
				dev_md.physical_size>>20, dev_md.segment_size, dev_md.grain_size);
			if (udepot_io_m.get_size() != dev_md.physical_size) {
				rc = EINVAL;
				UDEPOT_ERR("restore md with invalid expected device size=%lu found=%lu.\n",
					dev_md.physical_size, udepot_io_m.get_size());
				goto fail1;
			}
		}

		const char scm_fname[] = ""; // we run SCM in simulation, so no filename is needed
		snprintf(argv_base, sizeof(argv_base), "scm_dev=%s dev_size=%lu grain_size=%lu"	\
			" segment_size=%lu gc_type=%u gc_low_wm=%u gc_high_wm=%u gc_thread_nr=1 simulation=1",
			scm_fname, size_m, grain_size_m, segment_size_m, gc_type, gc_low_wm, gc_high_wm);
		argc = 0;
		token = strtok(argv_base, " ");
		do {
			argv[argc++] = token;
		} while ((token = strtok(nullptr, " ")));

		scm_m = new salsa::Scm();
		if (nullptr == scm_m) {
			rc = ENOMEM;
			UDEPOT_ERR("Failed to allocate Scm");
			goto fail1;
		}

		rc = scm_m->init(argc, argv, overprovision_m);
		if (0 == rc || restored)
			break;

		// try again with half the segment size
		segment_size_m = KV_conf::sanitize_segment_size(grain_size_m, segment_size_m / 2);
		if (segment_size_m <= 1) {
			rc = EINVAL;
			break;
		}
		// scm_m init failed _and we did not restore valid
		// device metadata _and the new segment size to be
		// tried is greater than zero. We'll try again with
		// the halved segment size, after deleting the now
		// stale scm_m
		delete scm_m;
	} while (1);

	if (0 != rc) {
		UDEPOT_ERR("scm->init ret=%d", rc);
		goto fail2;
	}

	do {
		rc = salsa::SalsaCtlr::init(scm_m, md_m.get_seg_md_size(), salsa_streams_nr_m, 1);
		if (0 == rc)
			break;
		salsa_streams_nr_m /= 2;
		UDEPOT_MSG("Halved salsa_streams_nr_m to %u", salsa_streams_nr_m);
		if (0 == salsa_streams_nr_m) {
			UDEPOT_ERR("Exhausted all stream counts.");
			rc = EINVAL;
			break;
		}
	} while (1);
	if (0 != rc) {
		UDEPOT_ERR("SalsaCtlr init failed with %d.\n", rc);
		goto fail3;
	}
	assert(grain_size_m == salsa::SalsaCtlr::get_grain_size());

	rc = map_m.init(scm_m, this);
	if (0 != rc) {
		UDEPOT_ERR("uDepotMap init failed with %d.\n", rc);
		goto fail4;
	}

	rc = scm_m->init_threads();
	if (0 != rc) {
		UDEPOT_ERR("scm init threads failed with %d.\n", rc);
		goto fail5;
	}

	rc = net_m.start_net_all();
	if (0 != rc) {
		UDEPOT_ERR("net_m start server and clients failed with %d.\n", rc);
		goto fail6;
	}

	if (!restored) {
		UDEPOT_MSG("No valid md header or force destroy was set, persisting one.");
		rc = md_m.persist_dev_md();
		if (0 != rc) {
			UDEPOT_ERR("Failed to persist md header. Error: %d (%s)\n", rc, strerror(rc));
			goto fail6;
		}
	}

	return 0;

fail6:
	net_m.stop_net_all();
	exit_gc_m = 1;
	scm_m->exit_threads();
fail5:
	map_m.shutdown(this);
fail4:
	salsa::SalsaCtlr::shutdown();
fail3:
fail2:
	delete scm_m;
fail1:
	unregister_local_region();

fail0:
	thread_local_exit();
	net_m.global_exit();
	scm_m = nullptr;
	return rc;
}

template<typename RT>
__attribute__((warn_unused_result))
int uDepotSalsa<RT>::init()
{
	return init_local(14, 2, 4);
}

template<>
__attribute__((warn_unused_result))
int uDepotSalsa<RuntimePosixODirectMC>::init()
{
	// no need for spare, we are not relocating, just purging
	overprovision_m = 40;	// 1%
	return init_local(1, 20, 40);
}

template<>
__attribute__((warn_unused_result))
int uDepotSalsa<RuntimeTrtMC>::init()
{
	// no need for spare, we are not relocating, just purging
	overprovision_m = 40;	// 1%
	return init_local(1, 20, 40);
}

template<>
__attribute__((warn_unused_result))
int uDepotSalsa<RuntimeTrtUringMC>::init()
{
	// no need for spare, we are not relocating, just purging
	overprovision_m = 40;	// 1%
	return init_local(1, 20, 40);
}

#if defined(UDEPOT_TRT_SPDK)
template<>
__attribute__((warn_unused_result))
int uDepotSalsa<RuntimeTrtSpdkArrayMC>::init()
{
	// no need for spare, we are not relocating, just purging
	overprovision_m = 40;	// 1%
	return init_local(1, 20, 40);
}
#endif

template<typename RT>
int uDepotSalsa<RT>::shutdown()
{
	net_m.stop_net_all();

	exit_gc_m = 1;
	scm_m->exit_threads();

	salsa::SalsaCtlr::shutdown();

	map_m.shutdown(this);

	unregister_local_region();

	thread_local_exit();
	net_m.global_exit();

	delete scm_m;
	scm_m = nullptr;

	return 0;
}

__attribute__((pure))
static inline bool
visited_pba(
	const u64 pba_key,
	const std::array<u64, _UDEPOT_HOP_SCOTCH_BUCKET_SIZE> &pbas_visited,
	const u32 lookup_nr)
{
	assert(lookup_nr < _UDEPOT_HOP_SCOTCH_BUCKET_SIZE);
	for (u32 i = 0; i < lookup_nr; ++i)
		if (pba_key == pbas_visited[i])
			return true;

	return false;
}

static inline void
update_visited_pba(
	const u64 pba_key,
	u32 &lookup_nr,
	std::array<u64, _UDEPOT_HOP_SCOTCH_BUCKET_SIZE> &pbas_visited)
{
	assert(lookup_nr < _UDEPOT_HOP_SCOTCH_BUCKET_SIZE);
	pbas_visited[lookup_nr++] = pba_key;
}


template<typename RT>
inline std::tuple<int, HashEntry *>
uDepotSalsa<RT>::lookup_common(const u64 h, const u32 lookup_nr,
	const std::array<u64, _UDEPOT_HOP_SCOTCH_BUCKET_SIZE> &pbas_visited)
{
	HashEntry *trgt = nullptr;
	int rc = map_m.lookup(h, &trgt);
	if (0 != rc)
		return std::make_tuple(rc, trgt);
	assert(nullptr != trgt);
	do {
		if (!visited_pba(trgt->pba, pbas_visited, lookup_nr))
			break;
		trgt = map_m.next(trgt, h); // search all possible matches
	} while (nullptr != trgt);
	return std::make_tuple(rc, trgt);
}

template<typename RT>
template<typename F, typename... Args>
typename std::result_of<F(uDepotSalsa<RT> *, Args...)>::type
uDepotSalsa<RT>::local_op_execute(u64 h, F &&op, Args &&... a)
{
	typename RT::RwpfTy *rwlpf = &map_m.dir_ref_m.rwpflock;
	uDepotMap<RT> *udm = nullptr;

	auto op_prepare = [this, h, rwlpf, &udm]() {
		rwlpf->rd_enter();
		assert(udm == nullptr);
		udm = this->map_m.hash_to_map(h);
		udm->lock(h);
	};

	auto op_rollback = [h, rwlpf, &udm] {
		assert(udm != nullptr);
		udm->unlock(h);
		rwlpf->rd_exit();
		udm = nullptr;
	};

	auto op_finalize = [h, rwlpf, &udm] {
		udm->unlock(h);
		rwlpf->rd_exit();
	};

	return rwlpf->rd_execute__(std::ref(op_prepare),
	                           std::ref(op_rollback),
	                           std::ref(op_finalize),
	                           std::forward<F>(op), this, std::forward<Args>(a)...);
}

// TODO: create mbuffs out of thin air using @key and @val_buff when possible to
// avoid copies.
template<typename RT>
__attribute__((warn_unused_result))
int
uDepotSalsa<RT>::get(const char key[],const size_t key_size,
                         char val_buff[], const size_t val_buff_size,
                         size_t &val_size_read, size_t &val_size)
{
	int err;
	size_t key_copied;

	Mbuff *key_mb = mb_cache_m.mb_get();
	Mbuff *val_mb = mb_cache_m.mb_get();
	if (key_mb == nullptr || val_mb == nullptr) {
		err =  ENOMEM;
		goto end;
	}

	if (key_mb->get_free_size() < key_size) {
		mbuff_add_buffers(*key_mb, key_size);
		if (key_mb->get_free_size() < key_size) {
			err = ENOMEM;
			goto end;
		}
	}

	key_copied = key_mb->append_from_buffer(key, key_size);
	if (key_copied != key_size) {
		err = ENOMEM;
		goto end;
	}

	err = get(*key_mb, *val_mb);
	if (err) {
		goto end;
	}

	val_size = val_mb->get_valid_size();
	val_size_read = val_mb->copy_to_buffer(0, val_buff, val_buff_size);

end:
	if (key_mb)
		mb_cache_m.mb_put(key_mb);
	if (val_mb)
		mb_cache_m.mb_put(val_mb);
	return err;
}

// TODO: create mbuffs out of thin air using @key and @val_buff when possible to
// avoid copies.
template<typename RT>
__attribute__((warn_unused_result))
int
uDepotSalsa<RT>::put(const char key[], const size_t key_size, const char val[], const size_t val_size)
{
	int err;
	const size_t prefix_size = putKeyvalPrefixSize();
	const size_t suffix_size = putKeyvalSuffixSize();
	const size_t total_size = prefix_size + key_size + val_size + suffix_size;
	size_t copied;

	Mbuff *kv = mb_cache_m.mb_get();
	if (!kv)
		return ENOMEM;

	if (kv->get_free_size() < total_size) {
		mbuff_add_buffers(*kv, total_size);
		if (kv->get_free_size() < total_size) {
			err = ENOMEM;
			goto end;
		}
	}

	// add space for prefix
	kv->append_uninitialized(prefix_size);

	// copy key
	copied = kv->append_from_buffer(key, key_size);
	if (copied != key_size) {
		err = ENOMEM;
		goto end;
	}

	// copy val
	copied = kv->append_from_buffer(val, val_size);
	if (copied != val_size) {
		err = ENOMEM;
		goto end;
	}

	// clear space for prefix, so that put() can prepend
	kv->reslice(key_size + val_size, prefix_size);

	err = put(*kv, key_size);


#ifdef	_UDEPOT_DATA_DEBUG_VERIFY
        {
                char val_out[val_size];
                size_t size_read, size_out;
                int rc2 = get(key, key_size, val_out, val_size, size_read, size_out);
                assert(0 == rc2 && size_read == val_size && size_out == size_read);
                assert(0 == memcmp(val_out, val, val_size));
        }
#endif
end:
	mb_cache_m.mb_put(kv);
	return err;
}

template<typename RT>
__attribute__((warn_unused_result))
int
uDepotSalsa<RT>::del(const char key[], const size_t key_size)
{
	int err;
	size_t copied;
	Mbuff *kv = mb_cache_m.mb_get();
	if (!kv)
		return ENOMEM;
	if (kv->get_free_size() < key_size) {
		mbuff_add_buffers(*kv, key_size);
		if (kv->get_free_size() < key_size) {
			err = ENOMEM;
			goto end;
		}
	}
	// copy key
	copied = kv->append_from_buffer(key, key_size);
	if (copied != key_size) {
		err = ENOMEM;
		goto end;
	}

	err = del(*kv);
end:
	mb_cache_m.mb_put(kv);

	return err;
}

// for crash recovery
template<typename RT>
int uDepotSalsa<RT>::try_restore_entry(const uDepotSalsaStore &s, const u64 grain_start, u64 *inval_grain_nr)
{
	HashEntry *trgt = nullptr;
	const u64 h = HashFn::hash(s.buf, s.key_size);
	uDepotMap<RT> *const udm = this->map_m.hash_to_map(h);
	uDepotSalsaStore old_md = { 0 }; // only valid if key exists
	Mbuff *const keysrc = mb_cache_m.mb_get();
	if (nullptr == keysrc) {
		return ENOMEM;
	}
	if (keysrc->get_free_size() < s.key_size) {
		mbuff_add_buffers(*keysrc, s.key_size);
		if (keysrc->get_free_size() < s.key_size) {
			mb_cache_m.mb_put(keysrc);
			return ENOMEM;
		}
	}
	size_t copied = keysrc->append_from_buffer(s.buf, s.key_size);
	if (copied != s.key_size) {
		mb_cache_m.mb_put(keysrc);
		return ENOMEM;
	}
	Mbuff *const keymb = mb_cache_m.mb_get();
	if (nullptr == keymb) {
		mb_cache_m.mb_put(keysrc);
		return ENOMEM;
	}
	int rc = 0;
	udm->lock(h);
	std::tie(rc, trgt) = lookup_mbuff_put(h, *keysrc, 0, s.key_size, old_md, *keymb);
	udm->unlock(h);
	mb_cache_m.mb_put(keymb);
	mb_cache_m.mb_put(keysrc);
	bool update = false;
	switch (rc) {
	case ENOSPC:
		rc = map_m.try_shift(h , &trgt);
		if (0 != rc) {
			// try to grow
			rc = map_m.grow();
			if (0 != rc) {
				UDEPOT_ERR("grow failed with %d.", rc);
				return ENOSPC;
			}
		}
		break;
	case ENODATA: // not found, insert from scratch
		break;
	case EEXIST:
		// update only if
		assert(nullptr != trgt);
		if (is_pba_order_equal_to_total_order(trgt->pba, grain_start, old_md)) {
			UDEPOT_DBG("new pba=%lu preserves total order. Replacing data at pba=%lu",
				grain_start, trgt->pba);
			update = true;
			const u64 old_grains = kv_tot_grains_get(old_md.key_size, old_md.val_size);
			*inval_grain_nr = old_grains; // report invalidate grains
		} else {
			// old entry is the one that preserves total order
			// cancel this entry and return success
			UDEPOT_DBG("existing pba=%lu preserves total order. Ignoring data at pba=%lu",
				trgt->pba, grain_start);
			const u64 old_grains = kv_tot_grains_get(s.key_size, s.val_size);
			// SalsaCtlr not initialized yet, have to used scm directly
			scm_m->invalidate_grains(grain_start, old_grains, false);
			return EEXIST;
		}
		break;
	case ENOMEM:
		UDEPOT_ERR("failed to restore kv pair with %s", strerror(rc));
		return rc;
	default:
		assert(0);
	}

	assert(trgt); // we have a new hash entry
	set_mapping(h, trgt, update, s.key_size, s.val_size, grain_start, old_md);
	return 0;
}

template<typename RT>
int uDepotSalsa<RT>::gc_callback_base(const u64 grain_start, const u64 grain_nr)
{
	const u64 end = (grain_start + grain_nr);
	const u64 endb = end * grain_size_m;
	auto op_gc = std::mem_fn(&uDepotSalsa::local_gc_callback);
	uDepotSalsa::gc_unit gcu;
	u64 relocb = 0;
	int rc = 0;
	for (u64 start = grain_start; 0 == rc && start < end && !exit_gc_m; ) {
		// read metadata and key
		const u64 startb = start * grain_size_m;
		const u64 sizeb = startb + sizeof(gcu) <= endb ? sizeof(gcu) : endb - startb;
		assert(sizeb <= sizeof(gcu));
		ssize_t n = udepot_io_m.pread(&gcu, sizeb, startb);
		if (n != static_cast<ssize_t>(sizeb)) {
			UDEPOT_ERR("pread ret=%ld errno=%d.", n, errno);
			rc = EIO;
			break;
		}
		if (0 == gcu.md.key_size && 0 == gcu.md.val_size)
			break;	// reached the end of the segment
		if (unlikely(endb < startb + gcu.md.val_size + gcu.md.key_size)) {
			UDEPOT_MSG("found an entry that goes out of bounds grain=%lu.", grain_start);
			// rc = EINVAL;
			break;
		}
		const u64 h = HashFn::hash(gcu.md.buf, gcu.md.key_size);

		rc = local_op_execute(h, op_gc, h, start, &gcu, relocb, false);

		start += kv_tot_grains_get(gcu.md.key_size, gcu.md.val_size);
	}
	reloc_b_m.fetch_add(relocb, std::memory_order_relaxed);
	UDEPOT_DBG("Finished reloc from %luKiB until %luKiB, relocated %luB",
		grain_start * grain_size_m >> 10, endb >> 10, relocb);
	if (0 == rc && exit_gc_m)
		rc = EAGAIN;
	return rc;
}

template<typename RT>
int uDepotSalsa<RT>::gc_callback(u64 grain_start, const u64 grain_nr)
{
	uDepot<RT>::thread_local_entry();
	int rc = 0;
	const u64 gc_sizeb = grain_nr * grain_size_m;
	const u64 gc_startb = grain_start * grain_size_m;
	// go over all key value pairs from grain_start until
	// grain_start + grain_nr and copy all valid key value pairs
	// to new locations
	UDEPOT_DBG("Starting reloc from %luKiB until %luKiB seg-idx=%lu",
		grain_start * grain_size_m >> 10, (gc_startb + gc_sizeb) >> 10,
		salsa::SalsaCtlr::grain_to_seg_idx(grain_start));
	// TODO: try with hugepages first
	// mmap in read only mode the whole region that we are about to GC
	const char *const gc_region = (const char *) udepot_io_m.mmap(
		nullptr, gc_sizeb, PROT_READ, MAP_SHARED, gc_startb);
	if (MAP_FAILED == gc_region) {
		UDEPOT_ERR("mmap failed with %d, falling back to non-mmap gc fn\n", errno);
		// fall back to non mmap gc fn (will do preads)
		rc = gc_callback_base(grain_start, grain_nr);
		uDepot<RT>::thread_local_exit();
		return rc;
	}
	u64 relocb = 0;
	auto op_gc = std::mem_fn(&uDepotSalsa::local_gc_callback);
	const char *const end = gc_region + gc_sizeb - sizeof(uDepotSalsaStore);
	for (const char *p = gc_region; p < end && 0 == rc && !exit_gc_m;) {
		const uDepotSalsa::gc_unit *const gcu = (const uDepotSalsa::gc_unit *) p;
		if (0 == gcu->md.key_size && 0 == gcu->md.val_size)
			break;	// reached the end of the segment
		if (unlikely(end < p + gcu->md.val_size + gcu->md.key_size)) {
			UDEPOT_DBG("found an entry that goes out of bounds grain=%lu.", grain_start);
			// rc = EINVAL;
			break;
		}

		const u64 h = HashFn::hash(gcu->md.buf, gcu->md.key_size);

		rc = local_op_execute(h, op_gc, h, grain_start, gcu, relocb, true);

		grain_start += kv_tot_grains_get(gcu->md.key_size, gcu->md.val_size);
		p           += kv_tot_bytes_get(gcu->md.key_size, gcu->md.val_size);
	}
	reloc_b_m.fetch_add(relocb, std::memory_order_relaxed);
	const int rc2 __attribute__((unused)) = udepot_io_m.munmap((void *) gc_region, gc_sizeb);
	assert(0 == rc2);
	if (0 == rc && exit_gc_m)
		rc = EAGAIN;	// shutting down
	if (0 != rc)
		UDEPOT_MSG("Failed reloc for seg-idx=%lu w/%s",
			salsa::SalsaCtlr::grain_to_seg_idx(grain_start), strerror(rc));
	// UDEPOT_MSG("Exiting reloc at %luKiB seg-idx=%lu at ",
	// 	grain_start * grain_size_m >> 10, salsa::SalsaCtlr::grain_to_seg_idx(grain_start));
	uDepot<RT>::thread_local_exit();
	return rc;
}

template<typename RT>
int
uDepotSalsa<RT>::local_gc_callback(const u64 h, const u64 start,
				const uDepotSalsa::gc_unit *const gcu, u64 &relocb,
				const bool mmap)
{
	HashEntry *const entry = map_m.lookup(h, start);
	UDEPOT_DBG("Lookup for h=%lu size=%u ret=%p.", h, gcu->md.key_size, entry);
	if (nullptr == entry)
		return 0;

	const u64 size_grains = kv_tot_grains_get(gcu->md.key_size, gcu->md.val_size);
	assert(entry->kv_size <= size_grains);
	if (1 == entry->deleted()) {
		assert(0 == entry->kv_size);
		// no need to move anything, purge the deleted entry
		salsa::SalsaCtlr::invalidate_grains(entry->pba, size_grains, true);
		relocb += size_grains * grain_size_m;
		map_m.purge(h, entry);
		return 0;
	}
	ssize_t n;
	const u64 payload_size = gcu->md.val_size + gcu->md.key_size + putKeyvalSuffixSize();
	const u64 stack_val_size = mmap ? 1 : payload_size;
	char val[stack_val_size];
	if (!mmap) {
		const u64 startb = start * grain_size_m;
		n = udepot_io_m.pread(val, payload_size, startb + sizeof(gcu->md));
		if (n != static_cast<ssize_t>(payload_size)) {
			UDEPOT_ERR("pread ret=%ld len=%lu offset=%lu errno=%d.",
				n, payload_size, startb + sizeof(gcu->md) + gcu->md.key_size, errno);
			return EIO;
		}
	}

	u64 grain;
	// bool merged = false;
	const int rc = salsa::SalsaCtlr::allocate_grains(size_grains, &grain, 0/*stream*/, true);
	if (0 != rc) {
		UDEPOT_ERR("allocate grains failed with %d\n", rc);
		return rc;
	}

	uDepotSalsaStoreSuffix new_sx;
	const u64 seg_idx = salsa::SalsaCtlr::grain_to_seg_idx(grain);
	const u64 timestamp = md_m.get_seg_ts(seg_idx);
	new_sx.crc16 = this->md_m.checksum16(timestamp, (const u8 *) &gcu->md, sizeof(uDepotSalsaStore));

	if (mmap) {
		const struct iovec iov[2] = {
			{.iov_base = (void *)gcu,     .iov_len = sizeof(gcu->md) + payload_size - putKeyvalSuffixSize()},
			{.iov_base = (void *)&new_sx, .iov_len = putKeyvalSuffixSize()},
		};
		n = udepot_io_m.pwritev(iov, 2, grain * grain_size_m);
	} else {
		// copy new suffix
		memcpy(val + payload_size - putKeyvalSuffixSize(), &new_sx, sizeof(new_sx));
		const struct iovec iov[2] = {
			{.iov_base = (void *)gcu, .iov_len = sizeof(gcu->md)},
			{.iov_base = (void *)val, .iov_len = payload_size},
		};
		n = udepot_io_m.pwritev(iov, 2, grain * grain_size_m);
	}
	if (n != static_cast<ssize_t>(sizeof(gcu->md) + payload_size)) {
		UDEPOT_ERR("pwrite ret=%ld errno=%d.", n, errno);
		salsa::SalsaCtlr::invalidate_grains(grain, size_grains, true);
		salsa::SalsaCtlr::release_grains(grain, size_grains, 0, true);
		return EIO;
	}

	// assert(is_pba_order_equal_to_total_order(entry->pba, grain));

	// update with new mapping
	const u64 invalidated_pba = entry->pba;
	map_m.update_inplace(h, entry, size_grains, grain);

	salsa::SalsaCtlr::release_grains(grain, size_grains, 0, true);

	// invalidate previous mapping, that's why we went through all this trouble
	salsa::SalsaCtlr::invalidate_grains(invalidated_pba, size_grains, true);
	UDEPOT_DBG("Relocated pba=%lu to new pba=%lu", invalidated_pba, grain);
	relocb += size_grains * grain_size_m;
	return 0;
}

// GC process for Memcache implementations No relocation of data, just
// dropping the entries Assuming the GC of SALSA is circular buffer =>
// we are dropping the oldest entries wrt to updates
template<typename RT>
int
uDepotSalsa<RT>::local_gc_callback_mc(const u64 h, const u64 start,
					const uDepotSalsa::gc_unit *const gcu, u64 &relocb,
					const bool mmap)
{
	HashEntry *const entry = map_m.lookup(h, start);
	UDEPOT_ERR("MC GC Lookup for h=%lu size=%u ret=%p.", h, gcu->md.key_size, entry);
	if (nullptr == entry)
		return 0;

	const u64 size_grains = kv_tot_grains_get(gcu->md.key_size, gcu->md.val_size);
	assert(entry->kv_size <= size_grains);
	if (!entry->deleted()) {
		// delete it from the hash
		kv_removed(kv_raw_bytes_get(gcu->md.key_size, gcu->md.val_size),
			kv_tot_bytes_get(gcu->md.key_size, gcu->md.val_size));
		map_m.remove(h, entry, entry->pba);
	}
	assert(entry->deleted());
	assert(0 == entry->kv_size);
	// no need to move anything, purge the deleted entry
	salsa::SalsaCtlr::invalidate_grains(entry->pba, size_grains, true);
	relocb += size_grains * grain_size_m;
	map_m.purge(h, entry);
	return 0;
}

template<>
int
uDepotSalsa<RuntimePosixODirectMC>::local_gc_callback(const u64 h, const u64 start,
						const uDepotSalsa::gc_unit *const gcu, u64 &relocb,
						const bool mmap)
{
	return local_gc_callback_mc(h, start, gcu, relocb, mmap);
}

template<>
int
uDepotSalsa<RuntimeTrtMC>::local_gc_callback(const u64 h, const u64 start,
					const uDepotSalsa::gc_unit *const gcu, u64 &relocb,
					const bool mmap)
{
	return local_gc_callback_mc(h, start, gcu, relocb, mmap);
}

#if defined(UDEPOT_TRT_SPDK)
template<>
int
uDepotSalsa<RuntimeTrtSpdkArrayMC>::local_gc_callback(const u64 h, const u64 start,
						const uDepotSalsa::gc_unit *const gcu, u64 &relocb,
						const bool mmap)
{
	return local_gc_callback_mc(h, start, gcu, relocb, mmap);
}
#endif

template<typename RT>
void
uDepotSalsa<RT>::seg_md_callback(const u64 grain_start, const u64 grain_nr)
{
	u64 ts = salsa::SalsaCtlr::get_seg_alloc_nr();
	assert(grain_nr == md_m.get_seg_md_size());
	UDEPOT_DBG("Allocating seg-idx=%lu ts=%lu local-puts=%lu",
		salsa::SalsaCtlr::grain_to_seg_idx(grain_start), ts, local_put_nr_m.load());
	md_m.persist_seg_md(grain_start, ts, type_m);
}

// KV_MbuffInterface methods
// Performs a lookup based on the given key.
//
// It finds a matching entry in the map based on fingerprint, performs
// IO to read the full key and value, and compares the read key with
// the user provided key. This continues until a matching key is found
// or we run out of entries in the map.
//
// Key is in @mb at offset @mb_key_off and has @mb_key_len length
// Uses @mb_dst to read from the device.
//
//  - returns EEXIST and the hash entry of the key was found
//  - returns ENODATA if no matching key was found, hash entry points to an
//    entry where the key can be written to
//  - returns ENOSPC if the the key was not found, but no free place exists
//    to place a new entry.
//
template<typename RT>
std::tuple<int, size_t>
uDepotSalsa<RT>::lookup_mbuff(
	const u64 key_hash, Mbuff const& mb_key, const size_t mb_key_off,
	const size_t mb_key_len, Mbuff &mb_dst, HashEntry *trgt_out)
{
	int err;
	uDepotMap<RT> *const udm = this->map_m.hash_to_map(key_hash);
	HashEntry *trgt = nullptr;
	HashEntry trgtcpy(0, 0);
	std::array<u64, _UDEPOT_HOP_SCOTCH_BUCKET_SIZE> pbas_visited;
	u32 lookup_nr = 0;
	do {
		std::tie(err, trgt) = lookup_common(key_hash, lookup_nr, pbas_visited);
		if (0 != err)
			return std::make_tuple(err, 0);
		if (nullptr == trgt)
			break;	// our work here is done, searched all possible matches

		trgtcpy = *trgt;
		udm->unlock(key_hash);

		const u64 offset = trgtcpy.pba * grain_size_m;
		// compute IO size, reset and resize mbuff (if needed)
		const u64 io_size = trgtcpy.kv_size * grain_size_m;
		const u64 alignment = std::min(512UL, grain_size_m);
		const u64 io_size_aligned = align_up(io_size, alignment);
		mb_dst.reslice(0);
		mbuff_add_buffers(mb_dst, io_size_aligned);
		if (mb_dst.get_free_size() < io_size_aligned) {
			UDEPOT_ERR("mbuff_add_buffers() failed: free:%zd needed:%zd\n", mb_dst.get_free_size(), io_size_aligned);
			udm->lock(key_hash);
			return std::make_tuple(ENOMEM, 0);
		}

		// perform read
		size_t bytes = 0;
		std::tie(err, bytes) = io_pread_mbuff_append_full(udepot_io_m, mb_dst, io_size_aligned, offset);
		if (err) {
			UDEPOT_ERR("reading into mbuff failed: %s (%d)", strerror(err), err);
			udm->lock(key_hash);
			return std::make_tuple(EIO, 0);
		}
		assert(io_size_aligned == bytes && bytes == mb_dst.get_valid_size());
		uDepotSalsaStore md;
		mb_dst.copy_to_buffer(0, (void *) &md, sizeof(md));

		#if !defined(NDEBUG)
		read_io_b_m.fetch_add(io_size_aligned, std::memory_order_relaxed);
		#endif
		bool match = md.key_size == mb_key_len;
		if (match)
			match = 0 == Mbuff::mem_compare(mb_key, mb_key_off,
		                                     mb_dst, putKeyvalPrefixSize(),
		                                     mb_key_len);
		if (match) {
			// check size, might need to read more
			if (unlikely(io_size != grain_size_m * kv_tot_grains_get(md.key_size, md.val_size))) {
				u64 new_io_size = align_up(kv_tot_grains_get(md.key_size, md.val_size) * grain_size_m, alignment);
				UDEPOT_DBG("KV size (%luB) at grain=%lu bigger than what's stored in the hash table (%luB).",
					kv_tot_raw_bytes_get(md.key_size, md.val_size), offset / grain_size_m, trgtcpy.kv_size * grain_size_m);
				UDEPOT_DBG("need to read more data");
				mb_dst.reslice(0);
				mbuff_add_buffers(mb_dst, new_io_size);
				if (mb_dst.get_free_size() < new_io_size) {
					UDEPOT_ERR("mbuff_add_buffers() failed: free:%zd needed:%zd\n",
						mb_dst.get_free_size(), new_io_size);
					udm->lock(key_hash);
					return std::make_tuple(ENOMEM, 0);
				}
				// TODO: read from where we left off
				std::tie(err, bytes) = io_pread_mbuff_append_full(udepot_io_m, mb_dst, new_io_size, offset);
				if (err) {
					UDEPOT_ERR("reading into mbuff failed: %s (%d)", strerror(err), err);
					udm->lock(key_hash);
					return std::make_tuple(EIO, 0);
				}
				assert(new_io_size == bytes && bytes == mb_dst.get_valid_size());
				#if !defined(NDEBUG)
				read_io_b_m.fetch_add(new_io_size, std::memory_order_relaxed);
				#endif
			}
			udm->lock(key_hash);
			*trgt_out = trgtcpy;
			return std::make_tuple(EEXIST, (u32) md.val_size);
		}
		// update visited so far (this is to avoid double reading shifted entries)
		update_visited_pba(trgtcpy.pba, lookup_nr, pbas_visited);
		#if !defined(NDEBUG)
		false_positive_lookups_m.fetch_add(1, std::memory_order_relaxed);
		#endif

		udm->lock(key_hash);
	} while (1);

	return std::make_tuple(ENODATA, 0);
}

// Performs a lookup based on the given key for a put operations

template<typename RT>
std::tuple<int, HashEntry *>
uDepotSalsa<RT>::lookup_mbuff_put(
	const u64 key_hash, Mbuff const& mb_key, const size_t mb_key_off,
	const size_t mb_key_len, uDepotSalsaStore &old_md, Mbuff &mb_dst)
{
	int err;
	uDepotMap<RT> *const udm = this->map_m.hash_to_map(key_hash);
	HashEntry *trgt = nullptr, trgtcpy(0, 0);
	std::array<u64, _UDEPOT_HOP_SCOTCH_BUCKET_SIZE> pbas_visited;
	u32 lookup_nr = 0;
	do {
		std::tie(err, trgt) = lookup_common(key_hash, lookup_nr, pbas_visited);
		if (0 != err)
			return std::make_tuple(err, trgt);
		if (nullptr == trgt)
			break;	// our work here is done, searched all possible matches

		trgtcpy = *trgt;
		udm->unlock(key_hash);

		const u64 offset = trgtcpy.pba * grain_size_m;
		// compute IO size, reset and resize mbuff (if needed)
		const u64 io_size = putKeyvalPrefixSize() + mb_key_len;
		const u64 alignment = std::min(512UL, grain_size_m);
		const u64 io_size_aligned = align_up(io_size, alignment);
		mb_dst.reslice(0);
		mbuff_add_buffers(mb_dst, io_size_aligned);
		if (mb_dst.get_free_size() < io_size_aligned) {
			UDEPOT_ERR("mbuff_add_buffers() failed: free:%zd needed:%zd\n", mb_dst.get_free_size(), io_size_aligned);
			udm->lock(key_hash);
			return std::make_tuple(ENOMEM, nullptr);
		}
		// perform read
		std::tie(err, std::ignore) = io_pread_mbuff_append_full(udepot_io_m, mb_dst, io_size_aligned, offset);
		if (err) {
			UDEPOT_ERR("reading into mbuff failed: %s (%d)", strerror(err), err);
			udm->lock(key_hash);
			return std::make_tuple(err, nullptr);
		}
		assert(io_size_aligned == mb_dst.get_valid_size());
		#if !defined(NDEBUG)
		read_io_b_m.fetch_add(io_size_aligned, std::memory_order_relaxed);
		#endif
		uDepotSalsaStore md;
		mb_dst.copy_to_buffer(0, (void *) &md, sizeof(md));
		bool match = md.key_size == mb_key_len;
		if (match)
			match = 0 == Mbuff::mem_compare(mb_key, mb_key_off,
							mb_dst, putKeyvalPrefixSize(),
							mb_key_len);
		if (match)
			old_md = md;
		udm->lock(key_hash);
		// check if entry has changed
		if (0 != memcmp(trgt, &trgtcpy, sizeof(*trgt))) {
			// hash entry pointer changed, get its up-to-date version
			trgt = map_m.lookup(key_hash, trgtcpy.pba);
			// 3 cases:
			// a) a delete happened
			// b) another insert happened and updated the entry
			if (match && nullptr == trgt) {
				// our change has been superseded by a concurrent update
				// return std::make_tuple(EALREADY, nullptr);
				match = false; // continue search
			}
			// c) the entry has been shifted, continue as usual
		}

		if (match)
			return std::make_tuple(EEXIST, trgt);

		// update visited so far
		update_visited_pba(trgtcpy.pba, lookup_nr, pbas_visited);
	        #if !defined(NDEBUG)
		false_positive_lookups_m.fetch_add(1, std::memory_order_relaxed);
		#endif
	} while (1);

	// No entry found. Try to allocate one.
	err = map_m.next_free(key_hash, &trgt);
	if (err) {
		assert(err == ENOSPC); // currently that's the only error next_free() returns.
		return std::make_tuple(ENOSPC, nullptr);
	}

	return std::make_tuple(ENODATA, trgt);
}

template<typename RT>
int
uDepotSalsa<RT>::mbuff_prepend_append_md(Mbuff &keyval, const u64 key_size, const u64 val_size, const u64 grain)
{
	// prepend the IO header
	bool prepend_ok = false, append_ok = false;
	const u64 seg_idx = salsa::SalsaCtlr::grain_to_seg_idx(grain);
	const u64 timestamp = md_m.get_seg_ts(seg_idx);
	const uDepotSalsaStore md_base = {.key_size=(u16)key_size, .val_size=(u32)val_size, .timestamp=timestamp};
	auto fn =
		[&prepend_ok, &md_base]
		(unsigned char *hdr, size_t hdr_len) -> size_t {
			uDepotSalsaStore *md;
			if (hdr_len != sizeof(*md)) {
				UDEPOT_ERR("prepend to mbuff failed. available prepend space: %zd != %zd", hdr_len, sizeof(*md));
				return 0;
			}
			md = (uDepotSalsaStore *)hdr;
			md->key_size = md_base.key_size;
			md->val_size = md_base.val_size;
			md->timestamp = md_base.timestamp;
			prepend_ok = true;
			return sizeof(*md);
		};
	auto append_fn =
		[&append_ok, this, &md_base, &timestamp]
		(unsigned char *ftr, size_t ftr_len) -> size_t {
			uDepotSalsaStoreSuffix *mds;
			if (ftr_len < sizeof(*mds)) {
				UDEPOT_ERR("prepend to mbuff failed. available append space: %zd != %zd", ftr_len, sizeof(*mds));
				return 0;
			}
			mds = (uDepotSalsaStoreSuffix *)ftr;
			// TODO: add seed based on pba
			mds->crc16 = this->md_m.checksum16(timestamp, (const u8 *) &md_base, sizeof(uDepotSalsaStore));
			UDEPOT_DBG("writing crc16=0x%x", mds->crc16);
			append_ok = true;
			return sizeof(*mds);
		};

	keyval.prepend_inline(std::ref(fn));
	if (!prepend_ok) // make sure that an error is printed out if prepend fails
		UDEPOT_ERR("prepend to mbuff failed.");

	keyval.append_inline(std::ref(append_fn));
	if (!append_ok) // make sure that an error is printed out if prepend fails
		UDEPOT_ERR("append to mbuff failed.");

#ifdef	_UDEPOT_DATA_DEBUG_VERIFY
	const u16 crc16 = md_m.checksum16(timestamp, (const u8 *) &md_base, sizeof(uDepotSalsaStore));
	assert(0 == keyval.mem_compare_buffer(putKeyvalPrefixSize() + key_size + val_size, (char *) &crc16, sizeof(crc16)));
#endif
	// at this point we can always copy if we want to perfrom the operation.
	// However, this should not happen, so if there is a case that it happens I
	// would like to know about it.
	return (prepend_ok && append_ok) ? 0 : EINVAL;
}

// set a mapping:
//   update == true  -> update an existing mapping
//   update == false -> insert a new one
template<typename RT>
void uDepotSalsa<RT>::set_mapping(const u64 h, HashEntry *trgt, const bool update,
                                const size_t key_len, const size_t val_len,
                                const u64 pba, uDepotSalsaStore const& old_md)
{
	if (update) { // update an existing entry
		assert(trgt->used() && !trgt->deleted());
		const u64 old_grains = kv_tot_grains_get(old_md.key_size, old_md.val_size);
		const u64 old_tot_bytes = kv_tot_bytes_get(old_md.key_size, old_md.val_size);
		const u64 old_raw_bytes = kv_raw_bytes_get(old_md.key_size, old_md.val_size);
		assert(0 < old_grains && 0 < old_raw_bytes);
		scm_m->invalidate_grains(trgt->pba, old_grains, false);
		kv_removed(old_raw_bytes, old_tot_bytes);
		map_m.update_inplace(h, trgt, kv_tot_grains_get(key_len, val_len), pba);
	} else {
		if (trgt->deleted()) {
			// if entry was deleted, have to invalidate the
			// tombstone that the del op had performed
			const u64 old_grains = kv_tot_grains_get(old_md.key_size, old_md.val_size);
			assert(0 == trgt->kv_size && 0 < old_grains);
			scm_m->invalidate_grains(trgt->pba, old_grains, false);
		}
		// map insert
		map_m.insert(h, trgt, kv_tot_grains_get(key_len, val_len), pba);
	}
	// update stats
	kv_added(kv_raw_bytes_get(key_len, val_len), kv_tot_bytes_get(key_len, val_len));
}

template<typename RT>
int uDepotSalsa<RT>::mbuff_append_padding(Mbuff &keyval, const u64 padding)
{
	if (keyval.append_avail_size() < padding) {
		assert(keyval.get_free_size() < padding);
		mbuff_add_buffers(keyval, padding - keyval.get_free_size());
	}

	if (keyval.append_avail_size() < padding) {
		UDEPOT_DBG("mbuff_add_buffers() failed. Available size:%zd required:%" PRIu64 , keyval.append_avail_size(), padding);
		return ENOMEM;
	}

	UDEPOT_DBG("Padding: valid_size:%zd padding:%zd", keyval.get_valid_size(), padding);
	size_t remaining = padding;
	keyval.append(
		[&remaining]
		(unsigned char *b, size_t b_len) -> size_t {
			size_t pad_len = std::min(b_len, remaining);
			if (pad_len == 0)
				return 0;
			remaining -= pad_len;
			// This is not needed, we can padd with garbage if we want to
			std::memset(b, 0, pad_len);
			return pad_len;
		}
	);

	if (remaining) {
		UDEPOT_DBG("Unexpected error: remaining=%ld", remote_put_nr_m.load());
		return ENOMEM;
	}

	return 0;
}

// Perform an PUT operation using a user-provided Mbuff.
//
// The keyval Mbuff contains the concatenation of key and value
//
// We want to avoid copies:
//  - caller is expected to use IO-aligned buffers for the Mbuffs
//  - caller is expected to leave putKeyvalPrefixSize() at the begining of the
//    buffer to prepend metadata
template<typename RT>
int
uDepotSalsa<RT>::local_put_mbuff(
	const u64 h, Mbuff &keyval, const size_t key_size,
	const u64 grain, const PutOp op, u64 *const old_pba_out)
{
	int rc;
	bool update = false;
	HashEntry *trgt;
	Mbuff key_mb(mbuff_type_index()); // (temp) Mbuff for reading key

	// move free buffers from keyval to key_mb to do the lookup
	// We do this to avoid buffer allocation in lookup_mbuff()
	size_t io_size = align_up(putKeyvalPrefixSize() + key_size, 512);
	const size_t val_len = keyval.get_valid_size() - key_size;
	keyval.move_free_buffs_to(key_mb, io_size);

	// lookup key
	uDepotSalsaStore old_md = { 0 }; // only valid if key exists
	std::tie(rc, trgt) = lookup_mbuff_put(h, keyval, 0, key_size, old_md, key_mb);
	// no need for key_mb anymore. Move free buffs back to keyval
	key_mb.reslice(0);
	key_mb.move_free_buffs_to(keyval);
	// if (UDEPOT_SALSA_MAX_KV_SIZE <= udiv_round_up(keyval.get_valid_size(), grain_size_m))
	// 	UDEPOT_MSG("large KV pair with val size=%lu", val_len);
	switch (rc) {
	case ENODATA:		// not found
		if (REPLACE == op)
			return EINVAL; // new entry can safely be ignored
		// if (trgt->deleted() && CREATE == op) {
		// 	// TODO: unrealistic to read and verify for deleted entries
		// }
		break;
	case EEXIST:		// update
		if (CREATE == op) {
			return EINVAL; // new entry can safely be ignored
		}
		update = true;
		break;
	case EIO:
		UDEPOT_ERR("lookup returned err=%d", rc);
		return rc;
	case ENOSPC:
		rc = map_m.try_shift(h , &trgt);
		if (0 != rc)
			return rc; // differentiate from ENOSPC from allocate_grains
		// if shifted we have to invalidate its grains
		assert(trgt->deleted() ^ !trgt->used());
		if (REPLACE == op)
			return EINVAL; // requests asked to replace an
				       // existing key, key does not exist
		break;
	default:
		UDEPOT_ERR("lookup_mbuff returned %s (%d)", strerror(rc), rc);
		return rc;
	}

	assert(trgt); // we have a new hash entry

	#if !defined(NDEBUG)
	local_put_nr_m.fetch_add(1, std::memory_order_relaxed); // NB: This will also count rollbacks
	#endif

	if (update) {
		assert(trgt->used() && !trgt->deleted());
		if (!is_pba_order_equal_to_total_order(trgt->pba, grain, old_md)) {
//#ifdef DEBUG
			const u64 old_pba = trgt->pba;
			const u64 old_seg_idx = salsa::SalsaCtlr::grain_to_seg_idx(old_pba);
			const u64 new_seg_idx = salsa::SalsaCtlr::grain_to_seg_idx(grain);
			const u64 old_ts = old_md.timestamp;
			const u64 new_ts = md_m.get_seg_ts(new_seg_idx);
			UDEPOT_MSG("Race with another write/del: have to retry to preserve total order for restore"\
				"new-pba=%lu old-pba=%lu new-seg=%lu old-seg=%lu new-ts=%lu old-ts=%lu tid=%u",
				grain, old_pba, new_seg_idx, old_seg_idx, new_ts, old_ts, ThreadId::get());
//#endif
			*old_pba_out = trgt->pba;
			return EALREADY; // race with GC => trigger switching to a new seg
		}
	}
	// set up propper mapping for the new key/val
	set_mapping(h, trgt, update, key_size, val_len, grain, old_md);

	return 0;
}

template<typename RT>
std::tuple<int, u64>
uDepotSalsa<RT>::local_put_mbuff_io(const u64 h, Mbuff &keyval, const size_t key_size)
{
	assert(keyval.get_valid_size() >= key_size);

	size_t val_len = keyval.get_valid_size() - key_size;
	int err;

	size_t io_size = keyval.get_valid_size() + putKeyvalPrefixSize() + putKeyvalSuffixSize();
	size_t io_size_grains = udiv_round_up(io_size, grain_size_m);

	// allocate storage space
	u64 io_grain0; // first grain for the IO operation
	err = salsa::SalsaCtlr::allocate_grains(io_size_grains, &io_grain0, salsa_stream_get_id(h));
	if (err) {
		if (EAGAIN != err)
			UDEPOT_ERR("failed to allocate grains: %s (%d)", strerror(err), err);
		return std::make_tuple(err, SALSA_INVAL_GRAIN);
	}

	// append metadata to mbuff
	err = mbuff_prepend_append_md(keyval, key_size, val_len, io_grain0);
	if (err) {
		UDEPOT_ERR("mbuff_prepend_append_md failed with %d", err);
		salsa::SalsaCtlr::invalidate_grains(io_grain0, io_size_grains, false);
		salsa::SalsaCtlr::release_grains(io_grain0, io_size_grains, salsa_stream_get_id(h));
		return std::make_tuple(err, SALSA_INVAL_GRAIN);
	}
	assert(keyval.get_valid_size() == putKeyvalPrefixSize() + key_size + val_len + putKeyvalSuffixSize());

	// append necessary padding to the mbuff to align the IO write to grain size
	err = mbuff_append_padding(keyval, io_size_grains*grain_size_m - io_size);
	if (err) {
		UDEPOT_ERR("mbuff_append_padding failed with %d", err);
		salsa::SalsaCtlr::invalidate_grains(io_grain0, io_size_grains, false);
		salsa::SalsaCtlr::release_grains(io_grain0, io_size_grains, salsa_stream_get_id(h));
		return std::make_tuple(ENOMEM, SALSA_INVAL_GRAIN);
	}

	// perform write
	assert(keyval.get_valid_size() == io_size_grains * grain_size_m);
	size_t bytes;
	std::tie(err, bytes) = io_pwrite_mbuff_full(
	                                   udepot_io_m,
	                                   keyval.get_valid_size(),
	                                   io_grain0 * grain_size_m,
	                                   keyval);
	if (err) {
		UDEPOT_ERR("io_pwrite_mbuff_full of mbuff failed: %s (%d) off=%lu size=%lu",
			strerror(errno), err, io_grain0 * grain_size_m, keyval.get_valid_size());
		salsa::SalsaCtlr::invalidate_grains(io_grain0, io_size_grains, false);
		salsa::SalsaCtlr::release_grains(io_grain0, io_size_grains, salsa_stream_get_id(h));
		return std::make_tuple(0 < errno ? errno : err, io_grain0);
	}
	assert(bytes == io_size_grains * grain_size_m);
	#if !defined(NDEBUG)
	write_io_b_m.fetch_add(keyval.get_valid_size(), std::memory_order_relaxed);
	#endif
	return std::make_tuple(0, io_grain0);
}

template<typename RT>
__attribute__((warn_unused_result))
bool
uDepotSalsa<RT>::is_pba_order_equal_to_total_order(const u64 old_pba, const u64 new_pba, const uDepotSalsaStore &old_md) const
{
	const u64 old_seg_idx = scm_m->grain_to_seg_idx(old_pba);
	const u64 new_seg_idx = scm_m->grain_to_seg_idx(new_pba);
	if (old_seg_idx != new_seg_idx) {
		const u64 old_ts = old_md.timestamp;
		const u64 new_ts = md_m.get_seg_ts(new_seg_idx);
		// TODO: account for wrap-around
		assert(0 < old_ts && 0 < new_ts && old_ts != new_ts);
		return old_ts < new_ts;
	}
	assert(old_pba != new_pba);
	return old_pba < new_pba;
}

template<typename RT>
__attribute__((warn_unused_result))
int
uDepotSalsa<RT>::put(Mbuff &keyval, const size_t key_size, const PutOp op)
{
	u64 h;
	int rc = HashFn::hash(h, keyval, 0, key_size);
	if (rc)
		return rc;

	typename RT::Net::Client *client = get_client(h);
	UDEPOT_DBG("PUT key_size=%lu h=%lu client:%p.", key_size, h, client);
	if (client) {
		#if !defined(NDEBUG)
		remote_put_nr_m.fetch_add(1);
		#endif
		PROBE_TICKS_START(remote_put_mbuff);
		int xret = client->remote_put(keyval, key_size);
		PROBE_TICKS_END(remote_put_mbuff);
		return xret;
	}
	// 1. allocate a grain and do the lock-free write I/O: it's out of place
	const size_t size = keyval.get_valid_size();
	u64 grain = SALSA_INVAL_GRAIN;
	const u64 size_grains = kv_tot_grains_get(key_size, size - key_size);
	// 1. allocate a grain and do the lock-free write I/O: it's out of place
	do {
		do {
			std::tie(rc, grain) = local_put_mbuff_io(h, keyval, key_size);
			keyval.reslice(size, putKeyvalPrefixSize());
			if (EAGAIN == rc) {
				UDEPOT_DBG("received EAGAIN, sleeping and re-trying");
				//std::this_thread::sleep_for(std::chrono::milliseconds(1));
				RT::Sched::yield();
				continue;
			}
			if (0 != rc) {
				UDEPOT_ERR("local put io returned %d", rc);
				return rc;
			}
		} while (EAGAIN == rc);
		assert(SALSA_INVAL_GRAIN != grain);
		// auto st_start = uDepotStats::put_start();
		PROBE_TICKS_START(local_put_mbuff);
		do {
			u64 old_pba = -1;
			rc = local_op_execute(h, local_put_mbuff_m, h, keyval, key_size, grain, op, &old_pba);
			switch (rc) {
			case 0:
				break;	// good path
			case EINVAL:
				break;	// REPLACE or CREATE was requested
			case ENOSPC:
			{
				const int rc2 = map_m.grow();
				// retry if grow() succeeded or returned EAGAIN
				if (0 == rc2 || EAGAIN == rc2)
					rc = EAGAIN;
				else
					UDEPOT_ERR("grow failed with %d.", rc2);
				break;
			}
			case EALREADY:
			{
				// total order not preserved, have to retry write on new
				// location, otherwise crash recovery will not work in a
				// consistent manner
				const u64 old_seg_idx = salsa::SalsaCtlr::grain_to_seg_idx(old_pba);
				const u64 new_seg_idx = salsa::SalsaCtlr::grain_to_seg_idx(grain);
				if (old_seg_idx != new_seg_idx) {
					UDEPOT_MSG("Total order not preserved, draining seg=%lu (old-seg=%lu) and retrying",
						new_seg_idx, old_seg_idx);
					salsa::SalsaCtlr::drain_remaining_grains(salsa_stream_get_id(h));
				}
				break;
			}
			default:
				UDEPOT_ERR("local_put failed with %s", strerror(rc));
			}
		} while (EAGAIN == rc);
		if (0 != rc) {
			// grain won't be used, invalidate
			salsa::SalsaCtlr::invalidate_grains(grain, size_grains, false);
		}
		salsa::SalsaCtlr::release_grains(grain, size_grains, salsa_stream_get_id(h));
	} while (EALREADY == rc);
	// uDepotStats::put_stop(st_start);
	PROBE_TICKS_END(local_put_mbuff);

	return rc;
}

template<typename RT>
int uDepotSalsa<RT>::local_get_mbuff(const u64 key_hash, Mbuff const& key, Mbuff &val_out)
{
	int err;
	size_t val_size;
	HashEntry trgt(0, 0);

	#if !defined(NDEBUG)
	local_get_nr_m.fetch_add(1, std::memory_order_relaxed);
	#endif

	const size_t key_size = key.get_valid_size();
	std::tie(err, val_size) = lookup_mbuff(key_hash, key, 0, key_size, val_out, &trgt);
	if (err == EEXIST) {
		// if sucessful, reslice val_out to point to value
		const u64 alignment __attribute__((unused)) = std::min(512UL, grain_size_m);
		err = 0;
		assert(0 < val_size);
		assert(trgt.kv_size <= val_out.get_valid_size() / grain_size_m);
		val_out.reslice(val_size, putKeyvalPrefixSize() + key_size);
	} else
		val_out.reslice(0); // val_out holds no valid data

	return err;
}

template<typename RT>
__attribute__((warn_unused_result))
int
uDepotSalsa<RT>::get(Mbuff const& key, Mbuff &val_out)
{
	u64 h;
	int err = HashFn::hash(h, key);
	if (err)
		return err;

	typename RT::Net::Client *cli = get_client(h);
	UDEPOT_DBG("GET key_size=%lu h=%lu cli=%p.", key.get_valid_size(), h, cli);
	if (cli) {
		#if !defined(NDEBUG)
		remote_get_nr_m.fetch_add(1);
		#endif
		//STimer t("REMOTE GET");
		PROBE_TICKS_START(remote_get_mbuff);
		int xret = cli->remote_get(key, val_out);
		PROBE_TICKS_END(remote_get_mbuff);
		return xret;
	}

	// auto st_start = uDepotStats::get_start();
	PROBE_TICKS_START(local_get_mbuff);

	//STimer t("LOCAL GET");
	auto ret = local_op_execute(h, local_get_mbuff_m, h, key, val_out);

	// uDepotStats::get_stop(st_start);
	PROBE_TICKS_END(local_get_mbuff);
	return ret;
}

// Perform an DEL operation
//
// The key Mbuff contains the key
//
template<typename RT>
int
uDepotSalsa<RT>::local_del_mbuff(const u64 h, Mbuff const &key)
{
	// local del
	// map lookup (read I/O in case we have one or more partial matches)
	int rc;
	HashEntry *trgt = nullptr;
	Mbuff key_mb(mbuff_type_index()); // (temp) Mbuff for reading key
	const size_t key_size = key.get_valid_size();
	const size_t io_size = align_up(putKeyvalPrefixSize() + key_size + putKeyvalSuffixSize(), 512);
	uDepotSalsaStore old_md = { 0 }; // only valid if key exists
	#if !defined(NDEBUG)
	local_del_nr_m.fetch_add(1, std::memory_order_relaxed); // NB: This will also count rollbacks
	#endif
	key.move_free_buffs_to(key_mb, io_size);
	std::tie(rc, trgt) = lookup_mbuff_put(h, key, 0, key_size, old_md, key_mb);
	if (EEXIST != rc) {
		// DEL will not do anything in this case: key was not found
		key_mb.reslice(0);
		key_mb.move_free_buffs_to(key);
		switch (rc) {
		case ENOSPC:
			UDEPOT_DBG("lookup returned err=%d", rc);
			return ENODATA;	// this is same as ENODATA for DEL, didn't find it
		case EIO:
			UDEPOT_ERR("lookup returned EIO err=%d", rc);
		case ENODATA:
			return rc;
		default:
			assert(0);
		}
		return ENOENT;
	}
	assert(trgt); // we have a new hash entry

	// allocate and write tombstone
	assert(key_mb.get_valid_size() >= putKeyvalPrefixSize() + key_size);
	u64 grain = SALSA_INVAL_GRAIN;
	u32 __attribute__((unused)) retries = 0;
	const u64 size_grains = kv_tot_grains_get(key_size, 0);

	do {
		key_mb.reslice(key_size, putKeyvalPrefixSize());
		std::tie(rc, grain) = local_put_mbuff_io(h, key_mb, key_size);
		if (EAGAIN == rc) {
			UDEPOT_DBG("received EAGAIN, sleeping and re-trying");
			RT::Sched::yield();
		}
	} while (EAGAIN == rc);
	key_mb.reslice(0);
	key_mb.move_free_buffs_to(key);
	if (0 != rc) {
		UDEPOT_ERR("local put io returned %s (%d)", strerror(rc), rc);
		return rc;
	}

	assert(SALSA_INVAL_GRAIN != grain);

	// TODO: retry if not in order
	if (!is_pba_order_equal_to_total_order(trgt->pba, grain, old_md)) {
		return EAGAIN;
	}

	// invalidate previous mapping
	const u64 old_grains = kv_tot_grains_get(old_md.key_size, old_md.val_size);
	const u64 old_tot_bytes = kv_tot_bytes_get(old_md.key_size, old_md.val_size);
	const u64 old_raw_bytes = kv_raw_bytes_get(old_md.key_size, old_md.val_size);
	assert(0 < old_grains);
	salsa::SalsaCtlr::invalidate_grains(trgt->pba, old_grains, false);

	// update stats
	kv_removed(old_raw_bytes, old_tot_bytes);
	// update shared mapping table
	map_m.remove(h, trgt, grain);

	salsa::SalsaCtlr::release_grains(grain, size_grains, salsa_stream_get_id(h));

	return 0;
}

template<typename RT>
__attribute__((warn_unused_result))
int
uDepotSalsa<RT>::del(Mbuff const& key)
{
	u64 h;
	int rc = HashFn::hash(h, key);
	if (unlikely(rc))
		return rc;

	typename RT::Net::Client *cli = get_client(h);
	UDEPOT_DBG("DEL key_size=%lu h=%lu cli=%p.", key.get_valid_size(), h, cli);
	if (cli) {
		#if !defined(NDEBUG)
		remote_get_nr_m.fetch_add(1);
		#endif
		//STimer t("REMOTE GET");
		PROBE_TICKS_START(remote_del_mbuff);
		int xret = cli->remote_del(key);
		PROBE_TICKS_END(remote_del_mbuff);
		return xret;
	}
	PROBE_TICKS_START(local_del_mbuff);
	//STimer t("LOCAL GET");
	do {
		rc = local_op_execute(h, local_del_mbuff_m, h, key);
		if (0 != rc) {
			if (EAGAIN == rc)
				RT::Sched::yield();
			else
				UDEPOT_ERR("del returned %s", strerror(rc));
		}
	} while (EAGAIN == rc);
	PROBE_TICKS_END(local_del_mbuff);
	return rc;

}

template<typename RT>
uint32_t
__attribute__((always_inline))
uDepotSalsa<RT>::salsa_stream_get_id(const u64 h)
{
	return h % salsa_streams_nr_m;
}

template<typename RT>
uint32_t
__attribute__((always_inline))
uDepotSalsa<RT>::salsa_stream_get_id()
{
	return salsa_stream_get_id(ThreadId::get());
}

template<typename RT>
typename RT::Net::Client *
__attribute__((always_inline))
uDepotSalsa<RT>::get_client(uint64_t h) {
	return net_m.get_client(h, ThreadId::get() % thread_nr_m);
}

// instantiate the templates
template class uDepotSalsa<RuntimePosix>;
template class uDepotSalsa<RuntimePosixODirect>;
template class uDepotSalsa<RuntimeTrt>;
template class uDepotSalsa<RuntimePosixODirectMC>;
template class uDepotSalsa<RuntimeTrtMC>;
template class uDepotSalsa<RuntimeTrtUring>;
template class uDepotSalsa<RuntimeTrtUringMC>;
#if defined(UDEPOT_TRT_SPDK)
template class uDepotSalsa<RuntimePosixSpdk>;
template class uDepotSalsa<RuntimeTrtSpdk>;
template class uDepotSalsa<RuntimeTrtSpdkArray>;
template class uDepot<RuntimePosixSpdk>;
template class uDepotSalsa<RuntimeTrtSpdkArrayMC>;
#endif

}; // namespace udepot

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


#include <cerrno>
#include <thread>
#include <map>
#include <list>
#include <chrono>

#include "util/debug.h"
#include "util/types.h"
#include "uDepot/io.hh"
#include "uDepot/backend.hh"
#include "uDepot/udepot-lsa.hh"
#include "uDepot/lsa/udepot-dir-map-or.hh"
#include "uDepot/lsa/udepot-map.hh"
#include "uDepot/lsa/metadata.hh"
#include "uDepot/lsa/store.hh"

namespace udepot {

template<typename RT>
uDepotDirMapOR<RT>::uDepotDirMapOR():
	grow_nr_m(0)
{
	dir_ref_m.directory = new std::vector<DirMapEntry> (0);
	dir_ref_m.shadow_directory = nullptr;
	dir_ref_m.state = 0;
}

template<typename RT>
uDepotDirMapOR<RT>::~uDepotDirMapOR()
{
	UDEPOT_MSG("Directory Map performed %u grows .", 0 < grow_nr_m ? grow_nr_m - 1 : 0);
}

template<typename RT>
__attribute__((warn_unused_result))
int
uDepotDirMapOR<RT>::init(const u64 seg_size, const u64 grain_size)
{
	int rc = 0;
	if (nullptr == dir_ref_m.directory) {
		rc = ENOMEM;
		UDEPOT_ERR("Malloc failed.");
		goto fail0;
	}
	rc = dir_ref_m.rwpflock.init();
	if (0 != rc) {
		UDEPOT_ERR("rwlock init failed w/rc=%d.", rc);
		goto fail1;
	}

	rc = grow(seg_size, grain_size);
	if (0 != rc) {
		UDEPOT_ERR("grow failed with %s %d.", strerror(rc), rc);
		goto fail2;
	}

	return 0;
fail2:
fail1:
fail0:
	return rc;
}

template<typename RT>
__attribute__((warn_unused_result))
int
uDepotDirMapOR<RT>::grow(const u64 seg_size, const u64 grain_size)
{
	int rc = 0;
	std::vector<DirMapEntry> *const old_dir = dir_ref_m.directory.load();

	grow_lock_m.lock();
	if (old_dir != dir_ref_m.directory.load()) {
		UDEPOT_DBG("Another grow happened in the meantime, retry.");
		grow_lock_m.unlock();
		return EAGAIN;
	}

	UDEPOT_MSG("Trying to grow the directory map.");
	const u32 new_size = std::max(1UL, 2U * old_dir->size());
	std::vector<DirMapEntry> *const new_dir = new std::vector<DirMapEntry> (new_size);
	// const u64 seg_size = salsa::SalsaCtlr::get_seg_size() - md_m.get_seg_md_size();
	// const u64 grain_size = salsa::SalsaCtlr::get_grain_size();
	const u64 mmap_size_b = align_up(seg_size * grain_size, 1UL<<21);
	// const u64 mmap_size_grains = mmap_size_b / grain_size;
	u32 lock_nr;

	if (nullptr == new_dir) {
		rc = ENOMEM;
		UDEPOT_ERR("failed to allocate directory.");
		goto fail0;
	}
	lock_nr = _UDEPOT_HOP_SCOTCH_LOCK_NR;

	// 1. allocate new directory
	for (auto &dme : (*new_dir)) {
		// allocate grains
		u64 grain;
		// do {
		// 	rc = salsa::SalsaCtlr::allocate_grains(mmap_size_grains, &grain);
		// } while (EAGAIN == rc);
		if (0 != rc) {
			UDEPOT_ERR("allocate grains failed with %d\n", rc);
			goto fail1;
		}
		const u64 mmap_offset = grain * grain_size;
		dme.mm_region = udepot_io_m.mmap(nullptr, mmap_size_b, PROT_READ|PROT_WRITE, MAP_SHARED, mmap_offset);
		// dme.mm_region = mmap(nullptr, mmap_size_b, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_ANONYMOUS|MAP_HUGETLB, -1, 0);
		if (MAP_FAILED == dme.mm_region) {
			UDEPOT_ERR("mmap (size=%zd) failed with %s %d\n", mmap_size_b, strerror(errno), errno);
			// salsa::SalsaCtlr::release_grains(grain, mmap_size_grains);
			rc = ENOMEM;
			goto fail1;
		}
		UDEPOT_DBG("succesfully mmaped %luB at %p", mmap_size_b, dme.mm_region);
		dme.size_b	 = mmap_size_b;
		dme.grain_offset = grain;
		// salsa::SalsaCtlr::release_grains(grain, mmap_size_grains);
	}
	// 2. init hash tables
	for (auto &dme : (*new_dir)) {
		// adjust for directory's metadata
		const u64 size = dme.size_b;
		void *const region = (u8 *) dme.mm_region;
		rc = dme.map.init(lock_nr, size, region);
		if (0 != rc) {
			UDEPOT_ERR("hash table init failed w=%d", rc);
			goto fail2;
		}
	}
	dir_ref_m.rwpflock.write_enter();

	// switch old directory to read only mode
	for (auto &dme : (*old_dir)) {
		const int err = mprotect(dme.mm_region, dme.size_b, PROT_READ);
		if (0 != err) {
			UDEPOT_ERR("mprotect switch to read only mode failed w=%d", errno);
			rc = errno;
			goto fail3;
		}
	}

	// 3. copy old to new
	{
		std::vector<uDepotMap<RT> *> trgts(new_size);
		for (u32 i = 0; i < new_size; ++i)
			trgts[i] = &((*new_dir)[i].map);
		for (u32 i = 0; i < old_dir->size(); ++i) {
			const uDepotMap<RT> &map = (*old_dir)[i].map;
			const HashEntry *const first = &map.ht_m[0];
			const HashEntry *const last  = &map.ht_m[map.entry_nr_m];
			for (const HashEntry *p = first; p < last; ++p) {
				if (!p->used())
					continue;
				const u64 h = map.entry_keyfp(p);
				const u32 idx = h % new_size;
				HashEntry *trgt = nullptr;
				int rc = trgts[idx]->lookup(h, &trgt);
				switch (rc) {
				case  ENOSPC:
					rc = trgts[idx]->try_shift(h , &trgt);
					if (unlikely(0 != rc)) {
						UDEPOT_ERR("fail to insert to new directory");
						rc = EFAULT;
						goto fail3;
					}
					assert(trgt->deleted() ^ !trgt->used());
				case ENODATA:
					trgts[idx]->insert(trgt, h, p->kv_size, p->pba);
					assert(trgt->used());
					break;
				case 0:
					rc = trgts[idx]->next_free(h, &trgt);
					assert (nullptr != trgt);
					trgts[idx]->insert(trgt, h, p->kv_size, p->pba);
					break;
				default:
					UDEPOT_ERR("unknown error in new directory lookup");
					rc = EFAULT;
					goto fail3;
				}
				assert(h == trgts[idx]->entry_keyfp(trgt));
				assert(trgt == trgts[idx]->lookup(h, p->pba));
			}
		}
	}

#ifdef	_UDEPOT_DATA_DEBUG_VERIFY
	for (u32 i = 0; i < old_dir->size(); ++i) {
		const uDepotMap<RT> &map = (*old_dir)[i].map;
		const HashEntry *const first = &map.ht_m[0];
		const HashEntry *const last  = &map.ht_m[map.entry_nr_m];
		for (const HashEntry *p = first; p < last; ++p) {
			if (!p->used())
				continue;
			const u64 h = map.entry_keyfp(p);
			const u32 idx = h % new_size;
			assert(nullptr != (*new_dir)[idx].map.lookup(h, p->pba));
		}
	}
#endif
	// switch old directory to invalid mode
	for (auto &dme : (*old_dir)) {
		const int err = mprotect(dme.mm_region, dme.size_b, PROT_NONE);
		if (0 != err) {
			UDEPOT_ERR("mprotect switch to invalid mode failed w=%d", errno);
			rc = errno;
			goto fail3;
		}
	}

	dir_ref_m.rwpflock.write_wait_readers();

	// nobody should be holding a reference to old_dir anymore
	for (auto &dme : (*old_dir)) {
		// unmap region
		const int err = munmap(dme.mm_region, dme.size_b);
		if (0 != err) {
			UDEPOT_ERR("munmap failed w=%d", errno);
			rc = errno;
			goto fail3;
		}
		// invalidate old mapping's grains
		// salsa::SalsaCtlr::invalidate_grains(dme.grain_offset, dme.size_b / grain_size, false);
	}

	// point to new directory
	dir_ref_m.directory = new_dir;
	dir_ref_m.state = new_dir->size() * (*new_dir)[0].map.lock_nr_m;

	dir_ref_m.rwpflock.write_exit();

	grow_nr_m++;
	grow_lock_m.unlock();

	delete old_dir;

	UDEPOT_MSG("Successfully grew directory for the %u time, to a size of %lu %s.",
		grow_nr_m, new_dir->size(), 1 == new_dir->size() ? "table" : "tables");
	return 0;
fail3:
	for (auto &dme : (*old_dir)) {
		const int err = mprotect(dme.mm_region, dme.size_b, PROT_READ|PROT_WRITE);
		if (0 != err) {
			UDEPOT_ERR("mprotect switch to invalid mode failed w=%d", errno);
			assert(0); // have no way of recovering from this now, we'll have to restart the process
		}
	}

	dir_ref_m.rwpflock.write_exit();
fail2:
fail1:
	for (auto &dme : (*new_dir)) {
		if (MAP_FAILED == dme.mm_region)
			continue;
		const int err __attribute__((unused)) = munmap(dme.mm_region, dme.size_b);
		assert(0 == err);
		// invalidate old mappings grains
		// salsa::SalsaCtlr::invalidate_grains(dme.grain_offset, dme.size_b / grain_size, false);
	}
	delete new_dir;
fail0:
	grow_lock_m.unlock();

	return rc;
}

template<typename RT>
int
uDepotDirMapOR<RT>::alloc_shadow(const u64 seg_size, const u64 grain_size)
{
	int rc = 0;
	std::vector<DirMapEntry> *const old_dir = dir_ref_m.directory.load();

	grow_lock_m.lock();
	if (old_dir != dir_ref_m.directory.load()) {
		UDEPOT_DBG("Another grow happened in the meantime, retry.");
		grow_lock_m.unlock();
		return EAGAIN;
	}

	UDEPOT_MSG("Trying to grow the directory map.");
	const u32 new_size = 0 == old_dir->size() ? 1 : 2 * old_dir->size();
	std::vector<DirMapEntry> *const new_dir = new std::vector<DirMapEntry> (new_size);
	// const u64 seg_size = salsa::SalsaCtlr::get_seg_size() - md_m.get_seg_md_size();
	// const u64 grain_size = salsa::SalsaCtlr::get_grain_size();
	const u64 mmap_size_b = seg_size * grain_size;
	const u64 mmap_size_huge_b = align_down(seg_size * grain_size, 1UL<<21);
	// const u64 mmap_size_grains = mmap_size_b / grain_size;
	u32 lock_nr;

	if (nullptr == new_dir) {
		rc = ENOMEM;
		UDEPOT_ERR("failed to allocate directory.");
		goto fail0;
	}
	lock_nr = _UDEPOT_HOP_SCOTCH_LOCK_NR;

	// 1. allocate new directory
	for (auto &dme : (*new_dir)) {
		// allocate grains
		u64 grain;
		// do {
		// 	rc = salsa::SalsaCtlr::allocate_grains(mmap_size_grains, &grain);
		// } while (EAGAIN == rc);
		if (0 != rc) {
			UDEPOT_ERR("allocate grains failed with %d\n", rc);
			goto fail1;
		}
		const u64 mmap_offset = grain * grain_size;
		dme.mm_region = udepot_io_m.mmap(nullptr, mmap_size_huge_b, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_HUGETLB,
						mmap_offset);
		if (MAP_FAILED == dme.mm_region && ENOMEM == errno) {
			UDEPOT_ERR("mmap (size=%zd) with huge pages failed. Falling back 4k mmap, might be slow\n",
				mmap_size_b, strerror(errno), errno);
			dme.mm_region = udepot_io_m.mmap(nullptr, mmap_size_b, PROT_READ|PROT_WRITE, MAP_SHARED, mmap_offset);
		} else {
			dme.size_b = mmap_size_huge_b;
		}
		if (MAP_FAILED == dme.mm_region) {
			UDEPOT_ERR("mmap (size=%zd) failed with %s %d\n", mmap_size_b, strerror(errno), errno);
			rc = ENOMEM;
			goto fail1;
		} else {
			dme.size_b = mmap_size_b;
		}
		UDEPOT_DBG("succesfully mmaped %luB at %p", dme.size_b, dme.mm_region);
		dme.grain_offset = grain;
	}
	// 2. init hash tables
	for (auto &dme : (*new_dir)) {
		// adjust for directory's metadata
		const u64 size = dme.size_b;
		void *const region = (u8 *) dme.mm_region;
		rc = dme.map.init(lock_nr, size, region);
		if (0 != rc) {
			UDEPOT_ERR("hash table init failed w=%d", rc);
			goto fail2;
		}
	}
	// point shadow to new directory
	dir_ref_m.shadow_directory = new_dir;

	grow_lock_m.unlock();

	UDEPOT_MSG("Successfully alloc'ed directory for the %u time, to a size of %lu %s.",
		grow_nr_m, new_dir->size(), 1 == new_dir->size() ? "table" : "tables");
	return 0;
fail2:
fail1:
	for (auto &dme : (*new_dir)) {
		if (MAP_FAILED == dme.mm_region)
			continue;
		const int err __attribute__((unused)) = munmap(dme.mm_region, dme.size_b);
		assert(0 == err);
	}
	delete new_dir;
fail0:
	grow_lock_m.unlock();

	return rc;
}

template<typename RT>
int
uDepotDirMapOR<RT>::cpy_lock_region(uDepotMap<RT> *const udm, uDepotLock *const l1, uDepotLock *const l2, u64 *rem_out)
{
	u64 start, end, rem = -1;
	// std::tie(start, end) = udm->hash_to_lock_range(h);
	std::tie(start, end) = udm->locks_to_grow_range(l1, l2);
	std::vector<DirMapEntry> *const new_dir = dir_ref_m.shadow_directory.load();
	const HashEntry *const first = &udm->ht_m[start];
	const HashEntry *const last  = &udm->ht_m[end];
	const u32 new_size = (*new_dir).size();
	#if	0
	const u64 shift_ = _UDEPOT_HOP_SCOTCH_BUCKET_SIZE * (_UDEPOT_HOP_MAX_BUCKET_DISPLACE + 1);
	for (const HashEntry *p = first; p < (first + shift_) && p < last - shift_; ++p) {
		if (!p->used())
			continue;
		const u64 h = udm->entry_keyfp(p);
		const u32 idx = h % new_size;
		const u64 pidx = p - &udm->ht_m[0];
		uDepotMap<RT> &new_map = (*new_dir)[idx].map;
		if (nullptr != new_map.lookup(h, p->pba)) {
			assert(0 != start); // had to overflow from previous bucket
			continue;
		}
		HashEntry *trgt = &new_map.ht_m[pidx];
		assert(!trgt->used());
		*trgt = *p;
		assert(h == new_map.entry_keyfp(trgt));
		assert(trgt == new_map.lookup(h, p->pba));
	}
	for (const HashEntry *p = last - shift_; p < last; ++p) {
		if (!p->used())
			continue;
		const u64 h = udm->entry_keyfp(p);
		const u32 idx = h % new_size;
		const u64 pidx = p - &udm->ht_m[0];
		uDepotMap<RT> &new_map = (*new_dir)[idx].map;
		if (nullptr != new_map.lookup(h, p->pba)) {
			assert(end <= udm->entry_nr_m); // had to overflow from next bucket
			continue;
		}
		HashEntry *trgt = &new_map.ht_m[pidx];
		assert(!trgt->used());
		*trgt = *p;
		assert(h == new_map.entry_keyfp(trgt));
		assert(trgt == new_map.lookup(h, p->pba));
	}
	#endif
	for (const HashEntry *p = first; p < last; ++p) {
		if (!p->used())
			continue;
		const u64 h = udm->entry_keyfp(p);
		const u32 idx = h % new_size;
		uDepotMap<RT> &new_map = (*new_dir)[idx].map;
		if (nullptr != new_map.lookup(h, p->pba)) {
			continue;
		}
		// const u64 pidx = p - &udm->ht_m[0];
		HashEntry *trgt;
		// *trgt = *p;
#if	1
		int rc = new_map.lookup(h, &trgt);
		switch (rc) {
		case  ENOSPC:
			rc = new_map.try_shift(h , &trgt);
			if (unlikely(0 != rc)) {
				UDEPOT_ERR("fail to insert to new directory");
				return EFAULT;
			}
			assert(trgt->deleted() ^ !trgt->used());
		case ENODATA:
			new_map.insert(trgt, h, p->kv_size, p->pba);
			assert(trgt->used());
			break;
		case 0:
			rc = new_map.next_free(h, &trgt);
			assert (nullptr != trgt);
			new_map.insert(trgt, h, p->kv_size, p->pba);
			break;
		default:
			UDEPOT_ERR("unknown error in new directory lookup");
			return EFAULT;
		}
#endif
		assert(h == new_map.entry_keyfp(trgt));
		assert(trgt == new_map.lookup(h, p->pba));
	}
	if (start < end) {
		u32 done = 0;
		if (l1->ref_cnt()) {
			u32 ret __attribute__((unused)) = l1->ref_cnt_dec_ret(1); assert(1 == ret);
			rem = lock_range_cpy_finished();
			done++;
		}
		if (l2 && l2->ref_cnt()) {
			u32 ret __attribute__((unused)) = l2->ref_cnt_dec_ret(1); assert(1 == ret);
			rem = lock_range_cpy_finished();
			done++;
		}
		assert(0 < done);
	}
	*rem_out = rem;
	return 0;
}

template<typename RT>
int
uDepotDirMapOR<RT>::grow_finished(const u64 seg_size, const u64 grain_size)
{
	int rc = 0;
	std::vector<DirMapEntry> *const old_dir = dir_ref_m.directory.load();
	std::vector<DirMapEntry> *const new_dir = dir_ref_m.shadow_directory.load();
	assert(0 == dir_ref_m.state.load());
	grow_lock_m.lock();
	if (old_dir != dir_ref_m.directory.load()) {
		UDEPOT_DBG("Another grow happened in the meantime, retry.");
		grow_lock_m.unlock();
		return EAGAIN;
	}

	dir_ref_m.state = new_dir->size() * (*new_dir)[0].map.lock_nr_m;
	dir_ref_m.directory = new_dir;

	// TODO: sys_membarrier before free
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	UDEPOT_MSG("grow finished, releasing old directory");
#ifdef DEBUG
	for (u32 i = 0; i < old_dir->size(); ++i) {
		const uDepotMap<RT> &map = (*old_dir)[i].map;
		const HashEntry *const first = &map.ht_m[0];
		const HashEntry *const last  = &map.ht_m[map.entry_nr_m];
		for (const HashEntry *p = first; p < last; ++p) {
			if (!p->used())
				continue;
			const u64 h = map.entry_keyfp(p);
			const u32 idx = h % new_dir->size();
			assert(nullptr != (*new_dir)[idx].map.lookup(h, p->pba));
		}
	}
#endif
	for (auto &dme : (*old_dir)) {
		// unmap region
		const int err = munmap(dme.mm_region, dme.size_b);
		if (0 != err) {
			UDEPOT_ERR("munmap failed w=%d", errno);
			rc = errno;
			break;
		}
	}

	delete old_dir;
	dir_ref_m.shadow_directory = nullptr;

	grow_nr_m++;
	grow_lock_m.unlock();

	return rc;
}

template<typename RT>
int
uDepotDirMapOR<RT>::shutdown()
{
	for (auto &dme : (*(dir_ref_m.directory))) {
		if (MAP_FAILED == dme.mm_region)
			continue;
		// const int rc __attribute__((unused)) = udepot_io_m.msync(dme.mm_region, dme.size_b, MS_SYNC);
		// assert(0 == rc);
		const int err __attribute__((unused)) = munmap(dme.mm_region, dme.size_b);
		assert(0 == err);
	}
	delete dir_ref_m.directory;
	dir_ref_m.directory = nullptr;
	return 0;
}

// instantiate the templates
template class uDepotDirMapOR<RuntimePosix>;
template class uDepotDirMapOR<RuntimePosixODirect>;
template class uDepotDirMapOR<RuntimePosixODirectMC>;
template class uDepotDirMapOR<RuntimeTrt>;
template class uDepotDirMapOR<RuntimeTrtMC>;
template class uDepotDirMapOR<RuntimeTrtUring>;
template class uDepotDirMapOR<RuntimeTrtUringMC>;
#if defined(UDEPOT_TRT_SPDK)
template class uDepotDirMapOR<RuntimeTrtSpdk>;
template class uDepotDirMapOR<RuntimeTrtSpdkArray>;
template class uDepotDirMapOR<RuntimePosixSpdk>;
template class uDepotDirMapOR<RuntimeTrtSpdkArrayMC>;
#endif

} // udepot

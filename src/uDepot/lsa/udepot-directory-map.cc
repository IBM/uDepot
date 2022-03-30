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
#include "uDepot/hash-function.hh"
#include "uDepot/lsa/udepot-directory-map.hh"
#include "uDepot/lsa/udepot-map.hh"
#include "uDepot/lsa/metadata.hh"
#include "uDepot/lsa/store.hh"

namespace udepot {

struct dirmap_hdr {
	union {
		struct {
			u16 dir_size;
			u16 idx;
			u32 csum;
			u64 ts;
		};
		char raw[512U];
	};
}__attribute__((packed));

struct dirmap_ftr {
	union {
		struct {
			u16 dir_size;
			u16 idx;
			u32 csum;
			u64 ts;
			u64 used_bytes_kv;
			u64 tot_bytes_kv;
		};
		char raw[512U];
	};
}__attribute__((packed));

template<typename RT>
uDepotDirectoryMap<RT>::uDepotDirectoryMap(uDepotSalsaMD<RT> &md, typename RT::IO &udepot_io) :
	salsa::SalsaCtlr(),
	type_m(uDepotSalsaType::Directory),
	udepot_io_m(udepot_io),
	md_m(md),
	grow_nr_m(0),
	map_entry_nr_bits_m(0)
{
	dir_ref_m.directory = new std::vector<DirMapEntry> (0);
}

template<typename RT>
uDepotDirectoryMap<RT>::~uDepotDirectoryMap()
{
	UDEPOT_MSG("Directory Map performed %u grows .", 0 < grow_nr_m ? grow_nr_m - 1 : 0);
}

template<typename RT>
__attribute__((warn_unused_result))
int
uDepotDirectoryMap<RT>::init(salsa::Scm *const scm, uDepotSalsa<RT> *const udpt)
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

	rc = salsa::SalsaCtlr::init(scm, md_m.get_seg_md_size());
	if (0 != rc) {
		UDEPOT_ERR("SalsaCtlr init failed with %d.", rc);
		goto fail1;
	}

	if (md_m.is_restored()) {
		rc = restore(scm, udpt);
		if (0 != rc) {
			UDEPOT_ERR("Directory map restore failed with %d.", rc);
			UDEPOT_ERR("Attempting crash recovery.");
			// TODO
			rc = prep_crash_recovery(scm, udpt);
			if (0 != rc) {
				UDEPOT_ERR("failed prep crash recovery with %s %d.", strerror(rc), rc);
				goto fail2;
			}
			rc = grow();
			if (0 != rc) {
				UDEPOT_ERR("failed grow with %s %d.", strerror(rc), rc);
				goto fail2;
			}
			rc = crash_recovery(scm, udpt);
			if (0 != rc) {
				UDEPOT_ERR("failed crash recovery with %s %d.", strerror(rc), rc);
				goto fail2;
			}
		} else {
			// invalidate persistent directory so that we don't
			// re-restore it by mistake in case of a future crash
			rc = invalidate_ftr();
			if (0 != rc) {
				UDEPOT_ERR("failed to invalidate dir with %s %d.", strerror(rc), rc);
				goto fail2;
			}
		}
	} else {
		// starting from scratch
		rc = grow();
		if (0 != rc) {
			UDEPOT_ERR("grow failed with %s %d.", strerror(rc), rc);
			goto fail2;
		}
	}

	{
		std::vector<DirMapEntry> *const directory = dir_ref_m.directory.load(std::memory_order_relaxed);
		const uDepotMap<RT> &map = ((*directory)[0]).map;
		map_entry_nr_bits_m = map.entry_nr_bits_m;
	}
	return 0;
fail2:
	salsa::SalsaCtlr::shutdown();
fail1:
fail0:
	return rc;
}

template<typename RT>
__attribute__((warn_unused_result))
int
uDepotDirectoryMap<RT>::grow()
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
	const u64 seg_size = salsa::SalsaCtlr::get_seg_size() - md_m.get_seg_md_size();
	const u64 grain_size = salsa::SalsaCtlr::get_grain_size();
	const u64 mmap_size_b = seg_size * grain_size;
	const u64 mmap_size_huge_b = align_down(seg_size * grain_size, 1UL<<21);
	const u64 mmap_size_grains = mmap_size_b / grain_size;
	u32 lock_nr;

	if (nullptr == new_dir) {
		rc = ENOMEM;
		UDEPOT_ERR("failed to allocate directory.");
		goto fail0;
	}
	lock_nr = std::max(1UL, _UDEPOT_HOP_SCOTCH_LOCK_NR / new_dir->size());
	// 1. allocate new directory
	for (auto &dme : (*new_dir)) {
		// allocate grains
		u64 grain;
		do {
			rc = salsa::SalsaCtlr::allocate_grains(mmap_size_grains, &grain);
		} while (EAGAIN == rc);
		if (0 != rc) {
			UDEPOT_ERR("allocate grains failed with %d\n", rc);
			goto fail1;
		}
		const u64 mmap_offset = grain * grain_size;
		bool huge = false;
		dme.mm_region = udepot_io_m.mmap(nullptr, mmap_size_huge_b, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_HUGETLB, mmap_offset);
		if (MAP_FAILED == dme.mm_region) {
			UDEPOT_DBG("mmap (size=%zd) with huge pages failed. Falling back 4k mmap, might be slow\n",
				mmap_size_huge_b);
			dme.mm_region = udepot_io_m.mmap(nullptr, mmap_size_b, PROT_READ|PROT_WRITE, MAP_SHARED, mmap_offset);
		} else {
			huge = true;
		}
		if (MAP_FAILED == dme.mm_region) {
			UDEPOT_ERR("mmap (size=%zd) failed with %s %d\n", mmap_size_b, strerror(errno), errno);
			salsa::SalsaCtlr::release_grains(grain, mmap_size_grains);
			rc = ENOMEM;
			goto fail1;
		}
		UDEPOT_DBG("succesfully mmaped %luB at %p", mmap_size_b, dme.mm_region);
		dme.size_b	 = huge ? mmap_size_huge_b : mmap_size_b;
		dme.grain_offset = grain;
		dme.huge = huge;
		salsa::SalsaCtlr::release_grains(grain, mmap_size_grains);
	}
	// 2. init hash tables
	for (auto &dme : (*new_dir)) {
		// adjust for directory's metadata
		const u32 md_ovrh = sizeof(dirmap_hdr) + sizeof(dirmap_ftr);
		const u64 size = dme.size_b - md_ovrh;
		void *const region = (u8 *) dme.mm_region + sizeof(dirmap_hdr);
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
				const u32 idx = hash_to_dir_idx_(h, grow_nr_m);
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
			const u32 idx = hash_to_dir_idx_(h, grow_nr_m);
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
		const int err = udepot_io_m.munmap(dme.mm_region, dme.size_b);
		if (0 != err) {
			UDEPOT_ERR("munmap failed w=%d", errno);
			rc = errno;
			goto fail3;
		}
		// invalidate old mapping's grains
		salsa::SalsaCtlr::invalidate_grains(dme.grain_offset, dme.size_b / grain_size, false);
	}

	// point to new directory
	dir_ref_m.directory = new_dir;

	dir_ref_m.rwpflock.write_exit();

	// dirmap metadata header
	for (u32 i = 0; i < new_dir->size(); ++i) {
		auto &dme = (*new_dir)[i];
		u8 *const p = reinterpret_cast<u8 *>(dme.mm_region);
		dirmap_hdr *const hdr = reinterpret_cast<dirmap_hdr *>(p);
		hdr->ts = md_m.timestamp();
		hdr->dir_size = new_dir->size();
		hdr->idx = i;
		hdr->csum = md_m.checksum32(hdr->ts, (const u8*) &hdr->dir_size, sizeof(hdr->dir_size) + sizeof(hdr->idx));
	}
	grow_nr_m++;
	grow_lock_m.unlock();

	delete old_dir;

	UDEPOT_MSG("Successfully grew directory for the %u time, to a size of %lu %s.\n",
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
		const int err __attribute__((unused)) = udepot_io_m.munmap(dme.mm_region, dme.size_b);
		assert(0 == err);
		// invalidate old mappings grains
		salsa::SalsaCtlr::invalidate_grains(dme.grain_offset, dme.size_b / grain_size, false);
	}
	delete new_dir;
fail0:
	grow_lock_m.unlock();

	return rc;
}

template<typename RT>
__attribute__((warn_unused_result))
int
uDepotDirectoryMap<RT>::prep_crash_recovery(salsa::Scm *const scm, uDepotSalsa<RT> *const udpt)
{
	salsa::ScmSegmentIterator it(scm);
	const u64 seg_size = scm->get_seg_size() - md_m.get_seg_md_size();
	// const u64 seg_size_raw = scm->get_seg_size();
	const uDepotSalsaType type = udpt->get_type();
	u64 restored_seg_nr = 0;
	// TODO: restore grain ranges first, do GROW later
	// TODO: make it multithreading
	for (it = scm->begin(); it != scm->end(); ++it) {
		salsa::GrainRange gr = *it;
		const u64 md_grain = gr.grain_start + seg_size;
		salsa::salsa_seg_md seg_md;
		int rc = md_m.read_seg_md(md_grain, &seg_md);
		if (0 != rc) {
			UDEPOT_ERR("Failed to read seg md pba=%lu.", md_grain);
			continue;
		}
		const bool valid = md_m.validate_seg_md(seg_md, type);
		if (!valid)
			continue;
		// TODO: make sure we are not opening a segment now mapped to the Mapping table
		scm->restore_grain_range(gr.grain_start, seg_size, udpt->get_salsa_ctlr_id());
		md_m.restore_seg_ts(scm->grain_to_seg_idx(gr.grain_start), seg_md.timestamp);
		restored_seg_nr++;
	}
	return 0 == restored_seg_nr ? ENODATA : 0;
}

template<typename RT>
__attribute__((warn_unused_result))
int
uDepotDirectoryMap<RT>::crash_recovery(salsa::Scm *const scm, uDepotSalsa<RT> *const udpt)
{
	salsa::ScmSegmentIterator it(scm);
	const u64 seg_size = scm->get_seg_size() - md_m.get_seg_md_size();
	// const u64 seg_size_raw = scm->get_seg_size();
	const u64 grain_size = scm->get_grain_size();
	const uDepotSalsaType type = udpt->get_type();
	const u64 mmap_size_b = seg_size * grain_size;
	u64 max_seg_alloc_nr = 0;
	struct ts_data {
		u64 h_;
		uDepotSalsaStore md_;
	};
	std::map<u64, ts_data> tombstones;
	u64 valid_grain_nr = 0;
	u64 restored_seg_nr = 0;
	// TODO: make it multithreading, mmap huge pages
	for (it = scm->begin(); it != scm->end(); ++it) {
		salsa::GrainRange gr = *it;
		const u64 md_grain = gr.grain_start + seg_size;
		const u64 mmap_offset = gr.grain_start * grain_size;
		salsa::salsa_seg_md seg_md;
		int rc = md_m.read_seg_md(md_grain, &seg_md);
		if (0 != rc) {
			UDEPOT_ERR("Failed to read seg md pba=%lu.", md_grain);
			continue;
		}
		const bool valid = md_m.validate_seg_md(seg_md, type);
		if (!valid)
			continue;
		// mmap in read only mode the whole region that we are about to restore
		char *const seg_region = (char *) udepot_io_m.mmap(
			nullptr, mmap_size_b, PROT_READ, MAP_SHARED, mmap_offset);
		if (MAP_FAILED == seg_region) {
			UDEPOT_ERR("failed to read seg during crash recovery");
			UDEPOT_ERR("mmap (size=%zd) failed with %d\n", mmap_size_b, errno);
			return ENOMEM;
		}
		rc = ENOENT;
		u32 kv_restored_nr = 0;
		u64 grain_start = gr.grain_start;
		const char *const end = seg_region + mmap_size_b;
		for (const char *p = seg_region; p < end; ++kv_restored_nr) {
			const uDepotSalsaStore *const s = (const uDepotSalsaStore *) p;
			if (unlikely(end < p + s->val_size + s->key_size)) {
				UDEPOT_DBG("found an entry that goes out of bounds grain=%lu.", grain_start);
				break;
			}
			const u16 exp_crc16 = md_m.checksum16(seg_md.timestamp, (u8 *) s, sizeof(*s));
			const uDepotSalsaStoreSuffix *const suffix =
				(const uDepotSalsaStoreSuffix *) (p + sizeof(*s) + s->key_size + s->val_size);
			if (exp_crc16 != suffix->crc16) {
				UDEPOT_DBG("found an entry that goes out of bounds grain=%lu.", grain_start);
				break;
			}
			// try to insert entry
			u64 inval_grain_nr = 0;
			rc = udpt->try_restore_entry(*s, grain_start, &inval_grain_nr);
			switch(rc) {
			case 0:
			{
				// new entry
				const bool is_tombstone = 0 == s->val_size;
				if (is_tombstone) {
					assert(tombstones.find(grain_start) == tombstones.end());
					const u64 h = HashFn::hash(s->buf, s->key_size);
					tombstones.emplace(grain_start, (ts_data) { h, *s });
				}
				const u64 grain_nr = udpt->kv_tot_grains_get(s->key_size, s->val_size);
				valid_grain_nr += grain_nr;
			} // fall through
			case EEXIST:
				rc = 0; // good path
				assert(inval_grain_nr <= valid_grain_nr);
				valid_grain_nr -= inval_grain_nr;
				break;
			case ENOSPC:
				// cannot restore
			case ENOENT:
				// catastrophic failure
				break;
			default:
				assert(0);
				rc = ENOENT;
			}
 			if (0 != rc) {
				break;
			}
			// good path, entry inserted and accounted for
			grain_start += udpt->kv_tot_grains_get(s->key_size, s->val_size);
			p           += udpt->kv_tot_bytes_get(s->key_size, s->val_size);
		}

		const int rc2 __attribute__((unused)) = udepot_io_m.munmap(seg_region, mmap_size_b);
		assert(0 == rc2);
		if (0 != rc) {
			UDEPOT_ERR("Failed to recover the data  after a crash, err=%s.", strerror(rc));
			return rc;
		}
		// invalidate grains, from last till end
		const u64 grain_end = gr.grain_start + seg_size;
		UDEPOT_MSG("Restored %u valid kv pairs for seg ts=%lu from pba=%lu to pba=%lu end=%lu",
			kv_restored_nr, seg_md.timestamp, gr.grain_start, grain_start, grain_end);
		if (grain_start < grain_end) {
			salsa::SalsaCtlr::invalidate_grains(grain_start, grain_end - grain_start, false);
		}
		max_seg_alloc_nr = std::max(max_seg_alloc_nr, seg_md.timestamp);
		restored_seg_nr++;
	}
	if (0 == restored_seg_nr) {
		assert(0 == valid_grain_nr);
		UDEPOT_MSG("Failed to restored any data from the data log");
		return ENODATA;
	}
	udpt->restore_seg_alloc_nr(max_seg_alloc_nr);
	// scan all entries and remove the ones that are tombstoned
	for (auto &tombstone: tombstones) {
		const u64 pba = tombstone.first;
		ts_data tsd = tombstone.second;
		HashEntry *const trgt = this->lookup(tsd.h_, pba);
		if (nullptr == trgt) {
			UDEPOT_DBG("restored tombstone pba=%lu not found after full restore", pba);
			continue;
		}
		assert(trgt->pba == pba && 0 == tsd.md_.val_size);
		udpt->kv_remove_restored_tombstone(tsd.md_);
	}
	return 0;
}

template<typename RT>
__attribute__((warn_unused_result))
int
uDepotDirectoryMap<RT>::restore(salsa::Scm *const scm, uDepotSalsa<RT> *const udpt)
{
	int rc = 0;
	struct restore_md { salsa::GrainRange gr; dirmap_hdr hdr; dirmap_ftr ftr; u64 valid_nr;};
	std::map<u32, std::list<restore_md> > restored;
	salsa::ScmSegmentIterator it(scm);
	const u64 seg_size = scm->get_seg_size() - md_m.get_seg_md_size();
	const u64 seg_size_raw = scm->get_seg_size();
	const u64 grain_size = scm->get_grain_size();
	// try to restore metadata first, good path in case of regular shutdown
	for (it = scm->begin(); it != scm->end(); ++it) {
		salsa::GrainRange gr = *it;
		const u64 md_grain = gr.grain_start + seg_size;
		const bool valid = md_m.validate_seg_md(md_grain, type_m);
		if (!valid)
			continue;
		dirmap_hdr hdr;
		dirmap_ftr ftr;
		u64 byte_offset = gr.grain_start * grain_size;
		ssize_t n = udepot_io_m.pread((u8 *) &hdr, sizeof(hdr), byte_offset);
		if (n != static_cast<ssize_t>(sizeof(hdr))) {
			UDEPOT_ERR("pread ret=%ld errno=%d size=%lu off=%lu.", n, errno, sizeof(hdr), byte_offset);
			rc = EIO;
			break;
		}
		byte_offset = md_grain * grain_size - sizeof(ftr);
		n = udepot_io_m.pread((u8 *) &ftr, sizeof(ftr), byte_offset);
		if (n != static_cast<ssize_t>(sizeof(ftr))) {
			UDEPOT_ERR("pread ret=%ld errno=%d size=%lu off=%lu.", n, errno, sizeof(ftr), byte_offset);
			rc = EIO;
			break;
		}
		const u32 hdr_csum = md_m.checksum32(hdr.ts, (const u8*) &hdr.dir_size,
						sizeof(hdr.dir_size) + sizeof(hdr.idx));
		const u32 ftr_csum = md_m.checksum32(ftr.ts, (const u8*) &ftr.dir_size,
						sizeof(ftr.dir_size) + sizeof(hdr.idx));
		if (hdr_csum != hdr.csum || ftr_csum != ftr.csum) {
			UDEPOT_ERR("Checksum mismatch he=%u hs=%u fe=%u fs=%u", hdr_csum, hdr.csum, ftr_csum, ftr.csum);
			continue;
		}
		assert(hdr.dir_size == ftr.dir_size && hdr.idx == ftr.idx);
		restore_md rmd = {.gr = gr, .hdr = hdr, .ftr = ftr, .valid_nr = seg_size};
		restored[hdr.dir_size - 1].emplace_back(rmd);
	}
	if (0 != rc)
		return rc;

	auto riter = restored.rbegin();
	if (riter == restored.rend()) {
		UDEPOT_ERR("Found no valid directory mapping segments restored=%lu", restored.size());
		return ENODATA;
	}
	const u32 new_size = riter->second.size();
	if (0 == new_size) {
		UDEPOT_ERR("No directory segments restored");
		return ENODATA;
	}
	for (auto &rmd: riter->second) {
		if (rmd.hdr.dir_size != new_size) {
			UDEPOT_ERR("Directory segments only partially restored: expected=%u restored=%u",
				rmd.hdr.dir_size, new_size);
			rc = ENODATA;
		}
		break;
	}

	UDEPOT_MSG("Restored %u directory mapping segments", new_size);
	std::vector<DirMapEntry> *const old_dir = dir_ref_m.directory.load();
	std::vector<DirMapEntry> *const new_dir = new std::vector<DirMapEntry> (new_size);
	if (nullptr == new_dir) {
		UDEPOT_ERR("Failed to allocate directory");
		return ENOMEM;
	}

	const u64 mmap_size_b = seg_size * grain_size;
	const u64 mmap_size_huge_b = align_down(seg_size * grain_size, 1UL<<21);
	const u32 lock_nr = std::max(1UL, _UDEPOT_HOP_SCOTCH_LOCK_NR / new_dir->size());
	for (auto &rmd: riter->second) {
		const u64 grain = rmd.gr.grain_start;
		const u64 mmap_offset = grain * grain_size;
		auto &dme = (*new_dir)[rmd.hdr.idx];
		bool huge = false;
		dme.mm_region = udepot_io_m.mmap(nullptr, mmap_size_huge_b, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_HUGETLB, mmap_offset);
		if (MAP_FAILED == dme.mm_region) {
			UDEPOT_DBG("mmap (size=%zd) with huge pages failed. Falling back 4k mmap, might be slow\n",
				mmap_size_huge_b);
			dme.mm_region = udepot_io_m.mmap(nullptr, mmap_size_b, PROT_READ|PROT_WRITE, MAP_SHARED, mmap_offset);
		} else {
			huge = true;
		}
		if (MAP_FAILED == dme.mm_region) {
			UDEPOT_ERR("mmap (size=%zd) failed with %d\n", mmap_size_b, errno);
			rc = ENOMEM;
			break;
		}
		dme.size_b	 = huge ? mmap_size_huge_b : mmap_size_b;
		dme.grain_offset = grain;
		dme.huge = huge;

		const u32 md_ovrh = sizeof(dirmap_hdr) + sizeof(dirmap_ftr);
		const u64 size = dme.size_b - md_ovrh;
		void *const region = (u8 *) dme.mm_region + sizeof(dirmap_hdr);
		rc = dme.map.restore(lock_nr, size, region);
		if (0 != rc) {
			UDEPOT_ERR("hash table restore failed w=%d", rc);
			break;
		}

		// restore the whole segment range now for the directory itself
		scm->restore_grain_range(grain, seg_size, salsa::SalsaCtlr::get_ctlr_id());
	}
	if (0 != rc) {
		delete new_dir;
		return rc;
	}

	u64 data_grains = 0;
	std::map<u64, u64> validity_map;
	for (auto &dme : (*new_dir)) {
		uDepotMap<RT> &map = dme.map;
		const HashEntry *const first = &map.ht_m[0];
		const HashEntry *const last  = &map.ht_m[map.entry_nr_m];
		for (const HashEntry *p = first; p < last; ++p) {
			if (!p->used())
				continue;
			// restore the valid segment range now for the data
			const u64 grain = align_down(p->pba, seg_size_raw);
			u64 grain_nr = p->kv_size;
			assert(grain_nr <= UDEPOT_SALSA_MAX_KV_SIZE);
			if (UDEPOT_SALSA_MAX_KV_SIZE == grain_nr && !p->deleted()) {
				// Have to read from disk the KV metadata to figure out its full size
				uDepotSalsaStore md = { 0 };
				const u64 byte_offset = p->pba * grain_size;
				ssize_t n = udepot_io_m.pread((u8 *) &md, sizeof(md), byte_offset);
				if (n != static_cast<ssize_t>(sizeof(md))) {
					UDEPOT_ERR("pread ret=%ld errno=%d size=%lu off=%lu.",
						n, errno, sizeof(md), byte_offset);
					rc = EIO;
					break;
				}
				grain_nr = udpt->kv_tot_grains_get(md.key_size, md.val_size);
				UDEPOT_DBG("grain=%lu grain_nr=%lu seg_size=%lu key_size%u val_size=%u restored-grain_nr=0%lx pba=0%lx",
					grain, grain_nr, seg_size, md.key_size, md.val_size, p->kv_size, p->pba);
				assert(UDEPOT_SALSA_MAX_KV_SIZE <= grain_nr);
			}
			#ifdef  _UDEPOT_DATA_VALIDATION_TEST
			if (!p->deleted()) {
				uDepotSalsaStore md = { 0 };
				uDepotSalsaStoreSuffix mds = { 0 };
				const u64 byte_offset = p->pba * grain_size;
				ssize_t n = udepot_io_m.pread((u8 *) &md, sizeof(md), byte_offset);
				if (n != static_cast<ssize_t>(sizeof(md))) {
					UDEPOT_ERR("pread ret=%ld errno=%d size=%lu off=%lu.",
						n, errno, sizeof(md), byte_offset);
					rc = EIO;
					break;
				}
				n = udepot_io_m.pread((u8 *) &mds, sizeof(mds), byte_offset + sizeof(md) + md.key_size + md.val_size);
				if (n != static_cast<ssize_t>(sizeof(mds))) {
					UDEPOT_ERR("pread ret=%ld errno=%d size=%lu off=%lu.",
						n, errno, sizeof(md), byte_offset);
					rc = EIO;
					break;
				}
				const u64 ts = md_m.get_seg_ts(salsa::SalsaCtlr::grain_to_seg_idx(p->pba));
				const u16 exp_crc16 = md_m.checksum16(ts, (u8 *) &md, sizeof(md));
				if (exp_crc16 != mds.crc16) {
					UDEPOT_ERR("crc mismatch: exp=0x%x found=0x%x grain=%lu grain_nr=%lu" \
						" seg_size=%lu key_size%u val_size=%u byte_offset=%lu",
						exp_crc16, mds.crc16, grain, grain_nr, seg_size, md.key_size,
						md.val_size, byte_offset + md.key_size + md.val_size);
				}
				UDEPOT_DBG("grain=%lu grain_nr=%lu seg_size=%lu key_size%u val_size=%u restored-grain_nr=0%lx pba=0%lx",
					grain, grain_nr, seg_size, md.key_size, md.val_size, p->kv_size, p->pba);
			}
			#endif
			auto it = validity_map.find(grain);
			if (it == validity_map.end())
				validity_map[grain] = grain_nr;
			else
				validity_map[grain] += grain_nr;
			data_grains += grain_nr;
			if (p->deleted())
				continue;
			map.entry_restored(map.entry_keyfp(p));
			// full bytes will be restored subsequently,
			// this is just for # of entries
			udpt->kv_restored(0, 0);
		}
		if (0 != rc)
			break;
	}
	if (0 != rc) {
		delete new_dir;
		return rc;
	}

	for (auto &rmd: riter->second) {
		assert(rmd.hdr.idx == rmd.ftr.idx);
		if (rmd.ftr.idx == new_size - 1) {
			udpt->bytes_restored(rmd.ftr.tot_bytes_kv, rmd.ftr.used_bytes_kv);
			UDEPOT_MSG("Restored directory had %luKiB used and %luKiB total",
				rmd.ftr.used_bytes_kv >> 10, rmd.ftr.tot_bytes_kv >> 10);
		}
	}

	for (auto &it : validity_map) {
		UDEPOT_DBG("Restoring seg-idx=%lu ctlr-id=%u",
			scm->grain_to_seg_idx(it.first), udpt->get_salsa_ctlr_id());
		scm->restore_grain_range(it.first, it.second, udpt->get_salsa_ctlr_id());
	}
	// point to new directory
	dir_ref_m.directory = new_dir;
	delete old_dir;
	u32 size = new_dir->size() * 2;
	while (size >>= 1)
		grow_nr_m++;
	// restore seg mds
	u32 seg_valid_nr = 0;
	u64 max_seg_alloc_nr = 0;
	for (it = scm->begin(); it != scm->end(); ++it) {
		salsa::GrainRange gr = *it;
		const u64 md_grain = gr.grain_start + seg_size;
		const uDepotSalsaType type = udpt->get_type();
		salsa::salsa_seg_md seg_md;
		int rc = md_m.read_seg_md(md_grain, &seg_md);
		if (0 != rc) {
			UDEPOT_ERR("Failed to read seg md pba=%lu.", md_grain);
			continue;
		}
		const bool valid = md_m.validate_seg_md(seg_md, type);
		if (!valid)
			continue;
		assert(1 <= seg_md.timestamp);
		md_m.restore_seg_ts(scm->grain_to_seg_idx(gr.grain_start), seg_md.timestamp);
		seg_valid_nr++;
		max_seg_alloc_nr = std::max(max_seg_alloc_nr, seg_md.timestamp);
	}
	udpt->restore_seg_alloc_nr(max_seg_alloc_nr);
	UDEPOT_MSG("Restored %u segs %lu KV pairs %lu data grains =%luMiB max-ts=%lu",
		seg_valid_nr, udpt->get_kv_nr(), data_grains, data_grains * grain_size >> 20, max_seg_alloc_nr);
	return rc;
}

template<typename RT>
int
uDepotDirectoryMap<RT>::shutdown(uDepotSalsa<RT> *const udpt)
{
	auto &directory = *(dir_ref_m.directory);
	const u64 seg_size = salsa::SalsaCtlr::get_seg_size() - md_m.get_seg_md_size();
	const u64 grain_size = salsa::SalsaCtlr::get_grain_size();
	for (u32 i = 0; i < directory.size(); ++i) {
		auto &dme = directory[i];
		u8 *const p = reinterpret_cast<u8 *>(dme.mm_region);
		dirmap_ftr *const ftr = reinterpret_cast<dirmap_ftr *>(p + seg_size * grain_size - sizeof(*ftr));
		ftr->ts = md_m.timestamp();
		ftr->dir_size = directory.size();
		ftr->idx = i;
		if (directory.size() - 1 != i) {
			ftr->tot_bytes_kv = 0;
			ftr->used_bytes_kv = 0;
		} else {
			ftr->tot_bytes_kv = udpt->get_total_size();
			ftr->used_bytes_kv = udpt->uDepot<RT>::get_size();
		}
		ftr->csum = md_m.checksum32(ftr->ts, (const u8*) &ftr->dir_size, sizeof(ftr->dir_size) + sizeof(ftr->idx));
	}
	for (auto &dme : (*(dir_ref_m.directory))) {
		if (MAP_FAILED == dme.mm_region)
			continue;
		const int rc __attribute__((unused)) = udepot_io_m.msync(dme.mm_region, dme.size_b, MS_SYNC);
		assert(0 == rc);
		const int err __attribute__((unused)) = udepot_io_m.munmap(dme.mm_region, dme.size_b);
		assert(0 == err);
	}
	delete dir_ref_m.directory;
	dir_ref_m.directory = nullptr;
	salsa::SalsaCtlr::shutdown();
	return 0;
}

template<typename RT>
int
uDepotDirectoryMap<RT>::invalidate_ftr()
{
	const u64 seg_size = salsa::SalsaCtlr::get_seg_size() - md_m.get_seg_md_size();
	const u64 grain_size = salsa::SalsaCtlr::get_grain_size();
	int rc = 0;
	for (auto &dme : (*(dir_ref_m.directory))) {
		if (MAP_FAILED == dme.mm_region)
			continue;
		u8 *const p = reinterpret_cast<u8 *>(dme.mm_region);
		dirmap_ftr *const ftr = reinterpret_cast<dirmap_ftr *>(p + seg_size * grain_size - sizeof(*ftr));
		memset(ftr, 0, sizeof(*ftr));
		// align to page size
		const u64 page_size = dme.huge ? 1UL << 21 : 1ULL << 12; // 2Mi or 4Ki
		uintptr_t aligned_ptr = align_down ((uintptr_t) ftr, page_size);
		assert((uintptr_t) dme.mm_region <= aligned_ptr);
		void *const ftr_aligned = (void *) aligned_ptr;
		// u64 len = (uintptr_t) ftr - aligned_ptr + sizeof(*ftr);
		rc = udepot_io_m.msync(ftr_aligned, page_size, MS_SYNC);
		if (0 != rc) {
			UDEPOT_ERR("msync p=%p l=%lu failed with %s", ftr_aligned, page_size, strerror(errno));
			break;
		}
	}
	return rc;
}

template<typename RT>
int
uDepotDirectoryMap<RT>::gc_callback(const u64 grain_start, const u64 grain_nr)
{
	// this should not happen
	return ENOSYS;
}

template<typename RT>
void
uDepotDirectoryMap<RT>::seg_md_callback(const u64 grain_start, const u64 grain_nr)
{
	std::vector<DirMapEntry> *const directory = dir_ref_m.directory;
	assert(grain_nr == md_m.get_seg_md_size());
	md_m.persist_seg_md(grain_start, directory->size(), type_m);
}

// instantiate the templates
template class uDepotDirectoryMap<RuntimePosix>;
template class uDepotDirectoryMap<RuntimePosixODirect>;
template class uDepotDirectoryMap<RuntimePosixODirectMC>;
template class uDepotDirectoryMap<RuntimeTrt>;
template class uDepotDirectoryMap<RuntimeTrtMC>;
template class uDepotDirectoryMap<RuntimeTrtUring>;
template class uDepotDirectoryMap<RuntimeTrtUringMC>;
#if defined(UDEPOT_TRT_SPDK)
template class uDepotDirectoryMap<RuntimeTrtSpdk>;
template class uDepotDirectoryMap<RuntimeTrtSpdkArray>;
template class uDepotDirectoryMap<RuntimePosixSpdk>;
template class uDepotDirectoryMap<RuntimeTrtSpdkArrayMC>;
#endif

} // udepot

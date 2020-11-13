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

#ifndef	_UDEPOT_SALSA_H_
#define	_UDEPOT_SALSA_H_

#include "uDepot.hh"
#include "frontends/usalsa++/SalsaCtlr.hh"
#include "kv-mbuff.hh"

#include <array>
#include <atomic>
#include <vector>
#include <deque>
#include <string>

#include "uDepot/mbuff-alloc.hh"
#include "uDepot/mbuff-cache.hh"
#include "util/types.h"
#include "uDepot/io.hh"
#include "uDepot/lsa/metadata.hh"
#include "uDepot/lsa/store.hh"
#include "uDepot/lsa/type.hh"
#include "uDepot/lsa/udepot-directory-map.hh"

namespace udepot {

#define	UDEPOT_DEFAULT_GRAIN_SIZE	4096UL // bytes
#define	UDEPOT_DEFAULT_SEGMENT_SIZEB	(1UL<<30) // bytes
#define	UDEPOT_DEFAULT_SEGMENT_SIZE	(UDEPOT_DEFAULT_SEGMENT_SIZEB / UDEPOT_DEFAULT_GRAIN_SIZE + 2UL) // grains
#define UDEPOT_DEFAULT_OVERPROVISION	(200U)
template<typename RT>
class uDepotSalsa final : public uDepot<RT>,
                          public virtual KV_MbuffInterface,
                          public MbuffAllocIO<typename RT::IO>,
                          private salsa::SalsaCtlr {
public:
	uDepotSalsa(u32 thread_nr,
	            std::vector<typename RT::Net::Client::Conf> self_clients,
	            const std::string &fname,
	            const size_t size,
	            typename RT::Net::Server::Conf self_server,
	            const bool force_destroy = false,
	            u64 grain_size = UDEPOT_DEFAULT_GRAIN_SIZE,
	            u64 segment_size = UDEPOT_DEFAULT_SEGMENT_SIZE,
	            u32 overprovision = UDEPOT_DEFAULT_OVERPROVISION);
	~uDepotSalsa();
	// uDepot methods
	int init_local(u32 gc_type, u32 gc_low_wm, u32 gc_high_wm);
	int init() override final;
	int shutdown() override final;
	int get(const char key[], size_t key_size,
	        char val_buff[], size_t val_buff_size,
	        size_t &val_size_read, size_t &val_size) override final;
	int put(const char key[], size_t key_size, const char val[], size_t val_size) override final;
	int del(const char key[], size_t key_size) override final;
	// KV_MbuffInterface methods
	size_t putKeyvalPrefixSize() const override final {
		return sizeof(uDepotSalsaStore);
	}

	size_t putKeyvalSuffixSize() const override final{
		return sizeof(uDepotSalsaStoreSuffix);
	}

	size_t get_kvmbuff_size()  {
		return uDepot<RT>::get_size();
	}

	uDepotSalsaType get_type() const { return type_m; }

	u8 get_salsa_ctlr_id(void) const { return salsa::SalsaCtlr::get_ctlr_id(); }

	// size_t putKeyvalPrefixSize() override final;
	// size_t putKeyvalSuffixSize() override final;
	int put(Mbuff &keyval, size_t key_len, PutOp op = NORMAL) override final;
	int get(Mbuff const& key, Mbuff &val_out)  override final;
	int del(Mbuff const& key)  override final;

	void thread_local_entry() override final;
	void thread_local_exit() override final;

	// SalsaCtlr methods
	int gc_callback(u64 grain_start, u64 grain_nr) final;
	void seg_md_callback(u64 grain_start, u64 grain_nr) final;

	void kv_restored(const u64 kv_bytes, const u64 tot_bytes) {
		kv_added(kv_bytes, tot_bytes);
	}
	void bytes_restored(const u64 total_bytes, const u64 used_bytes) {
                kv_bytes_restored(total_bytes, used_bytes);
	}
	u64 kv_tot_bytes_get(const u32 key_size, const u32 val_size) const {
		return grain_size_m * kv_tot_grains_get(key_size, val_size);
	}
	u64 kv_tot_raw_bytes_get(const u32 key_size, const u32 val_size) const {
		return val_size + key_size + sizeof(uDepotSalsaStore) + sizeof(uDepotSalsaStoreSuffix);
	}
	u64 kv_tot_grains_get(const u32 key_size, const u32 val_size) const {
		return udiv_round_up(val_size + key_size + sizeof(uDepotSalsaStore) + sizeof(uDepotSalsaStoreSuffix), grain_size_m);
	}
	u64 kv_raw_bytes_get(const u32 key_size, const u32 val_size) const {
		return val_size + key_size;
	}

	void restore_seg_alloc_nr(u64 nr) { salsa::SalsaCtlr::restore_seg_alloc_nr(nr); }
	int try_restore_entry(const uDepotSalsaStore &s, u64, u64 *);
	void kv_remove_restored_tombstone(const uDepotSalsaStore &s) {
		const u64 old_tot_bytes = kv_tot_bytes_get(s.key_size, s.val_size);
		const u64 old_raw_bytes = kv_raw_bytes_get(s.key_size, s.val_size);
		kv_removed(old_raw_bytes, old_tot_bytes);
	}
	using MbuffAllocIO<typename RT::IO>::mbuff_free_buffers;
	using MbuffAllocIO<typename RT::IO>::mbuff_add_buffers;
	using MbuffAllocIO<typename RT::IO>::mbuff_alloc;
	using MbuffAllocIO<typename RT::IO>::mbuff_type_index;

protected:
	using uDepot<RT>::size_m;
	using uDepot<RT>::thread_nr_m;
	using uDepot<RT>::fname_m;
	using uDepot<RT>::udepot_io_m;
	using uDepot<RT>::force_destroy_m;
	using uDepot<RT>::remote_get_nr_m;
	using uDepot<RT>::remote_put_nr_m;
	using uDepot<RT>::remote_del_nr_m;
	using uDepot<RT>::local_get_nr_m;
	using uDepot<RT>::local_put_nr_m;
	using uDepot<RT>::local_del_nr_m;
	using uDepot<RT>::kv_removed;
	using uDepot<RT>::kv_added;
	using uDepot<RT>::kv_bytes_restored;

private:
	const uDepotSalsaType type_m;
	salsa::Scm *scm_m;
	volatile int exit_gc_m;
	u64 grain_size_m; // bytes
	u64 segment_size_m;	// grains
	u32 overprovision_m; 	// thousandths
	std::atomic<u64> read_io_b_m;
	std::atomic<u64> write_io_b_m;
	std::atomic<u64> false_positive_lookups_m;
	std::atomic<u64> reloc_b_m;
	uDepotSalsaMD<RT> md_m;
	uDepotDirectoryMap<RT> map_m;
	u32 salsa_streams_nr_m;

	// class members for member functions we need to pass to local_op_execute()
	// (the goal is to avoid heap allocations on each operation)
	std::function<int(uDepotSalsa<RT> *, u64, Mbuff const& , Mbuff &)> local_get_mbuff_m;
	std::function<int(uDepotSalsa<RT> *, u64, Mbuff &, size_t , u64, PutOp, u64 *)> local_put_mbuff_m;
	std::function<int(uDepotSalsa<RT> *, u64, Mbuff const&)> local_del_mbuff_m;

	uDepotNet<typename RT::Net> net_m;

	MbuffCache<typename RT::LockTy>         mb_cache_m;

	int register_local_region() override;
	int unregister_local_region() override;

	bool is_pba_order_equal_to_total_order(u64, u64, const uDepotSalsaStore &) const;

	// Mbuff versions of internal functions
	int mbuff_prepend_append_md(Mbuff &keyval, u64 key_len, u64 val_len, u64 grain);
	int mbuff_append_padding(Mbuff &keyval, u64 pad);
	void set_mapping(u64 h, HashEntry *trgt, bool update, size_t key_len,
			size_t val_len, uint64_t pba, uDepotSalsaStore const& old_kv_md);
	std::tuple<int, HashEntry *>
	lookup_common(u64 h, u32 lookup_nr,
		const std::array<u64, _UDEPOT_HOP_SCOTCH_BUCKET_SIZE> &pbas_visited);

	std::tuple<int, size_t> lookup_mbuff(
		u64 h, Mbuff const& mb, size_t mb_key_off,
		size_t mb_key_len, Mbuff &mb_dst, HashEntry *);
	std::tuple<int, HashEntry *> lookup_mbuff_put(
		u64 h, Mbuff const& mb_key, size_t mb_key_off,
		size_t mb_key_len, uDepotSalsaStore &old_kv_md, Mbuff &mb_dst);
	std::tuple<int, u64> local_put_mbuff_io(u64 h, Mbuff &keyval, size_t key_size);
	int local_put_mbuff(u64 h, Mbuff & keyval, size_t key_len, u64 grain, PutOp op, u64 *old_pba);
	int local_get_mbuff(u64 key_hash, Mbuff const& key, Mbuff &val_out);
	int local_del_mbuff(u64 h, Mbuff const& key);

	// wrapper for executing local operations
	template<typename F, typename... Args>
	typename std::result_of<F(uDepotSalsa *, Args...)>::type
	local_op_execute(u64 h, F &&op, Args &&... a);

	// GC
	struct gc_unit {
		uDepotSalsaStore md;
	};
	int gc_callback_base(u64 grain_start, u64 grain_nr);
	int local_gc_callback(u64 h, u64 start, const gc_unit *gcu, u64 &relocb, bool);
	int local_gc_callback_mc(u64 h, u64 start, const gc_unit *gcu, u64 &relocb, bool);
	inline uint32_t salsa_stream_get_id(u64);
	inline uint32_t salsa_stream_get_id();
	inline typename RT::Net::Client *get_client(uint64_t h);

	uDepotSalsa(const uDepotSalsa &&)           = delete;
	uDepotSalsa& operator=(const uDepotSalsa &) = delete;

};
}

#endif	/* _UDEPOT_SALSA_H_ */

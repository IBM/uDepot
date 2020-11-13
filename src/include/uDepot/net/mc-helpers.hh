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
#ifndef	_UDEPOT_MC_HELPERS_
#define	_UDEPOT_MC_HELPERS_

#include <time.h>

#include "util/types.h"
#include "kv-mbuff.hh"
#include "uDepot/mbuff-cache.hh"
#include "uDepot/net/connection.hh"
#include "uDepot/sync.hh"

namespace memcache {

struct msg_buff {
	msg_buff(): p_(nullptr), ab_(0), rb_(0), pos_(nullptr) {}
	~msg_buff();
	int add_free_size(u32 new_free);
	void reset() {
		pos_ = p_;
		rb_ = 0U;
	}
	u8 *p_;		// buffer
	u32 ab_;	// allocated header bytes
	u32 rb_;	// bytes read
	u8 *pos_;	// current position
};

struct cmd;
struct token;
class parser {
public:
	static constexpr u32 max_key_length = 250;
	static constexpr u32 read_cmd_chunk = 1048576U;
	static cmd read_cmd(udepot::ConnectionBase &con, msg_buff &buff);
	static int start(void);
	static void stop(void);
	static constexpr u32 REALTIME_MAXDELTA = 60*60*24*30;
protected:
	static constexpr u32 COMMAND_TOKEN = 0;
	static constexpr u32 SUBCOMMAND_TOKEN = 1;
	static constexpr u32 KEY_TOKEN = 1;
	static constexpr u32 FLAGS_TOKEN = 1;
private:
	friend cmd;
	static void parse(cmd &m);
	static void parse_storage_arguments(cmd &m);
	static void parse_arithmetic_arguments(cmd &m);
	static int try_parse_command(cmd &m);
	static size_t tokenize_command(char *command, token *tokens, const size_t max_tokens);
	static bool safe_strtoul(const char *str, uint32_t *out);
	static bool safe_strtoull(const char *str, uint64_t *out);
	static bool safe_strtol(const char *str, int64_t *out);
	static inline s64 realtime(const time_t exptime);
};

#define X_REQ(FN)					\
	FN(REQ_INVAL) \
	FN(REQ_GET) \
	FN(REQ_GETS) \
	FN(REQ_SET) \
	FN(REQ_ADD) \
	FN(REQ_REPLACE) \
	FN(REQ_APPEND) \
	FN(REQ_PREPEND) \
	FN(REQ_CAS) \
	FN(REQ_INCR) \
	FN(REQ_DECR) \
	FN(REQ_DEL) \
	FN(REQ_TOUCH) \
	FN(REQ_FLUSH_ALL) \
	FN(REQ_QUIT) \
	FN(REQ_WATCH) \
	FN(REQ_LRU_CRAWLER) \
	FN(REQ_CACHE_MEMLIMIT) \
	FN(REQ_VERBOSITY) \
	FN(REQ_SLABS) \
	FN(REQ_STATS) \
	FN(REQ_VERSION) \
	FN(REQ_NOT_IMPLEMENTED) \
	FN(REQ_LAST)

#define X_RSP(FN)					\
	FN(RSP_INVAL) \
	FN(RSP_KEY_VALUE) \
	FN(RSP_STORED) \
	FN(RSP_NOT_STORED) \
	FN(RSP_EXISTS) \
	FN(RSP_NOT_FOUND) \
	FN(RSP_TOUCHED) \
	FN(RSP_DELETED) \
	FN(RSP_VERSION) \
	FN(RSP_CONNECTION_CLOSE) \
	FN(RSP_ERROR) \
	FN(RSP_CLIENT_ERROR_FORMAT) \
	FN(RSP_CLIENT_ERROR_NON_NUMERIC) \
	FN(RSP_CLIENT_ERROR_BAD_DATA_CHUNK) \
	FN(RSP_CLIENT_ERROR_BOUND) \
	FN(RSP_SERVER_ERROR) \
	FN(RSP_SERVER_ERROR_UNSUPPORTED_MULTIGET) \
	FN(RSP_LAST)

#define ENUMIFY(x)	x,
#define STRINGIFY(x)	#x,

enum mc_req_type {
	X_REQ(ENUMIFY)
};

enum mc_rsp_type {
	X_RSP(ENUMIFY)
};

struct token {
	char *value;
	u64 length;
};

struct cmd {
	static constexpr u32 MAX_TOKENS = 512;
	cmd(msg_buff &buff);
	cmd() = delete;
	~cmd() { }
	void handle(udepot::ConnectionBase &con, KV_MbuffInterface &kv, udepot::MbuffCacheBase &mb_cache, udepot::Mbuff &mbuff, udepot::Mbuff &keymbuff);
	void handle_store(udepot::ConnectionBase &con, KV_MbuffInterface &kv, udepot::Mbuff &mbuff);
	void handle_arithmetic_cmd(udepot::ConnectionBase &con, KV_MbuffInterface &kv, udepot::Mbuff &mbuff);
	void handle_stats_cmd(udepot::ConnectionBase &con, KV_MbuffInterface &kv, udepot::Mbuff &mbuff);
	void handle_get(udepot::ConnectionBase &con, KV_MbuffInterface &kv, udepot::MbuffCacheBase &mb_cache, udepot::Mbuff &mbuff, udepot::Mbuff &keymbuff);
	void handle_multiget(
		udepot::ConnectionBase &con,
		KV_MbuffInterface &kv,
		udepot::MbuffCacheBase &mb_cache,
		udepot::Mbuff &val_mbuff_in,
		udepot::Mbuff &key_mbuff_in);
	void handle_rsp(udepot::ConnectionBase &con);
	int handle_single_kv_get(
		KV_MbuffInterface &kv,
		const token *key,
		udepot::Mbuff &keymb,
		udepot::Mbuff &valmb,
		size_t &lenk, size_t &lenv,
		size_t &lenkb, size_t &lenvb);
	int prepare_kvmbuff_for_get(
		KV_MbuffInterface &kv,
		const token *key,
		udepot::Mbuff &key_mbuff,
		udepot::Mbuff &val_mbuff);
	int kv_get_for_mc(
		KV_MbuffInterface &kv,
		udepot::Mbuff &key_mbuff,
		udepot::Mbuff &val_mbuff);
	int prepare_kvmbuff_for_send(
		KV_MbuffInterface &kv,
		udepot::Mbuff &key_mbuff,
		udepot::Mbuff &val_mbuff);
	void sanitize_exp_time(time_t cur_time);
	int verify_exp_time(time_t cur_time) const;
	msg_buff        &buff_;
	enum mc_req_type req_;
	enum mc_rsp_type rsp_;
	u32              flags_;
	s32              vlen_;
	s64              expiry_;
	u64              cas_;
	u64              num_;
	int              err_;
	u32              done_:1;
	u32              noreply_:1;
	u32              unused_:30;
	u32              ntokens_;
	token            tokens_[MAX_TOKENS];
};

}
#endif	// _UDEPOT_MC_HELPERS_

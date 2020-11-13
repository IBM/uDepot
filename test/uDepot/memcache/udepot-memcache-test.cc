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
#include <thread>
#include <unistd.h>
#include <cstdio>
#include <random>

#include "util/debug.h"
#include "util/types.h"
#include "uDepot/kv-conf.hh"
#include "uDepot/kv-conf-helpers.hh"
#include "uDepot/net/memcache.hh"
#include "uDepot/net/mc-helpers.hh"

namespace udepot {
struct mctest_conf : public KV_conf {
	mctest_conf(): KV_conf(), seed_m(time(nullptr)), noexpire_m(false) {
	}
	u64 seed_m;
	bool noexpire_m;
	int parse_args(ParseArgs &args) override final;
	void print_usage(const char []) override final;
};

void mctest_conf::print_usage(const char name[])
{
	KV_conf::print_usage(name);
	printf("udepot-memcache-test specific parameters.\n");
	printf("--seed: provide a seed used for random ops\n");
}

int mctest_conf::parse_args(ParseArgs &args)
{
	int rc = KV_conf::parse_args(args);
	if (rc != 0)
		return rc;
	const int argc = args.argc_;
	const char *const *const argv = args.argv_;
	for (int i = 1; i < argc; ++i) {
		std::string arg = std::string(argv[i]);
		// all arguments take at least one value, except help
		std::string val = i < argc - 1 ? std::string(argv[i + 1]) : "";
		UDEPOT_DBG("arg given %s.\n", arg.c_str());
		if ("--seed" == arg) {
			seed_m = stoul(val);
			args.used_arg(i);
		}
		if ("--noexpire" == arg) {
			noexpire_m = true;
			args.used_arg(i);
		}
	}
	return 0;
}

};				// namespace udepot

udepot::mctest_conf conf;

static s64 randtsafe(void) {
	static thread_local std::mt19937 engine(conf.seed_m);
	std::uniform_int_distribution<s64> uniform_dist(0, std::numeric_limits<s64>::max());
	return uniform_dist(engine);
}

#define MAX_KEY_SIZE	250

/**
 * From the protocol (https://github.com/memcached/memcached/blob/master/doc/protocol.txt)
 Retrieval command:
 ------------------
 The retrieval commands "get" and "gets" operate like this:
 get <key>*\r\n
 gets <key>*\r\n
 - <key>* means one or more key strings separated by whitespace.
 After this command, the client expects zero or more items, each of
 which is received as a text line followed by a data block. After all
 the items have been transmitted, the server sends the string
 "END\r\n"
 to indicate the end of response.

 Each item sent by the server looks like this:
 VALUE <key> <flags> <bytes> [<cas unique>]\r\n
 <data block>\r\n
 If some of the keys appearing in a retrieval request are not sent back
 by the server in the item list this means that the server does not
 hold items with such keys (because they were never stored, or stored
 but deleted to make space for more items, or expired, or explicitly
 deleted by a client).
 *
 */
#define MAX_VAL_SIZE	(1<<20)
static int mc_get_key_verify(
	udepot::MemcacheClient &cli,
	const char *const key,	// expected key
	u32 key_size,
	const char *const val,
	u32 flags, 		// expected flags
	bool is_last = false,
	bool cas = false)
{
	const s32 sfd = cli.get_sfd();
	if (sfd < 0) {
		UDEPOT_ERR("client has invalid socket.");
		return EINVAL;
	}
	// read response
	char msg[MAX_VAL_SIZE + key_size + 256];
	ssize_t size = snprintf(msg, sizeof(msg), "VALUE %s %u %d \r\n", key, flags, (s32) (MAX_VAL_SIZE));
	if (cas) size += snprintf(msg, sizeof(msg), "%lu", (u64) (-1));
	memset(msg, 0, MAX_VAL_SIZE + key_size + 256);
	ssize_t resp = 0;
	// get upto first "\r\n" ()
	do {
		ssize_t n = read(sfd, msg + resp, size - resp);
		assert(0 < n);
		resp += n;
	} while (resp < size);

	u32 flags_out = 0;
	s32 val_size_out;
	assert(0 == strncmp(msg, "VALUE ", strlen("VALUE ")));
	char *p = msg;
	p += strlen("VALUE ");
	char *q = (char *) memchr(p, ' ', resp);
	assert(q);
	u32 key_size_out = q - p;
	char key_out[key_size_out + 1];
	memset(key_out, 0, sizeof(*key_out) * key_size_out + 1);
	memcpy(key_out, p, key_size_out);
	ssize_t n __attribute__((unused)) = sscanf(q, " %u %d", &flags_out, &val_size_out);
	assert(2 == n);
	q = (char *) memchr(p, '\n', resp);
	if (!q || *(q - 1) != '\r') {
		UDEPOT_ERR("improper item VALUE line termination= %s", msg);
		assert(0);
		return EINVAL;
	}
	if (key_size != key_size_out || 0 != memcmp(key, key_out, key_size)) {
		UDEPOT_ERR("keys differ %s retrieved=%s", key, key_out);
		return EINVAL;
	}
	if (flags_out != flags) {
		UDEPOT_ERR("flags differ 0x%x retrieved=0x%x", flags, flags_out);
		return EINVAL;
	}
	if (MAX_VAL_SIZE < val_size_out) {
		UDEPOT_ERR("invalid val size %d", val_size_out);
		return EINVAL;
	}

	if (cas)
		return 0;	// TODO: check for END
	// retrieve including the Value bytes
	ssize_t tot_size = q - msg + 1;
	tot_size += val_size_out + strlen("\r\n") + (is_last ? strlen("END\r\n") : 0);
	assert(resp < tot_size);
	do {
		ssize_t n = read(sfd, msg + resp, tot_size - resp);
		assert(0 < n);
		resp += n;
	} while (resp < tot_size);
	assert( resp == tot_size);
	msg[resp] = '\0';
	if (0 != memcmp(q + 1, val, val_size_out)) {
		UDEPOT_ERR("invalid value returned");
		return EINVAL;
	}
	if (0 != memcmp(q + 1 + val_size_out, "\r\n", 2)) {
		UDEPOT_ERR("data message end %s", q + 1 + val_size_out);
		return EINVAL;
	}
	if (is_last && 0 != memcmp(q + 1 + val_size_out + 2, "END\r\n", 5)) {
		UDEPOT_ERR("invalid message end %s", q + 1 + val_size_out + 2);
		return EINVAL;
	}

	return 0;
}

static int mc_put_get_test(udepot::MemcacheClient &cli, int exptime = 0)
{
	const s32 sfd = cli.get_sfd();
	if (sfd < 0) {
		UDEPOT_ERR("client has invalid socket.");
		return EINVAL;
	}
	// set test
	const u32 key_size = 4 + (randtsafe() % (MAX_KEY_SIZE - 4));
	const s32 val_size = randtsafe() % (MAX_VAL_SIZE); // 1M
	char key[key_size + 1];
	char val[val_size + 1];
	char msg[key_size + val_size + 256];
	for (char *k = key; k < &key[key_size]; ++k)
		do {
			*k = randtsafe();
		} while (0 == *k || ' ' == *k  || '\n' == *k);
	key[key_size] = '\0';
	for (char *v = val; v < &val[val_size]; ++v)
		do {
			*v = randtsafe();
		} while (0 == *v);
	val[val_size] = '\0';
	const u32 flags = 0xBEEF;
	// set test
	ssize_t size = snprintf(msg, sizeof(msg), "set %s %u %d %d\r\n%s\r\n", key, flags, exptime, (s32) val_size, val);
	assert(key_size + val_size < size);
	ssize_t n = write(sfd, msg, size);
	assert(size == n);
	// read response
	n = read(sfd, msg, sizeof(msg));
	assert(n == strlen("STORED\r\n"));
	msg[n] = '\0';
	assert(0 == strncmp(msg, "STORED\r\n", strlen("STORED\r\n")));
	UDEPOT_MSG("server responded with %s message.", msg);

	// get test
	size = snprintf(msg, sizeof(msg), "get %s\r\n", key);
	n = write(sfd, msg, size);
	assert(size == n);
	int rc __attribute__((unused)) = mc_get_key_verify(cli, key, key_size, val, flags, true /*last*/);
	assert(0 == rc);
	n = recv(sfd, msg, 1, MSG_DONTWAIT);
	assert(-1 == n && EAGAIN == errno);
	return 0;
}

static int mc_expire_test(udepot::MemcacheClient &cli)
{
	time_t cur_time = time(nullptr);
	assert(0 != cur_time);
	time_t exp_tests[] = {0, 1, 10, 7, -1, cur_time + 18, cur_time - 5};
	time_t *const first = &exp_tests[0];
	time_t *const last = &exp_tests[sizeof(exp_tests)/sizeof(exp_tests[0])];
	for (time_t *p = first; p < last; ++p) {
		time_t exptime = *p;
		const s32 sfd = cli.get_sfd();
		if (sfd < 0) {
			UDEPOT_ERR("client has invalid socket.");
			return EINVAL;
		}
		// set test
		const u32 key_size = 4 + (randtsafe() % (MAX_KEY_SIZE - 4));
		const s32 val_size = randtsafe() % (MAX_VAL_SIZE); // 1M
		char key[key_size + 1];
		char val[val_size + 1];
		char msg[key_size + val_size + 256];
		for (char *k = key; k < &key[key_size]; ++k)
			do {
				*k = randtsafe();
			} while (0 == *k || ' ' == *k  || '\n' == *k);
		key[key_size] = '\0';
		for (char *v = val; v < &val[val_size]; ++v)
			do {
				*v = randtsafe();
			} while (0 == *v);
		val[val_size] = '\0';
		const u32 flags = 0xBEEF;
		// set test
		if (0 < exptime && memcache::parser::REALTIME_MAXDELTA < exptime) {
			// absolute values
			exptime = exptime - cur_time;
			cur_time = time(nullptr);
			exptime = cur_time + exptime;
		}
		ssize_t size = snprintf(msg, sizeof(msg), "set %s %u %ld %d\r\n%s\r\n", key, flags, exptime, (s32) val_size, val);
		assert(key_size + val_size < size);
		ssize_t n = write(sfd, msg, size);
		assert(size == n);
		// valid exptimes
		// read response
		n = read(sfd, msg, sizeof(msg));
		assert(n == strlen("STORED\r\n"));
		msg[n] = '\0';
		assert(0 == strncmp(msg, "STORED\r\n", strlen("STORED\r\n")));
		UDEPOT_MSG("server responded with %s message.", msg);

		// get test
		size = snprintf(msg, sizeof(msg), "get %s\r\n", key);
		n = write(sfd, msg, size);
		assert(size == n);
		// if it's in the past we'll get an error message
		if (0 < exptime && memcache::parser::REALTIME_MAXDELTA < exptime) {
			// absolute values
			exptime = exptime - cur_time;
		}

		if (0 <= exptime) {
			int rc __attribute__((unused)) = mc_get_key_verify(cli, key, key_size, val, flags, true /*last*/);
			assert(0 == rc);
		} else {
			// < 0, they expired, consume end msg
			// read response
			n = read(sfd, msg, sizeof(msg));
			assert(n == strlen("END\r\n"));
			msg[n] = '\0';
			assert(0 == strncmp(msg, "END\r\n", strlen("END\r\n")));
			UDEPOT_MSG("key with exptime %lds correctly expired immediately.", exptime);
		}

		if (0 < exptime) {
			// test that they expire
			// sleep and get again
			sleep(exptime + 1);
			size = snprintf(msg, sizeof(msg), "get %s\r\n", key);
			n = write(sfd, msg, size);
			assert(size == n);
			// read response
			n = read(sfd, msg, sizeof(msg));
			assert(n == strlen("END\r\n"));
			msg[n] = '\0';
			assert(0 == strncmp(msg, "END\r\n", strlen("END\r\n")));
			UDEPOT_MSG("key with exptime %lds correctly expired.", exptime);
		}
		n = recv(sfd, msg, 1, MSG_DONTWAIT);
		assert(-1 == n && EAGAIN == errno);
	}
	return 0;
}

static int mc_append_prepend_test(udepot::MemcacheClient &cli, int exptime = 0)
{
	const s32 sfd = cli.get_sfd();
	if (sfd < 0) {
		UDEPOT_ERR("client has invalid socket.");
		return EINVAL;
	}
	// set test
	const u32 key_size = 4 + (randtsafe() % (MAX_KEY_SIZE - 4));
	const s32 val_size = randtsafe() % (MAX_VAL_SIZE - 300) + 300; // 1M
	char key[key_size + 1];
	char val[val_size + 1];
	char msg[key_size + val_size + 256];
	for (char *k = key; k < &key[key_size]; ++k)
		do {
			*k = randtsafe();
		} while (0 == *k || ' ' == *k  || '\n' == *k);
	key[key_size] = '\0';
	for (char *v = val; v < &val[val_size]; ++v)
		do {
			*v = randtsafe();
		} while (0 == *v);
	int idxs[3] = {val_size / 3, 2 * val_size / 3, val_size};
	char bak = val[idxs[1]];
	val[idxs[1]] = '\0';
	const u32 origflags = 0xBEEF;
	u32 flags = 0xBEEF;
	// set test
	ssize_t size = snprintf(msg, sizeof(msg), "set %s %u %d %d\r\n%s\r\n", key, flags, exptime, (s32) idxs[1] - idxs[0], &val[idxs[0]]);
	val[idxs[1]] = bak;
	ssize_t n = write(sfd, msg, size);
	assert(size == n);
	// read response
	n = read(sfd, msg, sizeof(msg));
	assert(n == strlen("STORED\r\n"));
	msg[n] = '\0';
	assert(0 == strncmp(msg, "STORED\r\n", strlen("STORED\r\n")));
	UDEPOT_MSG("server responded with %s message.", msg);

	// get test
	size = snprintf(msg, sizeof(msg), "get %s\r\n", key);
	n = write(sfd, msg, size);
	assert(size == n);
	int rc __attribute__((unused)) = mc_get_key_verify(cli, key, key_size, &val[idxs[0]], flags, true /*last*/);
	assert(0 == rc);
	n = recv(sfd, msg, 1, MSG_DONTWAIT);
	assert(-1 == n && EAGAIN == errno);

	// append
	bak = val[idxs[2]];
	val[idxs[2]] = '\0';
	flags = 0xBEEFDEAD;
	// set test
	size = snprintf(msg, sizeof(msg), "append %s %u %d %d\r\n%s\r\n", key, flags, 129, (s32) idxs[2] - idxs[1], &val[idxs[1]]);
	val[idxs[2]] = bak;
	n = write(sfd, msg, size);
	assert(size == n);
	// read response
	n = read(sfd, msg, sizeof(msg));
	assert(n == strlen("STORED\r\n"));
	msg[n] = '\0';
	assert(0 == strncmp(msg, "STORED\r\n", strlen("STORED\r\n")));
	UDEPOT_MSG("server responded with %s message.", msg);

	// get test
	size = snprintf(msg, sizeof(msg), "get %s\r\n", key);
	n = write(sfd, msg, size);
	assert(size == n);
	rc = mc_get_key_verify(cli, key, key_size, &val[idxs[0]], origflags, true /*last*/);
	assert(0 == rc);
	n = recv(sfd, msg, 1, MSG_DONTWAIT);
	assert(-1 == n && EAGAIN == errno);

	// prepend
	bak = val[idxs[0]];
	val[idxs[0]] = '\0';
	flags = 0xBEEFDEAD;
	// set test
	size = snprintf(msg, sizeof(msg), "prepend %s %u %d %d\r\n%s\r\n", key, origflags, 7, (s32) idxs[0] - 0, val);
	val[idxs[0]] = bak;
	n = write(sfd, msg, size);
	assert(size == n);
	// read response
	n = read(sfd, msg, sizeof(msg));
	assert(n == strlen("STORED\r\n"));
	msg[n] = '\0';
	assert(0 == strncmp(msg, "STORED\r\n", strlen("STORED\r\n")));
	UDEPOT_MSG("server responded with %s message.", msg);

	// get test
	size = snprintf(msg, sizeof(msg), "get %s\r\n", key);
	n = write(sfd, msg, size);
	assert(size == n);
	rc = mc_get_key_verify(cli, key, key_size, val, origflags, true /*last*/);
	assert(0 == rc);
	n = recv(sfd, msg, 1, MSG_DONTWAIT);
	assert(-1 == n && EAGAIN == errno);

	return 0;
}

static int mc_add_replace_test(udepot::MemcacheClient &cli, int exptime = 0)
{
	const s32 sfd = cli.get_sfd();
	if (sfd < 0) {
		UDEPOT_ERR("client has invalid socket.");
		return EINVAL;
	}
	// set test
	const u32 key_size = 4 + (randtsafe() % (MAX_KEY_SIZE - 4));
	const s32 val_size = randtsafe() % (MAX_VAL_SIZE - 300) + 300; // 1M
	char key[key_size + 1];
	char val[val_size + 1];
	char msg[key_size + val_size + 256];
	for (char *k = key; k < &key[key_size]; ++k)
		do {
			*k = randtsafe();
		} while (0 == *k || ' ' == *k  || '\n' == *k);
	key[key_size] = '\0';
	for (char *v = val; v < &val[val_size]; ++v)
		do {
			*v = randtsafe();
		} while (0 == *v);
	int idxs[3] = {val_size / 3, 2 * val_size / 3, val_size};
	u32 flags = 0xBEEF;
	// replace failure test
	char bak = val[idxs[1]];
	val[idxs[1]] = '\0';
	ssize_t size = snprintf(msg, sizeof(msg), "replace %s %u %d %d\r\n%s\r\n", key, flags, exptime, (s32) idxs[1] - idxs[0], &val[idxs[0]]);
	val[idxs[1]] = bak;
	ssize_t n = write(sfd, msg, size);
	assert(size == n);
	// read response
	n = read(sfd, msg, sizeof(msg));
	assert(n == strlen("NOT_STORED\r\n"));
	msg[n] = '\0';
	assert(0 == strncmp(msg, "NOT_STORED\r\n", strlen("NOT_STORED\r\n")));

	// get test, should not return data
	size = snprintf(msg, sizeof(msg), "get %s\r\n", key);
	n = write(sfd, msg, size);
	assert(size == n);
	n = read(sfd, msg, sizeof(msg));
	assert(n == strlen("END\r\n"));
	msg[n] = '\0';
	assert(0 == strncmp(msg, "END\r\n", strlen("END\r\n")));

	// add test
	bak = val[idxs[1]];
	val[idxs[1]] = '\0';
	size = snprintf(msg, sizeof(msg), "add %s %u %d %d\r\n%s\r\n", key, flags, exptime, (s32) idxs[1] - idxs[0], &val[idxs[0]]);
	val[idxs[1]] = bak;
	n = write(sfd, msg, size);
	assert(size == n);
	// read response
	n = read(sfd, msg, sizeof(msg));
	assert(n == strlen("STORED\r\n"));
	msg[n] = '\0';
	assert(0 == strncmp(msg, "STORED\r\n", strlen("STORED\r\n")));

	size = snprintf(msg, sizeof(msg), "get %s\r\n", key);
	n = write(sfd, msg, size);
	assert(size == n);
	int rc __attribute__((unused)) = mc_get_key_verify(cli, key, key_size, &val[idxs[0]], flags, true /*last*/);
	assert(0 == rc);
	n = recv(sfd, msg, 1, MSG_DONTWAIT);
	assert(-1 == n && EAGAIN == errno);

	// replace
	bak = val[idxs[2]];
	val[idxs[2]] = '\0';
	// replace test
	size = snprintf(msg, sizeof(msg), "replace %s %u %d %d\r\n%s\r\n", key, flags, exptime, (s32) idxs[2] - idxs[1], &val[idxs[1]]);
	val[idxs[2]] = bak;
	n = write(sfd, msg, size);
	assert(size == n);
	// read response
	n = read(sfd, msg, sizeof(msg));
	assert(n == strlen("STORED\r\n"));
	msg[n] = '\0';
	assert(0 == strncmp(msg, "STORED\r\n", strlen("STORED\r\n")));
	UDEPOT_MSG("server responded with %s message.", msg);

	// get test
	size = snprintf(msg, sizeof(msg), "get %s\r\n", key);
	n = write(sfd, msg, size);
	assert(size == n);
	rc = mc_get_key_verify(cli, key, key_size, &val[idxs[1]], flags, true /*last*/);
	assert(0 == rc);
	n = recv(sfd, msg, 1, MSG_DONTWAIT);
	assert(-1 == n && EAGAIN == errno);

	// add failure test
	bak = val[idxs[1]];
	val[idxs[1]] = '\0';
	size = snprintf(msg, sizeof(msg), "add %s %u %d %d\r\n%s\r\n", key, flags, exptime, (s32) idxs[1] - idxs[0], &val[idxs[0]]);
	val[idxs[1]] = bak;
	n = write(sfd, msg, size);
	assert(size == n);
	// read response
	n = read(sfd, msg, sizeof(msg));
	assert(n == strlen("NOT_STORED\r\n"));
	msg[n] = '\0';
	assert(0 == strncmp(msg, "NOT_STORED\r\n", strlen("NOT_STORED\r\n")));

	// get test
	size = snprintf(msg, sizeof(msg), "get %s\r\n", key);
	n = write(sfd, msg, size);
	assert(size == n);
	rc = mc_get_key_verify(cli, key, key_size, &val[idxs[1]], flags, true /*last*/);
	assert(0 == rc);
	n = recv(sfd, msg, 1, MSG_DONTWAIT);
	assert(-1 == n && EAGAIN == errno);

	return 0;
}

static int mc_incr_decr_test(udepot::MemcacheClient &cli, int exptime = 0)
{
	const s32 sfd = cli.get_sfd();
	if (sfd < 0) {
		UDEPOT_ERR("client has invalid socket.");
		return EINVAL;
	}

	const u32 key_size = 4 + (randtsafe() % (MAX_KEY_SIZE - 4));
	u64 val = randtsafe();
	char key[key_size + 1];
	char val_c[21];
	char msg[key_size + 21 + 256];
	for (char *k = key; k < &key[key_size]; ++k)
		do {
			*k = randtsafe();
		} while (0 == *k || ' ' == *k  || '\n' == *k);
	key[key_size] = '\0';
	u32 flags = 0xBEEF;
	ssize_t val_size = snprintf(val_c, sizeof(val_c), "%lu", val);

	// not_found response test
	ssize_t size = snprintf(msg, sizeof(msg), "incr %s %s\r\n", key, val_c);
	ssize_t n = write(sfd, msg, size);
	assert(size == n);
	// read response
	n = read(sfd, msg, sizeof(msg));
	assert(n == strlen("NOT_FOUND\r\n"));
	msg[n] = '\0';
	assert(0 == strncmp(msg, "NOT_FOUND\r\n", strlen("NOT_FOUND\r\n")));
	UDEPOT_MSG("server responded with %s message.", msg);

	// add test
	size = snprintf(msg, sizeof(msg), "add %s %u %d %ld\r\n%s\r\n", key, flags, exptime, val_size, val_c);
	n = write(sfd, msg, size);
	assert(size == n);
	// read response
	n = read(sfd, msg, sizeof(msg));
	assert(n == strlen("STORED\r\n"));
	msg[n] = '\0';
	assert(0 == strncmp(msg, "STORED\r\n", strlen("STORED\r\n")));

	size = snprintf(msg, sizeof(msg), "get %s\r\n", key);
	n = write(sfd, msg, size);
	assert(size == n);
	int rc __attribute__((unused)) = mc_get_key_verify(cli, key, key_size, val_c, flags, true /*last*/);
	assert(0 == rc);
	n = recv(sfd, msg, 1, MSG_DONTWAIT);
	assert(-1 == n && EAGAIN == errno);

	// incr test
	u64 incr = randtsafe();
	val_size = snprintf(val_c, sizeof(val_c), "%lu", incr);
	// add test
	size = snprintf(msg, sizeof(msg), "incr %s %s\r\n", key, val_c);
	n = write(sfd, msg, size);
	assert(size == n);
	// read response
	n = read(sfd, msg, sizeof(msg));
	val_size = snprintf(val_c, sizeof(val_c), "%lu", incr + val);
	assert((u64) n == strlen("\r\n") + val_size);
	msg[n] = '\0';
	assert(0 == strncmp(msg, val_c, val_size));
	assert(0 == strncmp(msg + val_size, "\r\n", 2));
	UDEPOT_MSG("server responded with %s message.", msg);

		// incr test
	u64 decr = randtsafe();
	val_size = snprintf(val_c, sizeof(val_c), "%lu", decr);
	// decr test
	size = snprintf(msg, sizeof(msg), "decr %s %s\r\n", key, val_c);
	n = write(sfd, msg, size);
	assert(size == n);
	// read response
	n = read(sfd, msg, sizeof(msg));
	u64 new_val = decr < incr + val ? incr + val - decr : 0;
	val_size = snprintf(val_c, sizeof(val_c), "%lu", new_val);
	assert((u64) n == strlen("\r\n") + val_size);
	msg[n] = '\0';
	assert(0 == strncmp(msg, val_c, val_size));
	assert(0 == strncmp(msg + val_size, "\r\n", 2));
	UDEPOT_MSG("server responded with %s message.", msg);

	// get test
	size = snprintf(msg, sizeof(msg), "get %s\r\n", key);
	n = write(sfd, msg, size);
	assert(size == n);
	rc = mc_get_key_verify(cli, key, key_size, val_c, flags, true /*last*/);
	assert(0 == rc);
	n = recv(sfd, msg, 1, MSG_DONTWAIT);
	assert(-1 == n && EAGAIN == errno);

	return 0;
}

static int mc_multi_get_test(udepot::MemcacheClient &cli)
{
	const s32 sfd = cli.get_sfd();
	if (sfd < 0) {
		UDEPOT_ERR("client has invalid socket.");
		return EINVAL;
	}
	char fullval[MAX_VAL_SIZE + 1] = { 0 };
	for (char *v = fullval; v < &fullval[MAX_VAL_SIZE]; ++v)
		do {
			*v = randtsafe();
		} while (0 == *v);
	// fill 100-200 keys
	int key_nr = 100 + randtsafe() % 101;
	// prepare msg buffer
	char multi_get_msg[key_nr * MAX_KEY_SIZE + key_nr + sizeof("get \r\n")];
	memset(multi_get_msg, 0, key_nr * MAX_KEY_SIZE + key_nr + sizeof("get \r\n"));
	char *p = multi_get_msg;
	memcpy(p, "get ", strlen("get "));
	p += strlen("get ");
	for (int i = 0; i < key_nr; ++i) {
		const u32 key_size = 4 + (randtsafe() % (MAX_KEY_SIZE - 4));
		const s32 val_size = randtsafe() % (MAX_VAL_SIZE); // 1M
		char key[key_size + 1];
		char val[val_size + 1];
		char msg[key_size + val_size + 256];
		for (char *k = key; k < &key[key_size]; ++k)
			do {
				*k = randtsafe();
			} while (0 == *k || ' ' == *k  || '\n' == *k);
		key[key_size] = '\0';
		memcpy(val, fullval, val_size);
		val[val_size] = '\0';
		const u32 flags = 0xBEEF;
		int exptime = 0;
		// set test
		ssize_t size = snprintf(msg, sizeof(msg), "set %s %u %d %d\r\n%s\r\n", key, flags, exptime, (s32) val_size, val);
		assert(key_size + val_size < size);
		ssize_t n = write(sfd, msg, size);
		assert(size == n);
		// read response
		char msg_in[128];
		n = read(sfd, msg_in, sizeof(msg_in));
		assert(n == strlen("STORED\r\n"));
		msg_in[n] = '\0';
		assert(0 == strncmp(msg_in, "STORED\r\n", strlen("STORED\r\n")));
		UDEPOT_MSG("server responded with %s message.", msg_in);
		memcpy(p, key, key_size);
		p += key_size;
		*p++ = ' ';
	}
	const u32 sizetocpy = sizeof("\r\n");
	strncpy(p, "\r\n", sizetocpy);
	p += strlen("\r\n");
	// do a multiget
	ssize_t multiget_len = p - multi_get_msg;
	ssize_t n __attribute__((unused)) = write(sfd, multi_get_msg, multiget_len);
	assert(multiget_len == n);
	p = multi_get_msg + strlen("get ");
	for (int i = 0; i < key_nr; ++i) {
		char *q = (char *) memchr(p, ' ', multiget_len - (p - multi_get_msg));
		assert(q && ' ' == *q);
		*q = '\0';
		int rc __attribute__((unused)) = mc_get_key_verify(cli, p, q - p, fullval, 0xBEEF, key_nr - 1 == i);
		assert(0 == rc);
		p = q + 1;
		assert(p - multi_get_msg < multiget_len);
	}
	assert(p - multi_get_msg == multiget_len - (ssize_t) strlen("\r\n"));
	n = recv(sfd, multi_get_msg, 1, MSG_DONTWAIT);
	assert(-1 == n && EAGAIN == errno);
	return 0;
}

static int mc_stat_test(udepot::MemcacheClient &cli)
{
	const s32 sfd = cli.get_sfd();
	if (sfd < 0) {
		UDEPOT_ERR("client has invalid socket.");
		return EINVAL;
	}
	const char *const msg= "stats\r\n";
	u64 bytes_out = 0;
	ssize_t n __attribute__((unused)) = write(sfd, msg, strlen(msg));
	assert(strlen(msg) == (u64) n);
	char read_msg[sizeof("STAT bytes \r\nEND\r\n") + 20]; // 2^64 < 10^20
	n = read(sfd, read_msg, sizeof(read_msg));
	assert((u64) n <= sizeof(read_msg));
	n = sscanf(read_msg, "STAT bytes %lu\r\nEND\r\n", &bytes_out);
	assert(n == 1);
	UDEPOT_MSG("STAT bytes %lu", bytes_out);
	return 0;
}

__attribute__((unused))
static int mc_put_stress_test(udepot::MemcacheClient &cli)
{
	const s32 sfd = cli.get_sfd();
	if (sfd < 0) {
		UDEPOT_ERR("client has invalid socket.");
		return EINVAL;
	}
	char fullval[MAX_VAL_SIZE + 1] = { 0 };
	for (char *v = fullval; v < &fullval[MAX_VAL_SIZE]; ++v)
		do {
			*v = randtsafe();
		} while (0 == *v);
	for (int i = 0; i < 1000000; ++i) {
		// set test
		const u32 key_size = 4 + (randtsafe() % (MAX_KEY_SIZE - 4));
		const s32 val_size = randtsafe() % (MAX_VAL_SIZE); // 1M
		char key[key_size + 1];
		char val[val_size + 1];
		char msg[key_size + val_size + 256];
		for (char *k = key; k < &key[key_size]; ++k)
			do {
				*k = randtsafe();
			} while (0 == *k || ' ' == *k  || '\n' == *k);
		key[key_size] = '\0';
		memcpy(val, fullval, val_size);
		val[val_size] = '\0';
		const u32 flags = 0xBEEF;
		int exptime = 0;
		// set test
		ssize_t size = snprintf(msg, sizeof(msg), "set %s %u %d %d\r\n%s\r\n", key, flags, exptime, (s32) val_size, val);
		assert(key_size + val_size < size);
		ssize_t n = write(sfd, msg, size);
		assert(size == n);
		// read response
		n = read(sfd, msg, sizeof(msg));
		assert(n == strlen("STORED\r\n"));
		msg[n] = '\0';
		assert(0 == strncmp(msg, "STORED\r\n", strlen("STORED\r\n")));
		UDEPOT_DBG("server responded with %s message.", msg);

	}
	return 0;
}

int main(const int argc, char **argv)
{
	udepot::ParseArgs args(argc, argv);
	conf.parse_args(args);
	if (0 == conf.remote_servers_m.size() || conf.help_m) {
		if (!conf.help_m)
			UDEPOT_ERR("have to supply at least one server as argument.");
		conf.print_usage(argv[0]);
		return -1;
	}
	auto serv_c = udepot::get_udepot_clients_conf<udepot::MemcacheNet>(conf.remote_servers_m);
	std::thread threads[serv_c.size()];
	int tid = 0;
	udepot::MemcacheNet net = udepot::MemcacheNet(udepot::MemcacheNet::Conf());
	for (auto &server: serv_c) {
		threads[tid++] = std::thread(
			[&net, &server]() {
				udepot::MemcacheClient cli(net, server);
				const int err = cli.start();
				if (0 != err) {
					UDEPOT_ERR("failed to start client");
					return;
				}
				mc_put_get_test(cli);
				mc_append_prepend_test(cli);
				mc_add_replace_test(cli);
				mc_incr_decr_test(cli);
				mc_multi_get_test(cli);
				mc_stat_test(cli);
				if (!conf.noexpire_m)
					mc_expire_test(cli);
				// mc_put_stress_test(cli);
				cli.stop();
			});
	}
	for (auto &th : threads)
		if (th.joinable())
			th.join();
	return 0;
}

/*
 *  Copyright (c) 2020 International Business Machines
 *  All rights reserved.
 *
 *  SPDX-License-Identifier: BSD-3-Clause
 *
 *  Authors: Nikolas Ioannou (nio@zurich.ibm.com),
 *
 */

#include "pyudepot.hh"

#include <cstdio>
#include <cstring>
#include <cassert>

#include "kv.hh"
#include "uDepot/kv-conf.hh"
#include "uDepot/kv-factory.hh"

extern "C" {

void * uDepotOpen(const char *const fname, const uint64_t size, const int force_destroy) {
	udepot::KV_conf conf (fname, size, force_destroy, 4096 /* grain */, (1<<19) /* segment */);
	if (0 == strncmp("/dev/shm/", fname, sizeof("/dev/shm")))
		conf.type_m = udepot::KV_conf::KV_UDEPOT_SALSA; // shm doesn't work with o_direct
	else
		conf.type_m = udepot::KV_conf::KV_UDEPOT_SALSA_O_DIRECT;
	::KV *const KV = udepot::KV_factory::KV_new(conf);
	if (nullptr == KV) {
		fprintf(stderr, "failed to create uDepot instance\n");
		return nullptr;
	}
	int rc = KV->init();
	if (0 != rc) {
		fprintf(stderr, "failed to create uDepot instance\n");
		delete KV;
		return nullptr;
	}
	return static_cast<void *>(KV);
}

void uDepotClose(void *const kv_p) {
	KV *const kv = static_cast<KV *>(kv_p);
	int rc = kv->shutdown();
	if (0 != rc) {
		fprintf(stderr, "failed to shut down uDepot instance err=%s\n", strerror(rc));
	}
	delete kv;
}

int uDepotGet(void *const kv_p, const uint8_t key[], uint32_t key_size, uint8_t val_buf[], uint64_t val_buf_size) {
	KV *const kv = static_cast<KV *>(kv_p);
	size_t val_size_read, val_size;
	int rc = kv->get((const char *) key, key_size, (char *) val_buf, val_buf_size, val_size_read, val_size);
	if (0 != rc && ENODATA != rc) {
		fprintf(stderr, "GET failed with %s\n", strerror(rc));
	} else if (0 == rc)
		assert(val_size_read == val_size);
	return rc;
}

int uDepotPut(void *const kv_p, const uint8_t key[], uint32_t key_size, const uint8_t val[], uint64_t val_size) {
	KV *const kv = static_cast<KV *>(kv_p);
	int rc = kv->put((const char *) key, key_size, (char *) val, val_size);
	if (0 != rc) {
		fprintf(stderr, "GET failed with %s\n", strerror(rc));
	}
	return rc;
}

} // extern "C"

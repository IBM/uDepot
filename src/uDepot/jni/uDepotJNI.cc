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

#include <jni.h>

#include "com_ibm_udepot_uDepotJNI.h"		// javah generated header file

#include <string>
#include <cstring>
#include <thread>
#include <cstdio>
#include <pthread.h>

#include "kv.hh"
#include "uDepot/kv-conf.hh"
#include "uDepot/kv-factory.hh"
#include "uDepot/uDepot.hh"
#include "uDepot/udepot-lsa.hh"
#include "uDepot/backend.hh"
#include "util/debug.h"
#include "util/types.h"

using namespace udepot;

static KV *KV;
pthread_mutex_t kv_mtx_g = PTHREAD_MUTEX_INITIALIZER;

#if	0

// Distributed KV store not used in JNI for now
JNIEXPORT jint JNICALL Java_com_ibm_udepot_uDepotJNI_init
(JNIEnv *const env, jobject jo, jlong thread_nr, jobjectArray machines, jstring fname, jlong size, jstring server_ip)
{
	jint err = 0;
	const char *fname_m = env->GetStringUTFChars(fname, 0);
	const char *server_ip_m = env->GetStringUTFChars(server_ip, 0);
	std::vector<std::string> machines_m = std::vector<std::string>();
	std::vector<const char *> machines_c_m = std::vector<const char *>();
	const int machines_len = env->GetArrayLength(machines);
	UDEPOT_ERR("received cluster of %d machines.\n", machines_len);
	for (int i = 0; i < machines_len; ++i) {
		jstring str = (jstring) env->GetObjectArrayElement(machines, i);
		const char *ip = env->GetStringUTFChars(str, 0);
		UDEPOT_ERR("machine %d=%s.\n", i, ip);
		machines_m.push_back(ip);
		machines_c_m.push_back(ip);
	}
	KV = new uDepotSalsa(thread_nr, machines_m, fname_m, size, server_ip_m);
	if (nullptr == KV)
		err = ENOMEM;
	env->ReleaseStringUTFChars(server_ip, server_ip_m);
	env->ReleaseStringUTFChars(fname, fname_m);
	for (int i = 0; i < machines_len; ++i) {
		jstring str = (jstring) env->GetObjectArrayElement(machines, i);
		env->ReleaseStringUTFChars(str, machines_c_m[i]);
	}
	if (0 != err)
		return err;

	err = KV->init();

	KV->thread_local_entry(0);

	return err;
}
#endif

JNIEXPORT jint JNICALL Java_com_ibm_udepot_uDepotJNI_init
(JNIEnv *const env, jobject jo, jstring fname, jlong size, jboolean force_destroy)
{
	jint err = 0;
	const char *fname_m = env->GetStringUTFChars(fname, 0);
	udepot::KV_conf conf (
		fname_m,
		size, /* file size, if file doesn't exist it will create one */
		force_destroy,
		32,
		262144); /* do not try to restore data (force destroy) */
	// conf.remote_servers_m.push_back("127.0.0.1:5555");
	if (0 == strncmp("/dev/shm/", fname_m, sizeof("/dev/shm")))
		conf.type_m = udepot::KV_conf::KV_UDEPOT_SALSA;
	else
		conf.type_m = udepot::KV_conf::KV_UDEPOT_SALSA_O_DIRECT;
	conf.thread_nr_m = 512;

	conf.validate_and_sanitize_parameters();

	pthread_mutex_lock(&kv_mtx_g);
	if (nullptr != KV) {
		env->ReleaseStringUTFChars(fname, fname_m);
		goto out0;
	}
	KV = udepot::KV_factory::KV_new(conf);
	if (nullptr == KV)
		err = ENOMEM;
	env->ReleaseStringUTFChars(fname, fname_m);
	if (0 != err) {
		goto fail0;
	}
	err = KV->init();
	if (0 != err)
		goto fail1;
	KV->thread_local_entry();
out0:
	pthread_mutex_unlock(&kv_mtx_g);
	return err;
fail1:
fail0:
	KV = nullptr;
	pthread_mutex_unlock(&kv_mtx_g);
	return err;
}

JNIEXPORT jint JNICALL Java_com_ibm_udepot_uDepotJNI_shutdown
(JNIEnv *const env, jobject jo)
{
	pthread_mutex_lock(&kv_mtx_g);
	if (KV != nullptr) {
		KV->thread_local_exit();
		KV->shutdown();
		delete KV;
		KV = nullptr;
	}
	pthread_mutex_unlock(&kv_mtx_g);
	return 0;
}

JNIEXPORT jint JNICALL Java_com_ibm_udepot_uDepotJNI_get
(JNIEnv *const env, jobject jo, jbyteArray key, jlong key_size, jbyteArray val, jlong val_size)
{
	jint err = 0;
	jboolean is_copy;
	jbyte *const key_ba = (jbyte *) env->GetPrimitiveArrayCritical(key, &is_copy);
	// if (env->ExceptionCheck())
	// 	return EINVAL;
	jbyte *const val_ba = (jbyte *) env->GetPrimitiveArrayCritical(val, &is_copy);
	// if (env->ExceptionCheck()) {
	// 	env->ReleasePrimitiveArrayCritical(key, key_ba, 0);
	// 	return EINVAL;
	// }
	const char *const key_m = reinterpret_cast<const char *>(key_ba);
	char *const val_m = reinterpret_cast<char *>(val_ba);

	DBG("key=%02x key-size=%ld.\n", (unsigned char)key_m[0], key_size);
	size_t val_size_read, val_kv_size;
	err = KV->get(key_m, key_size, val_m, val_size, val_size_read, val_kv_size);

	// TODO: deal properly with val_kv_size and val_size_read
	if (!err && (val_kv_size   > static_cast<size_t>(val_size) ||
	             val_size_read > static_cast<size_t>(val_size)))
		err = EINVAL;

	env->ReleasePrimitiveArrayCritical(val, val_ba, 0);

	env->ReleasePrimitiveArrayCritical(key, key_ba, 0);

	if (0 != err)
		return -err;

	return val_size_read;
}

JNIEXPORT jint JNICALL Java_com_ibm_udepot_uDepotJNI_put
(JNIEnv *const env, jobject jo, jbyteArray key, jlong key_size, jbyteArray val, jlong val_size)
{
	jint err = 0;
	jboolean is_copy;
	jbyte *const key_ba = (jbyte *) env->GetPrimitiveArrayCritical(key, &is_copy);
	// if (env->ExceptionCheck())
	// 	return EINVAL;
	jbyte *const val_ba = (jbyte *) env->GetPrimitiveArrayCritical(val, &is_copy);
	// if (env->ExceptionCheck()) {
	// 	env->ReleasePrimitiveArrayCritical(key, key_ba, 0);
	// 	return EINVAL;
	// }
	const char *const key_m = reinterpret_cast<const char *>(key_ba);
	const char *const val_m = reinterpret_cast<const char *>(val_ba);

	DBG("key=%02x val=%02x key-size=%ld val-size=%ld.\n", (unsigned char)key_m[0], (unsigned char) val_m[0], key_size, val_size);
	err = KV->put(key_m, key_size, val_m, val_size);

	env->ReleasePrimitiveArrayCritical(val, val_ba, 0);
	env->ReleasePrimitiveArrayCritical(key, key_ba, 0);
	return err;
}

JNIEXPORT jint JNICALL Java_com_ibm_udepot_uDepotJNI_del
(JNIEnv *const env, jobject jo, jbyteArray key, jlong key_size)
{
	jint err = 0;
	jboolean is_copy;
	jbyte *const key_ba = (jbyte *) env->GetPrimitiveArrayCritical(key, &is_copy);
	// if (env->ExceptionCheck())
	// 	return EINVAL;
	const char *const key_m = reinterpret_cast<const char *>(key_ba);

	DBG("key=%02x key-size=%ld.\n", (unsigned char)key_m[0], key_size);
	err = KV->del(key_m, key_size);

	UDEPOT_DBG("key=%02x key-size=%ld.\n", (unsigned char)key_m[0], key_size);

	env->ReleasePrimitiveArrayCritical(key, key_ba, 0);
	return err;
}

JNIEXPORT jlong JNICALL Java_com_ibm_udepot_uDepotJNI_getSize
(JNIEnv *const env, jobject jo)
{
	return static_cast<long>(KV->get_size());
}

JNIEXPORT jlong JNICALL Java_com_ibm_udepot_uDepotJNI_getRawDeviceCapacity
(JNIEnv *const env, jobject jo)
{
	return static_cast<long>(KV->get_raw_capacity());
}

JNIEXPORT void JNICALL Java_com_ibm_udepot_uDepotJNI_setTid
(JNIEnv *const env, jobject jo)
{
	KV->thread_local_entry();
}

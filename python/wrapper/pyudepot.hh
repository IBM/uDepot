/*
 *  Copyright (c) 2020 International Business Machines
 *  All rights reserved.
 *
 *  SPDX-License-Identifier: BSD-3-Clause
 *
 *  Authors: Nikolas Ioannou (nio@zurich.ibm.com)
 *
 */

#pragma once

#define DLLOUT __attribute__((visibility("default")))

#include <cstdint>

extern "C" DLLOUT void * uDepotOpen(const char *fname, uint64_t size, int force_destroy);
extern "C" DLLOUT void uDepotClose(void *kv);
extern "C" DLLOUT int uDepotGet(void *const kv, const uint8_t key[], uint32_t key_size, uint8_t val_buf[], uint64_t val_buf_size);
extern "C" DLLOUT int uDepotPut(void *const kv, const uint8_t key[], uint32_t key_size, const uint8_t val[], uint64_t val_size);

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

#ifndef	_UDEPOT_SALSA_STORE_H_
#define	_UDEPOT_SALSA_STORE_H_

#include "util/types.h"
namespace udepot {

struct uDepotSalsaStore {
	u16 key_size;		// bytes
	u32 val_size;		// bytes
	u64 timestamp;		// total order for crash recovery
	char buf[];
}__attribute__((packed));

struct uDepotSalsaStoreSuffix {
	u16 crc16;
}__attribute__((packed));

};

#endif	// _UDEPOT_SALSA_STORE_H_

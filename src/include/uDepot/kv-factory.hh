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
#ifndef	_UDEPOT_KV_FACTORY_H_
#define	_UDEPOT_KV_FACTORY_H_

#include "kv.hh"
#include "uDepot/kv-conf.hh"

class KV;
namespace udepot {
struct KV_factory {
	static ::KV *KV_new(const KV_conf &);
};

};

#endif	// _UDEPOT_KV_FACTORY_H_

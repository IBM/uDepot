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
#ifndef _SALSA_TIME_H_
#define _SALSA_TIME_H_

#include <unistd.h>
#define os_schedule() 			usleep(1)
#define os_msleep(msecs)		usleep(msecs * 1000U)
#define os_usleep(usecs)		usleep(usecs)
#define os_msleep_interruptible(msecs)	usleep(msecs * 1000U)

#endif	/* _SALSA_TIME_H_ */

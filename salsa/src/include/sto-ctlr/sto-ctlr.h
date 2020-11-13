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
#ifndef	_SALSA_STO_CTLR_H_
#define	_SALSA_STO_CTLR_H_

#include <stdint.h>

struct sto_ctlr;
struct sto_ctlr_fns;
struct sto_ctlr_parameters;

int sto_ctlr_ctr(struct sto_ctlr_parameters *, struct sto_ctlr **);

/**
 * Returns:
 * 0     : Successfully initialized a new instance of struct sto_ctlr
 * ENOMEM: Not enough memory
 *
 */
int sto_ctlr_init_threads(struct sto_ctlr *, const struct sto_ctlr_parameters *);

int sto_ctlr_exit_threads(struct sto_ctlr *);

/**
 * Description:
 * Destroy a sto_ctlr that was created with sto_ctlr_init by deallocating
 * memory
 *
 * Parameters:
 * @sc: Pointer to a previously allocated sto_ctlr
 *
 * Returns:
 * 0: Success
 */
int sto_ctlr_dtr(struct sto_ctlr *);

const struct sto_ctlr_fns *sto_ctlr_fns_get(struct sto_ctlr *);

typedef struct dummy_bio__	os_bio_t;
struct dummy_bio__ {
	u64              start;
	u64              len;
}__attribute__((aligned));


struct sto_ctlr_fns {
	/**
	 * Description:
         * Write data to storage
         *
         * Returns:
         * 0   : Success
	 * EIO : IO error
	 */
	int (*write) (struct sto_ctlr *, os_bio_t *);

	/**
	 * Description:
	 * Read data from storage
	 *
         * Returns:
         * 0   : Success
	 * EIO : IO error
	 */
	int (*read) (struct sto_ctlr *, os_bio_t *);

	/**
	 * Description:
	 * Discard data from storage
	 *
         * Returns:
         * 0   : Success
	 * EIO : IO error
	 */
	int (*discard) (struct sto_ctlr *, os_bio_t *);

	int (*flush) (struct sto_ctlr *, os_bio_t *);

	/**
	 * End I/O notification
	 */
	void (*end_io) (struct sto_ctlr *, os_bio_t *, int);

	void (*dump_stats) (const struct sto_ctlr *);

	int (*dump_mapping) (struct sto_ctlr *, u64, u64);

	/**
	 * IOCTL handler
	 */
	int (*ioctl) (struct sto_ctlr *, unsigned int, unsigned long);

	/**
	 * Message handler
	 */
	u64 (*get_nrelocates) (struct sto_ctlr *);

	u64 (*get_user_writes) (struct sto_ctlr *);

	u64 (*get_capacity) (const struct sto_ctlr *);

	u64 (*get_seg_size) (struct sto_ctlr *);
};

#endif	/* _SALSA_STO_CTLR_H_ */

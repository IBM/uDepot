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

#ifndef	_KV_H_
#define	_KV_H_

#include <cstddef>
/**
 * Key Value provider base class, a pure interface
 * For an example implementation using this base class see uDepot.h under include/uDepot/
 */
class KV {
public:
	virtual ~KV() { };
	/**
	 * Initialize the Key Value provider
	 * Returns:
	 * 0: Success
	 * !0: Failure with value indicating the error (e.g., ENOMEM, EINVAL, etc.)
	 */
	virtual int init() = 0;
	/**
	 * Gracefully shutdown the Key Value provider that has been
	 * previously initialized with a successful call to init()
	 * Returns:
	 * 0: success
	 * !0: Failure with value indicating the error (e.g., EINVAL, etc.)
	 */
	virtual int shutdown() = 0;
	/**
	 * Lookup a key in the Key Value provider and return its value if found
	 * @key:           byte array that represents the key we are looking for
	 * @key_size:      size of @key byte array
	 * @val_buff:      byte array the value of the KV pair is to be placed to
	 * @val_buff_size: size of @val byte array
	 * @val_size_read: number of bytes placed to @val_buff
	 * @val_size:      size of the value stored in the KV
	 * Returns:
	 * 0:       success, exists, value returned in @val up-to
	 *          @val_size_read bytes.  @val_size returns the size
	 *          of the value stored in the KV, which might exceed
	 *          @val_buff_size
	 * ENODATA: entry does not exist
	 * error path return values:
	 * EIO:     failed to perform get operation
	 */
	virtual int get(const char key[], size_t key_size,
	                char *val_buff, size_t val_buff_size,
	                size_t &val_size_read, size_t &val_size) = 0;

	/**
	 * Insert a key value pair in the Key Value provider, or
	 * update an existing key's value
	 * @key:      byte array that represents the key we want to insert
	 * @key_size: size of @key byte array
	 * @val:      byte array with the value that we want to insert together with @key
	 * @val_size: size of @val byte array
	 * Returns:
	 * 0:      success, key-value pair has been stored
	 * error path return values:
	 * EIO:    failed to perform put operation
	 * EINVAL: invalid parameters give (e.g., @key_size +
	 *         @val_size exceed the maximum supported size)
	 * ENOSPC: no more space in the repository
	 */
	virtual int put(const char key[], size_t key_size, const char *val, size_t val_size) = 0;

	/**
	 * Delete a key value pair from the Key Value provider
	 * @key:      byte array that represents the key we want to delete
	 * @key_size: size of @key byte array
	 * Returns:
	 * ENODATA: entry does not exist
	 * 0:       success, entry found and deleted
	 * error path return values:
	 * EIO:    failed to perform del operation
	 * ENOSPC: no more space in the repository, could not perform
	 *         the delete operation
	 */
	virtual int del(const char key[], size_t key_size) = 0;

	virtual unsigned long get_size() const = 0;

	virtual unsigned long get_raw_capacity() const = 0;

	/**
	 * For multithreaded Key Value providers, optional function so
	 * different threads could indicate their tid. The Key Value
	 * provider could internally keep a thread local variable, for
	 * example, and have separate thread context for data-parallel
	 * operations, like a per thread socket in a client server Key
	 * Value provider
	 */
	virtual void thread_local_entry() = 0;
	virtual void thread_local_exit() = 0;
};

#endif	/* _KV_H_ */

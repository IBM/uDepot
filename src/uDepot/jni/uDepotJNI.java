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

package com.ibm.udepot;

public class uDepotJNI {
  /**
   * Initialize the Key Value provider
   * @fname: Location of file/device to be used as backend store (e.g., /dev/nvme0n1)
   * @size: If the user wants to utilize less than the full capacity
   *        of the file/device @fname, or if it wants it to be created
   *        and truncated to a size, it should indicate the desired
   *        capacity in bytes. Leave 0 for the default full existing
   *        capacity of the file/device @fname.
   * @force_destroy: If set the backend device will be assumed empty
   *                 and no restore will be attempted. If not set the
   *                 KV provider will try to restore its state by
   *                 reading the persistent metadata and verifying
   *                 them. If the restore fails with partial data,
   *                 init will fail.
   * Returns: 0: Success
   *         !0: Failure with value indicating the error (e.g., ENOMEM,
   *             EINVAL, etc.)
   */
  public native int init(String fname, long size, boolean force_destroy);

  /**
   * Gracefully shutdown the Key Value provider that has been
   * previously initialized with a successful call to init()
   * Returns:
   * 0: success
   * !0: Failure with value indicating the error (e.g., EINVAL, etc.)
   */
  public native int shutdown();

  /**
   * Lookup a key in the Key Value provider and return its value if found
   * @key:      byte array that represents the key we are looking for
   * @key_size: size of @key byte array
   * @val:      byte array the value of the KV pair is to be returned to
   * @val_size: size of @val byte array
   * Returns:
   * >= 0:     success, exists, value associated with @key returned
   *           in @val. Returned value indicates the number of bytes
   *           successfully written in @val
   *
   * < 0:      failure, value indicates the error reason
   * -EINVAL:  invalid parameters given (e.g., value stored in uDepot
   *           for @key does not fit in given @val_size bytes)
   * -ENODATA: entry does not exist
   * -EIO:     I/O operation returned an error
   */
  public native int get(byte[] key, long key_size, byte[] value, long val_size);

  /**
   * Insert a key value pair in the Key Value provider
   * @key:      byte array that represents the key we want to insert
   * @key_size: size of @key byte array
   * @val:      byte array with the value that we want to insert together with @key
   * @val_size: size of @val byte array
   * Returns:
   * EEXIST: entry exists already
   * ENOSPC: no more space in the repository
   * EIO:    I/O operations returned an error
   * 0:      success, key-value pair has been stored
   */
  public native int put(byte[] key, long key_size, byte[] value, long val_size);

  /**
   * Delete a key value pair from the Key Value provider
   * @key:      byte array that represents the key we want to delete
   * @key_size: size of @key byte array
   * Returns:
   * ENODATA: entry does not exist
   * 0:       success, entry found and deleted
   */
  public native int del(byte[] key, long key_size);

  /**
   * Returns the total size in bytes of all the user-stored key-value
   * pairs. It does not include the capacity utilized for the KV
   * provider internals.
   */
  public native long getSize();

  /**
   * Returns the total capacity of the underlying devices in bytes
   */
  public native long getRawDeviceCapacity();

  public native void setTid();

  static {
    System.loadLibrary("uDepotJNI");
  }

}

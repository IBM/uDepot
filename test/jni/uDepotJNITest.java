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

import com.ibm.udepot.uDepotJNI;
import java.lang.Math;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

class uDepotJNITest {
	static {
		System.loadLibrary("uDepotJNI");
	}
	final static long prime = 2654435761L;

	private uDepotJNI KV;

	int put_test_thin(long start,  long end) {
		byte[] key_b = new byte[32];
		byte[] val_b = new byte[4096];
		for (long i = start; i < end; ++i) {
			long key = Math.abs(i * prime);
			key_b[0] = (byte) (key);
			key_b[1] = (byte) (key >> 8);
			key_b[2] = (byte) (key >> 16);
			key_b[3] = (byte) (key >> 24);
			key_b[4] = (byte) (key >> 32);
			key_b[5] = (byte) (key >> 40);
			key_b[6] = (byte) (key >> 48);
			key_b[7] = (byte) (key >> 54);

			long val = Math.abs((i + 1) * prime);
			ByteBuffer vb = ByteBuffer.wrap(val_b).order(ByteOrder.LITTLE_ENDIAN);
			vb.putLong(val);

			long key_size = 8 + (key % 25);
			long val_size = 4096; // 8 + (val % (4096 - 8));
			int err = KV.put(key_b, key_size, val_b, val_size);
			if (0 != err) {
				System.out.println("Put returned err =" + err);
				throw new RuntimeException("put error");
			}
		}
		return 0;
	}

	int del_test_thin(long start,  long end) {
		byte[] key_b = new byte[32];
		for (long i = start; i < end; ++i) {
			long key = Math.abs(i * prime);
			key_b[0] = (byte) (key);
			key_b[1] = (byte) (key >> 8);
			key_b[2] = (byte) (key >> 16);
			key_b[3] = (byte) (key >> 24);
			key_b[4] = (byte) (key >> 32);
			key_b[5] = (byte) (key >> 40);
			key_b[6] = (byte) (key >> 48);
			key_b[7] = (byte) (key >> 54);

			long key_size = 8 + (key % 25);
			int err = KV.del(key_b, key_size);
			if (0 != err) {
				System.out.println("Get returned err =" + err);
				throw new RuntimeException("del error");
			}
		}
		return 0;
	}

	int get_test_thin(long start,  long end) {
		byte[] key_b = new byte[32];
		byte[] val_b = new byte[4096];
		byte[] val_empty = new byte[4096];
		ByteBuffer vbe = ByteBuffer.wrap(val_empty).order(ByteOrder.LITTLE_ENDIAN);
		vbe.position(8);
		for (long i = start; i < end; ++i) {
			long key = Math.abs(i * prime);
			key_b[0] = (byte) (key);
			key_b[1] = (byte) (key >> 8);
			key_b[2] = (byte) (key >> 16);
			key_b[3] = (byte) (key >> 24);
			key_b[4] = (byte) (key >> 32);
			key_b[5] = (byte) (key >> 40);
			key_b[6] = (byte) (key >> 48);
			key_b[7] = (byte) (key >> 54);

			long val = Math.abs((i + 1) * prime);
			long key_size = 8 + (key % 25);
			long val_size = 4096; // 8 + (val % (4096 - 8));
			int err = KV.get(key_b, key_size, val_b, val_size);
			if (err < 0) {
				System.out.println("Get returned err =" + err);
				throw new RuntimeException("get error");
			}
			ByteBuffer vb = ByteBuffer.wrap(val_b).order(ByteOrder.LITTLE_ENDIAN);
			long val_ret = vb.getLong();
			if (val != val_ret) {
				System.out.println("Get returned different val =" + Long.toHexString(val_ret) + " than expected =" + Long.toHexString(val));
				throw new RuntimeException("get error");
			}
			vb.position(8);
			if (0 != vb.compareTo(vbe)) {
				System.out.println("Get value buffer not equal to expected val buffer");
				throw new RuntimeException("get error");
			}
		}
		return 0;
	}

	@Override
	protected void finalize() throws Throwable {
		if (null != KV)
			KV.shutdown();
	}

	public void shutdown() {
		if (null != KV)
			KV.shutdown();
	}

	public uDepotJNITest(String fname, long size, boolean force_destroy) throws RuntimeException{
		try {
			KV = new uDepotJNI();
		} catch (Exception e) {
			e.printStackTrace();
			throw new RuntimeException("new error");
		}
		int rc = KV.init(fname, size, force_destroy);
		if (0 != rc)
			throw new RuntimeException("init error");
		KV.setTid();
	}

	public long getSize() {
		return KV.getSize();
	}

  	public long getRawDeviceCapacity() {
		return KV.getRawDeviceCapacity();
	}

	public static void main (String[] argv) {
		if (argv.length == 0) {
			System.err.println("Please specify a filename");
			System.exit(1);
		}

		String uDepot_fname = argv[0];

		String[] machines = new String[0];
		final long read_nr =100000;
		final long write_nr=100000;
		final long size = (1048576L*1024L+4096L)*5L;
		System.out.println("Test with size =" + (size >> 20) + "MiB");
		uDepotJNITest udpt = new uDepotJNITest(uDepot_fname, size, true);
		System.out.println("udpt size =" + (udpt.getRawDeviceCapacity() >> 20) + "MiB");
		if (null == udpt)
			return;

		long before = System.nanoTime();
		int rc = udpt.put_test_thin(0, write_nr);
		long after = System.nanoTime();

		System.out.println("put time =" + (after - before)/1e9 + "s");

		before = System.nanoTime();
		rc = udpt.get_test_thin(0, write_nr);
		after = System.nanoTime();

		System.out.println("get time =" + (after - before)/1e9 + "s");

		System.out.println("KV utilization =" + (udpt.getSize() >> 10) + "KiB");

		udpt.shutdown();
		// restore
		udpt = new uDepotJNITest(uDepot_fname, size, false);
		before = System.nanoTime();
		rc = udpt.get_test_thin(0, write_nr);
		after = System.nanoTime();
		System.out.println("get time =" + (after - before)/1e9 + "s");

		before = System.nanoTime();
		rc = udpt.del_test_thin(0, write_nr);
		after = System.nanoTime();
		System.out.println("del time =" + (after - before)/1e9 + "s");

		System.out.println("KV utilization =" + (udpt.getSize() >> 10) + "KiB");

	}
}

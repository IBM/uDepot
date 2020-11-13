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

/**
 * uDepot client binding for YCSB.
 *
 */

package com.yahoo.ycsb.db;

import com.yahoo.ycsb.ByteIterator;
import com.yahoo.ycsb.DB;
import com.yahoo.ycsb.DBException;
import com.yahoo.ycsb.Status;
import com.yahoo.ycsb.ByteArrayByteIterator;
import com.yahoo.ycsb.StringByteIterator;

import java.util.HashMap;
import java.util.Map;
import java.util.Map.Entry;
import java.util.Iterator;
import java.util.List;
import java.util.Properties;
import java.util.Set;
import java.util.Vector;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

import com.ibm.udepot.uDepotJNI;

/**
 * YCSB binding for <a href="https://github.com/IBM/uDepot">uDepot</a>.
 *
 * See {@code udepot/README.md} for details.
 */
public class uDepotClient extends DB {

  private static uDepotJNI KV = null;

  public static final String SIZE_PROPERTY = "udepot.size";
  public static final String DEV_PROPERTY = "udepot.dev";
  public static final String FORCE_PROPERTY = "udepot.force-destroy";

  private static synchronized void syncInit(String devname, long size, boolean force_destroy) throws DBException{
    if (KV != null)
      return;
    KV = new uDepotJNI();
    int err = KV.init(devname, size, force_destroy);
    if( err != 0 )
      throw new RuntimeException("uDepot init failed: "+err);
  }

  private static synchronized void syncCleanup() {
    if (KV != null)
      KV.shutdown();
    KV = null;
  }

  public void init() throws DBException {
    Properties props = getProperties();
    long size = Long.valueOf(props.getProperty(SIZE_PROPERTY,"0"));
    String devname = props.getProperty(DEV_PROPERTY,"/dev/shm/udepot_test");
    boolean force_destroy = Boolean.valueOf(props.getProperty(FORCE_PROPERTY, "true"));

    System.out.println("size="+size +" devname="+devname+" fd="+force_destroy);
    syncInit(devname, size, force_destroy);
  }

  public void cleanup() throws DBException {
    syncCleanup();
  }

  private byte[] keybuf = new byte[1024];
  private byte[] valbuf = new byte[16*1024];
  ByteBuffer vb = ByteBuffer.wrap(valbuf).order(ByteOrder.LITTLE_ENDIAN);


  private int encodeKey (String table, String key) {
    int tlen = table.length();
    int klen = key.length();
    table.getBytes(0, tlen, keybuf, 0);
    key.getBytes(0, klen, keybuf, tlen);

    return tlen+klen;
  }

  public Status read(String table, String key, Set<String> fields, HashMap<String, ByteIterator> result) {
    int tlen = table.length();
    int klen = key.length();
    table.getBytes(0, tlen, keybuf, 0);
    key.getBytes(0, klen, keybuf, tlen);
    vb.rewind();
    int rc = KV.get(keybuf, tlen + klen, valbuf, 16*1024);
    if( rc <= 0 ) {
      System.out.println("get  error =" + rc);
      return Status.ERROR;
    }
    // System.out.println("get table=" + table + " key=" + key + " rc=" + rc + " valbufsize=" + valbuf.length);
    vb.limit(rc);
    int fsize = vb.getInt();
    int vsize = vb.getInt();
    byte[] field = new byte[fsize];
    byte[] val = new byte[vsize];
    while (vb.hasRemaining()) {
      vb.get(field);
      vb.get(val);
      result.put(field.toString(), new ByteArrayByteIterator(val));
    }
    return Status.OK;
    // TODO check the fields one by one and erase entries from result that are not in fields
  }

  @Override
  public Status insert(String table, String key, HashMap<String, ByteIterator> values) {
    return update(table, key, values);
  }

  @Override
  public Status delete(String table, String key) {
    return KV.del(keybuf, encodeKey(table,key)) == 0
      ? Status.OK :  /* XXX */ Status.OK;
  }

  @Override
  public Status update(String table, String key, HashMap<String, ByteIterator> values) {
    int tlen = table.length();
    int klen = key.length();
    boolean first = true;
    table.getBytes(0, tlen, keybuf, 0);
    key.getBytes(0, klen, keybuf, tlen);
    vb.rewind();
    vb.limit(valbuf.length);
    for (Map.Entry<String, ByteIterator> entry : values.entrySet()) {
      // System.out.println("put table=" + table + " key=" + key + " field=" + entry.getKey());
      int flen = entry.getKey().length();
      String ekey = entry.getKey();
      byte[] eb = entry.getValue().toArray();
      ekey.getBytes(0, flen, keybuf, tlen+klen);
      if (first) {
	vb.putInt(ekey.length());
	vb.putInt(eb.length);
	first = false;
      }
      vb.put(ekey.getBytes());
      vb.put(eb);
    }
    int rc = KV.put(keybuf, klen+tlen, valbuf, vb.position());
    if( rc != 0 ) {
      System.out.println("put  error =" + rc);
      return Status.ERROR;
    }
    return Status.OK;
  }

  @Override
  public Status scan(String table, String startkey, int recordcount,
		     Set<String> fields, Vector<HashMap<String, ByteIterator>> result) {
    return Status.ERROR;  // not supported right now
  }

}

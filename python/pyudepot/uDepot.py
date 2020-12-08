'''
  Copyright (c) 2020 International Business Machines
  All rights reserved.

  SPDX-License-Identifier: BSD-3-Clause

  Authors: Nikolas Ioannou (nio@zurich.ibm.com),
'''

import ctypes
from ctypes import *
import platform
import os
import numpy as np
import logging
import atexit
from numpy.ctypeslib import ndpointer

if platform.system() == 'Windows':
    raise OSError(22, 'Unsupported OS', 'windows')
else:
    libudepot = cdll.LoadLibrary("libpyudepot.so")

pyopen = libudepot.uDepotOpen
pyclose = libudepot.uDepotClose
pyget = libudepot.uDepotGet
pyput = libudepot.uDepotPut


pyopen.argtypes  = [c_char_p, c_ulonglong, c_int]
pyclose.argtypes = [c_void_p]
pyget.argtypes   = [c_void_p, ndpointer(ctypes.c_ubyte, flags="C_CONTIGUOUS"), c_uint, ndpointer(ctypes.c_ubyte, flags="C_CONTIGUOUS"), c_ulonglong]
pyput.argtypes   = [c_void_p, ndpointer(ctypes.c_ubyte, flags="C_CONTIGUOUS"), c_uint, ndpointer(ctypes.c_ubyte, flags="C_CONTIGUOUS"), c_ulonglong]

# TODO: del, 0copy
class uDepot:
    def __init__(self, **kwargs):
        self._fname = kwargs.get('file_name', '/tmp/pyudepot-test')
        self._size = kwargs.get('size', 1024*1024+4096)
        self._kv=pyopen(self._fname.encode('utf-8'), self._size, 0)
        if 0 == self._kv:
            raise IOError('failed to spawn uDepot for {}'.format(self._fname))
        atexit.register(self.__cleanup)

    def __cleanup(self):
        if self._kv:
            pyclose(self._kv)

    # key: np with key
    # val_out: np array to get the data to (e.g., np.empty(VALSIZE_BYTES))
    # returns True if get successful and data in val_out
    # False if data not found
    def get(self, key, val_out):
        rc = pyget(self._kv, key, key.size, val_out, val_out.size)
        if 0 != rc:
            logging.info('pyget returned={}'.format(rc))
            return False
        return True

    # key: np with key
    # val: empty np array (e.g., np.empty(VALSIZE_BYTES))
    def put(self, key, val):
        rc=pyput(self._kv, key, key.size, val, val.size)
        if 0 != rc:
            logging.info('pyput returned={}'.format(rc))
            return False
        return True


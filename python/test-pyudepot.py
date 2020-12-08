#!/usr/bin/env python3

import os
import tempfile
import numpy as np

from pyudepot import uDepot

key=b'Key1\xff'
knp=np.frombuffer(key, dtype=np.uint8)
val=b'Val\xDE\xAD\xBE\xEF'
vnp=np.frombuffer(val, dtype=np.uint8)
kv_params={}
# TODO: use tmpfname
tmp_fd, tmp_path =  tempfile.mkstemp(suffix='tst', dir='/dev/shm', text=False)
os.close(tmp_fd)

kv_params['file_name']=tmp_path
kv_params['size'] = 1024*1024
kv=uDepot(**kv_params)

ret = kv.get(knp,vnp)
assert False == ret
ret = kv.put(knp, vnp)
assert True == ret
val_out=np.empty(vnp.size,dtype=np.uint8)
ret = kv.get(knp, val_out)

assert (val_out == vnp).all()

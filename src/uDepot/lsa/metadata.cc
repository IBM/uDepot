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


#include <zlib.h>
#include <ctime>

#include "frontends/usalsa++/Scm.hh"
#include "frontends/usalsa++/SalsaMD.hh"
#include "util/types.h"
#include "util/debug.h"

#include "uDepot/io.hh"
#include "uDepot/lsa/metadata.hh"
#include "uDepot/backend.hh"


namespace udepot {
// TODO: introduce seed from device(s)
static const u32 seed_g = 0xDEADBEEF;

template<typename RT>
uDepotSalsaMD<RT>::uDepotSalsaMD(const size_t size, const u64 grain_size,
                                 typename RT::IO &udepot_io) :
	seed_m(0), grain_size_m(grain_size),
	udepot_io_m(udepot_io),
	dev_md_m({0}), seg_md_m({0})
{
	struct timespec time;
	clock_gettime(CLOCK_MONOTONIC, &time);
	seed_m = time.tv_sec;
}

template<typename RT>
u64
uDepotSalsaMD<RT>::dev_md_grain_offset() const
{
	const u64 dev_md_grain_nr = align_up(sizeof(salsa::salsa_dev_md), grain_size_m);
	const u64 dev_size_grain_nr = align_down(udepot_io_m.get_size(), grain_size_m);
	if (dev_md_grain_nr < dev_size_grain_nr)
		return dev_size_grain_nr - dev_md_grain_nr;

	return 0;
}

template<typename RT>
void
uDepotSalsaMD<RT>::init(const salsa::salsa_dev_md &dev_md)
{
	dev_md_m = dev_md;
	seg_md_m.segment_size = dev_md_m.segment_size;
	grain_size_m = seg_md_m.grain_size = dev_md_m.grain_size;
	restored_m = seed_m != dev_md.seed;
	seed_m = seg_md_m.seed = dev_md_m.seed;
	const u64 seg_nr = dev_md.physical_size / (dev_md_m.segment_size * dev_md_m.grain_size);
	seg_ts_m = std::vector<std::atomic<u64>> ( seg_nr );
	for (auto &t : seg_ts_m)
		t = 0;
}

template<typename RT>
__attribute__((warn_unused_result))
bool
uDepotSalsaMD<RT>::validate_dev_md(salsa::salsa_dev_md *const dev_md_out)
{
	salsa::salsa_dev_md dev_md;
	const u32 pad_b = align_up(sizeof(dev_md_m), grain_size_m) - sizeof(dev_md_m);
	char pad[pad_b + 1];
	const struct iovec iovec[2] = {
		{.iov_base = (void *)&dev_md, .iov_len = sizeof(dev_md) },
		{.iov_base = (void *)pad,     .iov_len = pad_b   },
	};
	const u64 byte_offset = dev_md_grain_offset();
	const u32 iovec_nr = 0 == pad_b ? 1 : 2;

	const ssize_t n = udepot_io_m.preadv(iovec, iovec_nr, byte_offset);

	if (n != static_cast<ssize_t>(sizeof(dev_md) + pad_b)) {
		UDEPOT_ERR("pread ret=%ld errno=%d size=%lu off=%lu.", n, errno, sizeof(dev_md), byte_offset);
		return false;
	}
	UDEPOT_MSG("read dev md size=%lu off=%lu", sizeof(dev_md), byte_offset);

	u32 csum = crc32(dev_md.seed, (const u8 *) &dev_md,
				sizeof(dev_md) - sizeof(dev_md.csum));
	csum = crc32(csum, (const u8 *) (&seed_g), sizeof(seed_g));
	if (csum != dev_md.csum) {
		UDEPOT_MSG("Checksum mismatch expected=0x%x found=0x%x", csum, dev_md.csum);
		return false;
	}
	*dev_md_out = dev_md;
	return true;
}

template<typename RT>
__attribute__((warn_unused_result))
int
uDepotSalsaMD<RT>::read_seg_md(const u64 grain, salsa::salsa_seg_md *const md_out)
{
	salsa::salsa_seg_md *const seg_md = md_out;
	const u32 pad_b = align_up(sizeof(*seg_md), grain_size_m) - sizeof(*seg_md);
	char pad[pad_b + 1];
	const struct iovec iovec[2] = {
		{.iov_base = (void *)seg_md, .iov_len = sizeof(*seg_md) },
		{.iov_base = (void *)pad,     .iov_len = pad_b   },
	};
	const u32 iovec_nr = 0 == pad_b ? 1 : 2;
	const u64 byte_offset = grain * grain_size_m;
	const ssize_t n = udepot_io_m.preadv(iovec, iovec_nr, byte_offset);
	if (n != static_cast<ssize_t>(sizeof*(seg_md) + pad_b)) {
		UDEPOT_ERR("pread ret=%ld errno=%d size=%lu off=%lu.", n, errno, sizeof(*seg_md), byte_offset);
		return EIO;
	}
	return 0;
}

template<typename RT>
__attribute__((warn_unused_result))
bool
uDepotSalsaMD<RT>::validate_seg_md(const salsa::salsa_seg_md &seg_md, const uDepotSalsaType ctlr_type)
{
	const uintptr_t len = (uintptr_t) &seg_md.csum - (uintptr_t) &seg_md;
	u32 csum = crc32(seg_md.seed, (const u8 *) &seg_md, len);
	csum = crc32(csum, (const u8 *) (&seed_g), sizeof(seed_g));
	return csum == seg_md.csum && seg_md.seed == dev_md_m.seed && static_cast<u8>(ctlr_type) == seg_md.ctlr_type
		&& seg_md.segment_size == dev_md_m.segment_size && seg_md.grain_size == dev_md_m.grain_size;
}

template<typename RT>
__attribute__((warn_unused_result))
bool
uDepotSalsaMD<RT>::validate_seg_md(const u64 grain, const uDepotSalsaType ctlr_type)
{
	salsa::salsa_seg_md seg_md;
	const int rc = read_seg_md(grain, &seg_md);
	if (0 != rc)
		return false;
	return validate_seg_md(seg_md, ctlr_type);
}

template<typename RT>
__attribute__((warn_unused_result))
int
uDepotSalsaMD<RT>::persist_seg_md(const u64 grain, const u64 timestamp, const uDepotSalsaType ctlr_type)
{
	salsa::salsa_seg_md seg_md = seg_md_m;
	const uintptr_t len = (uintptr_t) &seg_md.csum - (uintptr_t) &seg_md;
	const u64 byte_offset = grain * grain_size_m;
	const u32 pad_b = align_up(sizeof(seg_md), grain_size_m) - sizeof(seg_md);
	char pad[pad_b + 1];
	seg_md.ctlr_type = static_cast<u8>(ctlr_type);
	seg_md.timestamp = timestamp;
	seg_md.csum = crc32(seg_md.seed, (const u8 *) &seg_md, len);
	seg_md.csum = crc32(seg_md.csum, (const u8 *) &seed_g, sizeof(seed_g));
	const struct iovec iovec[2] = {
		{.iov_base = (void *)&seg_md, .iov_len = sizeof(seg_md) },
		{.iov_base = (void *)pad,     .iov_len = pad_b   },
	};
	const u32 iovec_nr = 0 == pad_b ? 1 : 2;
	const ssize_t n = udepot_io_m.pwritev(iovec, iovec_nr, byte_offset);
	if (n != static_cast<ssize_t>(sizeof(seg_md) + pad_b)) {
		UDEPOT_ERR("pwritev ret=%ld errno=%d size=%lu off=%lu.", n, errno, sizeof(seg_md) + pad_b, byte_offset);
		return EIO;
	}
	DBG("pwrite ret=%ld errno=%d size=%lu off=%lu ts=%lu.", n, errno, sizeof(seg_md), byte_offset, timestamp);
	#ifdef DEBUG
	{
		salsa::salsa_seg_md smd;
		int rc = read_seg_md(grain, &smd);
		assert(0 == rc);
		assert(true == validate_seg_md(seg_md, ctlr_type));
	}
	#endif
	const u64 seg_idx = grain / seg_md_m.segment_size;
	assert(seg_idx < seg_ts_m.size());
	seg_ts_m[seg_idx] = timestamp;
	return 0;
}

template <typename RT>
__attribute__((warn_unused_result))
int
uDepotSalsaMD<RT>::persist_dev_md()
{
	const u32 pad_b = align_up(sizeof(dev_md_m), grain_size_m) - sizeof(dev_md_m);
	salsa::salsa_dev_md dev_md = dev_md_m;
	char pad[pad_b + 1];
	dev_md.csum = crc32(dev_md.seed, (const u8 *) &dev_md, sizeof(dev_md) - sizeof(dev_md.csum));
	dev_md.csum = crc32(dev_md.csum, (const u8 *) &seed_g, sizeof(seed_g));
	const struct iovec iovec[2] = {
		{.iov_base = (void *)&dev_md, .iov_len = sizeof(dev_md) },
		{.iov_base = (void *)pad,     .iov_len = pad_b   },
	};
	const u64 byte_offset = dev_md_grain_offset();
	const u32 iovec_nr = 0 == pad_b ? 1 : 2;
	const ssize_t n = udepot_io_m.pwritev(iovec, iovec_nr, byte_offset);
	if (n != static_cast<ssize_t>(sizeof(dev_md) + pad_b)) {
		UDEPOT_ERR("pwritev ret=%ld errno=%d size=%lu off=%lu.", n, errno, sizeof(dev_md) + pad_b, byte_offset);
		return EIO;
	}
	UDEPOT_MSG("wrote dev md size=%lu off=%lu", sizeof(dev_md) + pad_b, byte_offset);
	return 0;
}

template<typename RT>
int
uDepotSalsaMD<RT>::check_dev_size(const u64 segment_size)
{
	// TODO: move this to a static function in Scm.cc, this is salsa knowledge
	const u64 physical_size = udepot_io_m.get_size();
	const u64 logical_size = align_down(physical_size, segment_size * grain_size_m);
	const u64 byte_offset = dev_md_grain_offset();
	if (0 == byte_offset || byte_offset < logical_size) {
		UDEPOT_ERR("Not enough spare capacity for device segment_size:%lu (grains) "
			"grain_size:%lu physical_size:%lu logical_size:%lu metadata=%lu min=%lu.",
			segment_size, grain_size_m, physical_size, logical_size,
			physical_size - logical_size, physical_size - byte_offset);
		return EINVAL;
	}
	return 0;
}

template<typename RT>
salsa::salsa_dev_md
uDepotSalsaMD<RT>::default_dev_md(const u64 grain_size, const u64 segment_size) const
{
	salsa::salsa_dev_md dev_md = { 0 };
	dev_md.seed = seed_m;
	dev_md.physical_size = udepot_io_m.get_size();
	dev_md.grain_size = grain_size;
	dev_md.segment_size = segment_size;
	return dev_md;
}

template<typename RT>
u64
uDepotSalsaMD<RT>::get_seg_md_size() const
{
	return align_up(get_seg_md_sizeb(),  grain_size_m) / grain_size_m;
}

template<typename RT>
u64
uDepotSalsaMD<RT>::get_dev_md_size() const
{
	return align_up(get_dev_md_sizeb(),  grain_size_m) / grain_size_m;
}

template<typename RT>
u64
uDepotSalsaMD<RT>::get_dev_size() const
{
	return dev_md_m.physical_size - align_up(get_dev_md_sizeb(),  grain_size_m);
}

template<typename RT>
u32
uDepotSalsaMD<RT>::checksum32(const u32 seed, const u8 *buf, u32 len) const
{
	u32 csum = crc32(seed, buf, len);
	return crc32(csum, (const u8 *) &seed_m, sizeof(seed_m));
}

template<typename RT>
u16
uDepotSalsaMD<RT>::checksum16(const u32 seed, const u8 *buf, u32 len) const
{
	return static_cast<u16>(checksum32(seed, buf,len));
}

template<typename RT>
u64
uDepotSalsaMD<RT>::timestamp()
{
	struct timespec time;
	clock_gettime(CLOCK_MONOTONIC, &time);
	return time.tv_sec;
}

// instantiate the templates
template class uDepotSalsaMD<RuntimePosix>;
template class uDepotSalsaMD<RuntimePosixODirect>;
template class uDepotSalsaMD<RuntimePosixODirectMC>;
template class uDepotSalsaMD<RuntimeTrt>;
template class uDepotSalsaMD<RuntimeTrtMC>;
#if defined(UDEPOT_TRT_SPDK)
template class uDepotSalsaMD<RuntimeTrtSpdk>;
template class uDepotSalsaMD<RuntimeTrtSpdkArray>;
template class uDepotSalsaMD<RuntimePosixSpdk>;
template class uDepotSalsaMD<RuntimeTrtSpdkArrayMC>;
#endif

};

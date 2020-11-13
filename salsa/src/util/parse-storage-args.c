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

#include "util/parse-storage-args.h"

#include <asm-generic/errno.h>

#include "gc/gc-io-work.h"
#include "generic/compiler.h"
#include "libos/os-debug.h"
#include "libos/os-string.h"
#include "sto-ctlr/private/sto-ctlr-common.h"
#include "sto-ctlr/scm-dev-properties.h"
#include "sto-ctlr/sto-capacity-mgr-parameters.h"
#include "sto-ctlr/sto-ctlr-parameters.h"
#include "sto-ctlr/sto-ctlr.h"

char errbuf[512];

#define str_starts_with(_s1, _s2) !strncmp(_s1, _s2, sizeof(_s2)-1)

int parse_scm_args(
	struct scm_parameters *const scm_cfg,
	const unsigned int           argc,
	char                 **const argv,
	char                 **const err)
{
	int pos;
	int rc = -EINVAL;
	char *token, *val, *valendp;

	if (0 == scm_cfg->simulation && argc < 1) {
		*err = "Device not specified.";
		goto out0;
	}

	/* record valid dev arguments (starting with "/dev/") */
	scm_cfg->dev_nr = 0;
	pos = 0;
	while (pos < argc && str_starts_with(argv[pos], "/dev/")) {
		unsigned di = scm_cfg->dev_nr++;
		char *dev_name = scm_cfg->cfg_dev_names[di];
		snprintf(dev_name, SCM_MAX_DEV_NAME, "%s", argv[pos]);
		pos++;
	}

	for (; pos < argc; pos++) {
		token = argv[pos];
		if (NULL == (val = strchr(token, '='))) {
			snprintf(errbuf, sizeof(errbuf), "Argument '%s' invalid!" \
				" (missing '=')\n", token);
			*err = errbuf;
			goto out0;
		}
		if (str_starts_with(token, "dev_size=")) {
			s64 argument_value = os_strtoul(val+1, &valendp, 0);
			scm_cfg->dev_size_raw = argument_value;
		} else if (str_starts_with(token, "grain_size=")) {
			s64 argument_value = os_strtoul(val+1, &valendp, 0);
			scm_cfg->grain_size = argument_value;
		} else if (str_starts_with(token, "rmap_grain_size=")) {
			s64 argument_value = os_strtoul(val+1, &valendp, 0);
			scm_cfg->rmap_grain_size = argument_value;
		} else if (str_starts_with(token, "segment_size=")) {
			s64 argument_value = os_strtoul(val+1, &valendp, 0);
			scm_cfg->segment_size = argument_value;
		} else if (str_starts_with(token, "raid=")) {
			s64 argument_value = os_strtoul(val+1, &valendp, 0);
			scm_cfg->raid = argument_value;
		} else if (str_starts_with(token, "dev_nr=")) {
			s64 argument_value = os_strtoul(val+1, &valendp, 0);
			scm_cfg->dev_nr = argument_value;
		} else if (str_starts_with(token, "gc_type=")) {
			s64 argument_value = os_strtoul(val+1, &valendp, 0);
			scm_cfg->gc_params.gc = argument_value;
		} else if (str_starts_with(token, "bin_nr=")) {
			s64 argument_value = os_strtoul(val+1, &valendp, 0);
			scm_cfg->gc_params.bin_nr = argument_value;
		} else if (str_starts_with(token, "gc_thread_nr=")) {
			s64 argument_value = os_strtoul(val+1, &valendp, 0);
			scm_cfg->gc_params.thread_nr = argument_value;
		} else if (str_starts_with(token, "simulation=")) {
			s64 argument_value = os_strtoul(val+1, &valendp, 0);
			scm_cfg->simulation = !!(argument_value);
		} else if (str_starts_with(token, "must_rebuild=")) {
			s64 argument_value = os_strtoul(val+1, &valendp, 0);
			scm_cfg->must_rebuild = !!(argument_value);
		} else if (str_starts_with(token, "use_rmap=")) {
			s64 argument_value = os_strtoul(val+1, &valendp, 0);
			scm_cfg->use_rmap = !!(argument_value);
		} else if (str_starts_with(token, "dont_rebuild=")) {
			s64 argument_value = os_strtoul(val+1, &valendp, 0);
			scm_cfg->dont_rebuild = !!(argument_value);
		} else if (str_starts_with(token, "gc_xnp_pct=")) {
			s64 argument_value = os_strtoul(val+1, &valendp, 0);
			scm_cfg->gc_params.xnp_pct = argument_value;
		} else if (str_starts_with(token, "gc_xnp_hot_pct=")) {
			s64 argument_value = os_strtoul(val+1, &valendp, 0);
			scm_cfg->gc_params.xnp_hot_pct = argument_value;
		} else if (str_starts_with(token, "gc_low_wm=")) {
			s64 argument_value = os_strtoul(val+1, &valendp, 0);
			scm_cfg->gc_params.low_watermark = argument_value;
		} else if (str_starts_with(token, "gc_high_wm=")) {
			s64 argument_value = os_strtoul(val+1, &valendp, 0);
			scm_cfg->gc_params.high_watermark = argument_value;
		} else if (str_starts_with(token, "seg_qs_conf=")) {
			s64 argument_value = os_strtoul(val+1, &valendp, 0);
			scm_cfg->seg_qs_conf = argument_value;
		} else if (str_starts_with(token, "scm_dev=")) {
			// in userspace we might want to use regulare files,
			// instead of device files so add another parameter to
			// add device files
			unsigned di = scm_cfg->dev_nr++;
			char *dev_name = scm_cfg->cfg_dev_names[di];
			snprintf(dev_name, SCM_MAX_DEV_NAME, "%s", val+1);
		// scm_props_devX=... X=0,1,...,6 (=SCM_MAX_DEVICES)
		} else if (str_starts_with(token, "scm_props_dev")) {
			int idx = strlen("scm_props_dev");
			int dev = os_strtoul(token + idx, &valendp, 0);
			if (dev >= SCM_MAX_DEVICES) {
				snprintf(errbuf, sizeof(errbuf), "Invalid dev (%d) for scm_props_dev: %s\n", dev, token);
				*err = errbuf;
				goto out0;
			}
			scm_dev_props_parse(scm_cfg->cfg_dev_props + dev, val + 1);
			MSG("user configuration for device %d:", dev);
			scm_dev_props_print(&scm_cfg->cfg_dev_props[dev], "\t");
		} else {
			snprintf(errbuf, sizeof(errbuf), "Argument '%s' is invalid! "\
				"(nonexisting key)\n", token);
			*err = errbuf;
			goto out0;
		}
	}


	if (scm_cfg->dev_size_raw && !scm_cfg->simulation) {
		MSG("----");
		MSG("WARNING: dev_size argument is ignored");
		MSG("For kernel, use the dm table to pass a user-specified device size");
		MSG("----");
		scm_cfg->dev_size_raw = 0;
	}

	switch (scm_cfg->dev_nr) {
		case 0:
			if (!scm_cfg->simulation) {
				snprintf(errbuf, sizeof(errbuf), "parse_scm_args(): no devices found\n");
				*err = errbuf;
				goto out0;
			} else {
				char *dev_name = scm_cfg->cfg_dev_names[0];
				scm_cfg->dev_nr = 1;
				snprintf(dev_name, SCM_MAX_DEV_NAME, "SIM-DEVICE");
			}
			break;
		case 1:
			scm_cfg->raid = RAID0;
			break;
	}

	rc = 0;

out0:
	return rc;
}

int parse_storage_args(
	struct sto_ctlr_parameters *const sto_cfg,
	const unsigned int                argc,
	char                      **const argv,
	char                      **const err)
{
	int pos;
	int rc = -EINVAL;
	char *token, *val, *valendp;
	s64 argument_value;
	pos = 0;
	/* skip valid dev arguments */
	while (pos < argc && str_starts_with(argv[pos], "/dev/"))
		pos++;
	for (; pos < argc; pos++) {
		token = argv[pos];
		if (NULL == (val = strchr(token, '='))) {
			snprintf(errbuf, sizeof(errbuf), "Argument '%s' invalid!" \
				" (missing '=')\n", token);
			*err = errbuf;
			goto out0;
		}
		argument_value = os_strtoul(val+1, &valendp, 0);
		if (str_starts_with(token, "dev_size=")) {
			sto_cfg->dev_size = argument_value;
		} else if (str_starts_with(token, "page_size=")) {
			sto_cfg->page_size = argument_value;
		} else if (str_starts_with(token, "small_page_size=")) {
			sto_cfg->small_page_size = argument_value;
		} else if (str_starts_with(token, "type=")) {
			sto_cfg->type = argument_value;
		} else if (str_starts_with(token, "stream_nr=")) {
			sto_cfg->stream_nr = argument_value;
		} else if (str_starts_with(token, "rel_stream_nr=")) {
			sto_cfg->rel_stream_nr = argument_value;
		} else if (str_starts_with(token, "generation_nr=")) {
			sto_cfg->generation_nr = argument_value;
		} else if (str_starts_with(token, "cache_size=")) {
			sto_cfg->cache_size = argument_value;
		} else if (str_starts_with(token, "pages_per_md_pg=")) {
			sto_cfg->pages_per_md_pg = argument_value;
		} else if (str_starts_with(token, "small_pages_per_md_pg=")) {
			sto_cfg->small_pages_per_md_pg = argument_value;
		} else if (str_starts_with(token, "precondition=")) {
			sto_cfg->precondition = argument_value;
		} else if (str_starts_with(token, "inject_read_delay=")) {
			sto_cfg->inject_read_delay = argument_value;
		} else if (str_starts_with(token, "inject_write_delay=")) {
			sto_cfg->inject_write_delay = argument_value;
		} else if (str_starts_with(token, "overprovision=")) {
			sto_cfg->overprovision = argument_value;
		} else if (str_starts_with(token, "uip_cache_size=")) {
			sto_cfg->uip_cache_size = argument_value;
		} else if (str_starts_with(token, "lba_heat_bits=")) {
			sto_cfg->lba_heat_bits = argument_value;
		} else if (str_starts_with(token, "precond_rnd_percent=")) {
			sto_cfg->precond_rnd_percent = argument_value;
		} else if (str_starts_with(token, "props_mask=")) {
			sto_cfg->sc_prop_set = (struct scm_prop_set){.mask__ = argument_value};
		} else if (str_starts_with(token, "small_props_mask=")) {
			sto_cfg->sc_small_prop_set = (struct scm_prop_set){.mask__ = argument_value};
		} else if (str_starts_with(token, "dont_rebuild=")) {
			sto_cfg->dont_rebuild = !!argument_value;
		} else if (str_starts_with(token, "magic_stream_oracle=")) {
			sto_cfg->magic_stream_oracle = !!argument_value;
		} else {
			snprintf(errbuf, sizeof(errbuf), "Argument '%s' invalid! "\
				"(nonexisting key)\n", token);
			*err = errbuf;
			goto out0;
		}
	}

	// if sc_small_prop_set was not set, use the sc_prop_set value
	if (scm_prop_set_is_empty(sto_cfg->sc_small_prop_set))
		sto_cfg->sc_small_prop_set = sto_cfg->sc_prop_set;

	rc = 0;
out0:
	return rc;
}

int check_storage_params(
	struct sto_ctlr_parameters *const sto_cfg,
	char                      **const err)
{
	int rc = -EINVAL;
	if (sto_cfg->page_size == 0 || !is_power_of_two(sto_cfg->page_size)) {
		*err = "Invalid cfg value: invalid page size";
		goto out0;
	}
	if (sto_cfg->stream_nr == 0) {
		*err = "Invalid cfg value: invalid stream_nr (==0)";
		goto out0;
	}
	if (sto_cfg->cache_size < 1) {
		*err = "Invalid cfg value: invalid cache_size (< 2)";
		goto out0;
	}
	if (sto_cfg->precondition < 0 || sto_cfg->precondition > 1) {
		*err = "Invalid cfg value: precondition "	\
			"should be in range [0, 1]";
		goto out0;
	}
	if (sto_cfg->overprovision < 20 ||
		1000 <= sto_cfg->overprovision) {
		*err = "Invalid cfg value: overprovision " \
			"should be in range (20, 1000)";
		goto out0;
	}

	DBG("Storage config: page_size        = %d", sto_cfg->page_size);
	DBG("Storage config: stream_nr        = %d", sto_cfg->stream_nr);
	DBG("Storage config: precondition     = %d", sto_cfg->precondition);
	DBG("Storage config: overprovision    = %d", sto_cfg->overprovision);
	rc = 0;
out0:
	return rc;
}

int check_sto_capacity_mgr_parameters(
	const struct scm_parameters *const scm_cfg)
{
	u64 dev_size;
	int err = -EINVAL;

	if (!scm_cfg->simulation && (scm_cfg->grain_size < PAGE_SIZE ||
			SALSA_MAX_GRAIN_SIZE < scm_cfg->grain_size)) {
		ERR("Invalid parameter grain_size=%u min=%lu max=%u",
			scm_cfg->grain_size, PAGE_SIZE,
			SALSA_MAX_GRAIN_SIZE);
		goto fail0;
	}
	if (unlikely(!is_power_of_two(scm_cfg->grain_size))) {
		ERR("Invalid parameter grain_size=%u not a power of two",
			scm_cfg->grain_size);
		goto fail0;
	}
	if (unlikely(scm_cfg->segment_size == 0 ||
		 SALSA_MAX_SEGMENT_SIZE < scm_cfg->segment_size)) {
		ERR("Invalid parameter segment_size=%"PRIu64" max=%u",
		    scm_cfg->segment_size, SALSA_MAX_SEGMENT_SIZE);
		goto fail0;
	}
	if (unlikely(scm_cfg->raid != RAID0 && scm_cfg->raid != RAID5 && scm_cfg->raid != SALSA_LINEAR)) {
		ERR("Unsupported raid type=%u",
			scm_cfg->raid);
		goto fail0;
	}

	dev_size = scm_dev_props_size(&scm_cfg->cfg_scm_props);
	if (unlikely(dev_size <
			(scm_cfg->segment_size * (u64)scm_cfg->grain_size *
				(SALSA_MIN_REL_SEGMENTS)))) {
		ERR("Invalid parameter dev_size=%"PRIu64" too small"\
			" (seg*page=%"PRIu64")", dev_size,
			(u64) scm_cfg->segment_size * scm_cfg->grain_size);
		goto fail0;
	}

	if (!scm_cfg->simulation && ((u64) PBA_INVAL_ENTRY * scm_cfg->grain_size < dev_size)) {
		ERR("Invalid parameter dev_size=%"PRIu64" too big"\
			" (max-size=%"PRIu64")", dev_size,
			(u64) PBA_INVAL_ENTRY * scm_cfg->grain_size);
		goto fail0;
	}


	err = 0;
fail0:
	return err;
}

int check_ctlr_storage_parameters(
	const struct sto_ctlr_parameters *const ctlr_cfg)
{
	int err = EINVAL;
	struct sto_capacity_mgr *const scm = ctlr_cfg->scm;
	const u64 grain_size = scm_get_grain_size(scm);
	const u64 queue_max_grains = scm_get_queue_max_hw_grains(scm);
	if (unlikely(NULL == scm)) {
		ERR("Invalid parameter null scm");
		goto fail0;
	}
	if (unlikely(0 == ctlr_cfg->page_size ||
			0 != (ctlr_cfg->page_size % scm_get_grain_size(scm)) ||
			queue_max_grains * grain_size < ctlr_cfg->page_size)) {
		ERR("Invalid parameter page_size=%u ", ctlr_cfg->page_size);
		goto fail0;
	}

	err = 0;
fail0:
	return err;
}

int check_lsa_storage_parameters(
	const struct sto_ctlr_parameters *const lsa_cfg)
{
	int err = EINVAL;
	const struct sto_capacity_mgr *const scm = lsa_cfg->scm;
	const u64 seg_size = scm_get_seg_size(scm);
	const u64 grain_size = scm_get_grain_size(scm);
	const u64 rmap_size = scm_get_rmap_size(scm);
	const u32 page_in_grains = lsa_cfg->page_size / grain_size;
	const u32 min_seg_nr = 1 + 2 * SALSA_MIN_REL_SEGMENTS +
		(lsa_cfg->rel_stream_nr + lsa_cfg->stream_nr) * lsa_cfg->cache_size;

	if (unlikely(0 == lsa_cfg->page_size ||
			0 != (lsa_cfg->page_size % scm_get_grain_size(scm)))) {
		ERR("Invalid parameter page_size=%u ",
			lsa_cfg->page_size);
		goto fail0;
	}

	if (unlikely(0 == lsa_cfg->page_size ||
			0 != (lsa_cfg->page_size % scm_get_grain_size(scm)))) {
		ERR("Invalid parameter page_size=%u ",
			lsa_cfg->page_size);
		goto fail0;
	}
	if(unlikely(0 != (seg_size % page_in_grains))) {
		ERR("Invalid parameter page_size=%u grain_size=%"PRIu64" seg_size=%"PRIu64,
			lsa_cfg->page_size, grain_size, seg_size);
		goto fail0;
	}
	if (unlikely(!is_power_of_two(lsa_cfg->page_size))) {
		ERR("Invalid parameter page_size=%u not a power of two",
			lsa_cfg->page_size);
		goto fail0;
	}
	if (SALSA_GC_MAX_IO_WORKERS < page_in_grains) {
		ERR("Invalid parameter page_size in grains=%u bigger than GC max IO buffer=%u grains",
			page_in_grains, SALSA_GC_MAX_IO_WORKERS);
		goto fail0;
	}
	if ((page_in_grains < rmap_size || 0 != (page_in_grains % rmap_size)) &&
		lsa_cfg->type != STO_CTLR_LSA_SMALL) {
		ERR("Invalid parameter page_size=%u not a multiple of rmap size=%"PRIu64,
			lsa_cfg->page_size, rmap_size * grain_size);
		goto fail0;
	}
	if (lsa_cfg->large_page_size <= lsa_cfg->small_page_size &&
		lsa_cfg->type == STO_CTLR_LSA_SMALL) {
		ERR("Invalid parameter small_page_size=%u >= large_page_size=%u",
			lsa_cfg->small_page_size, lsa_cfg->large_page_size);
		goto fail0;
	}
	if (unlikely(lsa_cfg->dev_size < seg_size * grain_size)) {
		ERR("Invalid parameter dev_size=%"PRIu64" too small"\
			" (seg*page=%"PRIu64")", lsa_cfg->dev_size,
			(u64) seg_size * grain_size);
		goto fail0;
	}
	if (unlikely(lsa_cfg->dev_size <= lsa_cfg->logical_size)) {
		ERR("Invalid parameter dev_size=%"PRIu64" smaller than "\
			"logical_size=%"PRIu64,
			lsa_cfg->dev_size, lsa_cfg->logical_size);
		goto fail0;
	}
	if (unlikely(lsa_cfg->dev_size % lsa_cfg->page_size != 0)) {
		ERR("Invalid parameter dev_size=%"PRIu64" not divisible by "\
			"page_size=%u", lsa_cfg->dev_size, lsa_cfg->page_size);
		goto fail0;
	}
	if (unlikely(lsa_cfg->logical_size % lsa_cfg->page_size != 0)) {
		ERR("Invalid parameter logical_size=%"PRIu64" not divisible "\
			"by page_size=%u", lsa_cfg->logical_size,
			lsa_cfg->page_size);
		goto fail0;
	}
	if (unlikely(SALSA_MAX_STREAMS < lsa_cfg->stream_nr)) {
		ERR("Invalid parameter stream_nr=%u larger than %u "\
			"(max for streams)", lsa_cfg->stream_nr,
			SALSA_MAX_STREAMS);
		goto fail0;
	}
	if (unlikely(SALSA_MAX_STREAMS < lsa_cfg->rel_stream_nr)) {
		ERR("Invalid parameter rel_stream_nr=%u larger than %u "\
			"(max for streams)", lsa_cfg->rel_stream_nr,
			SALSA_MAX_STREAMS);
		goto fail0;
	}
	if (lsa_cfg->cache_size < 1) {
		ERR("Invalid cfg value: invalid cache_size (< 1)");
		goto fail0;
	}
	if (unlikely(SALSA_MAX_GENERATIONS < lsa_cfg->generation_nr ||
			lsa_cfg->stream_nr < lsa_cfg->generation_nr)) {
		ERR("Invalid parameter generation_nr=%u larger than streams %u "\
			"(max for generations)", lsa_cfg->generation_nr,
			lsa_cfg->stream_nr);
		goto fail0;
	}
	if ((lsa_cfg->dev_size - lsa_cfg->logical_size) / (seg_size * grain_size) < min_seg_nr &&
		lsa_cfg->type != STO_CTLR_LSA_SMALL) {
		ERR("Overprovisioning space less than MIN segs =%u"\
			", aborting. logical=%"PRIu64" physical=%"PRIu64,
			min_seg_nr, lsa_cfg->logical_size,
			lsa_cfg->dev_size);
		goto fail0;
	}

	if ((lsa_cfg->dev_size / (u64) lsa_cfg->page_size) > (1ULL << LBA_PBA_BITS)) {
		ERR("Device larger than supported. Max 16TiB requested=%"PRIu64"GiB",
			lsa_cfg->dev_size / (1UL << 30));
		goto fail0;
	}

	if (300 < lsa_cfg->precond_rnd_percent) {
		ERR("Wrong number of rnd precondition given %u.",
			lsa_cfg->precond_rnd_percent);
		goto fail0;
	}

	err = 0;
fail0:
	return err;
}

int set_storage_params_dev_size(
	struct sto_ctlr_parameters *const sto_cfg,
	u64                               dev_size, /* # sectors */
	char                      **const err)
{
	int rc = 0;
	u64 logical_size;
	u64 logical_utilization;
	const u64 page_ssize = B2SEC(sto_cfg->page_size);
	dev_size = align_down(dev_size, page_ssize);
	if (unlikely(SEC2B(dev_size) < sto_cfg->page_size)) {
		*err = "Device size too small";
		rc = -EINVAL;
		goto out0;
	}
	logical_utilization = 1000ULL - sto_cfg->overprovision;
	logical_size = (dev_size * logical_utilization) / (1000ULL);
	/* make logical_size upper aligned to page_size */
	logical_size = uceil_up(logical_size, page_ssize);
	sto_cfg->dev_size     = SEC2B(dev_size);
	sto_cfg->logical_size = SEC2B(logical_size);
out0:
	return rc;
}

void reset_gc_config(struct gc_parameters *const gc_cfg)
{
	gc_cfg->bin_nr         = 4;
	gc_cfg->gc             = GC_NBIN_XNP_OVRW_HEAT_SEG;
	gc_cfg->low_watermark  = 3U;
	gc_cfg->high_watermark = 5U;
	gc_cfg->xnp_pct        = 9;
	gc_cfg->xnp_hot_pct    = 0;
	gc_cfg->thread_nr      = 1;
}

void reset_scm_config(struct scm_parameters *const scm_cfg)
{
	unsigned i;
	memset(scm_cfg, 0, sizeof(*scm_cfg));
	scm_cfg->dev_size_raw        = 0UL; /* init to impossible value */
	scm_cfg->grain_size          = 4096; /* # of bytes per page */
	scm_cfg->segment_size        = 64; /* # of pages per segment */
	scm_cfg->rmap_grain_size     = 1; /* # of pages per segment */
	scm_cfg->simulation          = 0;
	scm_cfg->must_rebuild        = 0;
	scm_cfg->use_rmap            = 0;
	scm_cfg->dont_rebuild        = 0;
	scm_cfg->raid                = RAID0;
	scm_cfg->dev_nr              = 1;
	scm_cfg->seg_qs_conf         = SCM_SEG_QUEUES_ONE;
	reset_gc_config(&scm_cfg->gc_params);

	scm_dev_props_reset(&scm_cfg->cfg_scm_props);
	for (i=0; i<SCM_MAX_DEVICES; i++)
		scm_dev_props_reset(scm_cfg->cfg_dev_props + i);

}

void reset_storage_config(struct sto_ctlr_parameters *const sto_cfg)
{
	struct timeval now;
	if (0 != gettimeofday(&now, NULL)) {
		perror("gettimeofday");
		exit(1);
	}
	memset(sto_cfg, 0, sizeof(*sto_cfg));
	sto_cfg->page_size           = 4096; /* # of bytes per page */
	sto_cfg->small_page_size     = 4096; /* # of bytes per page */
	sto_cfg->stream_nr           = 1;
	sto_cfg->rel_stream_nr       = 1;
	sto_cfg->generation_nr       = 1;
	sto_cfg->precondition        = 0;
	sto_cfg->overprovision       = 200; /* thousandths of physical space */
	sto_cfg->type                = STO_CTLR_LSA;
	sto_cfg->lba_heat_bits       = 0;
	sto_cfg->uip_cache_size      = (1U<<25); /* bytes */
	sto_cfg->precond_rnd_percent = 30;
	sto_cfg->dev_size            = 0UL; /* init to impossible value */
	sto_cfg->logical_size        = 0UL; /* init to impossible value */
	sto_cfg->small_logical_size  = 0UL; /* init to impossible value */
	sto_cfg->cache_size          = 4;
	sto_cfg->inject_write_delay  = 0;
	sto_cfg->inject_read_delay   = 0;
	sto_cfg->was_rebuild         = false;
	sto_cfg->dont_rebuild        = false;
	sto_cfg->sc_prop_set         = SCM_PROP_SET_ALL;
	sto_cfg->sc_small_prop_set   = SCM_PROP_SET_EMPTY;
	sto_cfg->seed                = now.tv_sec;
	sto_cfg->small_pages_per_md_pg = 510;
}

void set_private_storage_config(
	struct sto_ctlr_parameters *const sto_cfg,
	void                       *const private)
{
	sto_cfg->private = private;
}

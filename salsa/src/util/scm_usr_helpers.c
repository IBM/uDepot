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


#if !defined(_LARGEFILE_SOURCE)
#define _LARGEFILE_SOURCE
#endif

#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

#include "sto-ctlr/scm-dev-properties.h"
#include "sto-ctlr/sto-capacity-mgr-parameters.h"
#include "sto-ctlr/private/sto-capacity-mgr-common.h"
#include "util/parse-storage-args.h"
#include "util/scm_usr_helpers.h"

// TODO: zbc stuff (see dm_dev_set_props)
static int
usr_dev_set_props(const char *devname, struct scm_dev_properties *props)
{
	int fd, ret;
	off_t size;

	// already set by the user: return
	if (scm_dev_props_valid(props))
		return 0;

	fd = open(devname, O_RDONLY);
	if (fd < 0) {
		perror(devname);
		return errno;
	}

	ret = EINVAL;
	size =  lseek64(fd, 0, SEEK_END);
	if (size == (off_t)-1) {
		perror("lseek64");
		goto end;
	}

	ret = 0;
	scm_dev_props_reset(props);
	scm_dev_props_add_range_prop(props,
		SCM_DEV_RPROP(size, SCM_PROP_DEFAULT));

end:
	close(fd);
	return ret;
}

static int
usr_scm_parse_devs(struct scm_parameters *cfg)
{
	unsigned i;
	int err;

	for (i=0; i < cfg->dev_nr; i++) {
		char *devname = cfg->cfg_dev_names[i];
		struct scm_dev_properties *props = cfg->cfg_dev_props + i;

		err = usr_dev_set_props(devname, props);
		if (err)
			return err;

		MSG("Device properties for %s", devname);
		scm_dev_props_print(props, "  ");
	}

	return 0;
}

__attribute__ ((warn_unused_result)) int
scm_usr_init_cfg(char **const argv, const int argc, struct scm_parameters *scm_cfg)
{
	char *err_str=NULL;
	int err;

	reset_scm_config(scm_cfg);
	err = parse_scm_args(scm_cfg, argc, argv, &err_str);
	if (0 != err) {
		fprintf(stderr, "parse_scm_args failed with: %d %s.\n", err, err_str);
		return -1;
	}

	printf("dev_nr=%u sim=%d\n", scm_cfg->dev_nr, scm_cfg->simulation);
	if (!scm_cfg->simulation) {
		usr_scm_parse_devs(scm_cfg);
	} else {
		scm_dev_props_reset(&scm_cfg->cfg_dev_props[0]);
		scm_dev_props_add_range_prop(&scm_cfg->cfg_dev_props[0],
					SCM_DEV_RPROP(scm_cfg->dev_size_raw, SCM_PROP_DEFAULT));
	}

	err = scm_cfg_set_scm_props(scm_cfg, 0, &err_str);
	if (err < 0) {
		fprintf(stderr, "scm_cfg_set_scm_props failed: %s\n", err_str);
		return -EINVAL;
	}

	if (0 == scm_dev_props_size(&scm_cfg->cfg_scm_props)) {
		fprintf(stderr, "Could not determine device size! Abort.\n");
		return -EINVAL;
	}

	return 0;

}

/*
 *  Copyright (c) 2020 International Business Machines
 *  All rights reserved.
 *
 *  SPDX-License-Identifier: BSD-3-Clause
 *
 *  Authors: Kornilios Kourtis (kou@zurich.ibm.com, kornilios@gmail.com)
 *
 */

// vim: set expandtab softtabstop=4 tabstop:4 shiftwidth:4:

#include <unordered_map>
#include <utility>

#include "spdk.hh"

#include <rte_config.h>
#include <rte_mempool.h>
#include <rte_malloc.h>

#include <spdk/nvme.h>
#include <spdk/env.h>
#include "spdk/nvme_intel.h"

extern "C" {

#include "trt_util/cpumask_utils.h" // cpumask_to_str()

}

// most stuff copied from: spdk/examples/nvme/perf/perf.c

void
SpdkGlobalState::register_ns(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_ns *ns)
{
	SpdkNamespace *nspace;
	const struct spdk_nvme_ctrlr_data *cdata;

	cdata = spdk_nvme_ctrlr_get_data(ctrlr);

	if (!spdk_nvme_ns_is_active(ns)) {
		printf("Controller %-20.20s (%-20.20s): Skipping inactive NS %u\n",
		       cdata->mn, cdata->sn,
		       spdk_nvme_ns_get_id(ns));
		return;
	}

	#if 0
	if (spdk_nvme_ns_get_size(ns) < g_io_size_bytes ||
	    spdk_nvme_ns_get_sector_size(ns) > g_io_size_bytes) {
		printf("WARNING: controller %-20.20s (%-20.20s) ns %u has invalid "
		       "ns size %" PRIu64 " / block size %u for I/O size %u\n",
		       cdata->mn, cdata->sn, spdk_nvme_ns_get_id(ns),
		       spdk_nvme_ns_get_size(ns), spdk_nvme_ns_get_sector_size(ns), g_io_size_bytes);
		return;
	}
	#endif

	void *nspace__ = malloc(sizeof(SpdkNamespace));
	if (nspace__ == NULL) {
		perror("ns_entry malloc");
		exit(1);
	}
	nspace = new(nspace__) SpdkNamespace();

	nspace->sn_ctlr = ctrlr;
	nspace->sn_namespace = ns;

	#if 0
	entry->size_in_ios = spdk_nvme_ns_get_size(ns) /
			     g_io_size_bytes;
	entry->io_size_blocks = g_io_size_bytes / spdk_nvme_ns_get_sector_size(ns);
	#endif

	snprintf(nspace->sn_name, 44, "%-20.20s (%-20.20s)", cdata->mn, cdata->sn);
    fprintf(stderr, "adding namespace: %s\n", nspace->sn_name);
    sg_namespaces.push_back(*nspace);
}

void
SpdkGlobalState::register_ctrlr(struct spdk_nvme_ctrlr *ctrlr) {

    int nsid, num_ns;
    const struct spdk_nvme_ctrlr_data *cdata = spdk_nvme_ctrlr_get_data(ctrlr);
    SpdkController *c = (SpdkController *)malloc(sizeof(SpdkController));
    if (c == NULL) {
        perror("ctrlr_entry malloc");
        exit(1);
    }

    new (c) SpdkController();

    c->sc_gstate = this;
    c->sc_ctlr = ctrlr;
    c->sc_latency_page =
        (struct spdk_nvme_intel_rw_latency_page *)rte_zmalloc(
            "nvme latency", sizeof(struct spdk_nvme_intel_rw_latency_page),
            4096);
    if (c->sc_latency_page == NULL) {
        printf("Allocation error (latency page)\n");
        exit(1);
    }

    snprintf(c->sc_name, sizeof(c->sc_name), "%-20.20s (%-20.20s)",
             cdata->mn, cdata->sn);

    sg_controllers.push_back(*c);

    #if 0
    if (g_latency_tracking_enable &&
        spdk_nvme_ctrlr_is_feature_supported(
            ctrlr, SPDK_NVME_INTEL_FEAT_LATENCY_TRACKING))
        set_latency_tracking_feature(ctrlr, true);
    #endif

    num_ns = spdk_nvme_ctrlr_get_num_ns(ctrlr);
    for (nsid = 1; nsid <= num_ns; nsid++) {
        register_ns(ctrlr, spdk_nvme_ctrlr_get_ns(ctrlr, nsid));
    }
}

static bool probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
                     struct spdk_nvme_ctrlr_opts *opts) {
	struct spdk_pci_addr	pci_addr;
	struct spdk_pci_device	*pci_dev;
	struct spdk_pci_id	pci_id;

	if (trid->trtype != SPDK_NVME_TRANSPORT_PCIE) {
		printf("Attaching to NVMe over Fabrics controller at %s:%s: %s\n",
		       trid->traddr, trid->trsvcid,
		       trid->subnqn);
	} else {
		if (spdk_pci_addr_parse(&pci_addr, trid->traddr)) {
			return false;
		}

		pci_dev = spdk_pci_get_device(&pci_addr);
		if (!pci_dev) {
			return false;
		}

		pci_id = spdk_pci_device_get_id(pci_dev);

		printf("Attaching to NVMe Controller at %s [%04x:%04x]\n",
		       trid->traddr,
		       pci_id.vendor_id, pci_id.device_id);
	}

    return true;
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	struct spdk_pci_addr	pci_addr;
	struct spdk_pci_device	*pci_dev;
	struct spdk_pci_id	pci_id;

	if (trid->trtype != SPDK_NVME_TRANSPORT_PCIE) {
		printf("Attached to NVMe over Fabrics controller at %s:%s: %s\n",
		       trid->traddr, trid->trsvcid,
		       trid->subnqn);
	} else {
		if (spdk_pci_addr_parse(&pci_addr, trid->traddr)) {
			return;
		}

		pci_dev = spdk_pci_get_device(&pci_addr);
		if (!pci_dev) {
			return;
		}

		pci_id = spdk_pci_device_get_id(pci_dev);

		printf("Attached to NVMe Controller at %s [%04x:%04x]\n",
		       trid->traddr,
		       pci_id.vendor_id, pci_id.device_id);
	}

    SpdkGlobalState *spdk = static_cast<SpdkGlobalState *>(cb_ctx);
    spdk->register_ctrlr(ctrlr);
}

static int
spdk_init(void)
{
    struct spdk_env_opts opts;

    printf("Initializing SPDK\n");
    spdk_env_opts_init(&opts);
    // see include/spdk/env.h for members
    opts.name = "trt_spdk";
    opts.shm_id = -1;

    // pass our affinity as the core mask for SPDK
    int err;
    cpu_set_t cpuset[1];   // good for 1024 cpus
    size_t setsize = sizeof(cpuset);
    char cpuset_s[128];
    err = sched_getaffinity(0, setsize, cpuset);
    if (err) {
        perror("sched_getaffinity");
        return 1;
    }

    err = cpumask_to_str(cpuset, setsize, cpuset_s, sizeof(cpuset_s));
    if (err) {
        fprintf(stderr, "cpumask_to_str failed: %d (%s)", err, strerror(err));
        return 1;
    }

    opts.core_mask = cpuset_s;

    // NB: spdk_env_init() calls rte_eal_init().
    // It seems that it would be tricky if we want to initialize dpdk as well
    spdk_env_init(&opts);

    return 0;
}

bool SpdkGlobalState::initialized = false;

int SpdkGlobalState::register_controllers(void) {

    int err;

    if (SpdkGlobalState::initialized) {
        fprintf(stderr, "SPDK already initialized\n");
        abort();
    } else {
        SpdkGlobalState::initialized = true;
    }

    err = spdk_init();
    if (err) {
        fprintf(stderr, "spdk_init() failed\n");
        return 1;
    }

    printf("Initializing NVMe controllers\n");
    if (spdk_nvme_probe(NULL /* local PCIe */, this, probe_cb, attach_cb, NULL) != 0) {
        fprintf(stderr, "spdk_nvme_probe() failed\n");
        return 1;
    }

    return 0;
}

void SpdkGlobalState::unregister_controllers(void)
{
	while (sg_namespaces.size() != 0) {
	    SpdkNamespace &n = sg_namespaces.front();
	    sg_namespaces.pop_front();
	    free(&n);
	}

	while (sg_controllers.size() != 0) {
		SpdkController &c = sg_controllers.front();
		sg_controllers.pop_front();
		rte_free(c.sc_latency_page);
		#if 0
		if (g_latency_tracking_enable &&
		    spdk_nvme_ctrlr_is_feature_supported(entry->ctrlr, SPDK_NVME_INTEL_FEAT_LATENCY_TRACKING))
			set_latency_tracking_feature(entry->ctrlr, false);
		#endif
		free(&c);
	}

	// Cant find a function to shutdown the spdk environment
	// SpdkGlobalState::initialized = false;
}


// internal function to initialize local spdk state
void
SpdkState::init(SpdkGlobalState &sg) {
    assert(spdk_state == State::UNINITIALIZED);
    assert(sg.sg_state == SpdkGlobalState::State::INITIALIZED);
    // Create one queue pair for every namespace in the global state
    // NB: we assume that sg_controllers, sg_namespaces lists are immutable.
    //     If this assumption breaks, we need to synchronize with other
    //     SpdkStates.
    for (SpdkNamespace &ns: sg.sg_namespaces) {
        spdk_qpairs[&ns] = std::make_shared<SpdkQpair>(this, &ns);
        SpdkQpair *qp = spdk_qpairs[&ns].get();
        // printf("SpdkState: %p added queue: %p for ns: %s %p on thread: %lu\n", this, qp, ns.sn_name, &ns, pthread_self());
    }

    spdk_state = State::READY;

    // initialize buffer memory pool
    // for some reason rte_mempool_create() fails here.
    #if 0
    char name[128];
    snprintf(name, sizeof(name), "data_mpool-%p", this);
    size_t buff_size = SpdkState::BPOOL_BUFF_SIZE;
    spdk_bpool = rte_mempool_create(
        name,             // name of the mem pool (has to be unique)
        512,              // (total?) size of the pool
        buff_size,        // element size
        32,               // size of the local cache
        0,                // private mpool data
        NULL,             // mp_init: initialize mp fn (e.g., for private data)
        NULL,             // mp_init argument
        NULL,             // obj_init
        NULL,             // obj_init arg
        SOCKET_ID_ANY,
        MEMPOOL_F_SP_PUT | MEMPOOL_F_SC_GET // single-producer / single-consumer
    );
    if (spdk_bpool == nullptr) {
        fprintf(stderr, "failed to allocate rte %s mempool, aborting.\n", name);
        abort();
    }
    #endif
}

// Buffer allocation
//
// The standard way to allocate/free a buffer is to use
// SpdkAlloc::alloc_iobuff()/free_iobuff(). These functions use
// rte_malloc_socket() and rte_free().
//
// The functions below allow for a per-thread buffer allocation cache. The cache
// is currently not implemented becuase there is an issue with
// rte_mempool_create(), and also because in all the cases we care about
// performance we cache buffers on a higher level (e.g., using Mbuffs).

SpdkPtr
SpdkQpair::alloc_buffer(size_t nlbas, int numa_node, char *rte_type) {
    // NB: here we build an spdk buffer without going over RteAlloc. I could
    // probably remove this interface, but I just wanted to illustrate that
    // we can gnerate SpdkPtr (or IoPtr) out of thin air if we know what we
    // are doing.
    const size_t sector_size = get_sector_size();
    const size_t size = nlbas*sector_size;

    auto pool = sqp_owner->spdk_bpool;
    if (pool && size <= SpdkState::BPOOL_BUFF_SIZE) {
        void *p = nullptr;
        rte_mempool_get(pool, (void **)&p);
        return SpdkPtr(p, size);
    }

    void *ptr = rte_malloc_socket(rte_type, size, sector_size, numa_node);
    return SpdkPtr(ptr, size);
}

void
SpdkQpair::free_buffer(SpdkPtr &&ptr) {
    auto pool = sqp_owner->spdk_bpool;
    if (pool && ptr.size_m <= SpdkState::BPOOL_BUFF_SIZE) {
        void *p;
        std::tie(p, std::ignore) = ptr.reset();
        rte_mempool_put(pool, (void *)p);
    } else {
        rte_free(ptr.ptr_m);
    }
}

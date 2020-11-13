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

#ifndef SPDK_HH__
#define SPDK_HH__

#include <vector>
#include <unordered_map>
#include <memory> // shared_ptr
#include <algorithm> // min
#include <vector> // min

#include <sys/uio.h> // iovec
#include <pthread.h> // pthread_self

#include <cstring>

#include <boost/intrusive/list.hpp>
namespace bi = boost::intrusive;

#include <spdk/nvme.h>

#include <rte_config.h>
#include <rte_malloc.h>
#include <rte_mempool.h>
#include <rte_errno.h>

#include "trt_util/rte.hh"

#include "misc.h"

/*
 * Simple helper for spdk
 */

using SpdkAlloc = RteAlloc<512>;
using SpdkPtr   = SpdkAlloc::Ptr;

struct SpdkGlobalState;
struct SpdkState;

struct SpdkController {
    SpdkGlobalState        *sc_gstate;
    struct spdk_nvme_ctrlr *sc_ctlr;
    struct spdk_nvme_intel_rw_latency_page *sc_latency_page;
    char sc_name[1024];
    bi::list_member_hook<> sc_lnode;
};

struct SpdkNamespace {
    struct spdk_nvme_ctrlr *sn_ctlr;
    struct spdk_nvme_ns *sn_namespace;
    char sn_name[1024];
    bi::list_member_hook<> sn_lnode;

    uint32_t get_sector_size(void) { return spdk_nvme_ns_get_sector_size(sn_namespace); }
    uint64_t get_nsectors(void)    { return spdk_nvme_ns_get_num_sectors(sn_namespace); }
    uint64_t get_size(void)        { return spdk_nvme_ns_get_size(sn_namespace); }
};

struct SpdkGlobalState {
   friend SpdkState;

   enum class State {UNINITIALIZED, INITIALIZED};

   protected:
    using Ctlr = SpdkController;
    using CtlrList = bi::list<Ctlr, bi::member_hook<Ctlr, bi::list_member_hook<>, &Ctlr::sc_lnode>>;
    using Ns = SpdkNamespace;
    using NsList = bi::list<Ns, bi::member_hook<Ns, bi::list_member_hook<>, &Ns::sn_lnode>>;

    CtlrList sg_controllers;
    NsList sg_namespaces;
    State  sg_state;
    static bool initialized;

    int register_controllers(void);
    void unregister_controllers(void);
    void register_ns(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_ns *ns);

   public:
    SpdkGlobalState() : sg_state(State::UNINITIALIZED) {}

    void init() {
        if (sg_state == State::INITIALIZED)
            return;

        int ret = register_controllers();
        if (ret < 0) {
            fprintf(stderr, "Failed to initialize controllers");
            exit(1);
        }
        sg_state = State::INITIALIZED;
    }

    void register_ctrlr(struct spdk_nvme_ctrlr *ctlr);

    ~SpdkGlobalState() { unregister_controllers(); }

    void operator=(SpdkGlobalState const &) = delete;
    SpdkGlobalState(SpdkGlobalState const &) = delete;
};


struct SpdkQpair {
    enum class State {UNINITIALIZED, READY, DRAINING, DONE};

    SpdkNamespace          *sqp_namespace;
    struct spdk_nvme_qpair *sqp_qpair;
    size_t                 sqp_npending;
    State                  sqp_state;
    SpdkState              *sqp_owner;

    SpdkQpair()
        : sqp_namespace(nullptr),
          sqp_qpair(nullptr),
          sqp_npending(0),
          sqp_state(State::UNINITIALIZED),
          sqp_owner(nullptr) {}

    SpdkQpair(SpdkState *owner, SpdkNamespace *ns)
        : sqp_namespace(ns),
          sqp_npending(0),
          sqp_state(State::READY),
          sqp_owner(owner) {
        sqp_qpair = spdk_nvme_ctrlr_alloc_io_qpair(sqp_namespace->sn_ctlr, SPDK_NVME_QPRIO_URGENT);
        if (!sqp_qpair) {
            fprintf(stderr, "spdk: queue pair allocation failed\n");
            exit(1);
        }
    }

    ~SpdkQpair() {
        switch(sqp_state) {
            case State::UNINITIALIZED:
            return;

            case State::READY:
            case State::DRAINING:
            fprintf(stderr, "BUG: SpdkQpair destructor called while at READY or DRAINING state\n");
            abort();

            case State::DONE:
            break;
        }

        assert(sqp_state == State::DONE);
        int err = spdk_nvme_ctrlr_free_io_qpair(sqp_qpair);
        if (err)
            fprintf(stderr, "BUG: spdk_nvme_ctrlr_free_io_qpair() failed. Continuing.\n");
    }

    SpdkQpair(SpdkQpair &&o) {
        //printf("%s: qp=%p o.qp=%p\n", __PRETTY_FUNCTION__, sqp_qpair, o.sqp_qpair);
        std::swap(sqp_namespace, o.sqp_namespace);
        std::swap(sqp_qpair, o.sqp_qpair);
        std::swap(sqp_npending, o.sqp_npending);
        std::swap(sqp_state, o.sqp_state);
    }

    void operator=(SpdkQpair const &) = delete;
    SpdkQpair(SpdkQpair const &) = delete;

    size_t  get_sector_size(void) {
        return sqp_namespace->get_sector_size();
    }

    uint64_t get_size(void)  {
        return sqp_namespace->get_size();
    }

    uint64_t get_nsectors(void) {
        return sqp_namespace->get_nsectors();
    }


    int submit_read(SpdkPtr &buff, uint64_t lba, uint32_t lba_cnt,
                     spdk_nvme_cmd_cb cb_fn, void *cb_arg) {

        if (sqp_state != State::READY) { // TODO: return error here
            fprintf(stderr, "SpdkQpair: %s called on unready queue pair\n", __PRETTY_FUNCTION__);
            abort();
        }

        // TODO: check that buff has enough size
        int err = spdk_nvme_ns_cmd_read(sqp_namespace->sn_namespace, sqp_qpair,
                                        buff.ptr_m, lba, lba_cnt,
                                        cb_fn, cb_arg, 0 /* io_flags */);
        if (err) {
            fprintf(stderr, "spdk: submitting read request failed err=%d\n", err);
            return err;
        }

        sqp_npending++;
        return err;
    }

    int submit_write(SpdkPtr &buff, uint64_t lba, uint32_t lba_cnt,
                      spdk_nvme_cmd_cb cb_fn, void *cb_arg) {
        if (sqp_state != State::READY) { // TODO: return error here
            fprintf(stderr, "SpdkQpair: %s called on unready queue pair\n", __PRETTY_FUNCTION__);
            return EINVAL;
        }

        // TODO: check that buff has enough size
        int err = spdk_nvme_ns_cmd_write(sqp_namespace->sn_namespace, sqp_qpair,
                                        buff.ptr_m, lba, lba_cnt,
                                        cb_fn, cb_arg, 0 /* io_flags */);
        if (err) {
            fprintf(stderr, "spdk: submitting write request failed err=%d\n", err);
            return err;
        }

        sqp_npending++;
        return err;
    }

    bool is_done(void) { return sqp_state == State::DONE; }

    void done_(void);

    void stop(void) {
        if (sqp_state != State::READY) {
            fprintf(stderr, "%s: invalid state\n", __PRETTY_FUNCTION__);
            abort();
        }

        sqp_state = State::DRAINING;
        if (sqp_npending == 0)
            done_();
    }

    // Because we do not currently have a way to identify what are the
    // completions that we issued (e.g., via submit_read() and submit_write()),
    // we depend on the caller to decrease this count via npending_dec().
    //
    // We could have our own callbacks in submit_read(), submit_write, etc., but
    // I'm not sure how to do it without an allocation.
    void npending_dec(size_t r) {
        assert(r <= sqp_npending);
        sqp_npending -= r;
        if (sqp_npending == 0 && sqp_state == State::DRAINING)
            done_();
    }

    // nmax == 0 -> infinite
    size_t execute_completions(size_t nmax = 0) {

        size_t r = 0;
        if (sqp_npending > 0) {
            //printf("%s: POLLING pending:%lu\n",  __PRETTY_FUNCTION__, sqp_npending);
            int32_t r__ = spdk_nvme_qpair_process_completions(sqp_qpair, nmax);
            if (r__ < 0) {
                fprintf(stderr, "spdk_nvme_qpair_process_completions returned error. Aborting\n");
                abort();
            }
            assert(r__ >= 0);
            //printf("%s: GOT:%d\n",  __PRETTY_FUNCTION__, r__);
            r = static_cast<size_t>(r__);

            // The assertion below fails, so I guess the above function handles
            // additional completions to the ones we issue. see npending_dec().
            // assert(r <= sqp_npending);
            // sqp_npending  -= r;
        }

        return r;
    }

    SpdkPtr alloc_buffer(size_t nlbas, int numa_node = SOCKET_ID_ANY, char *rte_type = NULL);
    void free_buffer(SpdkPtr &&p);
};

// This is intended to be accesses by different tasks on the same thread. Hence,
// no synchronization is performed.
struct SpdkState {
    static constexpr uint32_t BPOOL_BUFF_SIZE = 8192; // buffer size for buffer pool

    enum class State {UNINITIALIZED, READY, DRAINING, DONE};
    using QpairMap = std::unordered_map<SpdkNamespace *, std::shared_ptr<SpdkQpair>>;

    State    spdk_state;
    QpairMap spdk_qpairs;
    rte_mempool *spdk_bpool;

    SpdkState() : spdk_state(State::UNINITIALIZED),
                  spdk_bpool(NULL) {} // buffer pool


    void init(SpdkGlobalState &sg);

    void operator=(SpdkState const &) = delete;
    SpdkState(SpdkState const &) = delete;
    ~SpdkState() { }

    SpdkState(SpdkGlobalState &sg) : spdk_state(State::UNINITIALIZED) {
        init(sg);
    }

    SpdkState(SpdkState &&st) {
        spdk_state  = std::move(st.spdk_state);
        spdk_qpairs = std::move(st.spdk_qpairs);
        spdk_bpool  = std::move(st.spdk_bpool);

        // Change the owner on the queues :-/
        for (auto &qp : spdk_qpairs) {
            assert(qp.second->sqp_owner == &st);
            qp.second->sqp_owner = this;
        }
    }

    void operator=(SpdkState &&st) {
        spdk_state  = std::move(st.spdk_state);
        spdk_qpairs = std::move(st.spdk_qpairs);
        spdk_bpool  = std::move(st.spdk_bpool);

        // Change the owner on the queues :-/
        for (auto &qp : spdk_qpairs) {
            assert(qp.second->sqp_owner == &st);
            qp.second->sqp_owner = this;
        }
    }

    // retrieve a queue by the namespace
    //   empty @ns -> any namespace
    std::shared_ptr<SpdkQpair> getQpair(std::string ns) {
        for (auto &x: spdk_qpairs) {
            if (ns.empty() || strcmp(x.first->sn_name, ns.c_str()) == 0) {
                return x.second;
            }
        }
        return nullptr;
    }

    std::shared_ptr<SpdkQpair> getQpair_by_prefix(std::string ns_prefix) {
        for (auto &x: spdk_qpairs) {
            std::string ns(x.first->sn_name);
            if (ns.find(ns_prefix) != std::string::npos)
                return x.second;
        }
        return nullptr;
    }

    std::vector<std::string> getNamespaceNames() {
        std::vector<std::string> ret;
        for (auto &x: spdk_qpairs) {
            ret.push_back(x.first->sn_name);
        }
        return ret;
    }

    void stop(void) {
        if (spdk_state != State::READY) {
            fprintf(stderr, "%s: invalid state\n", __PRETTY_FUNCTION__);
            abort();
        }

        spdk_state = State::DRAINING;
        if (spdk_qpairs.size() == 0 ) {
            done_();
            return;
        }

        // This is somewhat tricky because calling ->stop() on a queue might
        // end up removing it from the spdk_qpairs.
        //
        // To deal with this, we create a list of pointers for the queues, and
        // we call ->stop() on each of them later.
        std::vector<SpdkQpair *> qps;
        for (auto &t: spdk_qpairs) {
            SpdkQpair *qp = t.second.get();
            // printf("SpdkState: %p stopping queue: %p\n", this, qp);
            assert(qp->sqp_owner == this);
            assert(qp->sqp_state == SpdkQpair::State::READY);
            qps.push_back(qp);
        }

        for (auto qp: qps) {
            qp->stop();
        }

    }

    bool is_done(void) { return spdk_state == State::DONE; }

    void done_(void) {
        assert(spdk_state == State::DRAINING);
        assert(spdk_qpairs.size() == 0);
        spdk_state = State::DONE;
    }

    void queue_done(const SpdkQpair &qp) {
        assert(spdk_state == State::DRAINING);
        assert(qp.sqp_state == SpdkQpair::State::DONE);
        SpdkNamespace *ns = qp.sqp_namespace;
        spdk_qpairs.erase(ns);

        if (spdk_qpairs.size() == 0)
            done_();
    }

    // nmax == 0 -> infinite
    // not particularly fair, but for a single queue pair is not an issue
    size_t execute_completions(size_t nmax = 0) {
        size_t n = 0;
        for (auto &qp: spdk_qpairs) {
            //printf("Executing complentions on qp=%p thread=%lu\n", qp.second.get(), pthread_self());
            n += qp.second->execute_completions(nmax == 0 ? nmax : nmax - n);
            if (nmax != 0 && nmax == n) {
                break;
            }
        }

        return n;
    }
};

inline void
SpdkQpair::done_(void) {
    assert(sqp_state == State::DRAINING);
    assert(sqp_npending == 0);
    sqp_state = State::DONE;
    sqp_owner->queue_done(*this);
    // dtor will take care of the rest ...
}

// scatter-gather lists for SPDK (based on test/lib/nvme/sgl/nvme_sgl.c)
struct SpdkSgl  {
    static const size_t max_elems = 16;
    struct Sge {
        uint64_t phys_addr;
        size_t   len;
    };

    Sge      sges[max_elems];
    uint32_t sges_nr, sge_idx;

    SpdkSgl() : sges_nr(0), sge_idx(0) {}

    void add_sge(uint64_t paddr, size_t len) {
        if (sges_nr == max_elems) {
            fprintf(stderr, "insufficient size.");
            abort();
        }

        sges[sges_nr] = (Sge){ .phys_addr = paddr, .len = len };
        sges_nr++;
    }


    static void reset_sgl(void *cb_arg, uint32_t sgl_offset) {
        SpdkSgl *sgl = static_cast<SpdkSgl *>(cb_arg);
        if (sgl_offset != 0) {
            fprintf(stderr, "NYI!");
            abort();
        }
        sgl->sge_idx = 0;
    }

    static int next_sge(void *cb_arg, uint64_t *address, uint32_t *length) {
        SpdkSgl *sgl = static_cast<SpdkSgl *>(cb_arg);
        if (sgl->sge_idx >= sgl->sges_nr) {
            *length = *address = 0;
            return 0;
        }

        Sge *sge = &sgl->sges[sgl->sge_idx++];
        *address = sge->phys_addr;
        *length  = sge->len;
        return 0;
    }
};

#endif // SPDK_HH__

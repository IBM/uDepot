/*
 *  Copyright (c) 2020 International Business Machines
 *  All rights reserved.
 *
 *  SPDX-License-Identifier: BSD-3-Clause
 *
 *  Authors: Kornilios Kourtis (kou@zurich.ibm.com, kornilios@gmail.com)
 *
 */

// vim: set expandtab softtabstop=4 tabstop=4 shiftwidth=4:

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

#include "trt/uapi/trt.hh"
#include "trt_backends/trt_aio.hh"

#include <boost/intrusive/list.hpp>
namespace bi = boost::intrusive;

#define PAGE_SIZE 4096

// This is an example of building a page cache using the trt_aio facility. The
// code implements a single slot in the page cache. Multiple requests can arrive
// at the page slot while the I/O operation to fill it is in progress. We deal
// with:
//  - requests on the same page: these will defer their execution until the page
//  becomes available
//  - requests on a different page will be queued until all operations on the
//  previous page are finalized.
//
//  This  might be too complicated than it has to be, since we could do two I/O
//  requests for the same page, or reject operations on different pages, but it
//  is meant as a case-study on how to use trt_aio.
//
//
// XXX: This code does not work and is disabled since moving the aio backend to
// use LocalSingleAsyncObj. Under this (simpler) interface, you cannot have two
// waiters on the same object. So, we either need to support it or modify the
// page cache code below to issue a new IO request even for the in-progress
// page.

struct Slot {

    struct Page {

        bi::list_member_hook<> sp_lnode_;     // for allocation list
        uint64_t               sp_page_id_;   // page id
        trt::AioOp             sp_aio_;       // aio operation

        // invalid slot page
        Page() : sp_page_id_((uint64_t)-1), sp_aio_() { }

        Page(uint64_t page_id, int fd, char *buff, uint64_t offset,
             trt::AsyncObj::deallocFn dealloc_fn, trt::AsyncObj::deallocFnArg dealloc_fn_arg):
            sp_page_id_(page_id),
            sp_aio_(dealloc_fn, dealloc_fn_arg, IOCB_CMD_PREAD, fd, buff, PAGE_SIZE, offset) {}

        void submit(void) { sp_aio_.submit(); }
        trt::AsyncObj *get_ao(void) { return &sp_aio_.aio_aobj_; }
    };

    using SlotPageList = bi::list<Page, bi::member_hook<Page, bi::list_member_hook<>, &Page::sp_lnode_>>;

    std::deque<Page>  sl_pages_;
    SlotPageList      sl_free_pages_;
    Page             *sl_current_page_;
    SlotPageList      sl_pending_pages_;
    char              sl_buff_[PAGE_SIZE + 4096]; // add 4k for alignment
    spinlock_t        sl_lock_;

    Slot() : sl_current_page_(nullptr) {
        sl_pages_.resize(16);
        for (Page &sp: sl_pages_)
            sl_free_pages_.push_front(sp);
        spinlock_init(&sl_lock_);
    }

    // returns a slot page or null
    Page *alloc_page_(void) {
        if (sl_free_pages_.size() == 0)
            return nullptr;
        Page *sp = &sl_free_pages_.front();
        sl_free_pages_.pop_front();
        return sp;
    }

    // frees a slot page
    void free_page_(Page *sp) {
        sl_free_pages_.push_front(*sp);
    }

    void set_current_page_(Page *p) {
        sl_current_page_ = p;
        if (p)
            p->submit();
    }

    // Q: when is it safe to change sl_current_page? We need to ensure that all
    // readers waiting on it have read from the buffer. This whould be done when
    // all futures have been completed, so it has to be in the deallocation
    // function of the asynchronous object.
    void aio_dealloc(void) {
        spin_lock(&sl_lock_);
        assert(sl_current_page_);
        free_page_(sl_current_page_);
        Page *sp = nullptr;
        if (sl_pending_pages_.size() > 0) {
            sp = &sl_pending_pages_.front();
            sl_pending_pages_.pop_front();
        }
        set_current_page_(sp);
        spin_unlock(&sl_lock_);
    }

	static void aio_dealloc_static(trt::AsyncObj *ao, void *arg) {
		Slot *s = static_cast<Slot *>(arg);
		s->aio_dealloc();
	}

    // page_id is a unique identifier for the page. The system will used it to
    // determine if subsequent requests are for the same page or not.
    int read_page(int fd, uint64_t page_id, char *buff, off_t offset) {
        trt::Waitset ws(trt::T::self());
        trt::Future f;
        char *aligned_buff = (char *)(4096 * ((4095 + (uintptr_t)sl_buff_) / 4096));

        spin_lock(&sl_lock_);
        if (!sl_current_page_) {
            void  *p__ = alloc_page_();
            assert(p__);
            Page *p = new(p__) Page(page_id, fd, aligned_buff, offset, Slot::aio_dealloc_static, this);
            set_current_page_(p);
        }

        if (sl_current_page_->sp_page_id_ == page_id) {
            f = trt::Future(sl_current_page_->get_ao(), &ws, nullptr);
            spin_unlock(&sl_lock_);
        } else {
            void  *p__ = alloc_page_();
            if (p__) {
                Page *p = new(p__) Page(page_id, fd, aligned_buff, offset,
                                        &Slot::aio_dealloc_static, this);
                sl_pending_pages_.push_back(*p);
                f = trt::Future(p->get_ao(), &ws, nullptr);
                spin_unlock(&sl_lock_);
            } else {
                spin_unlock(&sl_lock_);
                return -ENOSPC;
            }
        }

        trt::Future *f__ __attribute__((unused)) = ws.wait_();
        assert(f__ == &f);
        int ret = f.get_val();
        assert(ret == PAGE_SIZE); // TODO: proper error handling
        memcpy(buff, aligned_buff, PAGE_SIZE);
        f.drop_ref();

        return ret;
    }
};

#define NPAGES 4
#define FILE_SIZE (NPAGES*4096) //(128*1024*1024)
#define FILE_FLAGS (O_DIRECT)
#define BUFF_SIZE (4096)


static int
make_tempfile(char *tmpname, size_t file_size, size_t buff_size) {
    int fd;
    char *buff;

    printf("Creating random file\n");
    fd = mkostemp(tmpname, FILE_FLAGS);
    if (fd < 0) {
        perror("mkostemp");
        exit(1);
    }

    int ret = ftruncate(fd, file_size);
    if (ret < 0) {
        perror("ftruncate");
        exit(1);
    }

    if (posix_memalign((void **)&buff, 4096, buff_size) != 0) {
        perror("posix_memalign");
        exit(1);
    }

    for (size_t off=0; off < file_size; off+= buff_size) {
        for (size_t i=0; i<buff_size; i++)
            buff[i] = 'a' + ((off / buff_size) % ('z' + 1));

        ret = pwrite(fd, buff, buff_size, off);
        if (ret != (int)buff_size) {
            perror("pwrite");
            exit(1);
        }
    }

    free(buff);
    return fd;
}

struct targ {
    int fd;
    uint64_t page_id;
    bool verify;
    Slot *slot;
};

void *t_getpage(void *arg__) {

    struct targ *arg = (struct targ *)arg__;
    uint64_t page_id = arg->page_id;
    char buff[PAGE_SIZE]; // does not have to be aligned, it will be copied from cache
    int ret;

    ret = arg->slot->read_page(arg->fd, page_id, buff, page_id*PAGE_SIZE);
    if (ret != (int)PAGE_SIZE) {
        fprintf(stderr, "read_page returned unexpected value");
        abort();
    }
    //trt_dmsg("PREAD returned\n");

    if (arg->verify) {
        const char c = 'a' + (page_id % ('z' + 1));
        for (size_t i=0; i<PAGE_SIZE; i++) {
            if (buff[i] != c) {
                fprintf(stderr, "Verification Error!");
                exit(1);
            }
        }
        trt_dmsg("Verified!\n");
    }

    //trt_dmsg("DONE\n");
    return nullptr;
}

void *t_main(void *arg__)
{
    trt::AIO::init();
    std::vector<targ> *args = (std::vector<targ> *)arg__;
    trt_dmsg("%s\n", __FUNCTION__);

    trt_dmsg("spawing aio_poller\n");
    trt::T::spawn(trt::AIO::poller_task, nullptr, nullptr, true, trt::TaskType::TASK);
    trt_dmsg("spawing readpage tasks\n");


    for (targ &t: *args) {
        trt_dmsg("spawning getpage task\n");
        trt::T::spawn(t_getpage, &t, nullptr);
    }

    for (unsigned i=0; i < args->size(); i++) {
        trt_dmsg("waiting for task...\n");
        trt::T::task_wait();
    }

    trt::AIO::stop(); // stop aio (this will stop the poller thread)
    trt_dmsg("%s: DONE\n", __FUNCTION__);
    return nullptr;
}

int main(int argc, char *argv[])
{
    char *fname;
    int fd;
    uint64_t file_size;
    bool verify;


    if (argc < 2) {
        char tempname[] = "/tmp/trt-cache-slot-test-XXXXXX";
        fname = tempname;
        fd = make_tempfile(fname, FILE_SIZE, BUFF_SIZE);
        file_size = FILE_SIZE;
        verify = true;
    } else {
        fname = argv[1];
        fd = open(fname, O_RDONLY | FILE_FLAGS);
        if (fd == -1) {
            perror(fname);
            exit(1);
        }

        size_t fsize = lseek64(fd, 0, SEEK_END);
        if (fsize == (size_t)-1) {
            perror("lseek64");
            exit(1);
        }

        lseek(fd, 0, SEEK_SET); // shouldn't matter really

        file_size = fsize;
        verify = false;
    }
    printf("f:%s s:%zd verify:%u\n", fname, file_size, verify);

    Slot mySlot;
    struct targ ta;

    ta.fd = fd;
    ta.verify = verify;
    ta.slot = &mySlot;

    std::vector<targ> targs;
    ta.page_id = 0; targs.push_back(ta);
    ta.page_id = 0; targs.push_back(ta);
    ta.page_id = 1; targs.push_back(ta);

    trt::Controller c;
    c.spawn_scheduler(t_main, &targs, trt::TaskType::TASK);

    return 0;
}

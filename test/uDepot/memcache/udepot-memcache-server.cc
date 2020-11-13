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
#include<unistd.h>
#include<thread>
#include<pthread.h>
#include<signal.h>
#include <atomic>
#if defined(UDEPOT_TRT_SPDK)
#include <rte_config.h>
#include <rte_lcore.h>
#endif

#include"util/debug.h"
#include"kv.hh"
#include"uDepot/kv-conf.hh"
#include"uDepot/kv-factory.hh"
#include"uDepot/backend.hh"

#include "trt/uapi/trt.hh"

static struct  {
	trt::ControllerBase   *controller;
	KV                    *kv;
	pthread_barrier_t     *barrier;
	// Not sure if the thread that called ->init() needs to call ->shutdown()
	// if not, we do not need to this.
	pthread_t             thread_that_called_init;
} Trt_g_ = {0};

// global variables (used in shutdown)

namespace udepot {
struct memcache_conf : public KV_conf {
	memcache_conf(): KV_conf(), daemon_m(false) {
		self_server_m = "*";
		thread_nr_m = 0;
		type_m = KV_conf::KV_UDEPOT_SALSA_TRT_AIO_MC;
		overprovision_m = 10; // 1%
	}
	bool daemon_m;
	virtual int parse_args(ParseArgs &args) override final;
	void validate_and_sanitize_parameters() override final;
	void print_usage(const char []) override final;
};

void memcache_conf::print_usage(const char name[])
{
	KV_conf::print_usage(name);
	printf("udepot-memcache specific parameters.\n");
	printf("-d, --daemon: spawn the server as a daemon process\n");
}

int memcache_conf::parse_args(ParseArgs &args)
{
	int rc = KV_conf::parse_args(args);
	if (rc != 0)
		return rc;

	const int argc = args.argc_;
	const char *const *const argv = args.argv_;
	for (int i = 1; i < argc; ++i) {
		std::string arg = std::string(argv[i]);
		// all arguments take at least one value, except help
		std::string val = i < argc - 1 ? std::string(argv[i + 1]) : "";
		UDEPOT_DBG("arg given %s.\n", arg.c_str());
		if ("-d" == arg || "--daemon" == arg) {
			daemon_m = true;
			args.used_arg(i);
		}
	}
	return 0;
}

void memcache_conf::validate_and_sanitize_parameters()
{
	u32 thread_max = std::thread::hardware_concurrency() * 16;
	KV_conf::validate_and_sanitize_parameters();
	// arg sanitation
	switch (type_m) {
	case KV_conf::KV_UDEPOT_SALSA_O_DIRECT_MC:
		break;
	case KV_conf::KV_UDEPOT_SALSA_TRT_AIO_MC:
		break;
	#if defined(UDEPOT_TRT_SPDK)
	case KV_conf::KV_UDEPOT_SALSA_TRT_SPDK_ARRAY_MC:
		thread_max = 8;
		break;
	#endif
	default:
		type_m = KV_conf::KV_UDEPOT_SALSA_O_DIRECT_MC;
	}
	if (0 == thread_nr_m)
		thread_nr_m = thread_max;
}

};				// namespace udepot

static udepot::memcache_conf conf_g;

static std::atomic<bool> exit_g;

// This task will trigger shutdown
static void *task_exit(void *arg)
{
	assert(Trt_g_.kv);
	if (pthread_self() != Trt_g_.thread_that_called_init) {
		UDEPOT_DBG("Thread local exit");
		Trt_g_.kv->thread_local_exit();
		pthread_barrier_wait(Trt_g_.barrier);
	} else {
		pthread_barrier_wait(Trt_g_.barrier);
		UDEPOT_MSG("Global exit");
		Trt_g_.kv->shutdown();
	}
	return nullptr;
}

__attribute__((unused))
static void
spawn_exit_task(trt::Scheduler &s) {
	trt::Task *t = trt::T::alloc_task(task_exit, nullptr /* arg */);
	if (!t) {
		UDEPOT_ERR("Unable to allocate task. Cannot exit");
		return;
	}

	bool ok = s.remote_push_task_front(*t);
	if (!ok) {
		UDEPOT_ERR("Unable to push task. Cannot exit");
		trt::T::free_task(t);
	}
}

static void signal_handler(int signum)
{
	UDEPOT_MSG("received signal %s %d", strsignal(signum), signum);
	if (exit_g) {
		// We might want to remove this eventually ...
		UDEPOT_MSG("Exit already triggered once. Bailing out after 1s\n");
		usleep(1000000);
		exit(1);
	}
	exit_g = true;
	// spawn an exit task on each scheduler
	if (Trt_g_.kv) {
		Trt_g_.controller->on_each_scheduler(spawn_exit_task);
	}
}

static int install_signal_handler()
{
	int err = 0;
	struct sigaction saction;
	saction.sa_handler = signal_handler;
	sigemptyset (&saction.sa_mask);
	saction.sa_flags = 0;
	err |= sigaction(SIGINT, &saction, NULL);
	err |= sigaction(SIGQUIT, &saction, NULL);
	err |= sigaction(SIGUSR1, &saction, NULL);
	err |= sigaction(SIGUSR2, &saction, NULL);
	err |= sigaction(SIGABRT, &saction, NULL);
	err |= sigaction(SIGTSTP, &saction, NULL);
	err |= sigaction(SIGSEGV, &saction, NULL);
	// ignore SIGCHLD
	saction.sa_handler = SIG_IGN;
	saction.sa_flags = SA_NOCLDSTOP;
	err |= sigaction(SIGCHLD, &saction, NULL);
	return err;
}

sigset_t sigs_to_ignore_g;
static int disable_signals()
{
	int rc = EFAULT;
        int err = sigfillset(&sigs_to_ignore_g);
	if (0 != err)
		goto fail0;
	/**
         * Disable signals temporarily in the main thread
         * so that  all the created threads have these signals
         * blocked
         */
	err = pthread_sigmask(SIG_BLOCK, &sigs_to_ignore_g, NULL);
	if (0 != err)
		goto fail1;
	rc = 0;
fail1:
fail0:
	return rc;
}

static int enable_signals()
{
	return pthread_sigmask(SIG_UNBLOCK, &sigs_to_ignore_g, NULL);
}

using namespace udepot;

template<typename RT>
static int memcache_server_trt();

int main(const int argc, char **argv)
{
	int rc = 0;
	// parse args
	udepot::ParseArgs args(argc, argv);
	rc = conf_g.parse_args(args);
	if (0 != rc || conf_g.help_m) {
		conf_g.print_usage(argv[0]);
		if (!conf_g.help_m)
			UDEPOT_ERR("daemon failed with %s (%d).", strerror(rc), rc);
		return -rc;
	} else if (args.has_unused_args()) {
		args.print_unused_args(std::cerr);
		conf_g.print_usage(argv[0]);
		return EINVAL;
	}
	conf_g.validate_and_sanitize_parameters();

	// deamonize if requested
	if (conf_g.daemon_m) {
		rc = daemon(0, 0);
		if (0 != rc) {
			UDEPOT_ERR("daemon failed with %s %d.", strerror(errno), errno);
			return -1;
		}
	}

	// set signal handler
	rc = install_signal_handler();
	if (0 != rc) {
		UDEPOT_ERR("installing sig handler failed with %s %d.", strerror(errno), errno);
		return rc;
	}

	switch (conf_g.type_m) {
#if defined(UDEPOT_TRT_SPDK)
	case KV_conf::KV_UDEPOT_SALSA_TRT_SPDK_ARRAY_MC:
		return memcache_server_trt<RuntimeTrtSpdkArrayMC>();
#endif
	case KV_conf::KV_UDEPOT_SALSA_TRT_AIO_MC:
		return memcache_server_trt<RuntimeTrtMC>();
	case KV_conf::KV_UDEPOT_SALSA_O_DIRECT_MC:
		break;
	default:
		UDEPOT_ERR("Unsupported backend: %u\n", static_cast<u32>(conf_g.type_m));
		exit(1);
	}
	// initialize KV store and its (Memcache) server
	KV *const KV = KV_factory::KV_new(conf_g);
	if (nullptr == KV) {
		UDEPOT_ERR(" failed to allocate kv.");
		rc = -ENOMEM;
		goto fail0;
	}

	// disable signal before initializing KV
	rc = disable_signals();
	if (0 != rc) {
		goto fail1;
	}

	rc = KV->init();
	if (0 != rc) {
		UDEPOT_ERR("udepot init failed w/%s %d.", strerror(rc), rc);
		goto fail2;
	}

	rc = enable_signals();
	if (0 != rc) {
		goto fail3;
	}

	sigset_t sigset;
        rc = sigfillset(&sigset);
	if (0 != rc)
		goto fail3;
	do {
		// rc = sigsuspend(&sigset);
		rc = pause();
		UDEPOT_ERR("pause ret %s %d rc=%d.", strerror(errno), errno, rc);
	} while (!exit_g);

fail3:
	UDEPOT_MSG("shutting down KV store.");
	rc = KV->shutdown();
	if (0 != rc) {
		UDEPOT_ERR("shutdown ret %s %d.", strerror(rc), rc);
	}
fail2:
fail1:
	delete KV;
fail0:
	return rc;
}

struct t_main_arg {
	std::shared_ptr<KV> kv;
	unsigned            thread_id;
	unsigned            nthreads;
	pthread_barrier_t  *barrier;
};

static std::atomic<int> exit_trt_g;

static  __attribute__((unused))
void *t_main(void *arg_in)
{
	t_main_arg *const arg = static_cast<t_main_arg *>(arg_in);
	if (0 == arg->thread_id) {
		// initialize KV
		arg->kv = std::shared_ptr<KV>(KV_factory::KV_new(conf_g));
		if (!arg->kv || arg->kv->init() != 0) {
			UDEPOT_ERR("KV init failed\n");
			exit_trt_g = EINVAL;
		}
		Trt_g_.kv = arg->kv.get();
		Trt_g_.thread_that_called_init = pthread_self();
		trt_dmsg("KV store ready\n");

		// copy KV to other tasks
		for (unsigned i=1; i<arg->nthreads; i++) {
			assert(arg[i].kv == nullptr);
			arg[i].kv = arg->kv;
		}
	}
	// initialize other threads's IO
	pthread_barrier_wait(arg->barrier);
	if (0 == exit_trt_g) {
		assert(arg->kv != nullptr);
		if (0 != arg->thread_id)
			arg->kv->thread_local_entry();
	}
	pthread_barrier_wait(arg->barrier);

	return 0;
}

template<typename RT>
static int memcache_server_trt()
{
	pthread_barrier_t barrier;
	t_main_arg args[conf_g.thread_nr_m];
	pthread_barrier_init(&barrier, NULL, conf_g.thread_nr_m);

	// disable signal before initializing KV
	int rc = disable_signals();
	if (0 != rc)
		return rc;

	for (u32 i = 0; i < conf_g.thread_nr_m; i++) {
		auto a = &args[i];
		a->kv = nullptr;
		a->thread_id = i;
		a->nthreads  = conf_g.thread_nr_m;
		a->barrier   = &barrier;
	}

	switch(conf_g.type_m) {
	case KV_conf::KV_UDEPOT_SALSA_TRT_AIO_MC:
	{
		RT::IO::global_init();
		trt::Controller c;
		Trt_g_.controller = &c;
		Trt_g_.barrier = &barrier;
		for (u32 i = 0; i < conf_g.thread_nr_m; i++) {
			c.spawn_scheduler(t_main, &args[i], trt::TaskType::TASK);
		}
		rc = enable_signals();
		if (0 != rc) {
			goto fail0;
		}
		c.set_exit_all();
		c.wait_for_all();
		break;
	}
#if defined(UDEPOT_TRT_SPDK)
	case KV_conf::KV_UDEPOT_SALSA_TRT_SPDK_ARRAY_MC:
	{
		RT::IO::global_init();
		trt::RteController c;
		Trt_g_.controller = &c;
		Trt_g_.barrier = &barrier;
		const u32 lcores_nr = rte_lcore_count();
		u32 lcore;
		int i = -1;

		if (lcores_nr == 0) {
			UDEPOT_ERR("Did you initialize RTE?\n");
			rc = EPERM;
			goto fail0;
		}

		conf_g.thread_nr_m = std::min(conf_g.thread_nr_m, lcores_nr);
		RTE_LCORE_FOREACH_SLAVE(lcore) {
			if (conf_g.thread_nr_m <= (u32) ++i)
				break;
			printf("spawning scheduler on lcore=%u i=%d\n", lcore, i);
			c.spawn_scheduler(t_main, &args[i], trt::TaskType::TASK, lcore);
		}
		rc = enable_signals();
		if (0 != rc) {
			goto fail1;
		}

		c.set_exit_all();
		c.wait_for_all();
		break;
	}
#endif
	default:
		UDEPOT_ERR("invalid udepot memcache type given, aborting.");
		rc = EINVAL;
		goto fail0;
	}

	return 0;

#if defined(UDEPOT_TRT_SPDK)
fail1:
	exit_g = true;
#endif
fail0:
	enable_signals();

	return rc;
}

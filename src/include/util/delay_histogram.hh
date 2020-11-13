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

#ifndef DELAY_HISTOGRAM_HH_
#define DELAY_HISTOGRAM_HH_

#include <vector>
#include <vector>
#include <cassert>
#include <memory>

#include "util/ticks.hh"

class delay_histogram {

public:
	delay_histogram(uint64_t sample_period, std::vector<uint64_t> const& buckets);

	class ctx;
	ctx start();
	void stop(ctx);

	class ctx {
		friend delay_histogram;
		uint64_t start;
		ctx() : start(ticks::get_ticks()) {}
	};

	uint64_t ticks_total();
	uint64_t count();
	std::vector<std::tuple<uint64_t, uint64_t>> histogram();

private:
	struct dh_item {
		const uint64_t val;
		uint64_t cnt;

		dh_item(uint64_t v) : val(v), cnt(0) {}
	};


	uint64_t ticks_total_, cnt_total_;
	uint64_t period_;
	std::vector<dh_item> buckets_;

	void add_to_histogram(uint64_t t);
};


inline
delay_histogram::delay_histogram(uint64_t period, std::vector<uint64_t> const& buckets)
	: ticks_total_(0)
	, cnt_total_(0)
	, period_(period)
{
	for (auto &b: buckets) {
		buckets_.push_back(dh_item(b));
	}
}

inline delay_histogram::ctx
delay_histogram::start(void)
{
	return ctx();
}

inline void
delay_histogram::stop(delay_histogram::ctx x)
{
	uint64_t end = ticks::get_ticks();
	assert(end >= x.start);
	uint64_t t = end - x.start;
	if (t % period_ == 0)
		add_to_histogram(t);
	ticks_total_ += t;
	cnt_total_++;
}

inline void
delay_histogram::add_to_histogram(uint64_t v)
{
	size_t i;
	for (i=0; i<buckets_.size() - 1; i++) {
		if (v <= buckets_[i].val)
			break;
	}
	buckets_[i].cnt++;
}

inline uint64_t delay_histogram::ticks_total() { return ticks_total_; } inline uint64_t delay_histogram::count() { return cnt_total_; }

inline std::vector<std::tuple<uint64_t, uint64_t>>
delay_histogram::histogram()
{
	std::vector<std::tuple<uint64_t, uint64_t>> ret;
	for (auto b: buckets_) {
		ret.emplace_back(b.val, b.cnt);
	}
	return ret;
}

#endif // DELAY_HISTOGRAM_HH_

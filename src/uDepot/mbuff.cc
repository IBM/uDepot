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

#include <cassert>
#include <utility>
#include <typeinfo>
#include <typeindex>
#include <sys/uio.h>

#include "util/debug.h"
#include "uDepot/mbuff.hh"

namespace udepot {

Mbuff::Node::Node(unsigned char *buff, size_t buff_len, Mbuff::Node *next)
    : mb_buff_(buff)
    , mb_buff_len_(buff_len)
    , mb_off_(0)
    , mb_len_(0)
    , mb_next_(next)
    #if !defined(NDEBUG)
    , mb_buff_orig_(buff)
    #endif
{
	// if buff is null, its length should be zero
	assert(buff != nullptr || buff_len == 0);
}

Mbuff::Node::~Node() {
	assert(mb_buff_ == nullptr);
	assert(mb_buff_orig_ == nullptr);
}

Mbuff::Mbuff(std::type_index ty_index)
	: mb_valid_size_(0)
	, mb_head_(nullptr)
	, mb_tail_(nullptr)
	, mb_free_size_(0)
	, mb_free_nodes_(0)
	, mb_free_(nullptr)
	, mb_node_cache_()
	, mb_buff_type_index(ty_index)
	{}

Mbuff::~Mbuff() {
	if (mb_head_ != nullptr || mb_tail_ != nullptr || mb_free_ != nullptr)
		UDEPOT_ERR("destructor called without releasing all buffers. Please call reset()");
}

void Mbuff::assert_valid_head_tail(void)
{
	#if !defined(NDEBUG)
	if (mb_head_ == nullptr) {
		assert(mb_tail_ == nullptr);
		return;
	}

	Node *n = mb_head_;
	for (;;) {
		if (n->mb_next_ == nullptr) {
			assert(mb_tail_ == n);
			break;
		} else n = n->mb_next_;
	}
	#endif
}

size_t Mbuff::nodes_nr(void)
{
	size_t ret = 0;
	Node *n = mb_head_;
	while (n) {
		ret++;
		n = n->mb_next_;
	}
	return ret;
}

size_t Mbuff::compute_valid_size(void)
{
	size_t ret = 0;
	Node *n = mb_head_;
	while (n) {
		ret += n->mb_len_;
		n = n->mb_next_;
	}
	return ret;
}

Mbuff::Chunk Mbuff::append_prepare_inline() {
	if (mb_tail_ != nullptr && mb_tail_->append_space() > 0)
		return mb_tail_->get_append_chunk();
	else
		return Chunk();
}

Mbuff::Chunk Mbuff::append_prepare() {
	// can we use the existing tail?
	if (mb_tail_ != nullptr && mb_tail_->append_space() > 0)
		return mb_tail_->get_append_chunk();

	// Try to allocate a new node
	Node *new_node = get_free_node();
	if (new_node == nullptr)
		return Chunk(); // no free nodes, return invalid chunk

	new_node->mb_next_ = nullptr;
	if (mb_tail_)
		mb_tail_->mb_next_ = new_node;
	if (!mb_head_)
		mb_head_ = new_node;
	mb_tail_ = new_node;
	assert(mb_tail_->mb_off_ == 0 && mb_tail_->mb_len_ == 0);
	return mb_tail_->get_append_chunk();
}

void
Mbuff::append_finalize(Mbuff::Chunk &&c, size_t append_len)
{
	assert(c.is_valid());
	mb_tail_->put_append_chunk(std::move(c), append_len);
	mb_valid_size_ += append_len;
}

Mbuff::Chunk Mbuff::prepend_prepare_inline() {
	if (mb_head_ != nullptr && mb_head_->prepend_space() > 0)
		return mb_head_->get_prepend_chunk();
	else
		return Chunk();
}

Mbuff::Chunk Mbuff::prepend_prepare() {
	// can we use the existing head node?
	if (mb_head_ != nullptr && mb_head_->prepend_space() > 0)
		return mb_head_->get_prepend_chunk();

	// Try to allocate a new node
	Node *new_node = get_free_node();
	if (new_node == nullptr)
		return Chunk(); // no free nodes, return invalid chunk

	new_node->mb_next_ = mb_head_;
	if (!mb_tail_)
		mb_tail_ = new_node;
	mb_head_ = new_node;
	assert(mb_head_->mb_off_ == 0 && mb_head_->mb_len_ == 0);
	return mb_head_->get_prepend_chunk();
}

void
Mbuff::prepend_finalize(Mbuff::Chunk &&c, size_t prepend_len)
{
	assert(c.is_valid());
	mb_head_->put_prepend_chunk(std::move(c), prepend_len);
	mb_valid_size_ += prepend_len;
}

void
Mbuff::add_buff(unsigned char *buf, size_t size) const {
	Node *node = mb_node_cache_.get();
	new (node) Node(buf, size);
	put_free_node(node);
}

Mbuff::Node *Mbuff::get_free_node() const {
	Node *ret = mb_free_;
	if (ret != nullptr) {
		mb_free_ = ret->mb_next_;
		assert(mb_free_size_ >= ret->mb_buff_len_);
		mb_free_size_ -= ret->mb_buff_len_;
		assert(mb_free_nodes_ > 0);
		mb_free_nodes_--;
	}
	return ret;
}


// caller is responsible for having the passing a node in the expected state
// (e.g., by calling ->reset())
void Mbuff::put_free_node(Node *n) const {
	assert(n->mb_off_ == 0 && n->mb_len_ == 0 && "Call ->reset()  before put_free_node()");
	n->mb_next_ = mb_free_;
	mb_free_ = n;
	mb_free_size_ += n->mb_buff_len_;
	mb_free_nodes_++;
}

std::tuple<unsigned char *, size_t> Mbuff::get_free_buff() const {
	unsigned char *b = nullptr;
	size_t l = 0;
	Node *n = get_free_node();
	if (n) {
		std::tie(b, l) = n->clear();
		mb_node_cache_.put(n);
	}

	return std::make_tuple(b, l);
}

size_t Mbuff::move_free_buffs_to(Mbuff const& dst, size_t max_size) const {
	// We cannot just move the list from one Mbuff to the other because each
	// uses its own Node cache.
	assert(mb_buff_type_index == dst.mb_buff_type_index);
	unsigned char *buff;
	size_t moved = 0, len;
	do {
		std::tie(buff, len) = get_free_buff();
		if (buff == nullptr)
			break;
		moved += len;
		dst.add_buff(buff, len);
	} while (moved < max_size);

	return moved;
}

int
Mbuff::reslice(size_t len, size_t off)
{
	if (mb_valid_size_ < off + len)
		return -1;

	const size_t slice_len = len;

	if (len == 0) {
		#if !defined(NDEBUG)
		if (off != 0)
			UDEPOT_ERR("WARN: %s(): offset set to zero because from (%zd) len=0\n", __FUNCTION__, off);
		#endif
		Node *nodes = mb_head_;
		while (nodes) {
			Node *n = nodes;
			nodes = n->mb_next_;
			n->reset();
			put_free_node(n);
		}
		mb_head_ = mb_tail_ = nullptr;
		mb_valid_size_ = 0;
		return 0;
	}

	if (!mb_head_) {
		assert(len == 0);
		return 0;
	}

	while (off > 0) {
		if (mb_head_->mb_len_ > off) {
			mb_head_->mb_off_ += off;
			mb_head_->mb_len_ -= off;
			off = 0;
		} else {
			Node *old_head = mb_head_;
			mb_head_ = old_head->mb_next_;
			off -= old_head->mb_len_;
			old_head->reset();
			put_free_node(old_head);
		}
	}

	Node *node = mb_head_;
	while (len > 0) {
		if (node->mb_len_ >= len)
			break;
		len -= node->mb_len_;
		node = node->mb_next_;
	}

	// truncate node: truncate ->mb_len_ and put subsequent nodes in the free
	// list
	node->mb_len_ = len;
	mb_tail_ = node;
	Node *to_free_list = node->mb_next_;
	node->mb_next_ = nullptr;
	while (to_free_list) {
		Node *f = to_free_list;
		to_free_list = to_free_list->mb_next_;
		f->reset();
		put_free_node(f);
	}

	mb_valid_size_ = slice_len;
	assert(compute_valid_size() == mb_valid_size_);
	assert_valid_head_tail();
	return 0;
}

void Mbuff::reset_list(Node *head, FreeBuffFn free_buffer)
{
	Node *next = head;
	while (next) {
		Node *n = next;
		next = n->mb_next_;
		unsigned char *buff = n->mb_buff_.release();
		assert(buff == n->mb_buff_orig_);
		free_buffer(buff);
		#if !defined(NDEBUG)
		n->mb_buff_orig_ = nullptr;
		#endif
		mb_node_cache_.put(n);
	}
}

void Mbuff::reset(FreeBuffFn free_buffer)
{
	reset_list(mb_head_, free_buffer);
	mb_tail_ = mb_head_ = nullptr;
	reset_list(mb_free_, free_buffer);
	mb_free_ = nullptr;
	mb_free_size_ = 0;
	mb_free_nodes_ = 0;
	mb_node_cache_.reset();
}

std::tuple<Mbuff::Node *, size_t> Mbuff::find_node(size_t off) const {
	Node *node = mb_head_;
	for (;;) {
		if (node == nullptr) {
			return std::make_tuple(nullptr, 0);
		} if (node->mb_len_ > off) {
			return std::make_tuple(node, off);
		} else {
			off -= node->mb_len_;
			node = node->mb_next_;
		}
	}
}

int Mbuff::iterate_valid_init(Mbuff::Iterator &iter, size_t off, size_t len) const {
	// iterator must be invalid
	assert(iter.is_invalid());

	// if length is zero, the iterator remains invalid.
	if (len == 0)
		return 0;

	// fail eagerly
	if (get_valid_size() < off + len)
		return EINVAL;

	do_initialize_iterator(iter, off, len);
	return 0;
}

int Mbuff::iterate_valid_partial_init(Mbuff::Iterator &iter, size_t off, size_t len) const {
	// iterator must be invalid
	assert(iter.is_invalid());

	// invalid offset: beyond valid size
	if (off > get_valid_size())
		return EINVAL;

	// update len
	len = std::min(len, get_valid_size() - off);

	// if length is zero, the iterator remains invalid.
	if (len == 0)
		return 0;

	do_initialize_iterator(iter, off, len);
	return 0;
}

void
Mbuff::do_initialize_iterator(Mbuff::Iterator &iter, size_t off, size_t len) const {
	assert(iter.is_invalid());
	assert(iter.i_chunk_.is_invalid());
	assert(iter.i_node_ == nullptr);

	Node *node;
	size_t node_off;

	// find proper node in the Mbuff list
	std::tie(node, node_off) = find_node(off);
	assert(node != nullptr);
	assert(node->mb_len_ > node_off);

	iter.i_len_ = len;
	iter.i_node_ = node;
	size_t chunk_len = std::min(iter.i_len_, node->mb_len_ - node_off);
	iter.i_chunk_ = std::move(node->get_valid_chunk(node_off, chunk_len));
	assert(iter.i_chunk_.is_valid());
}

void Mbuff::print_data(FILE *f, size_t off, size_t len) const {
	Iterator iter;
	int err;

	fprintf(f, "%s: Printing off=%zd len=%zd\n", __PRETTY_FUNCTION__, off, len);
	err = iterate_valid_init(iter, off, len);
	if (err) {
		fprintf(f, "%s: invalid iterator\n", __PRETTY_FUNCTION__);
		return;
	}

	size_t idx = 0;
	while (true) {
		Chunk const& c = iter.get_chunk();
		if (c.is_invalid())
			break;
		const unsigned char *buff = c.getConstPtr();
		for (size_t i=0; i<c.len; i++) {
			fprintf(f, "idx=%zd b=0x%x\n", idx++, buff[i]);
		}
		iter.next();
	}
}

bool Mbuff::Iterator::is_valid() { return i_node_ != nullptr; }

bool Mbuff::Iterator::is_next_valid() {
	assert(is_valid());
	assert(i_len_ >= i_chunk_.len);
	return i_len_ > i_chunk_.len;
}

void Mbuff::Iterator::next() {
	size_t chunk_len = i_chunk_.len;
	i_node_->put_valid_chunk(std::move(i_chunk_));
	assert(i_chunk_.is_invalid());
	assert(i_len_ >= chunk_len);

	i_len_ -= chunk_len;
	if (i_len_ == 0) {
		i_node_ = nullptr;
		return;
	}

	i_node_ = i_node_->mb_next_;
	// len > 0, but no more nodes on MBuff. This shouldn't happen since we test
	// it in construction time. Print an error message, and invalidate the
	// iterator.
	if (i_node_ == nullptr) {
		UDEPOT_ERR("iterator len > 0 but no more nodes. Invalidating iterator");
		i_node_ = nullptr;
		return;
	}

	size_t new_chunk_len = std::min(i_len_, i_node_->mb_len_);
	i_chunk_ = std::move(i_node_->get_valid_chunk(0, new_chunk_len));
	assert(i_chunk_.is_valid());
}

void Mbuff::Iterator::stop() {
	if (i_node_) {
		assert(i_chunk_.is_valid());
		i_node_->put_valid_chunk(std::move(i_chunk_));
		i_node_ = nullptr;
		i_len_= 0;
	}
	assert(i_chunk_.is_invalid());
}

Mbuff::Chunk      & Mbuff::Iterator::get_chunk() { return i_chunk_; }
Mbuff::Chunk const& Mbuff::Iterator::get_chunk() const { return i_chunk_; }

Mbuff::Iterator::~Iterator() {
	if (is_valid())
		stop();
}

} // end namespace udepot

#if defined(MBUFF_TESTS)

#if defined(NDEBUG)
#warning "Tests will not work properly with NDEBUG defined"
#endif

#include <unistd.h>
#include <fcntl.h>

static int
setnonblocking(int fd) {
	int flags;
    if (-1 == (flags = fcntl(fd, F_GETFL, 0)))
        flags = 0;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}


static inline size_t
iovec_len(const struct iovec *iov, unsigned iovcnt)
{
    size_t ret = 0;
    for (unsigned i=0; i < iovcnt; i++) {
        ret += iov[i].iov_len;
    }
    return ret;
}


// Builds an Mbuff of @buffs_nr buffers, each of @buff_size
static udepot::Mbuff
create_mbuff(size_t buff_size, size_t buffs_nr)
{
	udepot::Mbuff mbuff(std::type_index(typeid(unsigned char *)));
	for (size_t i=0; i < buffs_nr; i++) {
		unsigned char *b = static_cast<unsigned char *>(std::malloc(buff_size));
		if (!b) {
			perror("malloc");
			abort();
		}
		mbuff.add_buff(b, buff_size);
	}
	assert(mbuff.get_free_size() == buff_size*buffs_nr);
	return mbuff;
}

// Builds an Mbuff of @buffs_nr buffers, each of @buff_size
// Builds a (normal) buffer for @xbuff_size
//
// initializes value of the normal buffer, appends it to the mbuff, copies it to
// a differnt (normal) buffer, and checks that the two (normal) buffers have the
// same contents.
static void
test_append_from_and_copy_to_buffer(size_t buff_size, size_t buffs_nr, size_t xbuff_size)
{
	printf("%s: buff_size=%zd buffs_nr=%zd xbuff_size=%zd\n", __FUNCTION__, buff_size, buffs_nr, xbuff_size);

	udepot::Mbuff mbuff = create_mbuff(buff_size, buffs_nr);

	unsigned char *xbuff = static_cast<unsigned char *>(std::malloc(xbuff_size));
	if (!xbuff) {
		perror("malloc");
		abort();
	}
	for (size_t i=0; i<xbuff_size; i++) {
		xbuff[i] = static_cast<unsigned char>(i);
	}

	size_t append_ret = mbuff.append_from_buffer(xbuff, xbuff_size);
	assert(append_ret == xbuff_size);
	assert(mbuff.append_avail_size() == buff_size*buffs_nr - xbuff_size);

	unsigned char *xbuff2 = static_cast<unsigned char *>(std::malloc(xbuff_size));
	if (!xbuff2) {
		perror("malloc");
		abort();
	}

	size_t copied = mbuff.copy_to_buffer(0, xbuff2, xbuff_size);
	if (copied != xbuff_size) {
		UDEPOT_ERR("error: copied (=%zd) =/= xbuff_size (=%zd)\n", copied, xbuff_size);
		abort();
	}

	if (std::memcmp(xbuff, xbuff2, xbuff_size) != 0) {
		UDEPOT_ERR("error: xbuff and xbuff2 contents differ\n");
		abort();
	}

	mbuff.reset(std::free);
	assert(mbuff.get_free_size() == 0);
	std::free(xbuff);
	std::free(xbuff2);
}

static void
test_reslice(size_t buff_size, size_t buffs_nr, size_t mbuff_size,
             size_t reslice_size, size_t reslice_off)
{
	printf("%s: buff_size=%zd buffs_nr=%zd mbuff_size=%zd reslice_size=%zd reslice_off=%zd\n", __FUNCTION__, buff_size, buffs_nr, mbuff_size, reslice_size, reslice_off);
	udepot::Mbuff mbuff = create_mbuff(buff_size, buffs_nr);
	unsigned char count = 0;
	mbuff.append(
		[&count]
		(unsigned char *buff, size_t len) -> size_t {
			for (size_t i=0; i<len; i++) {
				buff[i] = count++;
			}
			return len;
		}
	);

	int err = mbuff.reslice(reslice_size, reslice_off);
	assert(!err);
	assert(mbuff.get_valid_size() == reslice_size);

	udepot::Mbuff::Iterator iter;
	err = mbuff.iterate_valid_init(iter);
	if (err) {
		UDEPOT_ERR("iterate_valid_init() failed.\n");
		abort();
	}

	// verify data
	count = static_cast<unsigned char>(reslice_off);
	size_t checked = 0;
	size_t chunk_idx = 0;
	while (true) {
		chunk_idx = 0;
		udepot::Mbuff::Chunk const& c = iter.get_chunk();
		if (c.is_invalid())
			break;
		const unsigned char *ptr = c.getConstPtr();
		for (size_t i=0; i < c.len; i++) {
			if (ptr[i] != count++) {
				UDEPOT_ERR("err chunk_idx=%zd i=%zd expected:%d got:%d\n", chunk_idx, i, ptr[i], count);
				abort();
			}
		}
		checked += c.len;
		iter.next();
		chunk_idx++;
	}
	if (checked != reslice_size) {
		UDEPOT_ERR("err checked=%zd =/= reslice_size=%zd\n", checked, reslice_size);
		abort();
	}
	mbuff.reset(std::free);
}

// Allocates an Mbuff with buffs_nr x buff_size
// Allocates and initializes a buffer with (buffs_nr-1) x buff_size
// Operations:
//  Appends cut_point % buff_size of nothing (seek?, skip?)
//  Appends buff[cut_point:]
//  reslices to [cut_point:]
//  Prepends buff[:cut_point]
// Copies to a new buffer and compares with the original
static void
test_prepend(size_t buff_size, size_t buffs_nr, size_t cut_point)
{
	int err;

	printf("%s: buff_size=%zd buffs_nr=%zd cut_point=%zd\n", __FUNCTION__, buff_size, buffs_nr, cut_point);
	assert(cut_point <= buff_size*buffs_nr);

	udepot::Mbuff mbuff = create_mbuff(buff_size, buffs_nr);
	size_t cut_buff_offset = cut_point % buff_size;

	// initialize buffer
	const size_t xbuff_size = buff_size*buffs_nr;
	unsigned char *xbuff = static_cast<unsigned char *>(std::malloc(xbuff_size));
	if (!xbuff) {
		perror("malloc");
		abort();
	}
	for (size_t i=0; i<xbuff_size; i++) {
		xbuff[i] = static_cast<unsigned char>(i);
	}

	// append nothing (seek? skip?)
	unsigned char count = 0;
	mbuff.append(
		[&count, buff_size, cut_buff_offset]
		(unsigned char *buff, size_t len) -> size_t {
			assert(len == buff_size);
			return cut_buff_offset;
		}
	);
	assert(mbuff.get_valid_size() == cut_buff_offset);

	// append
	size_t append_ret = mbuff.append_from_buffer(xbuff + cut_point, xbuff_size - cut_point);
	assert(append_ret == xbuff_size - cut_point);

	// reslice
	assert(mbuff.get_valid_size() == cut_buff_offset + xbuff_size - cut_point);
	err = mbuff.reslice(xbuff_size - cut_point, cut_buff_offset);
	assert(!err);

	// prepend
	size_t prepend_ret = mbuff.prepend_from_buffer(xbuff, cut_point);
	assert(prepend_ret == cut_point);
	assert(mbuff.get_valid_size() == xbuff_size);

	// verify
	unsigned char *xbuff2 = static_cast<unsigned char *>(std::malloc(xbuff_size));
	if (!xbuff2) {
		perror("malloc");
		abort();
	}

	size_t copied = mbuff.copy_to_buffer(0, xbuff2, xbuff_size);
	if (copied != xbuff_size) {
		UDEPOT_ERR("error: copied (=%zd) =/= xbuff_size (=%zd)\n", copied, xbuff_size);
		abort();
	}

	if (std::memcmp(xbuff, xbuff2, xbuff_size) != 0) {
		UDEPOT_ERR("error: xbuff and xbuff2 contents differ\n");
		abort();
	}

	std::free(xbuff2);
	std::free(xbuff);
	mbuff.reset(std::free);
}

static void
test_memcompare(size_t buff_size, size_t buffs_nr, size_t off1, size_t off2, size_t len)
{
	printf("%s: buff_size=%zd buffs_nr=%zd off1=%zd off2=%zd len=%zd\n", __FUNCTION__, buff_size, buffs_nr, off1, off2, len);
	size_t idx;
	udepot::Mbuff mb1 = create_mbuff(buff_size, buffs_nr);
	udepot::Mbuff mb2 = create_mbuff(buff_size, buffs_nr);

	idx = 0;
	mb1.append(
		[&idx, &off1] (unsigned char *buff, size_t len) -> size_t {
			for (size_t i=0; i<len; i++)
				buff[i] = static_cast<unsigned char>(idx++ - off1);
			return len;
		}
	);
	assert(idx == buff_size*buffs_nr);

	idx = 0;
	mb2.append(
		[&idx, &off2] (unsigned char *buff, size_t len) -> size_t {
			for (size_t i=0; i<len; i++)
				buff[i] = static_cast<unsigned char>(idx++ - off2);
			return len;
		}
	);
	assert(idx == buff_size*buffs_nr);

	int ret = udepot::Mbuff::mem_compare(mb1, off1, mb2, off2, len);
	assert(ret == 0);

	mb1.reset(std::free);
	mb2.reset(std::free);
}

static void
test_fill_iovec_valid(size_t buff_size, size_t buffs_nr, size_t mbuff_off, size_t len, size_t iov_len)
{
	printf("%s: buff_size=%zd buffs_nr=%zd off=%zd len=%zd iov_len=%zd\n", __FUNCTION__, buff_size, buffs_nr, mbuff_off, len, iov_len);
	udepot::Mbuff mb(std::type_index(typeid(unsigned char *)));
	struct iovec iov_mbuff[iov_len];
	unsigned char *buffs[buffs_nr];
	int ret_cnt;
	size_t ret_len;

	// create buffers for Mbuff
	for (size_t i=0; i < buffs_nr; i++) {
		unsigned char *b = static_cast<unsigned char *>(std::malloc(buff_size));
		if (!b) {
			perror("malloc");
			abort();
		}
		mb.add_buff(b, buff_size);
	}
	assert(mb.get_free_size() == buff_size*buffs_nr);
	// validate all of mbuff
	size_t b_idx = 0;
	mb.append(
		[&buffs, &b_idx, buff_size]
		(unsigned char *buff, size_t b_len) -> size_t {
			assert(b_len == buff_size);
			buffs[b_idx++] = buff;
			return b_len;
		}
	);
	assert(b_idx == buffs_nr);
	assert(mb.get_valid_size() == buff_size*buffs_nr);

	std::tie(ret_cnt, ret_len) = mb.fill_iovec_valid(mbuff_off, len, iov_mbuff, iov_len);
	if (mbuff_off + len > buff_size*buffs_nr) {
		assert(ret_cnt < 0);
		mb.reset(std::free);
		return;
	}

	assert(static_cast<size_t>(ret_cnt) <= iov_len);
	assert(ret_cnt >= 0);
	size_t first_buffer_idx = mbuff_off / buff_size;
	size_t first_buffer_off = mbuff_off % buff_size;
	size_t last_buffer_idx = (mbuff_off + len) / buff_size;
	size_t last_buffer_len = (mbuff_off + len) % buff_size;
	if (last_buffer_idx >= iov_len) { // truncate if iovcnt too small
		last_buffer_idx = iov_len - 1;
		last_buffer_len = buff_size;
	}

	size_t total_len = 0;
	for (size_t i = first_buffer_idx, j = 0; i <= last_buffer_idx; i++, j++) {
		size_t expected_off = i == first_buffer_idx ? first_buffer_off : 0;
		size_t expected_len = i == last_buffer_idx ? last_buffer_len : buff_size;
		assert(iov_mbuff[j].iov_base == buffs[i] + expected_off);
		assert(iov_mbuff[j].iov_len == expected_len);
		total_len += expected_len;
	}

	assert(ret_len == total_len);
	mb.reset(std::free);
}

static void
test_reslice_zero_len_bug1(void)
{
	udepot::Mbuff mbuff = create_mbuff(4096, 1);
	mbuff.append_zero(1024);
	mbuff.reslice(512, 512);
	assert(mbuff.get_valid_size() == 512);
	mbuff.reslice(0);
	assert(mbuff.get_valid_size() == 0);
	assert(mbuff.compute_valid_size() == 0);
	assert(mbuff.nodes_nr() == 0);
	mbuff.reset(std::free);
}

// this was not actually a bug, but I thought it was.
static void
test_reslice2(void)
{
	udepot::Mbuff mbuff = create_mbuff(1024, 2);
	mbuff.append_zero(2048);
	mbuff.reslice(1024, 1024);
	assert(mbuff.get_valid_size() == 1024);
	assert(mbuff.get_valid_size() == 1024);
	assert(mbuff.compute_valid_size() == 1024);
	assert(mbuff.nodes_nr() == 1);
	mbuff.reset(std::free);
}

static void
test_prepend1(void)
{
	char buff[] = { 'a', 'b', 'c'};
	char buff2[sizeof(buff)];

	printf("%s\n", __PRETTY_FUNCTION__);
	udepot::Mbuff mbuff = create_mbuff(1024, 2);
	mbuff.append_zero(2048);
	mbuff.reslice(2000, 48);

	size_t prepend_ret = mbuff.prepend_from_buffer(buff, sizeof(buff));
	assert(prepend_ret == sizeof(buff));

	mbuff.copy_to_buffer(0, buff2, prepend_ret);
	assert(memcmp(buff, buff2, sizeof(buff)) == 0);

	mbuff.reslice(0);
	mbuff.reset(std::free);
}

static void
test_partial_copy_to_buffer(void)
{
	printf("%s\n", __PRETTY_FUNCTION__);
	char buff[32] = {0};
	udepot::Mbuff mbuff = create_mbuff(16, 1);
	mbuff.append_val(16, 'x');

	size_t copied = mbuff.copy_to_buffer(0, buff, sizeof(buff));
	assert(copied == 16);

	for (size_t i=0; i < sizeof(buff); i++) {
		char expected_val = i < 16 ? 'x' : 0;
		assert(buff[i] == expected_val);
	}

	mbuff.reslice(0);
	mbuff.reset(std::free);
}

static size_t
do_test_fill_iovec_append(size_t data_len,
                          size_t split_len,
                          size_t mbuff_buff_size,
                          size_t mbuff_buffs_nr,
                          size_t iov_len)
{
	size_t ret;
	// create an mbuff with @mbuff_buff_size, @mbuff_bufs_nr
	auto mbuff = create_mbuff(mbuff_buff_size, mbuff_buffs_nr);
	assert(mbuff.get_valid_size() == 0);
	// create a buffer with data of size @data_len
	char *buff = static_cast<char *>(malloc(data_len));
	for (size_t i=0; i < data_len; i++) {
		buff[i] = i;
	}
	// append part of the buffer (@split_len) to the mbuff.
	ret = mbuff.append_from_buffer(buff, split_len);
	assert(ret == split_len);
	// place the rest in a pipe
	int pipefd[2];
	if (pipe(pipefd) == -1) {
		perror("pipe");
		abort();
	}
	if (write(pipefd[1], buff + split_len, data_len - split_len) != (ssize_t)data_len - (ssize_t)split_len) {
		perror("write");
		abort();
	}
	// build an iovec to append the rest
	struct iovec iov[iov_len];
	size_t iov_cnt;
	size_t iov_bytes;
	std::tie(iov_cnt, iov_bytes) = mbuff.fill_iovec_append(data_len - split_len, iov, iov_len);
	assert(iov_bytes == mbuff.get_valid_size() - split_len);
	assert(iov_bytes == iovec_len(iov, iov_cnt));
	ret += iov_bytes;
	// read from the pipe using the iovec
	if (setnonblocking(pipefd[0]) == -1) {
		perror("fcntl");
		abort();
	}
	if (readv(pipefd[0], iov, iov_cnt) != (ssize_t)iov_bytes) {
		perror("pread");
		abort();
	}
	// compare the contents of the mbuff to the original buffer
	assert(mbuff.mem_compare_buffer(0, buff, ret) == 0);
	// celebrate!
	close(pipefd[0]); close(pipefd[1]);
	mbuff.reset(std::free);
	return ret;
}

static void
test_fill_iovec_append(void) {
	printf("%s\n", __PRETTY_FUNCTION__);
	size_t ret;
	ret = do_test_fill_iovec_append(32*32, 10, 32, 32, 32);
	assert(ret == 32*32);
	// we run out of Mbuff buffers
	ret = do_test_fill_iovec_append(32*10, 10, 32, 32, 32);
	assert(ret == 32*10);
	// we run out iovecs
	ret = do_test_fill_iovec_append(32*32, 10, 32, 32, 2);
	assert(ret == 32*2);

}

int main(int argc, char *argv[])
{
	test_append_from_and_copy_to_buffer(1024, 16, 2000);
	test_append_from_and_copy_to_buffer(1024, 16, 128);
	test_append_from_and_copy_to_buffer(1024, 16, 1024);
	test_append_from_and_copy_to_buffer(1024, 16, 16*1024);

	test_reslice(1024, 16, 128, 0, 0);
	test_reslice(1024, 16, 128, 10, 10);
	test_reslice(1024, 16, 2000, 0, 0);
	test_reslice(1024, 16, 2000, 1500, 30);
	test_reslice(1024, 16, 2000, 2000, 0);

	test_prepend(16, 1, 8);
	test_prepend(16, 2, 0);
	test_prepend(16, 2, 8);
	test_prepend(16, 2, 15);
	test_prepend(16, 2, 16);
	test_prepend(16, 2, 16 + 8);
	test_prepend(16, 2, 31);
	test_prepend(16, 2, 32);

	test_memcompare(8, 4, 0, 0, 32);
	test_memcompare(8, 4, 0, 10, 10);
	test_memcompare(8, 4, 2, 10, 8);
	test_memcompare(8, 4, 2, 0, 30);

	test_fill_iovec_valid(8, 4, 0, 4*8, 4);
	test_fill_iovec_valid(8, 4, 4*8, 4*8, 4); // error
	test_fill_iovec_valid(8, 4, 4*8, 4*2, 4*2);
	test_fill_iovec_valid(8, 4, 4*8, 10, 20);

	test_reslice_zero_len_bug1();
	test_reslice2();

	test_prepend1();

	// The following code should trigger an assertion failure
	//unsigned char b1[16], b2[16];
	//udepot::Mbuff mb;
	//mb.add_buff(b1, sizeof(b1));
	//mb.add_buff<int>(b2, sizeof(b2));
	//mb.reset([](unsigned char *p) {});

	test_partial_copy_to_buffer();

	test_fill_iovec_append();

	return 0;
}

#endif // MBUFF_TESTS



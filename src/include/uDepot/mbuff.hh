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

#ifndef _MBUFF_HH__
#define _MBUFF_HH__

#include <memory>
#include <stddef.h>
#include <typeinfo>
#include <typeindex>
#include <limits>
#include <functional>

#include "util/inline-cache.hh"
#include "util/sys-helpers.hh"
#include "util/debug.h"

namespace udepot {

// An Mbuff is a linked list of buffers, used for network/storage I/O.
//
// The Mbuff implementation (purposly) does not include any allocation -- this
// happens externally.
//
// An Mbuff space is split into valid data and free space.
class Mbuff {
protected:
	struct Node;

public:
	//
	// Constructors et al.
	//
	// NB: the type_index thing is a stupid trick for tagging an Mbuff with a
	// "buffer type", and check that all the added buffers match this type.
	//
	// I've been trying the idea of using wrappers to represent pointers from
	// different allocators (e.g., malloc(), posix_memalign(), rte_malloc(),
	// etc.) that match different requirements of IO subsystems (buffered IO,
	// direct IO, SPDK/DPDK). The pointer wrapper (IoPtr) has a public ptr_m
	// void * member with the pointer. Type_index is a quick-n-dirty way for
	// checking that no different pointer types are intermixed into a single
	// Mbuff. Another approach would be to make Mbuff a template over the
	// pointer type, but this would be too intrusive.
	//
	// For adding buffers see: add_buff<T> and add_iobuff().
	//
	// Not sure how much sense it actually makes, but it's not a big change and
	// users can ignore it if they want to.
	//Mbuff(std::type_index &&ty_idx);
	Mbuff(std::type_index ty_idx);
	Mbuff(Mbuff &&);
	Mbuff(Mbuff const&) = delete;
	void operator=(Mbuff const&) = delete;

	// Without the deafult constructor is easier to catch errors
	//Mbuff() : Mbuff(std::type_index(typeid(unsigned char *))) {};
	~Mbuff();

	// Mbuff's interface is based on Chunk, which is a simple wrapper for a
	// buffer: (pointer, len)
	struct Chunk {
		friend Node;

		// Chunk Members
	protected:
		// we store the offset of this chunk's pointer within the buff that it
		// was created from. This allows us to restore the buff when we return
		// the chunk. See, for example, get_valid_chunk() / put_valid_chunk().
		size_t                         buff_offset_;
		std::unique_ptr<unsigned char> ptr;
	public:
		size_t                         len;

		// Constructors et al.
		Chunk() : buff_offset_(0), ptr(nullptr), len(0) {}
		Chunk(std::unique_ptr<unsigned char> &&ptr_, size_t len_, size_t b_off)
		: buff_offset_(b_off), len(len_)
		{
			std::swap(ptr, ptr_);
		}
		Chunk(Chunk &&);
		Chunk(Chunk const&) = delete;
		void operator=(Chunk const&) = delete;

		void operator=(Chunk &&c) {
			std::swap(c.buff_offset_, buff_offset_);
			std::swap(c.ptr, ptr);
			std::swap(c.len, len);
		}

		// Methods to access the pointer
		//
		// the idea is that if you have a const reference of a chunk you can
		// only get a constant pointer.
		unsigned char *getPtr() { return ptr.get(); }
		const unsigned char *getConstPtr() const { return ptr.get(); }

		bool is_invalid(void) const { return ptr == nullptr; }
		bool is_valid(void)   const { return !is_invalid(); }

	};

	// Append and prepend functions.
	//
	// Appending and prepending happen as follows:
	//   - user requests a chunk to write the new data (prepare function)
	//   - if the chunk is valid, users:
	//      - write data into the chunk (up to its size)
	//      - return the chunk to the Mbuff together with the length of the data
	//        that of the updated data (finalize function)
	//   - if the chunk is invalid, no free space exists. Users need to add
	//     buffers to the Mbuff
	//
	// See the append(AppendFn fn) implementation as an example

	// append functions:
	Chunk append_prepare();                             // if Chunk.is_invald(): need to add_buff()
	Chunk append_prepare_inline();                      // same as above, but it will not allocate a new node from the free list
	void append_finalize(Chunk &&, size_t append_len);  // finalize append (only if chunk returned was valid)
	// prepend functions:
	//  for a chunk (ptr, len), finalizing with prepend_len validates
	//  (ptr + len - prepend_prepare, ptr + len)
	Chunk prepend_prepare();                             // Chunk.is_invald(): need to add_buff()
	Chunk prepend_prepare_inline();                      // same as above, but it will not allocate a new node from the free list
	void prepend_finalize(Chunk &&, size_t prepend_len); // finalize prepend (only if chunk returned was valid)


	// A much better interface for appending/prepending is having the user
	// provide a function for the updates, so that the caller does not have to
	// worry about properly doing the prepare() and finalize() sequence.
	using AppendFn = std::function<size_t(unsigned char *b, size_t len)>;

	// append() will call the argument function to append data as long as it
	// does not return 0 or less than the given buffer size.
	// Invalid chunks (i.e,. no free space) cause the append() to stop without
	// passing the invalid chunk to the user function.
	inline void append(AppendFn f) {
		while (true) {
			Chunk c = append_prepare();
			if (c.is_invalid())
				return;
			size_t avail = c.len;
			size_t appended = f(c.getPtr(), avail);
			append_finalize(std::move(c), appended);
			if (appended == 0 || appended < avail)
				break;
		}
	}

	// Only append if there is space in the current Mbuff node.
	//
	// If there is space in the existing node, @f will be called _once_ with the
	// remaining space as arguments. Otherwise, @f will not be called
	inline void append_inline(AppendFn f) {
		Chunk c = append_prepare_inline();
		if (c.is_invalid())
			return;
		size_t appended = f(c.getPtr(), c.len);
		append_finalize(std::move(c), appended);
	}

	// see append(AppendFn f)
	using PrependFn = std::function<size_t(unsigned char *b, size_t len)>;
	inline void prepend(PrependFn f) {
		while (true) {
			Chunk c = prepend_prepare();
			if (c.is_invalid())
				return;
			size_t avail = c.len;
			size_t prepended = f(c.getPtr(), avail);
			prepend_finalize(std::move(c), prepended);
			if (prepended == 0 || prepended < avail)
				break;
		}
	}

	// see append_inline(AppendFn f)
	void prepend_inline(PrependFn f) {
		Chunk c = prepend_prepare_inline();
		if (c.is_invalid())
			return;
		size_t prepended = f(c.getPtr(), c.len);
		prepend_finalize(std::move(c), prepended);
	}

	// iterate over the valid chunks of an Mbuff
	class Iterator;

	// Initalize an iterator over the valid areas of an Mbuff.
	//
	// @iter must be an invalid iterator.
	//
	// returns an error or 0
	//
	// If @len is larger than the available mbuff valid data, it returns EINVAL.
	// That is, iter->get_len() after this call will always be equal to len if
	// the function returns 0.
	//
	// If the operation results in an iterator with size 0, the iterator is not
	// touched, i.e., it remains invalid. This happens if @len == 0.
	//
	// see also: iterate_valid_partial_init()
	int iterate_valid_init(Iterator &iter, size_t off, size_t len) const;
	int iterate_valid_init(Iterator &iter) const {
		return iterate_valid_init(iter, 0, get_valid_size());
	}

	// Initalize an iterator over the valid areas of an Mbuff.
	//
	// @iter must be an invalid iterator.
	//
	// returns an error or 0
	//
	// If @len is larger than the available mbuff valid data, the function will
	// adapt the iterator length to reflect that. That is, iter->get_len() after
	// this call might be less than @len.
	//
	// If the operation results in an iterator with size 0, the iterator is not
	// touched, i.e., it remains invalid. This happens if @len == 0 or if @off
	// points to the end to the valid region.
	//
	// see also: iterate_valid_init()
	int iterate_valid_partial_init(Iterator &iter, size_t off, size_t len) const;

	// redefines the valid area of the mbuff
	//    If Chunks are freed, they are placed in free list and become available
	//    with get_free_buff();
	//    returns 0 if succesful, or -1 if error
	// if len == 0, off is ignored and all the buffers are placed in the free
	// list
	int reslice(size_t len, size_t off=0);

	// reset Mbuf: release all heap-allocated storage, including the buffers
	// based on the given function. Both free and valid buffers are freed.
	// Another (maybe better) option is to provide a dealloc function with each
	// buffer which allows finer-grained control.
	using FreeBuffFn = std::function<void(unsigned char *)>;
	void reset(FreeBuffFn fn);

	// Mbuff does not perform any allocation, buffers have to be added manually
	// with add_buff()
	void add_buff(unsigned char *buff, size_t size) const;

	template<typename T>
	void add_buff(unsigned char *buff, size_t size) const {
		assert(ioptr_compatible<T>());
		add_buff(buff, size);
	}

	template<typename IOPtr>
	bool ioptr_compatible() const {
		return std::type_index(typeid(IOPtr)) == mb_buff_type_index;
	}

	template<typename IOptr>
	void add_iobuff(IOptr &ptr, size_t size) const {
		void *p = ptr.ptr_m;
		add_buff<IOptr>(static_cast<unsigned char *>(p), size);
	}

	// total size
	size_t get_total_size(void) const {
		size_t ret;
		ret = mb_free_size_ + mb_valid_size_;
		if (mb_head_) {
			assert(mb_head_->mb_buff_len_ >= mb_head_->mb_len_);
			ret += mb_head_->mb_buff_len_ - mb_head_->mb_len_;
				}
		if (mb_tail_ != mb_head_) {
			assert(mb_tail_->mb_buff_len_ >= mb_tail_->mb_len_);
			ret += mb_tail_->mb_buff_len_ - mb_tail_->mb_len_;
		}
		return ret;
	}

	// returns an available free buffer, or an invalid chunk.
	//  The returned buffer is no longer used by Mbuff.  It can, for example, be
	//  freed by the user.
	std::tuple<unsigned char *, size_t> get_free_buff() const;

	// move free buffers to @dst.
	// stop if we added >= max_size bytes
	size_t move_free_buffs_to(Mbuff const& dst,
	                          size_t max_size = std::numeric_limits<size_t>::max()) const;

	// for debugging
	size_t nodes_nr(void);
	size_t compute_valid_size(void);
	void assert_valid_head_tail(void);

protected:
	// class to hold buffers
	struct Node {
		Node(unsigned char *buff_, size_t buff_len, Node *next = nullptr);
		Node() : Node(nullptr, 0, nullptr) {};

		~Node();

		Node(Node const&) = delete;
		void operator=(Node const &) = delete;


		std::unique_ptr<unsigned char>  mb_buff_;      // buffer
		size_t                          mb_buff_len_;  // buffer length
		size_t                          mb_off_;       // offset of valid part in the buffer
		size_t                          mb_len_;       // length of valid part
		Node                           *mb_next_;      // linked list for next
		#if !defined(NDEBUG)
		unsigned char                  *mb_buff_orig_; // used for validation
		#endif

		void reset(void) {
			mb_off_ = 0;
			mb_len_ = 0;
		}

		std::tuple<unsigned char *, size_t> clear() {
			auto ret = std::make_tuple(mb_buff_.release(), mb_buff_len_);
			new (this) Node();
			return ret;
		}

		// How many bytes we can append to this node?
		size_t append_space() const {
			size_t end = mb_off_ + mb_len_;
			assert(end <= mb_buff_len_);
			return mb_buff_len_ - end;
		}

		// How many bytes we can prepend to this node?
		size_t prepend_space() const {
			// If the node is new, the whole space of the node is avaialable for
			// prepending
			if (mb_len_ == 0) {
				assert(mb_off_ == 0);
				return mb_buff_len_;
			}

			// otherwise, we can only prepend up to mb_off_ bytes
			return mb_off_;
		}

		// get a valid chunk from the buffer of a node. Assumes the proper
		// checks have been made by the caller (see assertions).
		Chunk get_valid_chunk(size_t off = 0, size_t len = (size_t)-1) {
			assert(mb_buff_ != nullptr);
			if (len == (size_t) -1)
				len = mb_len_;
			assert(len <= mb_len_);
			assert(mb_off_ + off + len <= mb_buff_len_);
			size_t ptr_off = mb_off_ + off;
			std::unique_ptr<unsigned char> ptr(mb_buff_.release() + ptr_off);
			return Chunk(std::move(ptr), len, ptr_off);
		}

		// return the chunk into the node
		void put_valid_chunk(Chunk &&c) {
			assert(mb_buff_ == nullptr);                          // get_valid_chunk() was called
			assert(c.getPtr() - c.buff_offset_ == mb_buff_orig_); // chunk has correct pointer
			unsigned char *p_ = c.ptr.release();
			mb_buff_.reset(p_ - c.buff_offset_);
			return;
		}

		// should be called only if append_space() > 0
		Chunk get_append_chunk(void) {
			size_t len = append_space();
			assert(len > 0);
			std::unique_ptr<unsigned char> ptr(mb_buff_.release() + mb_off_ + mb_len_);
			return Chunk(std::move(ptr), len, 0);
		}

		void put_append_chunk(Chunk &&c, size_t append_len) {
			assert(mb_buff_ == nullptr);                              // get_append_chunk() was called
			assert(c.ptr.get() - mb_off_ - mb_len_ == mb_buff_orig_); // chunk has correct pointer
			assert(c.len == append_space());                          // chunk has correct len
			assert(append_len <= append_space());                     // append_len is correct

			unsigned char *p_ = c.ptr.release();
			mb_buff_.reset(p_ - mb_off_ - mb_len_);
			mb_len_ += append_len;
			return;
		}

		// should be called only if prepend_space() > 0
		Chunk get_prepend_chunk(void) {
			size_t len = prepend_space();
			assert(len > 0);
			std::unique_ptr<unsigned char> ptr(mb_buff_.release());
			return Chunk(std::move(ptr), len, 0);
		}

		void put_prepend_chunk(Chunk &&c, size_t prepend_len) {
			assert(mb_buff_ == nullptr);            // get_prepend_chunk() was called
			assert(c.ptr.get() == mb_buff_orig_);   // chunk has correct pointer
			assert(c.len == prepend_space());       // chunk has correct len
			assert(prepend_len <= prepend_space()); // append_len is correct

			unsigned char *p_ = c.ptr.release();
			mb_buff_.reset(p_);

			if (mb_off_ == 0 && mb_len_ == 0) {
				// prepend to an empty Node (just allocated)
				assert(prepend_len <= mb_buff_len_);
				mb_off_ = mb_buff_len_ - prepend_len;
				mb_len_ = prepend_len;
			} else {
				// prepend to an existing node
				assert(mb_off_ >= prepend_len);
				mb_off_ -= prepend_len;
				mb_len_ += prepend_len;
			}
			return;
		}
	};

	// return The node and the offset within the node for a given Mbuff offset
	// node == nullptr if the offset is out of bounds
	std::tuple<Node *, size_t> find_node(size_t off) const;

private:
	size_t      mb_valid_size_; // Mbuff size
	Node       *mb_head_;
	Node       *mb_tail_;

	// fields related to buffer management.
	// NB: We declare them mutable, so that we can declare buffer
	// management functions as const.
	mutable size_t                   mb_free_size_;  // total free size on ->mb_free_ list
	mutable size_t                   mb_free_nodes_; // total number of free nodes
	mutable Node                    *mb_free_;
	mutable InlineObjCache<Node, 16> mb_node_cache_;

	std::type_index  mb_buff_type_index;

	Node *get_free_node() const;
	void put_free_node(Node *) const;

	void reset_list(Node *head, FreeBuffFn free_buff);

	// check invariants in node that is part of the valid list (i.e., between
	// ->mb_head_ and ->mb_tail_)
	bool is_list_node_legit(Node *n) {
		// only head node is allowed to have an offset
		if (n != mb_head_ && n->mb_off_ != 0)
			return false;
		// only tail node is allowed to have available space at the end
		if (n != mb_tail_ && (n->mb_len_ + n->mb_off_ != n->mb_buff_len_))
			return false;
		return true;
	}

	// helper function to initialize iterators.
	void do_initialize_iterator(Mbuff::Iterator &iter, size_t off, size_t len) const;

public: // inline helper functions

	// There are two sources of available size:
	//   - unused space in the currrent (either front or back) buffer.
	//   - unused buffers
	//  The functions below the sum of free space on each end
	inline size_t append_avail_size(void) {
		return mb_free_size_ + (mb_tail_ ? mb_tail_->append_space() : 0);
	}

	// how many available nchunks are there. This can be used, for example, to
	// determine the iovec count to pass into a function like
	// fill_iovec_append()
	inline size_t append_avail_nchunks(void) {
		return mb_free_nodes_ + (mb_tail_ ? !!mb_tail_->append_space() : 0);
	}

	size_t prepend_avail_size(void); // do me!

	// size of free buffers
	inline size_t get_free_size(void) const { return mb_free_size_; }
	// mbuff size
	inline size_t get_valid_size(void) const { return mb_valid_size_; }

	// This will return a chunk representing the valid data of this Mbuff *if*
	// there is only a single buffer of valid data. That is, the valid data can
	// be placed in a single contiguous space.
	Chunk get_valid_single_buffer(void) {
		if (mb_head_ == nullptr || mb_head_->mb_next_ != nullptr)
			return Chunk();
		else
			return mb_head_->get_valid_chunk();
	}

	// counterpart of get_valid_single_buffer()
	void put_valid_single_buffer(Chunk &&c) {
		assert(c.is_valid());
		mb_head_->put_valid_chunk(std::move(c));
	}

public:
	class Iterator {
			friend Mbuff;
		protected:
			Node  *i_node_;  // current node
			size_t i_len_;   // total length for the iterator
			Chunk  i_chunk_; // current chunk

			Iterator(Iterator const&) = delete;
			void operator=(Iterator const&) = delete;

		public:
			// For creating iterators on Mbuffs, use the appropriate Mbuff
			// methods, e.g., iterate_valid_init()
			Iterator() : i_node_(nullptr), i_len_(0) {}
			~Iterator();

			bool is_valid(); // Does the iterator point to a valid chunk?
			bool is_invalid() { return !is_valid(); }
			void next();     // move to the next chunk
			void stop();     // prematurely invalidate an iterator. (It's OK to call this on an invalid iterator)
			bool is_next_valid(); // returns true if the next chunk is valid

			Chunk const& get_chunk() const;
			Chunk&       get_chunk();

			// return the (current) length of the iterator
			size_t get_len() const { return i_len_; }
	};

public:
	// Helper functions.
	//
	// They could be implemented outside of Mbuff, but they are common enough to
	// do them here and they also have the property of illustrating the
	// interface. So as a rule, they should use the public interface only.
	//

	// append from buffer to Mbuff
	//   returns how many bytes were copied
	size_t append_from_buffer(const void *buff_src_, size_t buff_src_size) {
		size_t remaining = buff_src_size;
		size_t copied = 0;
		const char *buff_src = static_cast<const char *>(buff_src_);
		append(
			[&remaining, &copied, buff_src, buff_src_size]
			(unsigned char *dst_buff, size_t dst_len) -> size_t {
				assert(remaining + copied == buff_src_size);
				size_t cp_len = std::min(remaining, dst_len);
				if (cp_len == 0)
					return 0;
				std::memcpy(dst_buff, buff_src + copied, cp_len);
				remaining -= cp_len;
				copied += cp_len;
				return cp_len;
			}
		);
		return copied;
	}

	// prepend from buffer to Mbuff
	//   returns how many bytes were copied
	size_t prepend_from_buffer(const void *src_, size_t src_size) {
		size_t remaining = src_size;
		size_t copied = 0;
		const char *src = static_cast<const char *>(src_);
		prepend(
			[&remaining, &copied, src, src_size]
			(unsigned char *dst, size_t dst_len) -> size_t {
				assert(remaining + copied == src_size);
				size_t cp_len = std::min(remaining, dst_len);
				if (cp_len == 0)
					return 0;
				std::memcpy(dst + dst_len - cp_len, src + remaining - cp_len, cp_len);
				remaining -= cp_len;
				copied += cp_len;
				return cp_len;
			}
		);
		return copied;
	}

	size_t prepend_from_mbuff(Mbuff &mbuff_in, size_t mbuff_off, size_t len) {
		Iterator iter;
		size_t copied = 0;
		int err;

		err = mbuff_in.iterate_valid_init(iter, mbuff_off, len);
		if (err)
			return 0;

		while (true) {
			Chunk const& c = iter.get_chunk();
			if (c.is_invalid())
				break;
			size_t cp_len = std::min(len - copied, c.len);
			size_t copied_iter = prepend_from_buffer(c.getConstPtr(), cp_len);
			copied += copied_iter;
			iter.next();
			break;	// only support for single chunk now
		}
		if (copied != len) {
			UDEPOT_ERR("copied %luB out of %luB", copied, len);
		}
		assert(copied == len); // iterate_valid_init() guarantees that this will work
		return copied;
	}

	size_t append_from_mbuff(Mbuff &mbuff_in, size_t mbuff_off, size_t len) {
		Iterator iter;
		size_t copied = 0;
		int err;

		err = mbuff_in.iterate_valid_init(iter, mbuff_off, len);
		if (err)
			return 0;

		while (true) {
			Chunk const& c = iter.get_chunk();
			if (c.is_invalid())
				break;
			size_t cp_len = std::min(len - copied, c.len);
			size_t copied_iter = append_from_buffer(c.getConstPtr(), cp_len);
			copied += copied_iter;
			iter.next();
		}
		if (copied != len) {
			UDEPOT_ERR("copied %luB out of %luB", copied, len);
		}
		assert(copied == len); // iterate_valid_init() guarantees that this will work
		return copied;
	}

	// Copy a region of the mbuff to a buffer
	//
	// Copies @len bytes at @buff, starting at mbuff offset @mbuff_off.
	//
	// Returns how many butes were copied.
	//
	// If len is bigger than the available valid space, the function will do a
	// partial (i.e., less than @len) copy.
	size_t copy_to_buffer(size_t mbuff_off, void *buff, size_t len) const {
		Iterator iter;
		size_t copied = 0;
		int err;

		err = iterate_valid_partial_init(iter, mbuff_off, len);
		if (err)
			return 0;

		// update len
		len = iter.get_len();

		while (true) {
			Chunk const& c = iter.get_chunk();
			if (c.is_invalid())
				break;
			size_t cp_len = std::min(len - copied, c.len);
			std::memcpy(static_cast<unsigned char *>(buff) + copied, c.getConstPtr(), cp_len);
			copied += cp_len;
			iter.next();
		}

		assert(copied == len); // iterate_valid_init() guarantees that this will work
		return copied;
	}

	// Call recv() to append data to the mbuff
	//
	// returns:
	//   0:            @size data where appended to the mbuff
	//   !=0:          error, not all data appended to the mbuff
	//
	// In the later case the return value is errno if recv() returned -1, or
	// ECONNRESET if recv() returned 0 before appending @size data to the mbuff.
	//
	// If the user wants to check how many data were written to the mbuff, they
	// can check the mbuff size.
	int append_recv(int fd, size_t size, int flags) {
		if (append_avail_size() < size)
			return ENOMEM;

		int recv_err;
		size_t remaining = size;
		append(
			[&remaining, &recv_err, fd, flags]
			(unsigned char *buff, size_t len) -> size_t {
				size_t recv_len = std::min(remaining, len);
				if (recv_len == 0)
					return 0;
				std::tie(recv_err, recv_len) = recv_full(fd, buff, recv_len, flags);
				assert(recv_len == std::min(remaining, len) || recv_err);
				remaining -= recv_len;
				return recv_len;
			}
		);

		if (recv_err == -1)
			return errno;
		if (remaining != 0)
			return ECONNRESET;

		return 0;
	}

	// returns 0 or error
	//
	// An alternative way to implement this so that we execute a single syscall
	// is via sendmsg()
	int send(int fd, size_t off, size_t len, int flags) const {
		Iterator iter;
		int err;

		err = iterate_valid_init(iter, off, len);
		if (err)
			return EINVAL;

		while (true) {
			Chunk const& c = iter.get_chunk();
			if (c.is_invalid())
				break;
			size_t data_sent;
			std::tie(err, data_sent) = send_full(fd, c.getConstPtr(), c.len, flags);
			if (err) {
				iter.stop();
				return errno;
			}
			len -= data_sent;
			iter.next();
		}
		if (0 != len)
			return EINVAL;
		return 0;
	}

	// returns:
	//  0  -> if buffs match
	//  !0 -> error
	//
	// NB: return value has different semantics than memcmp()
	int mem_compare_buffer(size_t mbuff_off, char *buff, size_t len) const {
		Iterator iter;
		int err;

		err = iterate_valid_init(iter, mbuff_off, len);
		if (err)
			return err;

		size_t remaining = len;
		size_t buff_off  = 0;
		while (remaining > 0) {
			Chunk const& c = iter.get_chunk();
			if (c.is_invalid())
				break;
			size_t comp_len = std::min(remaining, c.len);
			if (memcmp(c.getConstPtr(), buff + buff_off, comp_len) != 0) {
				iter.stop();
				return 1;
			}

			remaining -= comp_len;
			buff_off  += comp_len;
			iter.next();
		}

		assert(!iter.is_valid());
		assert(remaining == 0);
		return 0;
	}

	// returns:
	//  0  -> if buffs match
	//  !0 -> error
	//
	// NB: return value has different semantics than memcmp()
	static int mem_compare(Mbuff const& mb1, size_t mb1_off,
	                       Mbuff const& mb2, size_t mb2_off,
	                       size_t len) {
		int err;
		Mbuff::Iterator iter1, iter2;
		if ((err = mb1.iterate_valid_init(iter1, mb1_off, len)))
			return err;
		if ((err = mb2.iterate_valid_init(iter2, mb2_off, len)))
			return err;
		assert(iter1.is_valid());
		assert(iter2.is_valid());
		size_t c1_off = 0, c2_off = 0;
		Chunk const& c1 = iter1.get_chunk();
		Chunk const& c2 = iter2.get_chunk();
		size_t total_compared = 0;
		while (true) {
			assert(c1_off < c1.len);
			assert(c2_off < c2.len);
			size_t cmp_len = std::min(c1.len - c1_off, c2.len - c2_off);
			const unsigned char *b1 = c1.getConstPtr() + c1_off;
			const unsigned char *b2 = c2.getConstPtr() + c2_off;
			if (memcmp(b1, b2, cmp_len) != 0) {
				iter1.stop();
				iter2.stop();
				return 1;
			}
			total_compared += cmp_len;
			c1_off += cmp_len;
			c2_off += cmp_len;
			if (c1_off == c1.len) {
				c1_off = 0;
				iter1.next();
			}
			if (c2_off == c2.len) {
				c2_off = 0;
				iter2.next();
			}

			if (total_compared == len) {
				assert(!iter1.is_valid());
				assert(!iter2.is_valid());
				return 0;
			} else {
				assert(iter1.is_valid());
				assert(iter2.is_valid());
			}
		}
	}

	// Fill an iovec with the valid data of Mbuff:
	// 	returns (-err, _) if error
	// 	        (iov filled count, total bytes count) if no error
	std::tuple<int, size_t>
	fill_iovec_valid(size_t mbuff_off, size_t len, struct iovec *iov, size_t iov_size) {
		size_t ret_cnt, ret_len;
		int err;
		Iterator iter;

		if ((err = iterate_valid_init(iter, mbuff_off, len))) {
			assert(err > 0);
			return std::make_tuple(-err, 0);
		}

		ret_len = 0;
		for (ret_cnt = 0; ret_cnt < iov_size; ret_cnt++) {
			if (iter.is_invalid())
				break;
			Chunk &c = iter.get_chunk();
			iov[ret_cnt].iov_base = static_cast<void *>(c.getPtr());
			iov[ret_cnt].iov_len = c.len;
			ret_len += c.len;
			iter.next();
		}

		iter.stop(); // in case we finished because we reached iov_size
		return std::make_tuple(ret_cnt, ret_len);
	}

	// Fill an iovec with the proper buffers, so that data can be appended to an
	// mbuff.
	//
	// After this call, the valid data are extended to include the buffers
	// placed in the iovec. Hence, it is typically expected that the iovec will
	// be used to initialize the data before the Mbuff is accessed. IOW, this
	// behaves similarly to append_uninitialized()
	//
	// 	returns (-err, _) if error
	// 	        (iov filled count, total bytes count) if no error
	std::tuple<int, size_t>
	fill_iovec_append(size_t len, struct iovec *iov, size_t iov_len) {
		size_t rem = len; // reamining size to append to iovec
		size_t iov_idx = 0;

		append(
			[&rem, iov, iov_len, &iov_idx] (unsigned char *b, size_t b_len) -> size_t {
				if (iov_idx == iov_len)
					return 0;
				size_t len = std::min(b_len, rem);
				if (len == 0)
					return 0;

				iov[iov_idx].iov_base = static_cast<void *>(b);
				iov[iov_idx].iov_len = len;
				iov_idx++;
				rem -= len;
				return len;
			}
		);

		return std::make_tuple(iov_idx, len - rem);
	}

	// get the number of chunks (i.e., consecutive buffer areas) for an offset
	// and len within the valid data of an Mbuff.
	std::tuple<int, size_t>
	get_valid_nchunks(const size_t mbuff_off, size_t len)  {
		Iterator iter;
		int err;
		size_t ret_cnt = 0;
		if ((err = iterate_valid_init(iter, mbuff_off, len))) {
			assert(err > 0);
			return std::make_tuple(-err, 0);
		}
		for (; len && !iter.is_invalid(); ret_cnt++, iter.next()) {
			Chunk &c = iter.get_chunk();
			const size_t l = std::min(c.len, len);
			len -= l;
		}
		return std::make_tuple(0, ret_cnt);
	}

	// This is a I-know-what-I'm-doing function.
	// Using it might kill your refrigerator.
	template<typename T>
	void write_val(size_t mbuff_off, T val) {
		int err;
		Iterator iter;

		if ((err = iterate_valid_init(iter, mbuff_off, sizeof(T))) || iter.is_invalid()) {
			UDEPOT_ERR("%s:%d: Error %d.", __PRETTY_FUNCTION__, __LINE__, err);
			abort();
		}

		Chunk &c = iter.get_chunk();
		T *val_ptr = (T *)c.getPtr();
		assert(c.len >= sizeof(T));
		iter.stop();

		*val_ptr = val;
	}

	// This is a I-know-what-I'm-doing function.
	// Using it might kill your refrigerator.
	template<typename T>
	T read_val(size_t mbuff_off) {
		int err;
		Iterator iter;

		if ((err = iterate_valid_init(iter, mbuff_off, sizeof(T))) || iter.is_invalid()) {
			UDEPOT_ERR("%s:%d: Error %d.", __PRETTY_FUNCTION__, __LINE__, err);
			abort();
		}

		Chunk &c = iter.get_chunk();
		T *val_ptr = (T *)c.getPtr();
		assert(c.len >= sizeof(T));
		iter.stop();

	        return *val_ptr;
	}

	// Append uninitialized data.
	// This is useful for leaving prepend space
	void append_uninitialized(size_t size) {
		size_t rem = size;
		append(
			[&rem] (unsigned char *b __attribute__((unused)), size_t b_len) -> size_t {
				assert(b_len != 0 && b != nullptr);
				size_t x = std::min(b_len, rem);
				rem -= x;
				return x;
			}
		);
	}

	void append_val(size_t size, char val) {
		size_t rem = size;
		append(
			[&rem, val] (unsigned char *b, size_t b_len) -> size_t {
				assert(b_len != 0 && b != nullptr);
				size_t x = std::min(b_len, rem);
				memset(b, val, b_len);
				rem -= x;
				return x;
			}
		);
	}

	void append_zero(size_t size) {
		return append_val(size, 0);
	}

	// for debugging
	void print_data(FILE *f, size_t off, size_t len) const;

};

} // end namespace udepot

#endif /* _MBUFF_HH__ */

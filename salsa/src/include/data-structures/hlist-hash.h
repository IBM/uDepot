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
#ifndef _CDS_HLIST_HASH_H_
#define _CDS_HLIST_HASH_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "generic/compiler.h"

struct hash_list{
	struct hash_list *next;
};

static inline int hash_list_empty(const struct hash_list *const l)
{
	return l->next == l;
}

static inline void hash_list_init(struct hash_list *const l)
{
	l->next = l; /* sentinel */
}

static inline void hash_list_add(
	struct hash_list *const node,
	struct hash_list *const l)
{
	struct hash_list *p = l->next;
	l->next = node;
	node->next = p;
}

int hash_list_delete(struct hash_list *node, struct hash_list *l);

int hash_list_find(struct hash_list *node, struct hash_list *l);

#define hash_list_for_each_entry(pos, head, member)			\
	for (pos = os_container_of((head)->next, typeof(*pos), member);	\
	     &pos->member != (head);					\
	     pos = os_container_of(pos->member.next, typeof(*pos), member))

struct hlist_hash;

int hlist_hash_init(
	const u32                 nbuckets,
	const bool                use_rwlock,
	struct hlist_hash **const h_out);

int hlist_hash_exit(struct hlist_hash *h);

int hlist_hash_insert(
	struct hlist_hash *const ht,
	struct hash_list  *const e);

int hlist_hash_delete(
	struct hlist_hash *const ht,
	struct hash_list  *const e);

int hlist_hash_lookup(
	struct hlist_hash       *const ht,
	const struct hash_list  *const e);

int hlist_hash_bare_insert(
	struct hlist_hash *const ht,
	struct hash_list  *const e);

int hlist_hash_bare_delete(
	struct hlist_hash *const ht,
	struct hash_list  *const e);

int hlist_hash_bare_lookup(
	struct hlist_hash       *const ht,
	const struct hash_list  *const e);

void hlist_hash_bucket_lock(
	struct hlist_hash       *const ht,
	const struct hash_list  *const e);

void hlist_hash_bucket_unlock(
	struct hlist_hash       *const ht,
	const struct hash_list  *const e);

void hlist_hash_down_write(
	struct hlist_hash       *const ht,
	const struct hash_list  *const e);

void hlist_hash_up_write(
	struct hlist_hash       *const ht,
	const struct hash_list  *const e);

void hlist_hash_down_read(
	struct hlist_hash       *const ht,
	const struct hash_list  *const e);

void hlist_hash_up_read(
	struct hlist_hash       *const ht,
	const struct hash_list  *const e);

u32 hlist_hash_get_size(struct hlist_hash *const ht);

u32 hlist_hash_get_max_collision_len(struct hlist_hash *const ht);

struct hash_list *hlist_hash_find_first_item_rlock(struct hlist_hash *ht);

struct hash_list *hlist_hash_find_next_item_rlock(
	struct hlist_hash       *ht,
	const struct hash_list  *e);

struct hash_list *hlist_hash_find_first_item_wlock(struct hlist_hash *ht);

struct hash_list *hlist_hash_find_next_item_wlock(
	struct hlist_hash       *ht,
	const struct hash_list  *e);

#endif	/* _CDS_HLIST_HASH_H_ */

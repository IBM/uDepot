/*
 *  Copyright (c) 2020 International Business Machines
 *  All rights reserved.
 *
 *  SPDX-License-Identifier: BSD-3-Clause
 *
 *  Authors: Kornilios Kourtis (kou@zurich.ibm.com, kornilios@gmail.com)
 *
 */
#ifndef NET_HELPERS_H__
#define NET_HELPERS_H__

// URL
// members are NULL-terminated strings or NULL;
// prot://node:service
//
//  TODO: in general a URL is:
//   scheme://domain:port/path?query_string#fragment_id
struct url {
	char *prot;
	char *node;
	char *serv;
};

// returns 0 of no error
//
// valid URLs:
//  {udp,tcp}://147.102.3.1:1234
//  {udp,tcp}://*:1234
//  147.102.3.1:1234
//  *:1234
int url_parse(struct url *url, const char *url_str);
// returns url or null
struct url *url_alloc_parse(const char *url_str);
void url_free(struct url *url);
void url_free_fields(struct url *url); // for struct url's allocated in the stack

// Uses getaddrinfo().
// Returns NULL if getaddrinfo() failed
// Return addrinfo can be freed with freeaddrinfo().
struct addrinfo *url_getaddrinfo(struct url *url, bool srv);

// bind an addrinfo, returns fd
// if @addr_ptr is set, it places the addrinfo list element used to bind
int
ai_bind(struct addrinfo *addr, struct addrinfo **addr_ptr);

// connect to an addrinfo, return fd
// if @addr_ptr is set, it places the addrinfo list element used to connect
// returns -1 if error
int
ai_connect(struct addrinfo *addr, struct addrinfo **addr_ptr);

#endif /* NET_HELPERS_H__ */

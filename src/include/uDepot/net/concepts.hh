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


#ifndef _UDEPOT_NET_CONCEPTS_HH_
#define _UDEPOT_NET_CONCEPTS_HH_

// Playing around with C++ concepts.
// Requires a recent gcc (6.1?) and -fconcepts

#if defined(__cpp_concepts)

#include "kv-mbuff.hh"
#include "kv-concepts.hh"

template<typename T, typename... Args>
concept bool
Constructible() {
	return std::is_constructible<T, Args...>::value;
}

template<typename T>
concept bool Config() {
	return requires(T a) {
		// has a default constructor
		requires Constructible<T>();
		// set from a string afetr default construction (returns error or 0)
		{ a.from_string(std::string("<configuration string>")) } -> int;
		// has a static method parse_format_help() that returns a help string,
		// about the format.
		{ T::parse_format_help() } -> std::string;
	};
}

template<typename T>
concept bool StartStop() {
	return requires(T a) {
		// start and stop functions to start/stop a subsystem
		{ a.start() } -> int;
		{ a.stop()  } -> int;
	};
}

template<typename T, typename ID>
concept bool GetServerID() {
	return requires(T a, ID id) {
		{ a.get_server_id() } -> ID;
		{ id.is_valid()   } -> bool;
	};
}

template<typename T>
concept bool RemoteKV() {
	return requires(T a, Mbuff const& key, Mbuff &val_out, Mbuff &keyval, size_t key_len) {
		{ a.remote_get(key, val_out) } -> int;
		{ a.remote_put(keyval, key_len) } -> int;
		{ a.remote_del(key) } -> int;
		{ a.nop() } -> int;
	};
}


template<typename T>
concept bool Net() {
	return requires(T a) {

		// global and per thread, init/exit  functions for the net subsystem
		{ a.global_init() } -> int;
		{ a.thread_init() } -> int;
		{ a.thread_exit() } -> int;
		{ a.global_exit() } -> int;

		// A nested configuration type for construction
		typename T::Conf;
		requires Constructible<T, typename T::Conf>();

		// Client class: for connecting to servers
		typename T::Client;
		typename T::Client::Conf;
		requires Config<typename T::Client::Conf>();
		requires Constructible<typename T::Client, T&, udepot::MbuffAllocIface &, typename T::Client::Conf>();
		requires StartStop<typename T::Client>();
		requires RemoteKV<typename T::Client>();

		// Server class: for accepting client requests
		typename T::Server;
		typename T::Server::Conf;
		//requires Config<T::Server::Conf>();
		requires Constructible<typename T::Server, T&, KV_MbuffInterface &, typename T::Server::Conf>();
		requires StartStop<typename T::Server>();

		// TODO: explain
		typename T::ServerID;
		requires GetServerID<typename T::Server, typename T::ServerID>();
		requires GetServerID<typename T::Server::Conf, typename T::ServerID>();
		requires GetServerID<typename T::Client::Conf, typename T::ServerID>();
	};
}

#endif

template<typename T>
#if defined(__cpp_concepts)
requires Net<T>()
#endif
constexpr bool NetCheck() {return true;}


#endif /* ifndef _UDEPOT_NET_CONCEPTS_HH_ */

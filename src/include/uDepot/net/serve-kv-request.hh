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

#ifndef UDEPOT_NET_SERVE_KV_REQUEST_H__
#define UDEPOT_NET_SERVE_KV_REQUEST_H__

#include "uDepot/kv-wire.hh"
#include "kv-mbuff.hh"
#include "uDepot/net/connection.hh"

namespace udepot {

// Serve a KV request
// Returns:
//   0   -> if request was served normally
// error -> if there was an error.
//
// If the client closed the connection (e.g., recv() returned 0)
// ECONNRESET is returned so that the caller can close the socket, etc.
int serve_kv_request(ConnectionBase &cli,
                     KV_MbuffInterface &kv,
                     Mbuff &req_body, Mbuff &result);

} // end namespace udepot

#endif /* ifndef UDEPOT_NET_SERVE_KV_REQUEST_H__ */

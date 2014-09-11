/*
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 */

#ifndef NETMAP_HH_
#define NETMAP_HH_

#include <memory>
#include "net.hh"
#include "core/sstring.hh"

std::unique_ptr<net::device> create_netmap_net_device(sstring netmap_device);

#endif /* NETMAP_HH_ */

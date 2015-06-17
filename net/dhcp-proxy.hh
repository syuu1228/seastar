/*
 * This file is open source software, licensed to you under the terms
 * of the Apache License, Version 2.0 (the "License").  See the NOTICE file
 * distributed with this work for additional information regarding copyright
 * ownership.  You may not use this file except in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
/*
 * Copyright 2014 Cloudius Systems
 */

#ifndef NET_DHCP_PROXY_HH_
#define NET_DHCP_PROXY_HH_

#include "dhcp.hh"
#include "core/reactor.hh"

namespace net {

/*
 * Simplistic DHCP query class.
 * Due to the nature of the native stack,
 * it operates on an "ipv4" object instead of,
 * for example, an interface.
 */
class dhcp_proxy {
public:
    dhcp_proxy(ipv4 &);
    dhcp_proxy(dhcp_proxy &&);
    ~dhcp_proxy();

    void set_lease(const dhcp::lease);
    ip_packet_filter* get_ipv4_filter();
private:
    class impl;
    std::unique_ptr<impl> _impl;
};

}

#endif /* NET_DHCP_PROXY_HH_ */

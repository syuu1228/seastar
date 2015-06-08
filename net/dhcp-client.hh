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

#ifndef NET_DHCP_CLIENT_HH_
#define NET_DHCP_CLIENT_HH_

#include "dhcp.hh"
#include "core/reactor.hh"

namespace net {

/*
 * Simplistic DHCP query class.
 * Due to the nature of the native stack,
 * it operates on an "ipv4" object instead of,
 * for example, an interface.
 */
class dhcp_client {
public:
    dhcp_client(ipv4 &);
    dhcp_client(dhcp_client &&);
    ~dhcp_client();

    static const clock_type::duration default_timeout;

    typedef future<bool, dhcp::lease> result_type;

    /**
     * Runs a discover/request sequence on the ipv4 "stack".
     * During this execution the ipv4 will be "hijacked"
     * more or less (through packet filter), and while not
     * inoperable, most likely quite less efficient.
     *
     * Please note that this does _not_ modify the ipv4 object bound.
     * It only makes queries and records replys for the related NIC.
     * It is up to caller to use the returned information as he se fit.
     */
    result_type discover(const clock_type::duration & = default_timeout);
    result_type renew(const dhcp::lease &, const clock_type::duration & = default_timeout);
    ip_packet_filter* get_ipv4_filter();
private:
    class impl;
    std::unique_ptr<impl> _impl;
};

}

#endif /* NET_DHCP_CLIENT_HH_ */

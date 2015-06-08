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
 * Copyright 2015 Cloudius Systems
 */

#ifndef NET_DHCP_HH_
#define NET_DHCP_HH_

#include "ip.hh"

namespace net {

namespace dhcp {
    struct lease {
        ipv4_address ip;
        ipv4_address netmask;
        ipv4_address broadcast;

        ipv4_address gateway;
        ipv4_address dhcp_server;

        std::vector<ipv4_address> name_servers;

        std::chrono::seconds lease_time;
        std::chrono::seconds renew_time;
        std::chrono::seconds rebind_time;

        uint16_t mtu = 0;
    };
}

}

#endif /* NET_DHCP_HH_ */

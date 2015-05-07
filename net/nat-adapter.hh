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
 * Copyright (C) 2015 Cloudius Systems, Ltd.
 *
 */

#ifndef NAT_ADAPTER_HH_
#define NAT_ADAPTER_HH_

#include "net.hh"
#include <boost/program_options.hpp>

namespace net {

struct ipv4_address;
struct ip_hdr;
struct tcp_hdr;
struct udp_hdr;

void create_nat_adapter_device(boost::program_options::variables_map opts, std::shared_ptr<device> seastar_dev);
void create_nat_adapter(boost::program_options::variables_map opts, std::shared_ptr<device> nat_adapter_dev, std::shared_ptr<device> seastar_dev);

enum class tcp_state : uint16_t;
struct nat_tcp_connection {
public:
    const uint16_t orig_port;
    const uint16_t remap_port;
    tcp_state state;
    uint32_t local_fin_seq;
    explicit nat_tcp_connection(const uint16_t o, const uint16_t r);
};

// rx/receive means the packet received from tap, Linux kernel *sending* packet to SeaStar.
// tx/send means the packet sent to tap, Linux kernel *receving* packet from SeaStar.
// Since we are proxying interfaces, sound's like opposite but it's correct.
class nat_adapter_interface {
    std::shared_ptr<device> _nat_adapter_dev;
    std::shared_ptr<device> _seastar_dev;
    subscription<packet> _rx;
    circular_buffer<packet> _txq;
    circular_buffer<packet> _rxq;
    std::random_device _rd;
    std::default_random_engine _e;
    std::uniform_int_distribution<uint16_t> _port_dist{41952, 65535};
    std::unordered_map<uint16_t, bool> _tcp_listening;
    std::unordered_map<uint16_t, lw_shared_ptr<nat_tcp_connection>> _tcp_nat_orig;
    std::unordered_map<uint16_t, lw_shared_ptr<nat_tcp_connection>> _tcp_nat_remap;
private:
    void recalc_tcp_checksum(packet &p, ipv4_address &src_ip, ipv4_address &dst_ip, tcp_hdr *th, unsigned offset);
    void close_tcp_connection(lw_shared_ptr<nat_tcp_connection> &con);
    future<> receive(packet p);
    void receive_tcp(packet &p, ipv4_address &src_ip, ipv4_address &dst_ip, unsigned offset);
    void send_tcp(packet &p, ipv4_address &src_ip, ipv4_address &dst_ip, unsigned offset);
public:
    explicit nat_adapter_interface(std::shared_ptr<device> nat_adapter_dev, std::shared_ptr<device> seastar_dev);
    void send(packet p);
    void send(packet p, eth_hdr eh);
    void send(packet p, eth_hdr eh, ip_hdr iph);
    void send(packet p, eth_hdr eh, ip_hdr iph, udp_hdr uh);
    void register_tcp_connection(const uint16_t port);
    void unregister_tcp_connection(const uint16_t port);
};

class nat_adapter {
private:
    nat_adapter_interface _netif;
    std::string _name;
    void down();
    void up();
public:
    static thread_local promise<lw_shared_ptr<nat_adapter>> ready_promise;
    static future<lw_shared_ptr<nat_adapter>> create(boost::program_options::variables_map opts, std::shared_ptr<device> seastar_dev);
    explicit nat_adapter(boost::program_options::variables_map opts, std::shared_ptr<device> nat_adapter_dev, std::shared_ptr<device> seastar_dev);
    void set_hw_address(ethernet_address addr);
    void send(packet p);
    void send(packet p, eth_hdr eh);
    void send(packet p, eth_hdr eh, ip_hdr iph);
    void send(packet p, eth_hdr eh, ip_hdr iph, udp_hdr uh);
    void register_tcp_connection(const uint16_t port) { _netif.register_tcp_connection(port); }
    void unregister_tcp_connection(const uint16_t port) { _netif.unregister_tcp_connection(port); }
};

}

#endif /* NAT_ADAPTER_HH_ */

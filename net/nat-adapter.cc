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
 */

#include <string>
#include <memory>
#include "core/future.hh"
#include "nat-adapter.hh"
#include "native-stack.hh"
#include "dpdk.hh"
#include "virtio.hh"
#include "proxy.hh"
#include "ip.hh"
#include "tcp.hh"
#include <sys/ioctl.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <net/route.h>

namespace net {

void create_nat_adapter_device(boost::program_options::variables_map opts, std::shared_ptr<device> seastar_dev)
{
     create_device(opts, false).then([opts, seastar_dev = std::move(seastar_dev)] (std::shared_ptr<device> nat_adapter_dev) {
        for (unsigned i = 0; i < smp::count; i++) {
            smp::submit_to(i, [opts, nat_adapter_dev, seastar_dev] {
                create_nat_adapter(opts, nat_adapter_dev, seastar_dev);
            });
        }
    });
}

void create_nat_adapter(boost::program_options::variables_map opts, std::shared_ptr<device> nat_adapter_dev, std::shared_ptr<device> seastar_dev) {
    nat_adapter::ready_promise.set_value(lw_shared_ptr<nat_adapter>(make_lw_shared<nat_adapter>(opts, nat_adapter_dev, seastar_dev)));
}

nat_tcp_connection::nat_tcp_connection(const uint16_t o, const uint16_t r) : orig_port(o), remap_port(r), state(tcp_state::ESTABLISHED) {}

nat_adapter_interface::nat_adapter_interface(std::shared_ptr<device> nat_adapter_dev, std::shared_ptr<device> seastar_dev)
    : _nat_adapter_dev(nat_adapter_dev)
    , _seastar_dev(seastar_dev)
    , _rx(_nat_adapter_dev->receive([this] (packet p) { return receive(std::move(p)); }))
{
    // Received from DPDK interface, forward to TAP interface
    nat_adapter_dev->local_queue().register_packet_provider([this] () mutable {
            std::experimental::optional<packet> p;
            if (!_txq.empty()) {
                p = std::move(_txq.front());
                _txq.pop_front();
            }
            return p;
        });
    // Received from TAP interface, forward to DPDK interface
    seastar_dev->local_queue().register_packet_provider([this] () mutable {
            std::experimental::optional<packet> p;
            if (!_rxq.empty()) {
                p = std::move(_rxq.front());
                _rxq.pop_front();
            }
            return p;
        });
}

void nat_adapter_interface::recalc_tcp_checksum(packet &p, ipv4_address &src_ip, ipv4_address &dst_ip, tcp_hdr *th, unsigned offset) {
    checksummer csum;
    auto p_th = p.share(offset, p.len() - offset);

    th->checksum = 0;
    ipv4_traits::tcp_pseudo_header_checksum(csum, src_ip, dst_ip, p_th.len());
    csum.sum(p_th);
    th->checksum = csum.get();
}

void nat_adapter_interface::close_tcp_connection(lw_shared_ptr<nat_tcp_connection> &con) {
    _tcp_listening.erase(con->remap_port);
    _tcp_nat_orig.erase(con->orig_port);
    _tcp_nat_remap.erase(con->remap_port);
}

future<> nat_adapter_interface::receive(packet p) {
    auto eh = p.get_header<eth_hdr>(0);
    if (eh) {
        auto ceh = ntoh(*eh);
        if (ceh.eth_proto == uint16_t(eth_protocol_num::ipv4)) {
            auto iph = p.get_header<ip_hdr>(sizeof(eth_hdr));
            if (iph) {
                auto ciph = ntoh(*iph);
                unsigned ip_hdr_len = ciph.ihl * 4;
                switch (ciph.ip_proto) {
                case uint8_t(ip_protocol_num::tcp):
                    receive_tcp(p, ciph.src_ip, ciph.dst_ip, sizeof(eth_hdr) + ip_hdr_len);
                    break;
                }
            }
        }
    }
    _rxq.push_back(std::move(p));
    return make_ready_future<>();
}

void nat_adapter_interface::receive_tcp(packet &p, ipv4_address &src_ip, ipv4_address &dst_ip, unsigned offset)
{
    auto th = p.get_header<tcp_hdr>(offset);
    if (th) {
        auto cth = ntoh(*th);
        // When the host initiates connection, we need to make sure local port is not conflict with SeaStar.
        // If it's conflicted, we need to remap.
        if (cth.f_syn && !cth.f_ack && _tcp_listening.count(cth.src_port)) {
            uint16_t remap_port;
            l4connid<ipv4_traits> id;
            do {
                remap_port = _port_dist(_e);
                id = l4connid<ipv4_traits>{src_ip, dst_ip, remap_port, cth.dst_port};
            } while (_seastar_dev->hash2cpu(id.hash()) != engine().cpu_id()
                     || _tcp_listening.count(remap_port));
            auto con = make_lw_shared<nat_tcp_connection>(cth.src_port, remap_port);
            _tcp_nat_orig[cth.src_port] = _tcp_nat_remap[remap_port] = con;
            _tcp_listening[remap_port] = true;
        }
        // If this flow is remmaped, modify packet header.
        if (_tcp_nat_orig.count(cth.src_port)) {
            auto &con = _tcp_nat_orig[cth.src_port];
            th->src_port = hton(con->remap_port);
            recalc_tcp_checksum(p, src_ip, dst_ip, th, offset);

            if (cth.f_rst) {
                close_tcp_connection(con);
            }
            // Track state to detect connection closing.
            if (cth.f_fin) {
                if (con->state == tcp_state::ESTABLISHED) {
                    con->local_fin_seq = tcp_seq(cth.seq).raw;
                    con->state = tcp_state::FIN_WAIT_1;
                }
                if (con->state == tcp_state::CLOSE_WAIT) {
                    con->local_fin_seq = tcp_seq(cth.seq).raw;
                    con->state = tcp_state::LAST_ACK;
                }
            }
        }
    }
}

void nat_adapter_interface::send(packet p) {
    auto eh = p.get_header<eth_hdr>(0);
    if (eh) {
        auto ceh = ntoh(*eh);
        if (ceh.eth_proto == uint16_t(eth_protocol_num::ipv4)) {
            auto iph = p.get_header<ip_hdr>(sizeof(eth_hdr));
            if (iph) {
                auto ciph = ntoh(*iph);
                unsigned ip_hdr_len = ciph.ihl * 4;
                switch (ciph.ip_proto) {
                case uint8_t(ip_protocol_num::tcp):
                    send_tcp(p, ciph.src_ip, ciph.dst_ip, sizeof(eth_hdr) + ip_hdr_len);
                    break;
                }
            }
        }
    }

    _txq.push_back(std::move(p));

}

void nat_adapter_interface::send(packet p, eth_hdr eh) {
    auto eh1 = p.prepend_header<eth_hdr>();
    *eh1 = hton(eh);
    send(std::move(p));
}

void nat_adapter_interface::send(packet p, eth_hdr eh, ip_hdr iph) {
    auto iph1 = p.prepend_header<ip_hdr>();
    *iph1 = hton(iph);
    send(std::move(p), std::move(eh));
}

void nat_adapter_interface::send(packet p, eth_hdr eh, ip_hdr iph, udp_hdr uh) {
    auto uh1 = p.prepend_header<udp_hdr>();
    *uh1 = hton(uh);
    send(std::move(p), std::move(eh), std::move(iph));
}

void nat_adapter_interface::send_tcp(packet &p, ipv4_address &src_ip, ipv4_address &dst_ip, unsigned offset) {
    auto th = p.get_header<tcp_hdr>(offset);
    if (th) {
        auto cth = ntoh(*th);
        // If this flow is remmaped, modify packet header.
        if (_tcp_nat_remap.count(cth.dst_port)) {
            auto &con = _tcp_nat_remap[cth.dst_port];
            th->dst_port = hton(con->orig_port);
            recalc_tcp_checksum(p, src_ip, dst_ip, th, offset);

            if (cth.f_rst) {
                close_tcp_connection(con);
            }
            // Track state to detect connection closing.
            if (cth.f_ack) {
                if (con->state == tcp_state::FIN_WAIT_1
                    && cth.ack == make_seq(con->local_fin_seq + 1)) {
                    con->state = tcp_state::FIN_WAIT_2;
                }
                if (con->state == tcp_state::LAST_ACK
                    && cth.ack == make_seq(con->local_fin_seq + 1)) {
                    close_tcp_connection(con);
                }
            }
            if (cth.f_fin) {
                if (con->state == tcp_state::ESTABLISHED) {
                    con->state = tcp_state::CLOSE_WAIT;
                }
                if (con->state == tcp_state::FIN_WAIT_2) {
                    close_tcp_connection(con);
                }
            }
        }
    }
}

void nat_adapter_interface::register_tcp_connection(const uint16_t port) {
    if (port >= 41952) {
        _tcp_listening[port] = true;
    }
}

void nat_adapter_interface::unregister_tcp_connection(const uint16_t port) {
    if (port >= 41952) {
        _tcp_listening.erase(port);
    }
}

thread_local promise<lw_shared_ptr<nat_adapter>> nat_adapter::ready_promise;

future<lw_shared_ptr<nat_adapter>> nat_adapter::create(boost::program_options::variables_map opts, std::shared_ptr<device> seastar_dev) {
    if (engine().cpu_id() == 0) {
            create_nat_adapter_device(opts, seastar_dev);
    }
    return ready_promise.get_future();
}

nat_adapter::nat_adapter(boost::program_options::variables_map opts, std::shared_ptr<device> nat_adapter_dev, std::shared_ptr<device> seastar_dev)
    : _netif(nat_adapter_dev, seastar_dev),
    _name(opts["tap-device"].as<std::string>()) {}

void nat_adapter::down()
{
    ifreq ifr = {};
    auto fd = file_desc::socket(AF_INET, SOCK_STREAM, 0);
    strcpy(ifr.ifr_ifrn.ifrn_name, _name.c_str());
    fd.ioctl(SIOCGIFFLAGS, ifr);
    ifr.ifr_flags &= ~IFF_UP;
    fd.ioctl(SIOCSIFFLAGS, ifr);
}

void nat_adapter::up()
{
    ifreq ifr = {};
    auto fd = file_desc::socket(AF_INET, SOCK_STREAM, 0);
    strcpy(ifr.ifr_ifrn.ifrn_name, _name.c_str());
    fd.ioctl(SIOCGIFFLAGS, ifr);
    ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
    fd.ioctl(SIOCSIFFLAGS, ifr);
}

void nat_adapter::set_hw_address(ethernet_address addr)
{
    down();
    ifreq ifr = {};
    auto fd = file_desc::socket(AF_INET, SOCK_STREAM, 0);
    strcpy(ifr.ifr_ifrn.ifrn_name, _name.c_str());
    ifr.ifr_hwaddr.sa_family = ARPHRD_ETHER;
    for (int i = 0; i < 6; i++)
        ifr.ifr_hwaddr.sa_data[i] = addr.mac[i];
    fd.ioctl(SIOCSIFHWADDR, ifr);
    up();
}

void nat_adapter::send(packet p)
{
    _netif.send(std::move(p));
}

void nat_adapter::send(packet p, eth_hdr eh)
{
    _netif.send(std::move(p), std::move(eh));
}

void nat_adapter::send(packet p, eth_hdr eh, ip_hdr iph)
{
    _netif.send(std::move(p), std::move(eh), std::move(iph));
}

void nat_adapter::send(packet p, eth_hdr eh, ip_hdr iph, udp_hdr uh)
{
    _netif.send(std::move(p), std::move(eh), std::move(iph), std::move(uh));
}

}

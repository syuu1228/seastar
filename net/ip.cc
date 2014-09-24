/*
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 *
 */

#include "ip.hh"
#include "core/print.hh"

namespace net {

std::ostream& operator<<(std::ostream& os, ipv4_address a) {
    auto ip = a.ip;
    return fprint(os, "%d.%d.%d.%d",
            (ip >> 24) & 0xff,
            (ip >> 16) & 0xff,
            (ip >> 8) & 0xff,
            (ip >> 0) & 0xff);
}

ipv4::ipv4(interface* netif)
    : _netif(netif)
    , _global_arp(netif)
    , _arp(_global_arp)
    , _l3(netif, 0x0800)
    , _rx_packets(_l3.receive([this] (packet p, ethernet_address ea) {
        return handle_received_packet(std::move(p), ea); }))
    , _tcp(*this)
    , _l4({ { 6, &_tcp } }) {
}

bool ipv4::in_my_netmask(ipv4_address a) const {
    return !((a.ip ^ _host_address.ip) & _netmask.ip);
}


future<>
ipv4::handle_received_packet(packet p, ethernet_address from) {
    auto iph = p.get_header<ip_hdr>(0);
    if (!iph) {
        return make_ready_future<>();
    }
    if (!hw_features().rx_csum_offload) {
        checksummer csum;
        csum.sum(reinterpret_cast<char*>(iph), sizeof(*iph));
        if (csum.get() != 0) {
            return make_ready_future<>();
        }
    }
    ntoh(*iph);
    // FIXME: process options
    if (in_my_netmask(iph->src_ip) && iph->src_ip != _host_address) {
        _arp.learn(from, iph->src_ip);
    }
    if (iph->frag & 0x3fff) {
        // FIXME: defragment
        return make_ready_future<>();
    }
    if (iph->dst_ip != _host_address) {
        // FIXME: forward
        return make_ready_future<>();
    }
    auto l4 = _l4[iph->ip_proto];
    if (l4) {
        p.trim_front(iph->ihl * 4);
        l4->received(std::move(p), iph->src_ip, iph->dst_ip);
    }
    return make_ready_future<>();
}

void ipv4::send(ipv4_address to, uint8_t proto_num, packet p) {
    // FIXME: fragment
    auto iph = p.prepend_header<ip_hdr>();
    iph->ihl = sizeof(*iph) / 4;
    iph->ver = 4;
    iph->dscp = 0;
    iph->ecn = 0;
    iph->len = p.len();
    iph->id = 0;
    iph->frag = 0;
    iph->ttl = 64;
    iph->ip_proto = proto_num;
    iph->csum = 0;
    iph->src_ip = _host_address;
    // FIXME: routing
    auto gw = to;
    iph->dst_ip = to;
    hton(*iph);
    checksummer csum;
    csum.sum(reinterpret_cast<char*>(iph), sizeof(*iph));
    iph->csum = csum.get();
    _arp.lookup(gw).then([this, p = std::move(p)] (ethernet_address e_dst) mutable {
        _send_sem.wait().then([this, e_dst, p = std::move(p)] () mutable {
            return _l3.send(e_dst, std::move(p));
        }).then([this] {
            _send_sem.signal();
        });
    });
}

void ipv4::set_host_address(ipv4_address ip) {
    _host_address = ip;
    _arp.set_self_addr(ip);
}

void ipv4::register_l4(ipv4::proto_type id, ip_protocol *protocol) {
    _l4.at(id) = protocol;
}

}

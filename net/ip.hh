/*
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 *
 */

#ifndef IP_HH_
#define IP_HH_

#include <boost/asio/ip/address_v4.hpp>
#include <arpa/inet.h>
#include <cstdint>
#include <array>
#include "core/array_map.hh"
#include "byteorder.hh"
#include "arp.hh"
#include "ip_checksum.hh"
#include "const.hh"

namespace net {

class ipv4;
template <ip_protocol_num ProtoNum>
class ipv4_l4;
struct ipv4_address;

template <typename InetTraits>
class tcp;

struct ipv4_address {
    ipv4_address() : ip(0) {}
    explicit ipv4_address(uint32_t ip) : ip(ip) {}
    explicit ipv4_address(const std::string& addr) {
        ip = static_cast<uint32_t>(boost::asio::ip::address_v4::from_string(addr).to_ulong());
    }
    ipv4_address(ipv4_addr addr) {
        ip = addr.ip;
    }

    packed<uint32_t> ip;

    template <typename Adjuster>
    auto adjust_endianness(Adjuster a) { return a(ip); }

    friend bool operator==(ipv4_address x, ipv4_address y) {
        return x.ip == y.ip;
    }
    friend bool operator!=(ipv4_address x, ipv4_address y) {
        return x.ip != y.ip;
    }
} __attribute__((packed));

static inline bool is_unspecified(ipv4_address addr) { return addr.ip == 0; }

std::ostream& operator<<(std::ostream& os, ipv4_address a);

}

namespace std {

template <>
struct hash<net::ipv4_address> {
    size_t operator()(net::ipv4_address a) const { return a.ip; }
};

}

namespace net {

struct ipv4_traits {
    using address_type = ipv4_address;
    using inet_type = ipv4_l4<ip_protocol_num::tcp>;
    static void tcp_pseudo_header_checksum(checksummer& csum, ipv4_address src, ipv4_address dst, uint16_t len) {
        csum.sum_many(src.ip.raw, dst.ip.raw, uint8_t(0), uint8_t(ip_protocol_num::tcp), len);
    }
    static void udp_pseudo_header_checksum(checksummer& csum, ipv4_address src, ipv4_address dst, uint16_t len) {
        csum.sum_many(src.ip.raw, dst.ip.raw, uint8_t(0), uint8_t(ip_protocol_num::udp), len);
    }
    static constexpr uint8_t ip_hdr_len_min = net::ipv4_hdr_len_min;
};

template <ip_protocol_num ProtoNum>
class ipv4_l4 {
public:
    ipv4& _inet;
public:
    ipv4_l4(ipv4& inet) : _inet(inet) {}
    future<> send(ipv4_address from, ipv4_address to, packet p);
};

class ip_protocol {
public:
    virtual ~ip_protocol() {}
    virtual void received(packet p, ipv4_address from, ipv4_address to) = 0;
    virtual unsigned forward(packet& p, size_t off, ipv4_address from, ipv4_address to) { return engine.cpu_id(); }
};

class ipv4_tcp final : public ip_protocol {
    ipv4_l4<ip_protocol_num::tcp> _inet_l4;
    std::unique_ptr<tcp<ipv4_traits>> _tcp;
public:
    ipv4_tcp(ipv4& inet);
    ~ipv4_tcp();
    virtual void received(packet p, ipv4_address from, ipv4_address to);
    virtual unsigned forward(packet& p, size_t off, ipv4_address from, ipv4_address to) override;
    friend class ipv4;
};

struct icmp_hdr {
    enum class msg_type : uint8_t {
        echo_reply = 0,
        echo_request = 8,
    };
    msg_type type;
    uint8_t code;
    packed<uint16_t> csum;
    packed<uint32_t> rest;
    template <typename Adjuster>
    auto adjust_endianness(Adjuster a) {
        return a(csum);
    }
} __attribute__((packed));


class icmp {
public:
    using ipaddr = ipv4_address;
    using inet_type = ipv4_l4<ip_protocol_num::icmp>;
    explicit icmp(inet_type& inet) : _inet(inet) {}
    void received(packet p, ipaddr from, ipaddr to);
private:
    inet_type& _inet;
};

class ipv4_icmp final : public ip_protocol {
    ipv4_l4<ip_protocol_num::icmp> _inet_l4;
    icmp _icmp;
public:
    ipv4_icmp(ipv4& inet) : _inet_l4(inet), _icmp(_inet_l4) {}
    virtual void received(packet p, ipv4_address from, ipv4_address to) {
        _icmp.received(std::move(p), from, to);
    }
    friend class ipv4;
};

struct ip_hdr;

struct ip_packet_filter {
    virtual ~ip_packet_filter() {};
    virtual future<> handle(packet& p, ip_hdr* iph, ethernet_address from, bool & handled) = 0;
    virtual unsigned forward(packet& p, size_t off, ipv4_address from, ipv4_address to, bool & handled) {
        return engine.cpu_id();
    }
};

class ipv4 {
public:
    using address_type = ipv4_address;
    using proto_type = uint16_t;
    static address_type broadcast_address() { return ipv4_address(0xffffffff); }
    static proto_type arp_protocol_type() { return proto_type(eth_protocol_num::ipv4); }
private:
    interface* _netif;
    arp _global_arp;
    arp_for<ipv4> _arp;
    ipv4_address _host_address;
    ipv4_address _gw_address;
    ipv4_address _netmask;
    l3_protocol _l3;
    subscription<packet, ethernet_address> _rx_packets;
    ipv4_tcp _tcp;
    ipv4_icmp _icmp;
    array_map<ip_protocol*, 256> _l4;
    ip_packet_filter * _packet_filter = nullptr;
private:
    future<> handle_received_packet(packet p, ethernet_address from);
    unsigned handle_on_cpu(packet& p, size_t off);
    bool in_my_netmask(ipv4_address a) const;
public:
    explicit ipv4(interface* netif);
    void set_host_address(ipv4_address ip);
    ipv4_address host_address();
    void set_gw_address(ipv4_address ip);
    ipv4_address gw_address() const;
    void set_netmask_address(ipv4_address ip);
    ipv4_address netmask_address() const;
    interface * netif() const {
        return _netif;
    }
    // TODO or something. Should perhaps truly be a list
    // of filters. With ordering. And blackjack. Etc.
    // But for now, a simple single raw pointer suffices
    void set_packet_filter(ip_packet_filter *);
    ip_packet_filter * packet_filter() const;
    future<> send(ipv4_address to, ip_protocol_num proto_num, packet p);
    future<> send_raw(ethernet_address, packet);
    tcp<ipv4_traits>& get_tcp() { return *_tcp._tcp; }
    void register_l4(proto_type id, ip_protocol* handler);
    net::hw_features hw_features() { return _netif->hw_features(); }
    static bool needs_frag(packet& p, ip_protocol_num proto_num, net::hw_features hw_features);
};

template <ip_protocol_num ProtoNum>
inline
future<> ipv4_l4<ProtoNum>::send(ipv4_address from, ipv4_address to, packet p) {
    return _inet.send(/* from, */ to, ProtoNum, std::move(p));
}

struct ip_hdr {
    uint8_t ihl : 4;
    uint8_t ver : 4;
    uint8_t dscp : 6;
    uint8_t ecn : 2;
    packed<uint16_t> len;
    packed<uint16_t> id;
    packed<uint16_t> frag;
    enum class frag_bits : uint8_t { mf = 13, df = 14, reserved = 15, offset_shift = 3 };
    uint8_t ttl;
    uint8_t ip_proto;
    packed<uint16_t> csum;
    ipv4_address src_ip;
    ipv4_address dst_ip;
    uint8_t options[0];
    template <typename Adjuster>
    auto adjust_endianness(Adjuster a) {
        return a(len, id, frag, csum, src_ip, dst_ip);
    }
    bool mf() { return frag & (1 << uint8_t(frag_bits::mf)); }
    bool df() { return frag & (1 << uint8_t(frag_bits::df)); }
    uint16_t offset() { return frag << uint8_t(frag_bits::offset_shift); }
} __attribute__((packed));

template <typename InetTraits>
struct l4connid {
    using ipaddr = typename InetTraits::address_type;
    using inet_type = typename InetTraits::inet_type;
    struct connid_hash;

    ipaddr local_ip;
    ipaddr foreign_ip;
    uint16_t local_port;
    uint16_t foreign_port;

    bool operator==(const l4connid& x) const {
        return local_ip == x.local_ip
                && foreign_ip == x.foreign_ip
                && local_port == x.local_port
                && foreign_port == x.foreign_port;
    }
};

template <typename InetTraits>
struct l4connid<InetTraits>::connid_hash : private std::hash<ipaddr>, private std::hash<uint16_t> {
    size_t operator()(const l4connid<InetTraits>& id) const noexcept {
        using h1 = std::hash<ipaddr>;
        using h2 = std::hash<uint16_t>;
        return h1::operator()(id.local_ip)
            ^ h1::operator()(id.foreign_ip)
            ^ h2::operator()(id.local_port)
            ^ h2::operator()(id.foreign_port);
    }
};

}

#endif /* IP_HH_ */

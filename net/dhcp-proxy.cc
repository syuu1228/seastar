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

#include <chrono>
#include <unordered_map>
#include <array>
#include <random>

#include "dhcp-proxy.hh"
#include "ip.hh"
#include "udp.hh"

using namespace std::literals::chrono_literals;
using namespace net::dhcp;

class net::dhcp_proxy::impl : public ip_packet_filter {
public:

    decltype(std::cout) & log() {
        return std::cout << "DHCP ";
    }

    enum class state {
        NONE,
        DISCOVER,
        REQUEST,
        DONE,
        FAIL,
    };

    enum class m_type : uint8_t {
        BOOTREQUEST = 1,
        BOOTREPLY = 2
    };

    enum class htype : uint8_t {
        ETHERNET = 1
    };

    enum class opt_type : uint8_t {
        PAD = 0,
        SUBNET_MASK = 1,
        ROUTER = 3,
        DOMAIN_NAME_SERVERS = 6,
        INTERFACE_MTU = 26,
        BROADCAST_ADDRESS = 28,
        REQUESTED_ADDRESS = 50,
        LEASE_TIME = 51,
        MESSAGE_TYPE = 53,
        DHCP_SERVER = 54,
        PARAMETER_REQUEST_LIST = 55,
        RENEWAL_TIME = 58,
        REBINDING_TIME = 59,
        CLASSLESS_ROUTE = 121,
        END = 255
    };

    enum class msg_type : uint8_t {
        DISCOVER = 1,
        OFFER = 2,
        REQUEST = 3,
        DECLINE = 4,
        ACK = 5,
        NAK = 6,
        RELEASE = 7,
        INFORM = 8,
        LEASEQUERY = 10,
        LEASEUNASSIGNED = 11,
        LEASEUNKNOWN = 12,
        LEASEACTIVE = 13,
        INVALID = 255
    };

    struct dhcp_header {
        m_type op = m_type::BOOTREQUEST; // Message op code / message type.
        htype type = htype::ETHERNET;             // Hardware address type
        uint8_t hlen = 6;           // Hardware address length
        uint8_t hops = 0;           // Client sets to zero, used by relay agents
        packed<uint32_t> xid = 0;           // Client sets Transaction ID, a random number
        packed<uint16_t> secs = 0;          // Client sets seconds elapsed since op start
        packed<uint16_t> flags = 0;         // Flags
        ipv4_address ciaddr;  // Client IP address
        ipv4_address yiaddr;  // 'your' (client) IP address.
        ipv4_address siaddr;  // IP address of next server to use in bootstrap
        ipv4_address giaddr;  // Relay agent IP address
        uint8_t chaddr[16] = { 0, };     // Client hardware address.
        char sname[64] = { 0, };         // unused
        char file[128] = { 0, };         // unused

        template <typename Adjuster>
        auto adjust_endianness(Adjuster a) {
            return a(xid, secs, flags, ciaddr, yiaddr, siaddr, giaddr);
        }
    } __attribute__((packed));

    typedef std::array<opt_type, 5> req_opt_type;

    static const req_opt_type requested_options;

    struct option_mark {
        option_mark(opt_type t = opt_type::END) : type(t) {};
        opt_type type;
    } __attribute__((packed));

    struct option : public option_mark {
        option(opt_type t, uint8_t l = 1) : option_mark(t), len(l) {};
        uint8_t len;
    } __attribute__((packed));

    struct type_option : public option {
        type_option(msg_type t) : option(opt_type::MESSAGE_TYPE), type(t) {}
        msg_type type;
    } __attribute__((packed));

    struct mtu_option : public option {
        mtu_option(uint16_t v) : option(opt_type::INTERFACE_MTU, 2), mtu((::htons)(v)) {}
        packed<uint16_t> mtu;
    } __attribute__((packed));

    struct ip_option : public option {
        ip_option(opt_type t = opt_type::BROADCAST_ADDRESS, const ipv4_address & ip = ipv4_address()) : option(t, sizeof(uint32_t)), ip(::htonl(ip.ip)) {}
        packed<uint32_t> ip;
    } __attribute__((packed));

    struct time_option : public option {
        time_option(opt_type t, uint32_t v) : option(t, sizeof(uint32_t)), time(::htonl(v)) {}
        packed<uint32_t> time;
    } __attribute__((packed));


    struct requested_option: public option {
        requested_option()
                : option(opt_type::PARAMETER_REQUEST_LIST,
                        uint8_t(requested_options.size())), req(
                        requested_options) {
        }
        req_opt_type req;
    }__attribute__((packed));

    static const uint16_t client_port = 68;
    static const uint16_t server_port = 67;

    typedef std::array<uint8_t, 4> magic_tag;

    static const magic_tag options_magic;

    struct dhcp_payload {
        dhcp_header bootp;
        magic_tag magic = options_magic;

        template <typename Adjuster>
        auto adjust_endianness(Adjuster a) {
            return a(bootp);
        }
    } __attribute__((packed));

    struct dhcp_packet_base {
        dhcp_payload dhp;

        template <typename Adjuster>
        auto adjust_endianness(Adjuster a) {
            return a(dhp);
        }
    } __attribute__((packed));

    struct ip_info : public lease {
        msg_type type = msg_type();

        void set(opt_type type, const ipv4_address & ip) {
            switch (type) {
            case opt_type::SUBNET_MASK: netmask = ip; break;
            case opt_type::ROUTER: gateway = ip; break;
            case opt_type::BROADCAST_ADDRESS: broadcast = ip; break;
            case opt_type::DHCP_SERVER: dhcp_server = ip; break;
            case opt_type::DOMAIN_NAME_SERVERS:
                name_servers.emplace_back(ip);
                break;
            default:
                break;
            }
        }

        void set(opt_type type, std::chrono::seconds s) {
            switch (type) {
            case opt_type::LEASE_TIME: lease_time = s; break;
            case opt_type::RENEWAL_TIME: renew_time = s; break;
            case opt_type::REBINDING_TIME: rebind_time = s; break;
            default:
                break;
            }
        }

        void parse_options(packet & p, size_t off) {
            for (;;) {
                auto * m = p.get_header<option_mark>(off);
                if (m == nullptr || m->type == opt_type::END) {
                    break;
                }
                auto * o = p.get_header<option>(off);
                if (o == nullptr) {
                    // TODO: report broken packet?
                    break;
                }

                switch (o->type) {
                case opt_type::SUBNET_MASK:
                case opt_type::ROUTER:
                case opt_type::BROADCAST_ADDRESS:
                case opt_type::DHCP_SERVER:
                case opt_type::DOMAIN_NAME_SERVERS:
                {
                    auto ipo = p.get_header<ip_option>(off);
                    if (ipo != nullptr) {
                        set(o->type, ipv4_address(::ntohl(ipo->ip)));
                    }
                }
                break;
                case opt_type::MESSAGE_TYPE:
                {
                    auto to = p.get_header<type_option>(off);
                    if (to != nullptr) {
                        type = to->type;
                    }
                }
                break;
                case opt_type::INTERFACE_MTU:
                {
                    auto mo = p.get_header<mtu_option>(off);
                    if (mo != nullptr) {
                        mtu = (::ntohs)(uint16_t(mo->mtu));
                    }
                }
                break;
                case opt_type::LEASE_TIME:
                case opt_type::RENEWAL_TIME:
                case opt_type::REBINDING_TIME:
                {
                    auto to = p.get_header<time_option>(off);
                    if (to != nullptr) {
                        set(o->type, std::chrono::seconds(::ntohl(to->time)));
                    }
                }
                break;
                default:
                    break;
                }

                off += sizeof(*o) + o->len;
            }
        }
    };

    impl(ipv4 & stack)
       : _stack(stack)
    {
        _sock = _stack.get_udp().make_channel({0, client_port});
    }

    future<> process_packet(packet p, dhcp_payload* dhp, size_t opt_off) {
        _retry_timer.cancel();

        auto h = ntoh(*dhp);

        ip_info info;

        info.ip = h.bootp.yiaddr;
        info.parse_options(p, opt_off);

        switch (info.type) {
        case msg_type::DISCOVER:
            log() << "Got discover" << std::endl;
            if (!_lease_avail)
                return make_ready_future<>();
            return send_offer(info);
        case msg_type::REQUEST:
            log() << "Got request" << std::endl;
            return send_pack(info);
        }
        return make_ready_future<>();
    }

    future<> handle(packet& p, eth_hdr& eh, ip_hdr& iph, bool & handled) override {
        if (p.len() < sizeof(dhcp_packet_base)) {
            return make_ready_future<>();
        }

        auto ipl = iph.ihl * 4;
        auto udp = p.get_header<udp_hdr>(ipl);
        auto dhp = p.get_header<dhcp_payload>(ipl + sizeof(*udp));

        const auto opt_off = ipl + sizeof(*udp) + sizeof(dhcp_payload);

        if (udp == nullptr || dhp == nullptr
                || iph.ip_proto != uint8_t(ip_protocol_num::udp)
                || (::ntohs)(udp->dst_port) != client_port
                || iph.len < (opt_off + sizeof(option_mark))
                || dhp->magic != options_magic) {
            return make_ready_future<>();
        }
        handled = true;
        auto src_cpu = engine().cpu_id();
        if (src_cpu == 0) {
            return process_packet(std::move(p), dhp, opt_off);
        }
        smp::submit_to(0, [this, p = std::move(p), src_cpu, dhp, opt_off]() mutable {
            process_packet(p.free_on_cpu(src_cpu), dhp, opt_off);
        });
        return make_ready_future<>();
    }

    template<typename T>
    future<> send(T && pkt) {
        pkt.dhp.bootp.xid = _xid;
        auto ipf = _stack.netif();
        auto mac = ipf->hw_address().mac;
        std::copy(mac.begin(), mac.end(), std::begin(pkt.dhp.bootp.chaddr));

        pkt = hton(pkt);

        _sock.send({0xffffffff, server_port}, packet(reinterpret_cast<char *>(&pkt), sizeof(pkt)));

        return make_ready_future<>();
    }

    future<> send_offer(ip_info info) {
        struct offer : public dhcp_packet_base {
            type_option type = type_option(msg_type::OFFER);
            ip_option subnet_mask;
            ip_option dhcp_server;
            ip_option router;

        } __attribute__((packed));

    }

    future<> send_discover(const ipv4_address & ip = ipv4_address()) {
        struct discover : public dhcp_packet_base {
            type_option type = type_option(msg_type::DISCOVER);
            ip_option requested_ip;
            requested_option req;
            option_mark end;
        } __attribute__((packed));

        discover d;

        d.requested_ip = ip_option(opt_type::REQUESTED_ADDRESS, ip);

        log() << "sending discover" << std::endl;

        std::random_device rd;
        std::default_random_engine e1(rd());
        std::uniform_int_distribution<uint32_t> xid_dist{};

        _xid = xid_dist(e1);
        _state = state::DISCOVER;
        return send(d);
    }

    future<> send_request(const lease & info) {
        struct request : public dhcp_packet_base {
            type_option type = type_option(msg_type::REQUEST);
            ip_option dhcp_server;
            ip_option requested_ip;
            requested_option req;
            option_mark end;
        } __attribute__((packed));

        request d;

        d.dhcp_server = ip_option(opt_type::DHCP_SERVER, info.dhcp_server);
        d.requested_ip = ip_option(opt_type::REQUESTED_ADDRESS, info.ip);

        log() << "sending request for " << info.ip << std::endl;
        _state = state::REQUEST;
        return send(d);
    }

    void set_lease(const dhcp::lease lease) {
        _lease = std::move(lease);
        _lease_avail = true;
    }

private:
    dhcp::lease _lease;
    bool _lease_avail = false;
    uint32_t _xid = 0;
};

const net::dhcp_proxy::impl::req_opt_type net::dhcp_proxy::impl::requested_options = { {
        opt_type::SUBNET_MASK, opt_type::ROUTER, opt_type::DOMAIN_NAME_SERVERS,
        opt_type::INTERFACE_MTU, opt_type::BROADCAST_ADDRESS } };

const net::dhcp_proxy::impl::magic_tag net::dhcp_proxy::impl::options_magic = { { 0x63, 0x82, 0x53,
        0x63 } };

const uint16_t net::dhcp_proxy::impl::client_port;
const uint16_t net::dhcp_proxy::impl::server_port;

net::dhcp_proxy::dhcp_proxy(ipv4 & ip)
: _impl(std::make_unique<impl>(ip))
{}

net::dhcp_proxy::dhcp_proxy(dhcp_proxy && v)
: _impl(std::move(v._impl))
{}

net::dhcp_proxy::~dhcp_proxy()
{}

void net::dhcp_proxy::set_lease(const dhcp::lease lease) {
    _impl->set_lease(std::move(lease));
}

net::ip_packet_filter* net::dhcp_proxy::get_ipv4_filter() {
    return _impl.get();
}

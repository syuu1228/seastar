/*
 * Copyright 2014 Cloudius Systems
 */

#include <chrono>
#include <unordered_map>
#include <array>
#include <random>

#include "dhcp.hh"
#include "ip.hh"
#include "udp.hh"

class net::dhcp::impl : public ip_packet_filter {
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
        ip_hdr ip;
        udp_hdr udp;

        dhcp_payload dhp;

        template <typename Adjuster>
        auto adjust_endianness(Adjuster a) {
            return a(ip, udp, dhp);
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
    {}

    future<> handle(packet& p, ethernet_address from, bool & handled) override {
        if (_state == state::NONE || p.len() < sizeof(dhcp_packet_base)) {
            return make_ready_future<>();
        }

        auto iph = p.get_header<ip_hdr>(0);
        auto ipl = iph->ihl * 4;
        auto udp = p.get_header<udp_hdr>(ipl);
        auto dhp = p.get_header<dhcp_payload>(ipl + sizeof(*udp));

        const auto opt_off = ipl + sizeof(*udp) + sizeof(dhcp_payload);

        if (udp == nullptr || dhp == nullptr
                || iph->ip_proto != uint8_t(ip_protocol_num::udp)
                || (::ntohs)(udp->dst_port) != client_port
                || iph->len < (opt_off + sizeof(option_mark))
                || dhp->magic != options_magic) {
            return make_ready_future<>();
        }

        ntoh(*dhp);

        ip_info info;

        info.ip = dhp->bootp.yiaddr;
        info.parse_options(p, opt_off);

        switch (_state) {
        case state::DISCOVER:
            if (info.type != msg_type::OFFER) {
                // TODO: log?
                break;
            }
            log() << "Got offer for " << info.ip << std::endl;
            // TODO, check for minimum valid/required fields sent back?
            handled = true;
            return send_request(info);
        case state::REQUEST:
            if (info.type == msg_type::NAK) {
                log() << "Got nak on request" << std::endl;
                _state = state::NONE;
                return send_discover();
            }
            if (info.type != msg_type::ACK) {
                break;
            }
            log() << "Got ack on request" << std::endl;
            log() << " ip: " << info.ip << std::endl;
            log() << " nm: " << info.netmask << std::endl;
            log() << " gw: " << info.gateway << std::endl;
            handled = true;
            _state = state::DONE;
            _result.set_value(true, info);
            break;
        default:
            break;
        }
        return make_ready_future<>();
    }

    future<bool, lease> run(const lease & l,
            const clock_type::duration & timeout) {

        _state = state::NONE;
        _timer.set_callback([this]() {
            _state = state::FAIL;
            log() << "timeout" << std::endl;
            _result.set_value(false, lease());
        });

        // Hijack the ip-stack.
        _stack.set_packet_filter(this);

        return send_discover(l.ip).then([this, timeout]() {
            if (timeout != clock_type::duration()) {
                _timer.arm(timeout);
            }
            return _result.get_future().finally([this]() {
                        assert(_stack.packet_filter() == this);
                        _stack.set_packet_filter(nullptr);
                    });
        });
    }

    template<typename T>
    future<> build_ip_headers_and_send(T && pkt) {
        auto size = sizeof(pkt);
        auto & ip = pkt.ip;

        ip.ihl = sizeof(ip) / 4;
        ip.ver = 4;
        ip.dscp = 0;
        ip.ecn = 0;
        ip.len = uint16_t(size);
        ip.id = 0;
        ip.frag = 0;
        ip.ttl = 64;
        ip.csum = 0;
        ip.ip_proto = uint8_t(ip_protocol_num::udp);
        ip.dst_ip = ipv4_address(0xffffffff);

        auto & udp = pkt.udp;

        udp.src_port = client_port;
        udp.dst_port = server_port;
        udp.len = uint16_t(size - sizeof(ip));
        udp.cksum = 0; // TODO etc.


        pkt.dhp.bootp.xid = _xid;
        auto ipf = _stack.netif();
        auto mac = ipf->hw_address().mac;
        std::copy(mac.begin(), mac.end(), std::begin(pkt.dhp.bootp.chaddr));

        hton(pkt);

        checksummer csum;
        csum.sum(reinterpret_cast<char*>(&ip), sizeof(ip));
        ip.csum = csum.get();

        packet p(reinterpret_cast<char *>(&pkt), sizeof(pkt));

        return _stack.send_raw(ethernet::broadcast_address(), std::move(p)).rescue([this](auto get_ex) {
            try {
                get_ex();
            } catch (std::exception & e) {
                this->log() << e.what() << std::endl;
                _state = state::FAIL;
                _result.set_value(false, lease());
            }
        });
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
        return build_ip_headers_and_send(d);
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
        return build_ip_headers_and_send(d);
    }

private:
    promise<bool, lease> _result;
    state _state = state::NONE;
    timer _timer;
    ipv4 & _stack;
    uint32_t _xid = 0;
};

const net::dhcp::impl::req_opt_type net::dhcp::impl::requested_options = {
        opt_type::SUBNET_MASK, opt_type::ROUTER, opt_type::DOMAIN_NAME_SERVERS,
        opt_type::INTERFACE_MTU, opt_type::BROADCAST_ADDRESS };

const net::dhcp::impl::magic_tag net::dhcp::impl::options_magic = { 0x63, 0x82, 0x53,
        0x63 };

const uint16_t net::dhcp::impl::client_port;
const uint16_t net::dhcp::impl::server_port;

const clock_type::duration net::dhcp::default_timeout = std::chrono::duration_cast<clock_type::duration>(std::chrono::seconds(30));

net::dhcp::dhcp(ipv4 & ip)
: _impl(std::make_unique<impl>(ip))
{}

net::dhcp::dhcp(dhcp && v)
: _impl(std::move(v._impl))
{}

net::dhcp::~dhcp()
{}

net::dhcp::result_type net::dhcp::discover(const clock_type::duration & timeout) {
    return _impl->run(lease(), timeout);
}

net::dhcp::result_type net::dhcp::renew(const lease & l, const clock_type::duration & timeout) {
    return _impl->run(l, timeout);
}

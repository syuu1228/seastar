/*
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 */

#include "native-stack.hh"
#include "native-stack-impl.hh"
#include "net.hh"
#include "ip.hh"
#include "tcp-stack.hh"
#include "udp.hh"
#include "virtio.hh"
#include "dpdk.hh"
#include "xenfront.hh"
#include "proxy.hh"
#include "dhcp.hh"
#include <memory>
#include <queue>
#ifdef HAVE_OSV
#include <osv/firmware.hh>
#include <gnu/libc-version.h>
#endif
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

namespace net {

enum class xen_info {
    nonxen = 0,
    userspace = 1,
    osv = 2,
};

#ifdef HAVE_XEN
static xen_info is_xen()
{
    struct stat buf;
    if (!stat("/proc/xen", &buf) || !stat("/dev/xen", &buf)) {
        return xen_info::userspace;
    }

#ifdef HAVE_OSV
    const char *str = gnu_get_libc_release();
    if (std::string("OSv") != str) {
        return xen_info::nonxen;
    }
    auto firmware = osv::firmware_vendor();
    if (firmware == "Xen") {
        return xen_info::osv;
    }
#endif

    return xen_info::nonxen;
}
#endif

void create_native_net_device(boost::program_options::variables_map opts) {
    std::unique_ptr<device> dev;

#ifdef HAVE_XEN
    auto xen = is_xen();
    if (xen != xen_info::nonxen) {
        dev = xen::create_xenfront_net_device(opts, xen == xen_info::userspace);
    } else
#endif

#ifdef HAVE_DPDK
    if (opts.count("dpdk-pmd")) {
        // Hardcoded port index 0.
        // TODO: Inherit it from the opts
        dev = create_dpdk_net_device(0, smp::count);
    } else
#endif
    dev = create_virtio_net_device(opts);

    std::shared_ptr<device> sdev(dev.release());
    for (unsigned i = 0; i < smp::count; i++) {
        smp::submit_to(i, [opts, sdev] {
            uint16_t qid = engine.cpu_id();
            if (qid < sdev->hw_queues_count()) {
                auto qp = sdev->init_local_queue(opts, qid);
                for (unsigned i = sdev->hw_queues_count() + qid % sdev->hw_queues_count(); i < smp::count; i+= sdev->hw_queues_count()) {
                    qp->add_proxy(i);
                }
                sdev->set_local_queue(std::move(qp));
            } else {
                auto master = qid % sdev->hw_queues_count();
                sdev->set_local_queue(create_proxy_net_device(master, sdev.get()));
            }
            create_native_stack(opts, sdev);
        });
    }
}

// native_network_stack
class native_network_stack : public network_stack {
public:
    static thread_local promise<std::unique_ptr<network_stack>> ready_promise;
private:
    interface _netif;
    ipv4 _inet;
    udp_v4 _udp;
    bool _dhcp = false;
    promise<> _config;
    timer<> _timer;

    future<> run_dhcp(bool is_renew = false, const dhcp::lease & res = dhcp::lease());
    void on_dhcp(bool, const dhcp::lease &, bool);
    void set_ipv4_packet_filter(ip_packet_filter* filter) {
        _inet.set_packet_filter(filter);
    }
    using tcp4 = tcp<ipv4_traits>;
public:
    explicit native_network_stack(boost::program_options::variables_map opts, std::shared_ptr<device> dev);
    virtual server_socket listen(socket_address sa, listen_options opt) override;
    virtual connected_socket connect(socket_address sa) override;
    virtual udp_channel make_udp_channel(ipv4_addr addr) override;
    virtual future<> initialize() override;
    static future<std::unique_ptr<network_stack>> create(boost::program_options::variables_map opts) {
        if (engine.cpu_id() == 0) {
            create_native_net_device(opts);
        }
        return ready_promise.get_future();
    }
    virtual bool has_per_core_namespace() override { return true; };
    void arp_learn(ethernet_address l2, ipv4_address l3) {
        _inet.learn(l2, l3);
    }
    friend class native_server_socket_impl<tcp4>;
};

thread_local promise<std::unique_ptr<network_stack>> native_network_stack::ready_promise;

udp_channel
native_network_stack::make_udp_channel(ipv4_addr addr) {
    return _udp.make_channel(addr);
}

void
add_native_net_options_description(boost::program_options::options_description &opts) {

#ifdef HAVE_XEN
    auto xen = is_xen();
    if (xen != xen_info::nonxen) {
        opts.add(xen::get_xenfront_net_options_description());
        return;
    }
#endif
    opts.add(get_virtio_net_options_description());
#ifdef HAVE_DPDK
    opts.add(get_dpdk_net_options_description());
#endif
}

native_network_stack::native_network_stack(boost::program_options::variables_map opts, std::shared_ptr<device> dev)
    : _netif(std::move(dev))
    , _inet(&_netif)
    , _udp(_inet) {
    _inet.set_host_address(ipv4_address(opts["host-ipv4-addr"].as<std::string>()));
    _inet.set_gw_address(ipv4_address(opts["gw-ipv4-addr"].as<std::string>()));
    _inet.set_netmask_address(ipv4_address(opts["netmask-ipv4-addr"].as<std::string>()));
    _udp.set_queue_size(opts["udpv4-queue-size"].as<int>());
    _dhcp = opts["host-ipv4-addr"].defaulted()
            && opts["gw-ipv4-addr"].defaulted()
            && opts["netmask-ipv4-addr"].defaulted() && opts["dhcp"].as<bool>();
}

server_socket
native_network_stack::listen(socket_address sa, listen_options opts) {
    assert(sa.as_posix_sockaddr().sa_family == AF_INET);
    return tcpv4_listen(_inet.get_tcp(), ntohs(sa.as_posix_sockaddr_in().sin_port), opts);
}

connected_socket
native_network_stack::connect(socket_address sa) {
    assert(sa.as_posix_sockaddr().sa_family == AF_INET);
    return tcpv4_connect(_inet.get_tcp(), sa);
}

using namespace std::chrono_literals;

future<> native_network_stack::run_dhcp(bool is_renew, const dhcp::lease& res) {
    shared_ptr<dhcp> d = make_shared<dhcp>(_inet);

    // Hijack the ip-stack.
    for (unsigned i = 0; i < smp::count; i++) {
        smp::submit_to(i, [d] {
            auto & ns = static_cast<native_network_stack&>(engine.net());
            ns.set_ipv4_packet_filter(d->get_ipv4_filter());
        });
    }

    net::dhcp::result_type fut = is_renew ? d->renew(res) : d->discover();

    return fut.then([this, d, is_renew](bool success, const dhcp::lease & res) {
        for (unsigned i = 0; i < smp::count; i++) {
            smp::submit_to(i, [] {
                auto & ns = static_cast<native_network_stack&>(engine.net());
                ns.set_ipv4_packet_filter(nullptr);
            });
        }
        on_dhcp(success, res, is_renew);
    });
}

void native_network_stack::on_dhcp(bool success, const dhcp::lease & res, bool is_renew) {
    if (success) {
        _inet.set_host_address(res.ip);
        _inet.set_gw_address(res.gateway);
        _inet.set_netmask_address(res.netmask);
    }
    // Signal waiters.
    if (!is_renew) {
        _config.set_value();
    }

    if (engine.cpu_id() == 0) {
        // And the other cpus, which, in the case of initial discovery,
        // will be waiting for us.
        for (unsigned i = 1; i < smp::count; i++) {
            smp::submit_to(i, [success, res, is_renew]() {
                auto & ns = static_cast<native_network_stack&>(engine.net());
                ns.on_dhcp(success, res, is_renew);
            });
        }
        if (success) {
            // And set up to renew the lease later on.
            _timer.set_callback(
                    [this, res]() {
                        _config = promise<>();
                        run_dhcp(true, res);
                    });
            _timer.arm(
                    std::chrono::duration_cast<clock_type::duration>(
                            res.lease_time));
        }
    }
}

future<> native_network_stack::initialize() {
    return network_stack::initialize().then([this]() {
        if (!_dhcp) {
            return make_ready_future();
        }

        // Only run actual discover on main cpu.
        // All other cpus must simply for main thread to complete and signal them.
        if (engine.cpu_id() == 0) {
            run_dhcp();
        }
        return _config.get_future();
    });
}

void arp_learn(ethernet_address l2, ipv4_address l3)
{
    for (unsigned i = 0; i < smp::count; i++) {
        smp::submit_to(i, [l2, l3] {
            auto & ns = static_cast<native_network_stack&>(engine.net());
            ns.arp_learn(l2, l3);
        });
    }
}

void create_native_stack(boost::program_options::variables_map opts, std::shared_ptr<device> dev) {
    native_network_stack::ready_promise.set_value(std::unique_ptr<network_stack>(std::make_unique<native_network_stack>(opts, std::move(dev))));
}

boost::program_options::options_description nns_options() {
    boost::program_options::options_description opts(
            "Native networking stack options");
    opts.add_options()
        ("tap-device",
                boost::program_options::value<std::string>()->default_value("tap0"),
                "tap device to connect to")
        ("host-ipv4-addr",
                boost::program_options::value<std::string>()->default_value("192.168.122.2"),
                "static IPv4 address to use")
        ("gw-ipv4-addr",
                boost::program_options::value<std::string>()->default_value("192.168.122.1"),
                "static IPv4 gateway to use")
        ("netmask-ipv4-addr",
                boost::program_options::value<std::string>()->default_value("255.255.255.0"),
                "static IPv4 netmask to use")
        ("udpv4-queue-size",
                boost::program_options::value<int>()->default_value(udp_v4::default_queue_size),
                "Default size of the UDPv4 per-channel packet queue")
        ("dhcp",
                boost::program_options::value<bool>()->default_value(true),
                        "Use DHCP discovery")
#ifdef HAVE_DPDK
        ("dpdk-pmd", "Use DPDK PMD drivers")
#endif
        ;

    add_native_net_options_description(opts);
    return opts;
}

network_stack_registrator nns_registrator{
    "native", nns_options(), native_network_stack::create
};

}

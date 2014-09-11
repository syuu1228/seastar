/*
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 */

#include "netmap.hh"
#include "core/posix.hh"
#include "core/vla.hh"
#include "core/reactor.hh"
#include <atomic>
#include <vector>
#include <queue>
#include <fcntl.h>
#include <poll.h>

#define NETMAP_WITH_LIBS
#include <net/netmap_user.h>

using namespace net;

class netmap_net_device : public net::device {
private:
    struct nm_desc *_nd;
    struct pollfd _pollfd[1] = {};
    std::queue<packet> _rx_queue;
public:
    explicit netmap_net_device(sstring netmap_device);
    virtual future<packet> receive() override;
    virtual future<> send(packet p) override;
    virtual ethernet_address hw_address() override;

};

netmap_net_device::netmap_net_device(sstring netmap_device) {
    _nd = nm_open(netmap_device.c_str(), NULL, 0, NULL);
    _pollfd[0].fd = _nd->fd;
}

future<packet>
netmap_net_device::receive() {
    if (_rx_queue.size() > 0) {
        auto p = std::move(_rx_queue.front());
        _rx_queue.pop();
        return make_ready_future<packet>(std::move(p));
    }
    char *ret_buf = nullptr;
    uint32_t ret_len = 0;
    while (!ret_buf) {
        _pollfd[0].events = POLLIN;
        _pollfd[0].revents = 0;

        int ret = poll(_pollfd, 2, 2500);
        if (ret < 0)
            continue;

        for (auto i = _nd->first_rx_ring; i <= _nd->last_rx_ring; i++) {
            auto rxring = NETMAP_RXRING(_nd->nifp, i);
            if (nm_ring_empty(rxring))
                continue;
            auto j = rxring->cur;
            auto space = nm_ring_space(rxring);
            while (space-- > 0) {
                auto slot = &rxring->slot[j];
                auto *rxbuf = NETMAP_BUF(rxring, slot->buf_idx);
                if (!ret_buf) {
                    ret_buf = rxbuf;
                    ret_len = slot->len;
                } else {
                    packet p(fragment{rxbuf, slot->len}, [] {});
                    _rx_queue.push(std::move(p));
                }
                j = nm_ring_next(rxring, j);
            }
        }
    }
    packet p(fragment{ret_buf, ret_len}, [] {});
    return make_ready_future<packet>(std::move(p));
}

future<>
netmap_net_device::send(packet p) {
    return make_ready_future<>();
}

ethernet_address netmap_net_device::hw_address() {
    return { 0x12, 0x23, 0x34, 0x56, 0x67, 0x78 };
}

std::unique_ptr<net::device> create_netmap_net_device(sstring netmap_device) {
    return std::make_unique<netmap_net_device>(netmap_device);
}

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
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 */

#pragma once

#include <seastar/net/packet.hh>
#include <seastar/util/modules.hh>
#ifndef SEASTAR_MODULE
#include <cstdint>
#include <cstddef>
#include <arpa/inet.h>
#endif

namespace seastar {

namespace net {

uint16_t ip_checksum(const void* data, size_t len);

struct checksummer {
    __int128 csum = 0;
    bool odd = false;
    void sum(const char* data, size_t len);
    void sum(const packet& p);
    void sum(uint8_t data) {
        if (!odd) {
            csum += data << 8;
        } else {
            csum += data;
        }
        odd = !odd;
    }
    void sum(uint16_t data) {
        if (odd) {
            sum(uint8_t(data >> 8));
            sum(uint8_t(data));
        } else {
            csum += data;
        }
    }
    void sum(uint32_t data) {
        if (odd) {
            sum(uint16_t(data));
            sum(uint16_t(data >> 16));
        } else {
            csum += data;
        }
    }
    void sum_many() {}
    template <typename T0, typename... T>
    void sum_many(T0 data, T... rest) {
        sum(data);
        sum_many(rest...);
    }
    uint16_t get() const;
};

}

}


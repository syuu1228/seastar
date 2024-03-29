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

#include <seastar/core/deleter.hh>
#include <seastar/core/temporary_buffer.hh>
#include <seastar/net/packet.hh>
#include <seastar/core/sstring.hh>
#include <seastar/util/std-compat.hh>
#include <seastar/util/modules.hh>
#ifndef SEASTAR_MODULE
#include <memory>
#include <vector>
#endif

namespace seastar {

SEASTAR_MODULE_EXPORT
template <typename CharType>
class scattered_message {
private:
    using fragment = net::fragment;
    using packet = net::packet;
    using char_type = CharType;
    packet _p;
public:
    scattered_message() {}
    scattered_message(scattered_message&&) = default;
    scattered_message(const scattered_message&) = delete;

    void append_static(const char_type* buf, size_t size) {
        if (size) {
            _p = packet(std::move(_p), fragment{(char_type*)buf, size}, deleter());
        }
    }

    template <size_t N>
    void append_static(const char_type(&s)[N]) {
        append_static(s, N - 1);
    }

    void append_static(const char_type* s) {
        append_static(s, strlen(s));
    }

    template <typename size_type, size_type max_size>
    void append_static(const basic_sstring<char_type, size_type, max_size>& s) {
        append_static(s.begin(), s.size());
    }

    void append_static(const std::string_view& s) {
        append_static(s.data(), s.size());
    }

    void append(std::string_view v) {
        if (v.size()) {
            _p = packet(std::move(_p), temporary_buffer<char>::copy_of(v));
        }
    }

    void append(temporary_buffer<CharType> buff) {
        if (buff.size()) {
            _p = packet(std::move(_p), std::move(buff));
        }
    }

    template <typename size_type, size_type max_size>
    void append(basic_sstring<char_type, size_type, max_size> s) {
        if (s.size()) {
            _p = packet(std::move(_p), std::move(s).release());
        }
    }

    template <typename size_type, size_type max_size, typename Callback>
    void append(const basic_sstring<char_type, size_type, max_size>& s, Callback callback) {
        if (s.size()) {
            _p = packet(std::move(_p), fragment{s.begin(), s.size()}, make_deleter(std::move(callback)));
        }
    }

    void reserve(int n_frags) {
        _p.reserve(n_frags);
    }

    packet release() && {
        return std::move(_p);
    }

    template <typename Callback>
    void on_delete(Callback callback) {
        _p = packet(std::move(_p), make_deleter(std::move(callback)));
    }

    operator bool() const {
        return _p.len();
    }

    size_t size() {
        return _p.len();
    }
};

}

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
 * Copyright (C) 2016 ScyllaDB
 */

#pragma once

#ifndef SEASTAR_MODULE
#include <seastar/core/timer.hh>
#include <seastar/util/modules.hh>

#include <atomic>
#include <chrono>
#endif

namespace seastar {

SEASTAR_MODULE_EXPORT
class manual_clock {
public:
    using rep = int64_t;
    using period = std::chrono::nanoseconds::period;
    using duration = std::chrono::duration<rep, period>;
    using time_point = std::chrono::time_point<manual_clock, duration>;
private:
    static std::atomic<rep> _now;
    static void expire_timers() noexcept;
public:
    manual_clock() noexcept;
    static time_point now() noexcept {
        return time_point(duration(_now.load(std::memory_order_relaxed)));
    }
    static void advance(duration d) noexcept;
};

extern template class timer<manual_clock>;

}


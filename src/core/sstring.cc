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
 * Copyright (C) 2020 ScyllaDB
 */

#ifdef SEASTAR_MODULE
module;
#include <cstddef>
#include <new>
#include <stdexcept>
module seastar;
#else
#include <seastar/core/sstring.hh>
#endif

using namespace seastar;

[[noreturn]] void internal::throw_bad_alloc() {
    throw std::bad_alloc();
}

[[noreturn]] void internal::throw_sstring_overflow() {
    throw std::overflow_error("sstring overflow");
}

[[noreturn]] void internal::throw_sstring_out_of_range() {
    throw std::out_of_range("sstring out of range");
}

/*
 * cql.hh
 *
 *  Created on: Jul 31, 2014
 *      Author: avi
 */

#ifndef CQL_HH_
#define CQL_HH_

#include "sstring.hh"
#include <stdint.h>
#include <vector>
#include <boost/optional.hpp>
#include <unordered_map>
#include <type_traits>

namespace cql {

namespace protocol {

using boost::optional;

struct version {
    uint8_t version;
};

enum class flags_type {
    compression = 0x01,
    tracing = 0x02,
};

enum class opcode {
    error = 0x00,
    startup = 0x01,
    ready = 0x02,
    authenticate = 0x03,
    options = 0x05,
    supported = 0x06,
    query = 0x07,
    result = 0x08,
    prepare = 0x09,
    execute = 0x0a,
    register_ = 0x0b,
    event = 0x0c,
    batch = 0x0d,
    auth_challenge = 0x0e,
    auth_response = 0x0f,
    auth_success = 0x10,
};

struct frame_header {
    flags_type flags;
    int16_t stream;
    uint8_t opcode;
    uint32_t length;
    static constexpr uint32_t max_length = 256 << 20;
};

struct uuid {
    uint8_t data[16];
};

using string_list = std::vector<sstring>;
using bytes = std::vector<uint8_t>;

struct inet {
    uint8_t size;
    uint8_t addr[16];
    uint32_t port;
};

enum class consistency {
    any = 0x0000,
    one = 0x0001,
    two = 0x0002,
    three = 0x0003,
    quorum = 0x0004,
    all = 0x0005,
    local_quorum = 0x0006,
    each_quorum = 0x0007,
    serial = 0x0008,
    local_serial = 0x0009,
    local_one = 0x000a,
};

template <typename discrim_type, discrim_type discrim, typename T>
struct option_element {
};

template <typename discrim_type, typename... elements>
struct option;

namespace detail {

inline constexpr size_t max_size() {
    return 0;
}

template <typename T, typename... types>
inline constexpr size_t max_size() {
    return sizeof(T) > max_size<types...>() ? sizeof(T) : max_size<types...>();
}

inline constexpr size_t max_align() {
    return 0;
}

template <typename T, typename... types>
inline constexpr size_t max_align() {
    return alignof(T) > max_align<types...>() ? alignof(T) : max_align<types...>();
}



}

template <typename discrim_type, discrim_type... discrims, typename... elements>
class option<discrim_type, option_element<discrim_type, discrims, elements>...> {
    discrim_type discrim;
    typename std::aligned_storage<detail::max_size<elements...>(), detail::max_align<elements...>()>::type
        _storage;
    template <discrim_type discrim, typename... elements_>
    struct type_helper;
    template <discrim_type discrim, typename T, typename... rest>
    struct type_helper<discrim, option_element<discrim_type, discrim, T>, rest...> {
        using type = T;
    };
    template <discrim_type discrim, typename option_element, typename... rest>
    struct type_helper<discrim, option_element, rest...> {
        using type = typename type_helper<discrim, rest...>::type;
    };
    template <discrim_type discrim>
    using type_for
        = typename type_helper<discrim, option_element<discrim_type, discrims, elements>...>::type;
    template <discrim_type discrim>
    auto get() -> type_for<discrim>& {
        return *reinterpret_cast<type_for<discrim>*>(reinterpret_cast<char*>(&_storage));
    }
};

namespace v3 {

}


}

}


#endif /* CQL_HH_ */

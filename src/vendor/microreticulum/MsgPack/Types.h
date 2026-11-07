#pragma once
#ifndef HT_SERIAL_MSGPACK_TYPES_H
#define HT_SERIAL_MSGPACK_TYPES_H

#include <stddef.h>
#ifdef ARDUINO
#include <Arduino.h>
#else
#include <string>
#endif

#ifdef INT5
// avoid conflict with interrrupt macro on e.g. Arduino Mega. This
// should not break code that rely on this constant, by keeping the
// value available as a C++-level constant. This first allocates a
// temporary constant, to capture the value before undeffing, and
// then defines the actual INT5 constant. Finally, INT5 is redefined
// as itself to make sure #ifdef INT5 still works.
static constexpr uint8_t INT5_TEMP_VALUE = INT5;
#undef INT5
static constexpr uint8_t INT5 = INT5_TEMP_VALUE;
#define INT5 INT5
#endif

#include <ArxTypeTraits.h>
#include <ArxContainer.h>

namespace arduino {
namespace msgpack {

#if ARX_HAVE_LIBSTDCPLUSPLUS >= 201103L  // Have libstdc++11

    using idx_t = std::vector<size_t>;
    template <typename T>
    using arr_t = std::vector<T>;
    template <typename T, size_t N>
    using fix_arr_t = std::array<T, N>;
    template <typename T, typename U>
    using map_t = std::map<T, U>;
    template <typename T>
    using bin_t = std::vector<
        typename std::enable_if<std::is_same<T, uint8_t>::value || std::is_same<T, char>::value, T>::type,
        std::allocator<T> >;

#else  // Do not have libstdc++11

#ifndef MSGPACK_MAX_PACKET_BYTE_SIZE
#define MSGPACK_MAX_PACKET_BYTE_SIZE 128
#endif  // MSGPACK_MAX_PACKET_BYTE_SIZE
#ifndef MSGPACK_MAX_ARRAY_SIZE
#define MSGPACK_MAX_ARRAY_SIZE 8
#endif  // MSGPACK_MAX_ARRAY_SIZE
#ifndef MSGPACK_MAX_MAP_SIZE
#define MSGPACK_MAX_MAP_SIZE 8
#endif  // MSGPACK_MAX_MAP_SIZE
#ifndef MSGPACK_MAX_OBJECT_SIZE
#define MSGPACK_MAX_OBJECT_SIZE 32
#endif  // MSGPACK_MAX_OBJECT_SIZE

    using idx_t = arx::stdx::vector<size_t, MSGPACK_MAX_OBJECT_SIZE>;
    template <typename T, size_t N = MSGPACK_MAX_ARRAY_SIZE>
    using arr_t = arx::stdx::vector<T, N>;
    template <typename T, size_t N>
    using fix_arr_t = arx::stdx::array<T, N>;
    template <typename T, typename U, size_t N = MSGPACK_MAX_MAP_SIZE>
    using map_t = arx::stdx::map<T, U, N>;
    template <typename T, size_t N = MSGPACK_MAX_PACKET_BYTE_SIZE>
    using bin_t = arx::stdx::vector<typename std::enable_if<std::is_same<T, uint8_t>::value || std::is_same<T, char>::value, T>::type, N>;

#endif  // Do not have libstdc++11

#ifdef ARDUINO
    using str_t = String;
#else
    using str_t = std::string;
#ifndef F
    inline const char* F(const char* c) {
        return c;
    }
#endif
#endif

    namespace type {
        class ArraySize {
            size_t sz;

        public:
            explicit ArraySize(const size_t size = 0) : sz(size) {}
            size_t size() const {
                return sz;
            }
            void size(const size_t s) {
                sz = s;
            }
        };
        class MapSize {
            size_t sz;

        public:
            explicit MapSize(const size_t size = 0) : sz(size) {}
            size_t size() const {
                return sz;
            }
            void size(const size_t s) {
                sz = s;
            }
        };
    }  // namespace type

    using arr_size_t = type::ArraySize;
    using map_size_t = type::MapSize;

    namespace object {
        struct nil_t {
            bool is_nil {false};
            nil_t& operator=(const nil_t& rhs) {
                this->is_nil = rhs.is_nil;
                return *this;
            }
            nil_t& operator=(const bool b) {
                this->is_nil = b;
                return *this;
            }
            bool operator()() const {
                return this->is_nil;
            }
            bool operator==(const nil_t& x) {
                return (*this)() == x();
            }
            bool operator!=(const nil_t& x) {
                return !(*this == x);
            }
        };

        class ext {
            bin_t<uint8_t> m_data;

        public:
            ext() : m_data() {}
            ext(int8_t t, const uint8_t* p, uint32_t s) {
                m_data.reserve(static_cast<size_t>(s) + 1);
                m_data.push_back(static_cast<uint8_t>(t));
                m_data.insert(m_data.end(), p, p + s);
            }
            ext(int8_t t, uint32_t s) {
                m_data.resize(static_cast<size_t>(s) + 1);
                m_data[0] = static_cast<char>(t);
            }
            int8_t type() const {
                return static_cast<int8_t>(m_data[0]);
            }
            const uint8_t* data() const {
                return &(m_data[0]) + 1;
            }
            uint8_t* data() {
                return &(m_data[0]) + 1;
            }
            uint32_t size() const {
                return static_cast<uint32_t>(m_data.size()) - 1;
            }
            bool operator==(const ext& x) const {
                return m_data == x.m_data;
            }
            bool operator!=(const ext& x) const {
                return !(*this == x);
            }
        };

        struct timespec {
            int64_t tv_sec;    // seconds
            uint32_t tv_nsec;  // nanoseconds

            bool operator==(const timespec& x) const {
                return (tv_sec == x.tv_sec) && (tv_nsec == x.tv_nsec);
            }
            bool operator!=(const timespec& x) const {
                return !(*this == x);
            }
            bool operator<(const timespec& x) const {
                if (tv_sec < x.tv_sec)
                    return true;
                else if (tv_sec > x.tv_sec)
                    return false;
                else
                    return tv_nsec < x.tv_nsec;
            }
            bool operator>(const timespec& x) const {
                return (*this != x) && (*this < x);
            }
        };

    }  // namespace object

    enum class Type : uint8_t {
        NA = 0xC1,  // never used
        NIL = 0xC0,
        BOOL = 0xC2,
        UINT7 = 0x00,  // same as POSITIVE_FIXINT
        UINT8 = 0xCC,
        UINT16 = 0xCD,
        UINT32 = 0xCE,
        UINT64 = 0xCF,
        INT5 = 0xE0,  // same as NEGATIVE_FIXINT
        INT8 = 0xD0,
        INT16 = 0xD1,
        INT32 = 0xD2,
        INT64 = 0xD3,
        FLOAT32 = 0xCA,
        FLOAT64 = 0xCB,
        STR5 = 0xA0,  // same as FIXSTR
        STR8 = 0xD9,
        STR16 = 0xDA,
        STR32 = 0xDB,
        BIN8 = 0xC4,
        BIN16 = 0xC5,
        BIN32 = 0xC6,
        ARRAY4 = 0x90,  // same as FIXARRAY
        ARRAY16 = 0xDC,
        ARRAY32 = 0xDD,
        MAP4 = 0x80,  // same as FIXMAP
        MAP16 = 0xDE,
        MAP32 = 0xDF,
        FIXEXT1 = 0xD4,
        FIXEXT2 = 0xD5,
        FIXEXT4 = 0xD6,
        FIXEXT8 = 0xD7,
        FIXEXT16 = 0xD8,
        EXT8 = 0xC7,
        EXT16 = 0xC8,
        EXT32 = 0xC9,
        TIMESTAMP32 = 0xD6,
        TIMESTAMP64 = 0xD7,
        TIMESTAMP96 = 0xC7,

        POSITIVE_FIXINT = 0x00,
        NEGATIVE_FIXINT = 0xE0,
        FIXSTR = 0xA0,
        FIXARRAY = 0x90,
        FIXMAP = 0x80,
    };

    enum class BitMask : uint8_t {
        BOOL = 0x01,
        POSITIVE_FIXINT = 0x7F,
        UINT7 = 0x7F,  // same as POSITIVE_FIXINT
        NEGATIVE_FIXINT = 0x1F,
        INT5 = 0x1F,  // same as NEGATIVE_FIXINT
        FIXSTR = 0x1F,
        STR5 = 0x1F,  // same as FIXSTR
        FIXARRAY = 0x0F,
        ARRAY4 = 0x0F,  // same as FIXARRAY
        FIXMAP = 0x0F,
        MAP4 = 0x0F,  // same as FIXMAP
    };

    template <typename ClassType, typename ArgType>
    using has_to_msgpack_impl = typename std::enable_if<
        std::is_same<decltype(&ClassType::to_msgpack), void (ClassType::*)(ArgType) const>::value>::type;
    template <typename ClassType, typename ArgType>
    using has_to_msgpack = arx::is_detected<has_to_msgpack_impl, ClassType, ArgType>;

    template <typename ClassType, typename ArgType>
    using has_from_msgpack_impl = typename std::enable_if<
        std::is_same<decltype(&ClassType::from_msgpack), void (ClassType::*)(ArgType)>::value>::type;
    template <typename ClassType, typename ArgType>
    using has_from_msgpack = arx::is_detected<has_from_msgpack_impl, ClassType, ArgType>;

}  // namespace msgpack
}  // namespace arduino

#define MSGPACK_DEFINE(...)                          \
    void to_msgpack(MsgPack::Packer& packer) const { \
        packer.to_array(__VA_ARGS__);                \
    }                                                \
    void from_msgpack(MsgPack::Unpacker& unpacker) { \
        unpacker.from_array(__VA_ARGS__);            \
    }

#define MSGPACK_DEFINE_MAP(...)                      \
    void to_msgpack(MsgPack::Packer& packer) const { \
        packer.to_map(__VA_ARGS__);                  \
    }                                                \
    void from_msgpack(MsgPack::Unpacker& unpacker) { \
        unpacker.from_map(__VA_ARGS__);              \
    }

#define MSGPACK_BASE(base) (*const_cast<base*>(static_cast<base const*>(this)))

#endif  // HT_SERIAL_MSGPACK_TYPES_H

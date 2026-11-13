#pragma once
#ifndef HT_SERIAL_MSGPACK_PACKER_H
#define HT_SERIAL_MSGPACK_PACKER_H

#include <ArxTypeTraits.h>
#include <ArxContainer.h>
#if ARX_HAVE_LIBSTDCPLUSPLUS >= 201103L  // Have libstdc++11
#include <vector>
#include <array>
#include <deque>
#include <tuple>
#include <list>
#include <forward_list>
#include <set>
#include <unordered_set>
#include <map>
#include <unordered_map>
#include <limits>
#else  // Do not have libstdc++11
// containers are disabled
#endif

#ifdef TEENSYDUINO
#include "util/TeensyDirtySTLErrorSolution/TeensyDirtySTLErrorSolution.h"
#endif  // TEENSYDUINO

#include "Types.h"
#include "Unpacker.h"
#include <DebugLog.h>

namespace arduino {
namespace msgpack {

    class Packer {
        bin_t<uint8_t> buffer;
        size_t n_indices {0};

    public:
        void reserve_buffer(const size_t size) {
            buffer.reserve(size);
        }

        template <typename First, typename... Rest>
        auto serialize(const First& first, Rest&&... rest) -> typename std::enable_if<
            !std::is_pointer<First>::value || std::is_same<First, const char*>::value
#ifdef ARDUINO
            || std::is_same<First, const __FlashStringHelper*>::value
#endif
            >::type {
            pack(first);
            serialize(std::forward<Rest>(rest)...);
        }

        void serialize() {}

        template <typename... Args>
        void serialize(const arr_size_t& arr_size, Args&&... args) {
            packArraySize(arr_size.size());
            serialize(std::forward<Args>(args)...);
        }

        template <typename... Args>
        void serialize(const map_size_t& map_size, Args&&... args) {
            packMapSize(map_size.size());
            serialize(std::forward<Args>(args)...);
        }

#ifdef ARDUINOJSON_VERSION

        //! Note: ArduinoJson cannot handle Arduino's String class correctly.
        //! So the external buffer size is required.
        void serialize_arduinojson(const JsonDocument& doc, const size_t num_max_string_type = 32) {
            const size_t size = measureMsgPack(doc);
            uint8_t* data = new uint8_t[size + num_max_string_type];
            serializeMsgPack(doc, data, size + num_max_string_type);
            packRawBytes(data, size);
            // TODO: smarter way to calcurate size of indices?
            Unpacker unpacker;
            unpacker.feed(data, size);
            n_indices += unpacker.size();
            delete[] data;
        }

        template <size_t N>
        void serialize(const StaticJsonDocument<N>& doc, const size_t num_max_string_type = 32) {
            serialize_arduinojson(doc, num_max_string_type);
        }
        void serialize(const DynamicJsonDocument& doc, const size_t num_max_string_type = 32) {
            serialize_arduinojson(doc, num_max_string_type);
        }

#endif  // ARDUINOJSON_VERSION

        template <typename... Args>
        void to_array(Args&&... args) {
            serialize(arr_size_t(sizeof...(args)), std::forward<Args>(args)...);
        }

        template <typename... Args>
        void to_map(Args&&... args) {
            size_t size = sizeof...(args);
            if ((size % 2) == 0)
                serialize(map_size_t(size / 2), std::forward<Args>(args)...);
            else
                LOG_ERROR(F("serialize arg size must be even for map:"), size);
        }

        const bin_t<uint8_t>& packet() const {
            return buffer;
        }
        const uint8_t* data() const {
            return buffer.data();
        }
        size_t size() const {
            return buffer.size();
        }
        size_t indices() const {
            return n_indices;
        }
        void clear() {
            buffer.clear();
            n_indices = 0;
        }

        /////////////////////////////////////////////////
        // ---------- msgpack type adaptors ---------- //
        /////////////////////////////////////////////////

        // adaptation of types to msgpack
        // https://github.com/msgpack/msgpack-c/wiki/v2_0_cpp_adaptor

        // ---------- NIL format family ----------
        // - N/A

        template <typename T>
        auto pack(const T& n) -> typename std::enable_if<
            std::is_same<T, object::nil_t>::value || std::is_same<T, std::nullptr_t>::value>::type {
            (void)n;
            packNil();
        }

        // ---------- BOOL format family ----------
        // - bool

        template <typename T>
        auto pack(const T& b) -> typename std::enable_if<std::is_same<T, bool>::value>::type {
            packBool(b);
        }

        // ---------- INT format family ----------
        // - char (signed/unsigned)
        // - ints (signed/unsigned)

        template <typename T>
        auto pack(const T& value) -> typename std::enable_if<
            std::is_arithmetic<T>::value && std::is_integral<T>::value && !std::is_same<T, bool>::value
            && !std::is_same<typename std::remove_cv<T>::type, char*>::value>::type {
            packInteger(value);
        }

        // ---------- FLOAT format family ----------
        // - float
        // - double

        template <typename T>
        auto pack(const T& value) ->
            typename std::enable_if<std::is_arithmetic<T>::value && std::is_floating_point<T>::value>::type {
            packFloat(value);
        }

        // ---------- STRING format family ----------
        // - char*
        // - char[]
        // - std::string

        void pack(const str_t& str) {
            packString(str, getStringSize(str));
        }
        void pack(const char* str) {
            packString(str, getStringSize(str));
        }
        void pack(const char* str, const size_t size) {
            packString(str, size);
        }
#ifdef ARDUINO
        void pack(const __FlashStringHelper* str) {
            packString(str, getStringSize(str));
        }
#endif

        // ---------- BIN format family ----------
        // - unsigned char*
        // - unsigned char[]
        // - std::vector<char>
        // - std::vector<uint8_t>
        // - std::array<char>
        // - std::array<uint8_t>

        void pack(const uint8_t* bin, const size_t size) {
            packBinary(bin, size);
        }

        void pack(const bin_t<char>& bin) {
            packBinary((const uint8_t*)bin.data(), bin.size());
        }

        void pack(const bin_t<uint8_t>& bin) {
            packBinary(bin.data(), bin.size());
        }

#if ARX_HAVE_LIBSTDCPLUSPLUS >= 201103L  // Have libstdc++11

        template <size_t N>
        void pack(const std::array<char, N>& bin) {
            packBinary((const uint8_t*)bin.data(), bin.size());
        }

        template <size_t N>
        void pack(const std::array<uint8_t, N>& bin) {
            packBinary(bin.data(), bin.size());
        }

#endif  // have libstdc++11

        // ---------- ARRAY format family ----------
        // - T[]
        // - std::vector
        // - std::array
        // - std::deque
        // - std::pair
        // - std::tuple
        // - std::list
        // - std::forward_list
        // - std::set
        // - std::multiset
        // - std::unordered_set *
        // - std::unordered_multiset *
        // * : not supported in arduino

        template <typename T>
        auto pack(const T* arr, const size_t size) ->
            typename std::enable_if<!std::is_same<T, char>::value && !std::is_same<T, unsigned char>::value>::type {
            packArraySize(size);
            for (size_t i = 0; i < size; ++i) pack(arr[i]);
        }

        template <typename T>
        auto pack(const arr_t<T>& arr) ->
            typename std::enable_if<!std::is_same<T, char>::value && !std::is_same<T, unsigned char>::value>::type {
            packArrayContainer(arr);
        }

#if ARX_HAVE_LIBSTDCPLUSPLUS >= 201103L  // Have libstdc++11

        template <typename T, size_t N>
        auto pack(const std::array<T, N>& arr) ->
            typename std::enable_if<!std::is_same<T, char>::value && !std::is_same<T, unsigned char>::value>::type {
            packArrayContainer(arr);
        }

        template <typename T>
        void pack(const std::deque<T>& arr) {
            packArrayContainer(arr);
        }

        template <typename T, typename U>
        void pack(const std::pair<T, U>& arr) {
            packArraySize(2);
            pack(arr.first);
            pack(arr.second);
        }

#endif  // have libstdc++11

        template <size_t I = 0, typename... Ts>
            auto pack(const std::tuple<Ts...>& t) -> typename std::enable_if < I<sizeof...(Ts)>::type {
            if (I == 0) packArraySize(sizeof...(Ts));
            pack(std::get<I>(t));
            pack<I + 1, Ts...>(t);
        }

        template <size_t I = 0, typename... Ts>
        auto pack(const std::tuple<Ts...>&) -> typename std::enable_if<I == sizeof...(Ts)>::type {}

#if ARX_HAVE_LIBSTDCPLUSPLUS >= 201103L  // Have libstdc++11

        template <typename T>
        void pack(const std::list<T>& arr) {
            packArrayContainer(arr);
        }

        template <typename T>
        void pack(const std::forward_list<T>& arr) {
            auto size = std::distance(arr.begin(), arr.end());
            packArraySize(size);
            for (const auto& a : arr) pack(a);
        }

        template <typename T>
        void pack(const std::set<T>& arr) {
            packArrayContainer(arr);
        }

        template <typename T>
        void pack(const std::multiset<T>& arr) {
            packArrayContainer(arr);
        }

        template <typename T>
        void pack(const std::unordered_set<T>& arr) {
            packArrayContainer(arr);
        }

        template <typename T>
        void pack(const std::unordered_multiset<T>& arr) {
            packArrayContainer(arr);
        }

#endif  // have libstdc++11

        // ---------- MAP format family ----------
        // - std::map
        // - std::multimap
        // - std::unordered_map *
        // - std::unordered_multimap *
        // * : not supported in arduino

        template <typename T, typename U>
        void pack(const map_t<T, U>& mp) {
            packMapContainer(mp);
        }

#if ARX_HAVE_LIBSTDCPLUSPLUS >= 201103L  // Have libstdc++11

        template <typename T, typename U>
        void pack(const std::multimap<T, U>& mp) {
            packMapContainer(mp);
        }

        template <typename T, typename U>
        void pack(const std::unordered_map<T, U>& mp) {
            packMapContainer(mp);
        }

        template <typename T, typename U>
        void pack(const std::unordered_multimap<T, U>& mp) {
            packMapContainer(mp);
        }

#endif  // have libstdc++11

        // ---------- EXT format family ----------

        void pack(const object::ext& e) {
            packExt(e);
        }

        // ---------- TIMESTAMP format family ----------

        void pack(const object::timespec& t) {
            packTimestamp(t);
        }

        // ---------- CUSTOM format ----------

        template <typename C>
        auto pack(const C& c) -> typename std::enable_if<has_to_msgpack<C, Packer&>::value>::type {
            c.to_msgpack(*this);
        }

        // ---------- Array/Map Size format ----------

        void pack(const arr_size_t& t) {
            packArraySize(t.size());
        }

        void pack(const map_size_t& t) {
            packMapSize(t.size());
        }

        /////////////////////////////////////////////////////
        // ---------- msgpack types abstraction ---------- //
        /////////////////////////////////////////............

        // ---------- INT format family ----------

        template <typename T>
        auto packInteger(const T& value) -> typename std::enable_if<
            std::is_arithmetic<T>::value && std::is_integral<T>::value && !std::is_same<T, bool>::value
            && !std::is_same<typename std::remove_cv<T>::type, char*>::value>::type {
            if (value >= 0) {
                if ((uint64_t)value < (uint64_t)BitMask::UINT7)
                    packUInt7(value);
                else if ((uint64_t)value <= (uint64_t)std::numeric_limits<uint8_t>::max())
                    packUInt8(value);
                else if ((uint64_t)value <= (uint64_t)std::numeric_limits<uint16_t>::max())
                    packUInt16(value);
                else if ((uint64_t)value <= (uint64_t)std::numeric_limits<uint32_t>::max())
                    packUInt32(value);
                else
                    packUInt64(value);
            } else {
                if ((int64_t)value > -(int64_t)BitMask::INT5)
                    packInt5(value);
                else if ((int64_t)value >= (int64_t)std::numeric_limits<int8_t>::min())
                    packInt8(value);
                else if ((int64_t)value >= (int64_t)std::numeric_limits<int16_t>::min())
                    packInt16(value);
                else if ((int64_t)value >= (int64_t)std::numeric_limits<int32_t>::min())
                    packInt32(value);
                else
                    packInt64(value);
            }
        }

        // ---------- FLOAT format family ----------

        template <typename T>
        auto packFloat(const T& value) ->
            typename std::enable_if<std::is_arithmetic<T>::value && std::is_floating_point<T>::value>::type {
            if (sizeof(T) == sizeof(float))
                packFloat32(value);
            else
                packFloat64(value);
        }

        // ---------- STR format family ----------

        template <typename T>
        void packString(const T& str) {
            packString(str, getStringSize(str));
        }

        template <typename T>
        void packString(const T& str, const size_t len) {
            if (len <= (size_t)BitMask::STR5)
                packString5(str, len);
            else if (len <= std::numeric_limits<uint8_t>::max())
                packString8(str, len);
            else if (len <= std::numeric_limits<uint16_t>::max())
                packString16(str, len);
            else if (len <= std::numeric_limits<uint32_t>::max())
                packString32(str, len);
        }

        // ---------- BIN format family ----------

        void packBinary(const uint8_t* bin, const size_t size) {
            if (size <= std::numeric_limits<uint8_t>::max())
                packBinary8(bin, (uint8_t)size);
            else if (size <= std::numeric_limits<uint16_t>::max())
                packBinary16(bin, (uint16_t)size);
            else if (size <= std::numeric_limits<uint32_t>::max())
                packBinary32(bin, (uint32_t)size);
        }

        // ---------- ARRAY format family ----------

        void packArraySize(const size_t size) {
            if (size < (1 << 4))
                packArraySize4((uint8_t)size);
            else if (size <= std::numeric_limits<uint16_t>::max())
                packArraySize16((uint16_t)size);
            else if (size <= std::numeric_limits<uint32_t>::max())
                packArraySize32((uint32_t)size);
        }

        // ---------- MAP format family ----------

        void packMapSize(const size_t size) {
            if (size < (1 << 4))
                packMapSize4((uint8_t)size);
            else if (size <= std::numeric_limits<uint16_t>::max())
                packMapSize16((uint16_t)size);
            else if (size <= std::numeric_limits<uint32_t>::max())
                packMapSize32((uint32_t)size);
        }

        // ---------- EXT format family ----------

        template <typename T>
        auto packFixExt(const int8_t type, const T value) ->
            typename std::enable_if<std::is_integral<T>::value>::type {
            size_t size = sizeof(T);
            if (size == sizeof(uint8_t))
                packFixExt1(type, value);
            else if (size == sizeof(uint16_t))
                packFixExt2(type, value);
            else if (size == sizeof(uint32_t))
                packFixExt4(type, value);
            else if (size == sizeof(uint64_t))
                packFixExt8(type, value);
        }

        void packFixExt(const int8_t type, const uint64_t value_h, const uint64_t value_l) {
            packFixExt16(type, value_h, value_l);
        }

        void packFixExt(const int8_t type, const uint8_t* ptr, const uint8_t size) {
            if (size == 0) return;
            if (size == sizeof(uint8_t))
                packFixExt1(type, (uint8_t) * (ptr));
            else if (size == sizeof(uint16_t))
                packFixExt2(type, ptr);
            else if (size <= sizeof(uint32_t))
                packFixExt4(type, ptr);
            else if (size <= sizeof(uint64_t))
                packFixExt8(type, ptr);
            else if (size <= sizeof(uint64_t) * 2)
                packFixExt16(type, ptr);
        }

        void packFixExt(const int8_t type, const uint16_t* ptr, const uint8_t size) {
            packFixExt(type, (const uint8_t*)ptr, size);
        }

        void packFixExt(const int8_t type, const uint32_t* ptr, const uint8_t size) {
            packFixExt(type, (const uint8_t*)ptr, size);
        }

        void packFixExt(const int8_t type, const uint64_t* ptr, const uint8_t size) {
            packFixExt(type, (const uint8_t*)ptr, size);
        }

        template <typename T, typename U>
        auto packExt(const int8_t type, const T* ptr, const U size) ->
            typename std::enable_if<std::is_integral<T>::value && std::is_integral<U>::value>::type {
            if (size <= sizeof(uint64_t) * 2)
                packFixExt(type, ptr, size);
            else {
                if (size <= std::numeric_limits<uint8_t>::max())
                    packExtSize8(type, size);
                else if (size <= std::numeric_limits<uint16_t>::max())
                    packExtSize16(type, size);
                else if (size <= std::numeric_limits<uint32_t>::max())
                    packExtSize32(type, size);
                packRawBytes((const uint8_t*)ptr, size);
            }
        }

        void packExt(const object::ext& e) {
            packExt(e.type(), e.data(), e.size());
        }

        // ---------- TIMESTAMP format family ----------

        void packTimestamp(const object::timespec& time) {
            if ((time.tv_sec >> 34) == 0) {
                uint64_t data64 = ((uint64_t)time.tv_nsec << 34) | time.tv_sec;
                if ((data64 & 0xffffffff00000000L) == 0)
                    packTimestamp32((uint32_t)data64);
                else
                    packTimestamp64(data64);
            } else
                packTimestamp96(time.tv_sec, time.tv_nsec);
        }

        /////////////////////////////////////////
        // ---------- msgpack types ---------- //
        /////////////////////////////////////////

        // MessagePack Specification
        // https://github.com/msgpack/msgpack/blob/master/spec.md

        // ---------- NIL format family ----------

        void packNil() {
            packRawByte(Type::NIL);
            ++n_indices;
        }

        void packNil(const object::nil_t& n) {
            (void)n;
            packRawByte(Type::NIL);
            ++n_indices;
        }

        // ---------- BOOL format family ----------

        void packBool(const bool b) {
            packRawByte((uint8_t)Type::BOOL | ((uint8_t)b & (uint8_t)BitMask::BOOL));
            ++n_indices;
        }

        // ---------- INT format family ----------

        void packUInt7(const uint8_t value) {
            packRawByte((uint8_t)Type::UINT7 | (value & (uint8_t)BitMask::UINT7));
            ++n_indices;
        }

        void packUInt8(const uint8_t value) {
            packRawByte(Type::UINT8);
            packRawByte(value);
            ++n_indices;
        }

        void packUInt16(const uint16_t value) {
            packRawByte(Type::UINT16);
            packRawReversed(value);
            ++n_indices;
        }

        void packUInt32(const uint32_t value) {
            packRawByte(Type::UINT32);
            packRawReversed(value);
            ++n_indices;
        }

        void packUInt64(const uint64_t value) {
            packRawByte(Type::UINT64);
            packRawReversed(value);
            ++n_indices;
        }

        void packInt5(const int8_t value) {
            packRawByte((uint8_t)Type::INT5 | ((uint8_t)value & (uint8_t)BitMask::INT5));
            ++n_indices;
        }

        void packInt8(const int8_t value) {
            packRawByte(Type::INT8);
            packRawByte(value);
            ++n_indices;
        }

        void packInt16(const int16_t value) {
            packRawByte(Type::INT16);
            packRawReversed(value);
            ++n_indices;
        }

        void packInt32(const int32_t value) {
            packRawByte(Type::INT32);
            packRawReversed(value);
            ++n_indices;
        }

        void packInt64(const int64_t value) {
            packRawByte(Type::INT64);
            packRawReversed(value);
            ++n_indices;
        }

        // ---------- FLOAT format family ----------

        void packFloat32(const float value) {
            packRawByte(Type::FLOAT32);
            packRawReversed(value);
            ++n_indices;
        }

        void packFloat64(const double value) {
#if ARX_HAVE_LIBSTDCPLUSPLUS >= 201103L  // Have libstdc++11
            packRawByte(Type::FLOAT64);
            packRawReversed(value);
            ++n_indices;
#else
            packFloat32((const float)value);  // Uno, etc. does not support double
#endif  // have libstdc++11
        }

        // ---------- STR format family ----------

        void packString5(const str_t& str) {
            packString5(str, getStringSize(str));
        }
        void packString5(const str_t& str, const size_t len) {
            packString5(str.c_str(), len);
        }
        void packString5(const char* value) {
            packString5(value, getStringSize(value));
        }
        void packString5(const char* value, const size_t len) {
            packRawByte((uint8_t)Type::STR5 | ((uint8_t)len & (uint8_t)BitMask::STR5));
            packRawBytes(value, len);
            ++n_indices;
        }

        void packString8(const str_t& str) {
            packString8(str, getStringSize(str));
        }
        void packString8(const str_t& str, const size_t len) {
            packString8(str.c_str(), len);
        }
        void packString8(const char* value) {
            packString8(value, getStringSize(value));
        }
        void packString8(const char* value, const size_t len) {
            packRawByte(Type::STR8);
            packRawByte((uint8_t)len);
            packRawBytes(value, len);
            ++n_indices;
        }

        void packString16(const str_t& str) {
            packString16(str, getStringSize(str));
        }
        void packString16(const str_t& str, const size_t len) {
            packString16(str.c_str(), len);
        }
        void packString16(const char* value) {
            packString16(value, getStringSize(value));
        }
        void packString16(const char* value, const size_t len) {
            packRawByte(Type::STR16);
            packRawReversed((uint16_t)len);
            packRawBytes(value, len);
            ++n_indices;
        }

        void packString32(const str_t& str) {
            packString32(str, getStringSize(str));
        }
        void packString32(const str_t& str, const size_t len) {
            packString32(str.c_str(), len);
        }
        void packString32(const char* value) {
            packString32(value, getStringSize(value));
        }
        void packString32(const char* value, const size_t len) {
            packRawByte(Type::STR32);
            packRawReversed((uint32_t)len);
            packRawBytes(value, len);
            ++n_indices;
        }

#ifdef ARDUINO

        void packString5(const __FlashStringHelper* str) {
            packString5(str, getStringSize(str));
        }
        void packString5(const __FlashStringHelper* str, const size_t len) {
            packRawByte((uint8_t)Type::STR5 | ((uint8_t)len & (uint8_t)BitMask::STR5));
            packFlashString(str);
            ++n_indices;
        }

        void packString8(const __FlashStringHelper* str) {
            packString8(str, getStringSize(str));
        }
        void packString8(const __FlashStringHelper* str, const size_t len) {
            packRawByte(Type::STR8);
            packRawByte((uint8_t)len);
            packFlashString(str);
            ++n_indices;
        }

        void packString16(const __FlashStringHelper* str) {
            packString16(str, getStringSize(str));
        }
        void packString16(const __FlashStringHelper* str, const size_t len) {
            packRawByte(Type::STR16);
            packRawReversed((uint16_t)len);
            packFlashString(str);
            ++n_indices;
        }

        void packString32(const __FlashStringHelper* str) {
            packString32(str, getStringSize(str));
        }
        void packString32(const __FlashStringHelper* str, const size_t len) {
            packRawByte(Type::STR32);
            packRawReversed((uint32_t)len);
            packFlashString(str);
            ++n_indices;
        }

#endif

        // ---------- BIN format family ----------

        void packBinary8(const uint8_t* value, const uint8_t size) {
            packRawByte(Type::BIN8);
            packRawByte(size);
            packRawBytes(value, size);
            ++n_indices;
        }

        void packBinary16(const uint8_t* value, const uint16_t size) {
            packRawByte(Type::BIN16);
            packRawReversed(size);
            packRawBytes(value, size);
            ++n_indices;
        }

        void packBinary32(const uint8_t* value, const uint32_t size) {
            packRawByte(Type::BIN32);
            packRawReversed(size);
            packRawBytes(value, size);
            ++n_indices;
        }

        // ---------- ARRAY format family ----------

        void packArraySize4(const uint8_t value) {
            packRawByte((uint8_t)Type::ARRAY4 | (value & (uint8_t)BitMask::ARRAY4));
            ++n_indices;
        }

        void packArraySize16(const uint16_t value) {
            packRawByte(Type::ARRAY16);
            packRawReversed(value);
            ++n_indices;
        }

        void packArraySize32(const uint32_t value) {
            packRawByte(Type::ARRAY32);
            packRawReversed(value);
            ++n_indices;
        }

        // ---------- MAP format family ----------

        void packMapSize4(const uint8_t value) {
            packRawByte((uint8_t)Type::MAP4 | (value & (uint8_t)BitMask::MAP4));
            ++n_indices;
        }

        void packMapSize16(const uint16_t value) {
            packRawByte(Type::MAP16);
            packRawReversed(value);
            ++n_indices;
        }

        void packMapSize32(const uint32_t value) {
            packRawByte(Type::MAP32);
            packRawReversed(value);
            ++n_indices;
        }

        // ---------- EXT format family ----------

        void packFixExt1(const int8_t type, const uint8_t value) {
            packRawByte(Type::FIXEXT1);
            packRawByte((uint8_t)type);
            packRawByte(value);
            ++n_indices;
        }

        void packFixExt2(const int8_t type, const uint16_t value) {
            packRawByte(Type::FIXEXT2);
            packRawByte((uint8_t)type);
            packRawBytes((const uint8_t*)&value, sizeof(value));
            ++n_indices;
        }

        void packFixExt2(const int8_t type, const uint8_t* ptr) {
            packRawByte(Type::FIXEXT2);
            packRawByte((uint8_t)type);
            packRawBytes((const uint8_t*)ptr, 2);
            ++n_indices;
        }

        void packFixExt2(const int8_t type, const uint16_t* ptr) {
            packFixExt2(type, (const uint8_t*)ptr);
        }

        void packFixExt4(const int8_t type, const uint32_t value) {
            packRawByte(Type::FIXEXT4);
            packRawByte((uint8_t)type);
            packRawBytes((const uint8_t*)&value, sizeof(value));
            ++n_indices;
        }

        void packFixExt4(const int8_t type, const uint8_t* ptr) {
            packRawByte(Type::FIXEXT4);
            packRawByte((uint8_t)type);
            packRawBytes((const uint8_t*)ptr, 4);
            ++n_indices;
        }

        void packFixExt4(const int8_t type, const uint32_t* ptr) {
            packFixExt4(type, (const uint8_t*)ptr);
        }

        void packFixExt8(const int8_t type, const uint64_t value) {
            packRawByte(Type::FIXEXT8);
            packRawByte((uint8_t)type);
            packRawBytes((const uint8_t*)&value, sizeof(value));
            ++n_indices;
        }

        void packFixExt8(const int8_t type, const uint8_t* ptr) {
            packRawByte(Type::FIXEXT8);
            packRawByte((uint8_t)type);
            packRawBytes((const uint8_t*)ptr, 8);
            ++n_indices;
        }

        void packFixExt8(const int8_t type, const uint64_t* ptr) {
            packFixExt8(type, (const uint8_t*)ptr);
        }

        void packFixExt16(const int8_t type, const uint64_t value_h, const uint64_t value_l) {
            packRawByte(Type::FIXEXT16);
            packRawByte((uint8_t)type);
            packRawBytes((const uint8_t*)&value_h, sizeof(value_h));
            packRawBytes((const uint8_t*)&value_l, sizeof(value_l));
            ++n_indices;
        }

        void packFixExt16(const int8_t type, const uint8_t* ptr) {
            packRawByte(Type::FIXEXT16);
            packRawByte((uint8_t)type);
            packRawBytes((const uint8_t*)ptr, 16);
            ++n_indices;
        }

        void packFixExt16(const int8_t type, const uint64_t* ptr) {
            packFixExt16(type, (const uint8_t*)ptr);
        }

        void packExtSize8(const int8_t type, const uint8_t size) {
            packRawByte(Type::EXT8);
            packRawByte(size);
            packRawByte((uint8_t)type);
            ++n_indices;
        }

        void packExtSize16(const int8_t type, const uint16_t size) {
            packRawByte(Type::EXT16);
            packRawReversed(size);
            packRawByte((uint8_t)type);
            ++n_indices;
        }

        void packExtSize32(const int8_t type, const uint32_t size) {
            packRawByte(Type::EXT32);
            packRawReversed(size);
            packRawByte((uint8_t)type);
            ++n_indices;
        }

        // ---------- TIMESTAMP format family ----------

        void packTimestamp32(const uint32_t unix_time_sec) {
            packRawByte(Type::FIXEXT4);
            packRawByte((uint8_t)-1);
            packRawReversed(unix_time_sec);
            ++n_indices;
        }

        void packTimestamp64(const uint64_t unix_time) {
            packRawByte(Type::FIXEXT8);
            packRawByte((uint8_t)-1);
            packRawReversed(unix_time);
            ++n_indices;
        }

        void packTimestamp64(const uint64_t unix_time_sec, const uint32_t unix_time_nsec) {
            uint64_t utime = ((unix_time_nsec & 0x00000003FFFFFFFF) << 34) | (uint64_t)(unix_time_sec & 0x3FFFFFFFF);
            packTimestamp64(utime);
        }

        void packTimestamp96(const int64_t unix_time_sec, const uint32_t unix_time_nsec) {
            packExtSize8(-1, 12);
            packRawReversed(unix_time_nsec);
            packRawReversed(unix_time_sec);
            ++n_indices;
        }

    private:
        void packRawByte(const uint8_t value) {
            buffer.emplace_back(value);
        }

        void packRawByte(const Type& type) {
            buffer.emplace_back((uint8_t)type);
        }

        void packRawBytes(const uint8_t* value, const size_t size) {
            for (size_t i = 0; i < size; ++i) buffer.emplace_back(value[i]);
        }

        void packRawBytes(const char* value, const size_t size) {
            for (size_t i = 0; i < size; ++i) buffer.emplace_back(value[i]);
        }

        template <typename DataType>
        void packRawReversed(const DataType& value) {
            const auto size = sizeof(DataType);
            for (size_t i = 0; i < size; ++i) {
                buffer.emplace_back(((uint8_t*)&value)[size - 1 - i]);
            }
        }

        template <typename A>
        void packArrayContainer(const A& arr) {
            packArraySize(arr.size());
            for (const auto& a : arr) pack(a);
        }

        template <typename M>
        void packMapContainer(const M& mp) {
            packMapSize(mp.size());
            for (const auto& m : mp) {
                pack(m.first);
                pack(m.second);
            }
        }

        size_t getStringSize(const str_t& str) const {
            return str.length();
        }

        size_t getStringSize(const char* str) const {
            return strlen(str);
        }

#ifdef ARDUINO

        size_t getStringSize(const __FlashStringHelper* str) const {
            return strlen_P((const char*)str);
        }

        void packFlashString(const __FlashStringHelper* str) {
            char* p = (char*)str;
            while (1) {
                uint8_t c = pgm_read_byte(p++);
                if (c == 0) break;
                packRawByte(c);
            }
        }

#endif
    };

}  // namespace msgpack
}  // namespace arduino

#endif  // HT_SERIAL_MSGPACK_PACKER_H

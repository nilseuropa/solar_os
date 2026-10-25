#pragma once
#ifndef HT_SERIAL_MSGPACK_UNPACKER_H
#define HT_SERIAL_MSGPACK_UNPACKER_H

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
#include <DebugLog.h>

namespace arduino {
namespace msgpack {

    class Unpacker {
        uint8_t* raw_data {nullptr};
        idx_t indices;
        size_t curr_index {0};
        bool b_size_matched {false};
        bool b_decode_success {false};

#define MSGPACK_DECODABLE_CHECK(T)                 \
    if (curr_index >= indices.size()) {            \
        LOG_ERROR(F("no more decodable objects")); \
        b_decode_success = false;                  \
        return T();                                \
    } else {                                       \
        b_decode_success = true;                   \
    }

    public:
        void reserve_indices(const size_t size) {
            indices.reserve(size);
        }

        bool feed(const uint8_t* data, const size_t size) {
            raw_data = (uint8_t*)data;
            for (size_t i = 0; i < size; i += getElementSize(indices.size() - 1)) indices.emplace_back(i);
            const size_t decoded_size = indices.back() + getElementSize(indices.size() - 1);
            b_size_matched = (size == decoded_size);
            if (!b_size_matched) LOG_ERROR(F("decoded binary size"), decoded_size, F("not matched to"), size);
            return b_size_matched;
        }

        template <typename First, typename... Rest>
        bool deserialize(First& first, Rest&&... rest) {
            if (!b_size_matched || indices.empty()) {
                LOG_WARN(F("correct binary data was not supplied yet"));
                b_decode_success = false;
                return b_decode_success;
            }
            if (curr_index >= indices.size()) {
                LOG_ERROR(F("too many args: obj index overflow"));
                b_decode_success = false;
                return b_decode_success;
            }
            if (!unpack(first)) {
                return b_decode_success;
            }

            return deserialize(std::forward<Rest>(rest)...);
        }

        bool deserialize() {
            if (curr_index > indices.size()) {
                LOG_ERROR("index overflow:", curr_index, "must be <=", indices.size());
                b_decode_success = false;
            }

            // do nothing if curr_index is <= indices.size()
            // - curr_index == indices.size()
            // - to be decoded after this deserialize()

            return b_decode_success;
        }

#ifdef ARDUINOJSON_VERSION

    private:
        bool deserialize_arduinojson(JsonDocument& doc) {
            auto err = deserializeMsgPack(doc, raw_data);
            if (err) {
                LOG_ERROR("deserializeJson() faled: ", err.c_str());
                b_decode_success = false;
            } else {
                b_decode_success = true;
            }
            return b_decode_success;
        }

    public:
        template <size_t N>
        bool deserialize(StaticJsonDocument<N>& doc) {
            return deserialize_arduinojson(doc);
        }
        bool deserialize(DynamicJsonDocument& doc) {
            return deserialize_arduinojson(doc);
        }

#endif  // ARDUINOJSON_VERSION

        template <typename... Args>
        bool from_array(Args&&... args) {
            static arr_size_t sz;
            return deserialize(sz, std::forward<Args>(args)...);
        }

        template <typename... Args>
        bool from_map(Args&&... args) {
            if ((sizeof...(args) % 2) == 0) {
                static map_size_t sz;
                return deserialize(sz, std::forward<Args>(args)...);
            } else {
                LOG_ERROR(F("arg size must be even for map:"), sizeof...(args));
                b_decode_success = false;
                return b_decode_success;
            }
        }

        template <typename... Ts>
        void to_tuple(std::tuple<Ts...>& t) {
            to_tuple(std::make_index_sequence<sizeof...(Ts)>(), t);
        }
        template <typename... Ts, size_t... Is>
        void to_tuple(std::index_sequence<Is...>&&, std::tuple<Ts...>& t) {
            size_t i {0};
            idx_t {(unpack(std::get<Is>(t)), i++)...};
        }
        void to_tuple() {
            // nothing to do
        }

        bool decode_ready() const {
            return b_size_matched;
        }
        bool decoded() const {
            return b_decode_success;
        }
        size_t size() const {
            return indices.size();
        }
        void index(const size_t i) {
            curr_index = i;
        }
        size_t index() const {
            return curr_index;
        }
        void clear() {
            indices.clear();
            index(0);
            b_size_matched = false;
            b_decode_success = false;
            raw_data = nullptr;
        }

        /////////////////////////////////////////////////
        // ---------- msgpack type adaptors ---------- //
        /////////////////////////////////////////////////

        // adaptation of types to msgpack
        // https://github.com/msgpack/msgpack-c/wiki/v2_0_cpp_adaptor

        // ---------- NIL format family ----------
        // - N/A

        template <typename T>
        auto unpack(T& value) -> typename std::enable_if<std::is_same<T, object::nil_t>::value, bool>::type {
            value = unpackNil();
            return b_decode_success;
        }

        // ---------- BOOL format family ----------
        // - bool

        template <typename T>
        auto unpack(T& value) -> typename std::enable_if<std::is_same<T, bool>::value, bool>::type {
            value = unpackBool();
            return b_decode_success;
        }

        // ---------- INT format family ----------
        // - char (signed/unsigned)
        // - ints (signed/unsigned)

        template <typename T>
        auto unpack(T& value) -> typename std::enable_if<
            std::is_arithmetic<T>::value && std::is_integral<T>::value && !std::is_same<T, bool>::value
                && !std::is_same<typename std::remove_cv<T>::type, char*>::value && !std::is_signed<T>::value,
            bool>::type {
            value = unpackUInt<T>();
            return b_decode_success;
        }

        template <typename T>
        auto unpack(T& value) -> typename std::enable_if<
            std::is_arithmetic<T>::value && std::is_integral<T>::value && !std::is_same<T, bool>::value
                && !std::is_same<typename std::remove_cv<T>::type, char*>::value && std::is_signed<T>::value,
            bool>::type {
            value = unpackInt<T>();
            return b_decode_success;
        }

        // ---------- FLOAT format family ----------
        // - float
        // - double

        template <typename T>
        auto unpack(T& value) ->
            typename std::enable_if<std::is_arithmetic<T>::value && std::is_floating_point<T>::value, bool>::type {
            value = unpackFloat<T>();
            return b_decode_success;
        }

        // ---------- STRING format family ----------
        // - char*
        // - char[]
        // - std::string

        bool unpack(str_t& str) {
            str = unpackString();
            return b_decode_success;
        }

        // ---------- BIN format family ----------
        // - unsigned char*
        // - unsigned char[]
        // - std::vector<char>
        // - std::vector<unsigned char>
        // - std::array<char>
        // - std::array<unsigned char>

#if ARX_HAVE_LIBSTDCPLUSPLUS >= 201103L  // Have libstdc++11

        template <typename T>
        auto unpack(bin_t<T>& bin) ->
            typename std::enable_if<std::is_same<T, char>::value || std::is_same<T, uint8_t>::value, bool>::type {
            bin = unpackBinary<T>();
            return b_decode_success;
        }

#else

        template <typename T, size_t N>
        auto unpack(arx::stdx::vector<T, N>& bin)  // bin_t<T>
            -> typename std::enable_if<std::is_same<T, char>::value || std::is_same<T, uint8_t>::value, bool>::type {
            bin = unpackBinary<T>();
            return b_decode_success;
        }

#endif  // Do not have libstdc++11

#if ARX_HAVE_LIBSTDCPLUSPLUS >= 201103L  // Have libstdc++11

        template <typename T, size_t N>
        auto unpack(std::array<T, N>& bin) ->
            typename std::enable_if<std::is_same<T, char>::value || std::is_same<T, uint8_t>::value, bool>::type {
            bin = unpackBinary<T, N>();
            return b_decode_success;
        }

#endif  // Do not have libstdc++11

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
        auto unpack(arr_t<T>& arr) ->
            typename std::enable_if<!std::is_same<T, char>::value && !std::is_same<T, uint8_t>::value, bool>::type {
            unpackArrayContainerArray(arr);
            arr.shrink_to_fit();
            return b_decode_success;
        }

#if ARX_HAVE_LIBSTDCPLUSPLUS >= 201103L  // Have libstdc++11

        template <typename T, size_t N>
        auto unpack(std::array<T, N>& arr) ->
            typename std::enable_if<!std::is_same<T, char>::value && !std::is_same<T, uint8_t>::value, bool>::type {
            const size_t size = unpackArraySize();
            if (N == size)
                for (auto& a : arr) unpack(a);
            else {
                LOG_ERROR(F("array size mismatch:"), size, F("must be"), N);
                b_decode_success = false;
            }
            return b_decode_success;
        }

        template <typename T>
        bool unpack(std::deque<T>& arr) {
            unpackArrayContainerArray(arr);
            arr.shrink_to_fit();
            return b_decode_success;
        }

        template <typename T, typename U>
        bool unpack(std::pair<T, U>& arr) {
            const size_t size = unpackArraySize();
            if (size == 2) {
                unpack(arr.first);
                unpack(arr.second);
            } else {
                LOG_ERROR(F("array size mismatch:"), size, F("must be"), 2);
                b_decode_success = false;
            }
            return b_decode_success;
        }

        template <typename... Args>
        bool unpack(std::tuple<Args...>& t) {
            const size_t size = unpackArraySize();
            if (sizeof...(Args) == size) {
                to_tuple(t);
            } else {
                LOG_ERROR(F("array size mismatch:"), size, F("must be"), sizeof...(Args));
                b_decode_success = false;
            }
            return b_decode_success;
        }

        template <typename T>
        bool unpack(std::list<T>& arr) {
            unpackArrayContainerArray(arr);
            return b_decode_success;
        }

        template <typename T>
        bool unpack(std::forward_list<T>& arr) {
            const size_t arr_size = std::distance(arr.begin(), arr.end());
            const size_t size = unpackArraySize();
            if (size == 0) {
                LOG_ERROR(F("array size mismatch:"), size, F("must be"), arr_size);
                b_decode_success = false;
            } else if (arr_size == size)
                for (auto& a : arr) unpack(a);
            else {
                arr.clear();
                for (size_t a = 0; a < size; ++a) {
                    T t;
                    unpack(t);
                    arr.emplace_after(std::next(arr.before_begin(), a), t);
                }
            }
            return b_decode_success;
        }

        template <typename T>
        bool unpack(std::set<T>& arr) {
            unpackArrayContainerSet(arr);
            return b_decode_success;
        }

        template <typename T>
        bool unpack(std::multiset<T>& arr) {
            unpackArrayContainerSet(arr);
            return b_decode_success;
        }

        template <typename T>
        bool unpack(std::unordered_set<T>& arr) {
            unpackArrayContainerSet(arr);
            return b_decode_success;
        }

        template <typename T>
        bool unpack(std::unordered_multiset<T>& arr) {
            unpackArrayContainerSet(arr);
            return b_decode_success;
        }

#endif  // Do not have libstdc++11

        // ---------- MAP format family ----------
        // - std::map
        // - std::multimap
        // - std::unordered_map *
        // - std::unordered_multimap *
        // * : not supported in arduino

        template <typename T, typename U>
        bool unpack(map_t<T, U>& mp) {
            unpackMapContainer(mp);
            return b_decode_success;
        }

#if ARX_HAVE_LIBSTDCPLUSPLUS >= 201103L  // Have libstdc++11

        template <typename T, typename U>
        bool unpack(std::multimap<T, U>& mp) {
            unpackMapContainer(mp);
            return b_decode_success;
        }

        template <typename T, typename U>
        bool unpack(std::unordered_map<T, U>& mp) {
            unpackMapContainer(mp);
            return b_decode_success;
        }

        template <typename T, typename U>
        bool unpack(std::unordered_multimap<T, U>& mp) {
            unpackMapContainer(mp);
            return b_decode_success;
        }

#endif  // Do not have libstdc++11

        // ---------- EXT format family ----------

        bool unpack(object::ext& e) {
            e = unpackExt();
            return b_decode_success;
        }

        // ---------- TIMESTAMP format family ----------

        bool unpack(object::timespec& t) {
            t = unpackTimestamp();
            return b_decode_success;
        }

        // ---------- CUSTOM format ----------

        template <typename C>
        auto unpack(C& c) -> typename std::enable_if<has_from_msgpack<C, Unpacker&>::value, bool>::type {
            c.from_msgpack(*this);
            return b_decode_success;
        }

        // ---------- Array/Map Size format ----------

        bool unpack(arr_size_t& t) {
            t = arr_size_t(unpackArraySize());
            return b_decode_success;
        }

        bool unpack(map_size_t& t) {
            t = map_size_t(unpackMapSize());
            return b_decode_success;
        }

        /////////////////////////////////////////////////////
        // ---------- msgpack types abstraction ---------- //
        /////////////////////////////////////////............

        // ---------- INT format family ----------

        template <typename T = uint64_t>
        auto unpackUInt() -> typename std::enable_if<
            std::is_arithmetic<T>::value && std::is_integral<T>::value && !std::is_same<T, bool>::value
                && !std::is_same<typename std::remove_cv<T>::type, char*>::value && !std::is_signed<T>::value,
            T>::type {
            MSGPACK_DECODABLE_CHECK(T);
            switch (getType()) {
                case Type::UINT7:
                    return (T)unpackUInt7();
                case Type::UINT8:
                    return (T)unpackUInt8();
                case Type::UINT16:
                    return (T)unpackUInt16();
                case Type::UINT32:
                    return (T)unpackUInt32();
                case Type::UINT64:
                    return (T)unpackUInt64();
                default:
                    LOG_ERROR("type error");
                    return type_error<T>();
            }
        }

        template <typename T = int64_t>
        auto unpackInt() -> typename std::enable_if<
            std::is_arithmetic<T>::value && std::is_integral<T>::value && !std::is_same<T, bool>::value
                && !std::is_same<typename std::remove_cv<T>::type, char*>::value && std::is_signed<T>::value,
            T>::type {
            MSGPACK_DECODABLE_CHECK(T);
            switch (getType()) {
                case Type::INT5:
                    return (T)unpackInt5();
                case Type::INT8:
                    return (T)unpackInt8();
                case Type::INT16:
                    return (T)unpackInt16();
                case Type::INT32:
                    return (T)unpackInt32();
                case Type::INT64:
                    return (T)unpackInt64();
                case Type::UINT7:
                    return (T)unpackUInt7();
                case Type::UINT8:
                    return uint_to_int<T, uint8_t, int8_t>(unpackUInt8());
                case Type::UINT16:
                    return uint_to_int<T, uint16_t, int16_t>(unpackUInt16());
                case Type::UINT32:
                    return uint_to_int<T, uint32_t, int32_t>(unpackUInt32());
                case Type::UINT64:
                    return uint_to_int<T, uint64_t, int64_t>(unpackUInt64());
                default:
                    LOG_ERROR("type error");
                    return type_error<T>();
            }
        }

        // ---------- FLOAT format family ----------

        template <typename T = double>
        auto unpackFloat() ->
            typename std::enable_if<std::is_arithmetic<T>::value && std::is_floating_point<T>::value, T>::type {
            MSGPACK_DECODABLE_CHECK(T);
            switch (getType()) {
                case Type::UINT7:
                    return (T)unpackUInt7();
                case Type::UINT8:
                    return (T)unpackUInt8();
                case Type::UINT16:
                    return (T)unpackUInt16();
                case Type::UINT32:
                    return (T)unpackUInt32();
                case Type::UINT64:
                    return (T)unpackUInt64();
                case Type::INT5:
                    return (T)unpackInt5();
                case Type::INT8:
                    return (T)unpackInt8();
                case Type::INT16:
                    return (T)unpackInt16();
                case Type::INT32:
                    return (T)unpackInt32();
                case Type::INT64:
                    return (T)unpackInt64();
                case Type::FLOAT32:
                    return (T)unpackFloat32();
                case Type::FLOAT64:
                    return (T)unpackFloat64();
                default:
                    LOG_ERROR("type error");
                    return type_error<T>();
            }
        }

        // ---------- STR format family ----------

        str_t unpackString() {
            MSGPACK_DECODABLE_CHECK(str_t);
            switch (getType()) {
                case Type::STR5:
                    return unpackStringUnchecked5();
                case Type::STR8:
                    return unpackStringUnchecked8();
                case Type::STR16:
                    return unpackStringUnchecked16();
                case Type::STR32:
                    return unpackStringUnchecked32();
                default:
                    LOG_ERROR("type error");
                    return type_error<str_t>();
            }
        }

        // ---------- BIN format family ----------

        template <typename T = uint8_t>
        auto unpackBinary() ->
            typename std::enable_if<std::is_same<T, char>::value || std::is_same<T, uint8_t>::value, bin_t<T>>::type {
            MSGPACK_DECODABLE_CHECK(bin_t<T>);
            switch (getType()) {
                case Type::BIN8:
                    return unpackBinaryUnchecked8<T>();
                case Type::BIN16:
                    return unpackBinaryUnchecked16<T>();
                case Type::BIN32:
                    return unpackBinaryUnchecked32<T>();
                default:
                    LOG_ERROR("type error");
                    return type_error<bin_t<T>>();
            }
        }

#if ARX_HAVE_LIBSTDCPLUSPLUS >= 201103L  // Have libstdc++11

        template <typename T, size_t N>
        auto unpackBinary() -> typename std::
            enable_if<std::is_same<T, char>::value || std::is_same<T, uint8_t>::value, std::array<T, N>>::type {
            using t = std::array<T, N>;
            MSGPACK_DECODABLE_CHECK(t);
            switch (getType()) {
                case Type::BIN8:
                    return unpackBinaryUnchecked8<T, N>();
                case Type::BIN16:
                    return unpackBinaryUnchecked16<T, N>();
                case Type::BIN32:
                    return unpackBinaryUnchecked32<T, N>();
                default:
                    LOG_ERROR("type error");
                    return type_error<std::array<T, N>>();
            }
        }

#endif  // Do not have libstdc++11

        // ---------- ARRAY format family ----------

        size_t unpackArraySize() {
            MSGPACK_DECODABLE_CHECK(size_t);
            switch (getType()) {
                case Type::ARRAY4:
                    return unpackArraySizeUnchecked4();
                case Type::ARRAY16:
                    return unpackArraySizeUnchecked16();
                case Type::ARRAY32:
                    return unpackArraySizeUnchecked32();
                default:
                    LOG_ERROR("type error");
                    return type_error<size_t>();
            }
        }

        // ---------- MAP format family ----------

        size_t unpackMapSize() {
            MSGPACK_DECODABLE_CHECK(size_t);
            switch (getType()) {
                case Type::MAP4:
                    return unpackMapSizeUnchecked4();
                case Type::MAP16:
                    return unpackMapSizeUnchecked16();
                case Type::MAP32:
                    return unpackMapSizeUnchecked32();
                default:
                    LOG_ERROR("type error");
                    return type_error<size_t>();
            }
        }

        // ---------- EXT format family ----------

        object::ext unpackExt() {
            MSGPACK_DECODABLE_CHECK(object::ext);
            switch (getType()) {
                case Type::FIXEXT1:
                    return unpackFixExtUnchecked1();
                case Type::FIXEXT2:
                    return unpackFixExtUnchecked2();
                case Type::FIXEXT4:
                    return unpackFixExtUnchecked4();
                case Type::FIXEXT8:
                    return unpackFixExtUnchecked8();
                case Type::FIXEXT16:
                    return unpackFixExtUnchecked16();
                case Type::EXT8:
                    return unpackExtUnchecked8();
                case Type::EXT16:
                    return unpackExtUnchecked16();
                case Type::EXT32:
                    return unpackExtUnchecked32();
                default:
                    LOG_ERROR("type error");
                    return type_error<object::ext>();
            }
        }

        // ---------- TIMESTAMP format family ----------

        object::timespec unpackTimestamp() {
            // because only timestamp has mutiple condition to satisfy,
            // used isTimestampXX() instead of switch + type tag
            MSGPACK_DECODABLE_CHECK(object::timespec);
            if (isTimestamp32())
                return unpackTimestampUnchecked32();
            else if (isTimestamp64())
                return unpackTimestampUnchecked64();
            else if (isTimestamp96())
                return unpackTimestampUnchecked96();
            else {
                LOG_ERROR("type error");
                return type_error<object::timespec>();
            }
        }

        //////////////////////////////////////////////////////
        // ---------- msgpack types with checker ---------- //
        //////////////////////////////////////////////////////

        // ---------- NIL format family ----------

        bool unpackNil() {
            MSGPACK_DECODABLE_CHECK(bool);
            if (isNil())
                return unpackNilUnchecked();
            else
                return type_error<bool>(Type::NIL);
        }

        // ---------- BOOL format family ----------

        bool unpackBool() {
            MSGPACK_DECODABLE_CHECK(bool);
            if (isBool())
                return unpackBoolUnchecked();
            else
                return type_error<bool>(Type::BOOL);
        }

        // ---------- INT format family ----------

        uint8_t unpackUInt7() {
            MSGPACK_DECODABLE_CHECK(uint8_t);
            if (isUInt7())
                return unpackUIntUnchecked7();
            else
                return type_error<uint8_t>(Type::UINT7);
        }

        uint8_t unpackUInt8() {
            MSGPACK_DECODABLE_CHECK(uint8_t);
            if (isUInt8())
                return unpackUIntUnchecked8();
            else
                return type_error<uint8_t>(Type::UINT8);
        }

        uint16_t unpackUInt16() {
            MSGPACK_DECODABLE_CHECK(uint16_t);
            if (isUInt16())
                return unpackUIntUnchecked16();
            else
                return type_error<uint16_t>(Type::UINT16);
        }

        uint32_t unpackUInt32() {
            MSGPACK_DECODABLE_CHECK(uint32_t);
            if (isUInt32())
                return unpackUIntUnchecked32();
            else
                return type_error<uint32_t>(Type::UINT32);
        }

        uint64_t unpackUInt64() {
            MSGPACK_DECODABLE_CHECK(uint64_t);
            if (isUInt64())
                return unpackUIntUnchecked64();
            else
                return type_error<uint64_t>(Type::UINT64);
        }

        int8_t unpackInt5() {
            MSGPACK_DECODABLE_CHECK(int8_t);
            if (isInt5())
                return unpackIntUnchecked5();
            else
                return type_error<int8_t>(Type::INT5);
        }

        int8_t unpackInt8() {
            MSGPACK_DECODABLE_CHECK(int8_t);
            if (isInt8())
                return unpackIntUnchecked8();
            else
                return type_error<int8_t>(Type::INT8);
        }

        int16_t unpackInt16() {
            MSGPACK_DECODABLE_CHECK(int16_t);
            if (isInt16())
                return unpackIntUnchecked16();
            else
                return type_error<int16_t>(Type::INT16);
        }

        int32_t unpackInt32() {
            MSGPACK_DECODABLE_CHECK(int32_t);
            if (isInt32())
                return unpackIntUnchecked32();
            else
                return type_error<int32_t>(Type::INT32);
        }

        int64_t unpackInt64() {
            MSGPACK_DECODABLE_CHECK(int64_t);
            if (isInt64())
                return unpackIntUnchecked64();
            else
                return type_error<int64_t>(Type::INT64);
        }

        // ---------- FLOAT format family ----------

        float unpackFloat32() {
            MSGPACK_DECODABLE_CHECK(float);
            if (isFloat32())
                return unpackFloatUnchecked32();
            else
                return type_error<float>(Type::FLOAT32);
        }

        double unpackFloat64() {
            MSGPACK_DECODABLE_CHECK(double);
#if ARX_HAVE_LIBSTDCPLUSPLUS >= 201103L  // Have libstdc++11
            if (isFloat64())
                return unpackFloatUnchecked64();
            else
                return type_error<double>(Type::FLOAT64);
#else
            if (isFloat32() || isFloat64())
                return unpackFloat32();  // AVR does not support double
            else
                return type_error<float>(Type::FLOAT64);
#endif
        }

        // ---------- STR format family ----------

        str_t unpackString5() {
            MSGPACK_DECODABLE_CHECK(str_t);
            if (isStr5())
                return unpackStringUnchecked5();
            else
                return type_error<str_t>(Type::STR5);
        }

        str_t unpackString8() {
            MSGPACK_DECODABLE_CHECK(str_t);
            if (isStr8())
                return unpackStringUnchecked8();
            else
                return type_error<str_t>(Type::STR8);
        }

        str_t unpackString16() {
            str_t str("");
            MSGPACK_DECODABLE_CHECK(str_t);
            if (isStr16())
                return unpackStringUnchecked16();
            else
                return type_error<str_t>(Type::STR16);
        }

        str_t unpackString32() {
            MSGPACK_DECODABLE_CHECK(str_t);
            if (isStr32())
                return unpackStringUnchecked32();
            else
                return type_error<str_t>(Type::STR32);
        }

        // ---------- BIN format family ----------

        template <typename T = uint8_t>
        auto unpackBinary8() ->
            typename std::enable_if<std::is_same<T, char>::value || std::is_same<T, uint8_t>::value, bin_t<T>>::type {
            MSGPACK_DECODABLE_CHECK(bin_t<T>);
            if (isBin8())
                return unpackBinaryUnchecked8<T>();
            else
                return type_error<bin_t<T>>(Type::BIN8);
        }

        template <typename T = uint8_t>
        auto unpackBinary16() ->
            typename std::enable_if<std::is_same<T, char>::value || std::is_same<T, uint8_t>::value, bin_t<T>>::type {
            MSGPACK_DECODABLE_CHECK(bin_t<T>);
            if (isBin16())
                return unpackBinaryUnchecked16<T>();
            else
                return type_error<bin_t<T>>(Type::BIN16);
        }

        template <typename T = uint8_t>
        auto unpackBinary32() ->
            typename std::enable_if<std::is_same<T, char>::value || std::is_same<T, uint8_t>::value, bin_t<T>>::type {
            MSGPACK_DECODABLE_CHECK(bin_t<T>);
            if (isBin32())
                return unpackBinaryUnchecked32<T>();
            else
                return type_error<bin_t<T>>(Type::BIN32);
        }

#if ARX_HAVE_LIBSTDCPLUSPLUS >= 201103L  // Have libstdc++11

        template <typename T, size_t N>
        auto unpackBinary8() -> typename std::
            enable_if<std::is_same<T, char>::value || std::is_same<T, uint8_t>::value, std::array<T, N>>::type {
            using t = std::array<T, N>;
            MSGPACK_DECODABLE_CHECK(t);
            if (isBin8())
                return unpackBinaryUnchecked8<T, N>();
            else
                return type_error<std::array<T, N>>(Type::BIN8);
        }

        template <typename T, size_t N>
        auto unpackBinary16() -> typename std::
            enable_if<std::is_same<T, char>::value || std::is_same<T, uint8_t>::value, std::array<T, N>>::type {
            using t = std::array<T, N>;
            MSGPACK_DECODABLE_CHECK(t);
            if (isBin16())
                return unpackBinaryUnchecked16<T, N>();
            else
                return type_error<std::array<T, N>>(Type::BIN16);
        }

        template <typename T, size_t N>
        auto unpackBinary32() -> typename std::
            enable_if<std::is_same<T, char>::value || std::is_same<T, uint8_t>::value, std::array<T, N>>::type {
            using t = std::array<T, N>;
            MSGPACK_DECODABLE_CHECK(t);
            if (isBin32())
                return unpackBinaryUnchecked32<T, N>();
            else
                return type_error<std::array<T, N>>(Type::BIN32);
        }

#endif  // Do not have libstdc++11

        // ---------- ARRAY format family ----------

        size_t unpackArraySize4() {
            MSGPACK_DECODABLE_CHECK(size_t);
            if (isArray4())
                return unpackArraySizeUnchecked4();
            else
                return type_error<size_t>(Type::ARRAY4);
        }

        size_t unpackArraySize16() {
            MSGPACK_DECODABLE_CHECK(size_t);
            if (isArray16())
                return unpackArraySizeUnchecked16();
            else
                return type_error<size_t>(Type::ARRAY16);
        }

        size_t unpackArraySize32() {
            MSGPACK_DECODABLE_CHECK(size_t);
            if (isArray32())
                return unpackArraySizeUnchecked32();
            else
                return type_error<size_t>(Type::ARRAY32);
        }

        // ---------- MAP format family ----------

        size_t unpackMapSize4() {
            MSGPACK_DECODABLE_CHECK(size_t);
            if (isMap4())
                return unpackMapSizeUnchecked4();
            else
                return type_error<size_t>(Type::MAP4);
        }

        size_t unpackMapSize16() {
            MSGPACK_DECODABLE_CHECK(size_t);
            if (isMap16())
                return unpackMapSizeUnchecked16();
            else
                return type_error<size_t>(Type::MAP16);
        }

        size_t unpackMapSize32() {
            MSGPACK_DECODABLE_CHECK(size_t);
            if (isMap32())
                return unpackMapSizeUnchecked32();
            else
                return type_error<size_t>(Type::MAP32);
        }

        // ---------- EXT format family ----------

        object::ext unpackFixExt1() {
            MSGPACK_DECODABLE_CHECK(object::ext);
            if (isFixExt1())
                return unpackFixExtUnchecked1();
            else
                return type_error<object::ext>(Type::FIXEXT1);
        }

        object::ext unpackFixExt2() {
            MSGPACK_DECODABLE_CHECK(object::ext);
            if (isFixExt2())
                return unpackFixExtUnchecked2();
            else
                return type_error<object::ext>(Type::FIXEXT2);
        }

        object::ext unpackFixExt4() {
            MSGPACK_DECODABLE_CHECK(object::ext);
            if (isFixExt4())
                return unpackFixExtUnchecked4();
            else
                return type_error<object::ext>(Type::FIXEXT4);
        }

        object::ext unpackFixExt8() {
            MSGPACK_DECODABLE_CHECK(object::ext);
            if (isFixExt8())
                return unpackFixExtUnchecked8();
            else
                return type_error<object::ext>(Type::FIXEXT8);
        }

        object::ext unpackFixExt16() {
            MSGPACK_DECODABLE_CHECK(object::ext);
            if (isFixExt16())
                return unpackFixExtUnchecked16();
            else
                return type_error<object::ext>(Type::FIXEXT16);
        }

        object::ext unpackExt8() {
            MSGPACK_DECODABLE_CHECK(object::ext);
            if (isExt8())
                return unpackExtUnchecked8();
            else
                return type_error<object::ext>(Type::EXT8);
        }

        object::ext unpackExt16() {
            MSGPACK_DECODABLE_CHECK(object::ext);
            if (isExt16())
                return unpackExtUnchecked16();
            else
                return type_error<object::ext>(Type::EXT16);
        }

        object::ext unpackExt32() {
            MSGPACK_DECODABLE_CHECK(object::ext);
            if (isExt32())
                return unpackExtUnchecked32();
            else
                return type_error<object::ext>(Type::EXT32);
        }

        // ---------- TIMESTAMP format family ----------

        object::timespec unpackTimestamp32() {
            MSGPACK_DECODABLE_CHECK(object::timespec);
            if (isTimestamp32())
                return unpackTimestampUnchecked32();
            else
                return type_error<object::timespec>(Type::TIMESTAMP32);
        }

        object::timespec unpackTimestamp64() {
            MSGPACK_DECODABLE_CHECK(object::timespec);
            if (isTimestamp64())
                return unpackTimestampUnchecked64();
            else
                return type_error<object::timespec>(Type::TIMESTAMP64);
        }

        object::timespec unpackTimestamp96() {
            MSGPACK_DECODABLE_CHECK(object::timespec);
            if (isTimestamp96())
                return unpackTimestampUnchecked96();
            else
                return type_error<object::timespec>(Type::TIMESTAMP96);
        }

        /////////////////////////////////////////////////////////
        // ---------- msgpack types without checker ---------- //
        /////////////////////////////////////////////////////////

        // ---------- NIL format family ----------

        bool unpackNilUnchecked() {
            ++curr_index;
            return true;
        }

        // ---------- BOOL format family ----------

        bool unpackBoolUnchecked() {
            return (bool)(getRawBytes<uint8_t>(curr_index++, 0) & (uint8_t)BitMask::BOOL);
        }

        // ---------- INT format family ----------

        uint8_t unpackUIntUnchecked7() {
            return (uint8_t)(getRawBytes<uint8_t>(curr_index++, 0) & (uint8_t)BitMask::UINT7);
        }

        uint8_t unpackUIntUnchecked8() {
            return getRawBytes<uint8_t>(curr_index++, 1);
        }

        uint16_t unpackUIntUnchecked16() {
            return getRawBytes<uint16_t>(curr_index++, 1);
        }

        uint32_t unpackUIntUnchecked32() {
            return getRawBytes<uint32_t>(curr_index++, 1);
        }

        uint64_t unpackUIntUnchecked64() {
            return getRawBytes<uint64_t>(curr_index++, 1);
        }

        int8_t unpackIntUnchecked5() {
            return (int8_t)getRawBytes<uint8_t>(curr_index++, 0);
        }

        int8_t unpackIntUnchecked8() {
            return getRawBytes<int8_t>(curr_index++, 1);
        }

        int16_t unpackIntUnchecked16() {
            return getRawBytes<int16_t>(curr_index++, 1);
        }

        int32_t unpackIntUnchecked32() {
            return getRawBytes<int32_t>(curr_index++, 1);
        }

        int64_t unpackIntUnchecked64() {
            return getRawBytes<int64_t>(curr_index++, 1);
        }

        // ---------- FLOAT format family ----------

        float unpackFloatUnchecked32() {
            return getRawBytes<float>(curr_index++, 1);
        }

        double unpackFloatUnchecked64() {
#if ARX_HAVE_LIBSTDCPLUSPLUS >= 201103L  // Have libstdc++11
            return getRawBytes<double>(curr_index++, 1);
#else
            return unpackFloatUnchecked32();  // AVR does not support double
#endif
        }

        // ---------- STR format family ----------

        str_t unpackStringUnchecked5() {
            str_t str;
            uint8_t size = getRawBytes<uint8_t>(curr_index, 0) & (uint8_t)BitMask::STR5;
            for (uint8_t c = 0; c < size; ++c) str += getRawBytes<char>(curr_index, c + 1);
            ++curr_index;
            return str;
        }

        str_t unpackStringUnchecked8() {
            str_t str;
            uint8_t size = getRawBytes<uint8_t>(curr_index, 1);
            for (uint8_t c = 0; c < size; ++c) str += getRawBytes<char>(curr_index, c + 1 + sizeof(uint8_t));
            ++curr_index;
            return str;
        }

        str_t unpackStringUnchecked16() {
            str_t str;
            uint16_t size = getRawBytes<uint16_t>(curr_index, 1);
            for (uint16_t c = 0; c < size; ++c) str += getRawBytes<char>(curr_index, c + 1 + sizeof(uint16_t));
            ++curr_index;
            return str;
        }

        str_t unpackStringUnchecked32() {
            str_t str;
            uint32_t size = getRawBytes<uint32_t>(curr_index, 1);
            for (uint32_t c = 0; c < size; ++c) str += getRawBytes<char>(curr_index, c + 1 + sizeof(uint32_t));
            ++curr_index;
            return str;
        }

        // ---------- BIN format family ----------

        template <typename T = uint8_t>
        auto unpackBinaryUnchecked8() ->
            typename std::enable_if<std::is_same<T, char>::value || std::is_same<T, uint8_t>::value, bin_t<T>>::type {
            bin_t<T> data;
            uint8_t size = getRawBytes<uint8_t>(curr_index, 1);
            for (uint8_t v = 0; v < size; ++v) data.emplace_back(getRawBytes<T>(curr_index, v + 1 + sizeof(uint8_t)));
            ++curr_index;
            return data;
        }

        template <typename T = uint8_t>
        auto unpackBinaryUnchecked16() ->
            typename std::enable_if<std::is_same<T, char>::value || std::is_same<T, uint8_t>::value, bin_t<T>>::type {
            bin_t<T> data;
            uint16_t size = getRawBytes<uint16_t>(curr_index, 1);
            for (uint16_t v = 0; v < size; ++v)
                data.emplace_back(getRawBytes<T>(curr_index, v + 1 + sizeof(uint16_t)));
            ++curr_index;
            return data;
        }

        template <typename T = uint8_t>
        auto unpackBinaryUnchecked32() ->
            typename std::enable_if<std::is_same<T, char>::value || std::is_same<T, uint8_t>::value, bin_t<T>>::type {
            bin_t<T> data;
            uint32_t size = getRawBytes<uint32_t>(curr_index, 1);
            for (uint32_t v = 0; v < size; ++v)
                data.emplace_back(getRawBytes<T>(curr_index, v + 1 + sizeof(uint32_t)));
            ++curr_index;
            return data;
        }

#if ARX_HAVE_LIBSTDCPLUSPLUS >= 201103L  // Have libstdc++11

        template <typename T, size_t N>
        auto unpackBinaryUnchecked8() -> typename std::
            enable_if<std::is_same<T, char>::value || std::is_same<T, uint8_t>::value, std::array<T, N>>::type {
            std::array<T, N> data;
            uint8_t size = getRawBytes<uint8_t>(curr_index, 1);
            for (uint8_t v = 0; v < size; ++v) data[v] = getRawBytes<T>(curr_index, v + 1 + sizeof(uint8_t));
            ++curr_index;
            return data;
        }

        template <typename T, size_t N>
        auto unpackBinaryUnchecked16() -> typename std::
            enable_if<std::is_same<T, char>::value || std::is_same<T, uint8_t>::value, std::array<T, N>>::type {
            std::array<T, N> data;
            uint16_t size = getRawBytes<uint16_t>(curr_index, 1);
            for (uint16_t v = 0; v < size; ++v) data[v] = getRawBytes<T>(curr_index, v + 1 + sizeof(uint16_t));
            ++curr_index;
            return data;
        }

        template <typename T, size_t N>
        auto unpackBinaryUnchecked32() -> typename std::
            enable_if<std::is_same<T, char>::value || std::is_same<T, uint8_t>::value, std::array<T, N>>::type {
            std::array<T, N> data;
            uint32_t size = getRawBytes<uint32_t>(curr_index, 1);
            for (uint32_t v = 0; v < size; ++v) data[v] = getRawBytes<T>(curr_index, v + 1 + sizeof(uint32_t));
            ++curr_index;
            return data;
        }

#endif  // Do not have libstdc++11

        // ---------- ARRAY format family ----------

        size_t unpackArraySizeUnchecked4() {
            return (size_t)((getRawBytes<uint8_t>(curr_index++, 0) & (uint8_t)BitMask::ARRAY4));
        }

        size_t unpackArraySizeUnchecked16() {
            return (size_t)getRawBytes<uint16_t>(curr_index++, 1);
        }

        size_t unpackArraySizeUnchecked32() {
            return (size_t)getRawBytes<uint32_t>(curr_index++, 1);
        }

        // ---------- MAP format family ----------

        size_t unpackMapSizeUnchecked4() {
            return (size_t)((getRawBytes<uint8_t>(curr_index++, 0) & (uint8_t)BitMask::MAP4));
        }

        size_t unpackMapSizeUnchecked16() {
            return (size_t)getRawBytes<uint16_t>(curr_index++, 1);
        }

        size_t unpackMapSizeUnchecked32() {
            return (size_t)getRawBytes<uint32_t>(curr_index++, 1);
        }

        // ---------- EXT format family ----------

        object::ext unpackFixExtUnchecked1() {
            int8_t ext_type = getRawBytes<int8_t>(curr_index, 1);
            uint8_t* ptr = getRawBytePtr(curr_index++, 2);
            return object::ext(ext_type, ptr, 1);
        }

        object::ext unpackFixExtUnchecked2() {
            int8_t ext_type = getRawBytes<int8_t>(curr_index, 1);
            uint8_t* ptr = getRawBytePtr(curr_index++, 2);
            return object::ext(ext_type, ptr, 2);
        }

        object::ext unpackFixExtUnchecked4() {
            int8_t ext_type = getRawBytes<int8_t>(curr_index, 1);
            uint8_t* ptr = getRawBytePtr(curr_index++, 2);
            return object::ext(ext_type, ptr, 4);
        }

        object::ext unpackFixExtUnchecked8() {
            int8_t ext_type = getRawBytes<int8_t>(curr_index, 1);
            uint8_t* ptr = getRawBytePtr(curr_index++, 2);
            return object::ext(ext_type, ptr, 8);
        }

        object::ext unpackFixExtUnchecked16() {
            int8_t ext_type = getRawBytes<int8_t>(curr_index, 1);
            uint8_t* ptr = getRawBytePtr(curr_index++, 2);
            return object::ext(ext_type, ptr, 16);
        }

        object::ext unpackExtUnchecked8() {
            uint8_t size = getRawBytes<uint8_t>(curr_index, 1);
            int8_t ext_type = getRawBytes<int8_t>(curr_index, 2);
            uint8_t* ptr = getRawBytePtr(curr_index++, 3);
            return object::ext(ext_type, ptr, size);
        }

        object::ext unpackExtUnchecked16() {
            uint16_t size = getRawBytes<uint16_t>(curr_index, 1);
            int8_t ext_type = getRawBytes<int8_t>(curr_index, 3);
            uint8_t* ptr = getRawBytePtr(curr_index++, 4);
            return object::ext(ext_type, ptr, size);
        }

        object::ext unpackExtUnchecked32() {
            uint32_t size = getRawBytes<uint32_t>(curr_index, 1);
            int8_t ext_type = getRawBytes<int8_t>(curr_index, 5);
            uint8_t* ptr = getRawBytePtr(curr_index++, 6);
            return object::ext(ext_type, ptr, size);
        }

        // ---------- TIMESTAMP format family ----------

        object::timespec unpackTimestampUnchecked32() {
            object::timespec ts;
            ts.tv_nsec = 0;
            ts.tv_sec = getRawBytes<uint32_t>(curr_index++, 2);
            return ts;
        }

        object::timespec unpackTimestampUnchecked64() {
            object::timespec ts;
            uint64_t data64 = getRawBytes<uint64_t>(curr_index++, 2);
            ts.tv_nsec = data64 >> 34;
            ts.tv_sec = data64 & 0x00000003ffffffffL;
            return ts;
        }

        object::timespec unpackTimestampUnchecked96() {
            object::timespec ts;
            ts.tv_nsec = getRawBytes<uint32_t>(curr_index, 3);
            ts.tv_sec = getRawBytes<uint64_t>(curr_index++, 7);
            return ts;
        }

        /////////////////////////////////////////
        // ---------- type checkers ---------- //
        /////////////////////////////////////////

        // ---------- NIL format family ----------
        // - N/A

        template <typename T>
        auto unpackable(const T& value) const ->
            typename std::enable_if<std::is_same<T, object::nil_t>::value, bool>::type {
            (void)value;
            return isNil();
        }

        // ---------- BOOL format family ----------
        // - bool

        template <typename T>
        auto unpackable(const T& value) const -> typename std::enable_if<std::is_same<T, bool>::value, bool>::type {
            (void)value;
            return isBool();
        }

        // ---------- INT format family ----------
        // - char (signed/unsigned)
        // - ints (signed/unsigned)

        template <typename T>
        auto unpackable(const T&) const -> typename std::enable_if<
            std::is_arithmetic<T>::value && !std::is_floating_point<T>::value && !std::is_same<T, bool>::value
                && !std::is_same<typename std::remove_cv<T>::type, char*>::value && !std::is_signed<T>::value,
            bool>::type {
            if (isUInt7())
                return sizeof(T) >= sizeof(uint8_t);
            else if (isUInt8())
                return sizeof(T) >= sizeof(uint8_t);
            else if (isUInt16())
                return sizeof(T) >= sizeof(uint16_t);
            else if (isUInt32())
                return sizeof(T) >= sizeof(uint32_t);
            else if (isUInt64())
                return sizeof(T) >= sizeof(uint64_t);
            else
                return false;
        }

        template <typename T>
        auto unpackable(const T&) const -> typename std::enable_if<
            std::is_arithmetic<T>::value && !std::is_floating_point<T>::value && !std::is_same<T, bool>::value
                && !std::is_same<typename std::remove_cv<T>::type, char*>::value && std::is_signed<T>::value,
            bool>::type {
            if (isInt5())
                return sizeof(T) >= sizeof(int8_t);
            else if (isInt8())
                return sizeof(T) >= sizeof(int8_t);
            else if (isInt16())
                return sizeof(T) >= sizeof(int16_t);
            else if (isInt32())
                return sizeof(T) >= sizeof(int32_t);
            else if (isInt64())
                return sizeof(T) >= sizeof(int64_t);
            else if (isUInt7())
                return sizeof(T) >= sizeof(int8_t);
            else if (isUInt8())
                return sizeof(T) >= sizeof(int8_t);
            else if (isUInt16())
                return sizeof(T) >= sizeof(int16_t);
            else if (isUInt32())
                return sizeof(T) >= sizeof(int32_t);
            else if (isUInt64())
                return sizeof(T) >= sizeof(int64_t);
            else
                return false;
        }

        // ---------- FLOAT format family ----------
        // - float
        // - double

        template <typename T>
        auto unpackable(const T&) const ->
            typename std::enable_if<std::is_arithmetic<T>::value && std::is_floating_point<T>::value, bool>::type {
            switch (getType()) {
                case Type::UINT7:
                case Type::UINT8:
                case Type::UINT16:
                case Type::UINT32:
                case Type::UINT64:
                case Type::INT5:
                case Type::INT8:
                case Type::INT16:
                case Type::INT32:
                case Type::INT64:
                case Type::FLOAT32:
                case Type::FLOAT64:
                    return true;
                default:
                    return false;
            }
        }

        // ---------- STRING format family ----------
        // - char*
        // - char[]
        // - std::string

        bool unpackable(const str_t& str) const {
            (void)str;
            return isStr();
        }

        // ---------- BIN format family ----------
        // - unsigned char*
        // - unsigned char[]
        // - std::vector<char>
        // - std::vector<unsigned char>
        // - std::array<char>
        // - std::array<unsigned char>

        template <typename T = uint8_t>
        auto unpackable(const bin_t<T>& bin) const ->
            typename std::enable_if<std::is_same<T, char>::value || std::is_same<T, uint8_t>::value, bool>::type {
            (void)bin;
            return isBin();
        }

#if ARX_HAVE_LIBSTDCPLUSPLUS >= 201103L  // Have libstdc++11

        template <typename T, size_t N>
        auto unpackable(const std::array<T, N>& bin) const ->
            typename std::enable_if<std::is_same<T, char>::value || std::is_same<T, uint8_t>::value, bool>::type {
            (void)bin;
            return isBin();
        }

#endif  // Do not have libstdc++11

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
        // - std::unordered_set
        // - std::multiset
        // - std::unordered_multiset

        template <typename T>
        auto unpackable(const arr_t<T>& arr) const ->
            typename std::enable_if<!std::is_same<T, char>::value && !std::is_same<T, uint8_t>::value, bool>::type {
            (void)arr;
            return isArray();
        }

#if ARX_HAVE_LIBSTDCPLUSPLUS >= 201103L  // Have libstdc++11

        template <typename T, size_t N>
        auto unpackable(const std::array<T, N>& arr) const ->
            typename std::enable_if<!std::is_same<T, char>::value && !std::is_same<T, uint8_t>::value, bool>::type {
            (void)arr;
            return isArray();
        }

        template <typename T>
        bool unpackable(const std::deque<T>& arr) const {
            (void)arr;
            return isArray();
        }

        template <typename T, typename U>
        bool unpackable(const std::pair<T, U>& arr) const {
            (void)arr;
            return isArray();
        }

        template <typename... Args>
        bool unpackable(const std::tuple<Args...>& arr) const {
            (void)arr;
            return isArray();
        }

        template <typename T>
        bool unpackable(const std::list<T>& arr) const {
            (void)arr;
            return isArray();
        }

        template <typename T>
        bool unpackable(const std::forward_list<T>& arr) const {
            (void)arr;
            return isArray();
        }

        template <typename T>
        bool unpackable(const std::set<T>& arr) const {
            (void)arr;
            return isArray();
        }

        template <typename T>
        bool unpackable(const std::unordered_set<T>& arr) const {
            (void)arr;
            return isArray();
        }

        template <typename T>
        bool unpackable(const std::multiset<T>& arr) const {
            (void)arr;
            return isArray();
        }

        template <typename T>
        bool unpackable(std::unordered_multiset<T>& arr) const {
            (void)arr;
            return isArray();
        }

#endif  // Do not have libstdc++11

        // ---------- MAP format family ----------
        // - std::map
        // - std::multimap
        // - std::unordered_map *
        // - std::unordered_multimap *
        // * : not supported in arduino

        template <typename T, typename U>
        bool unpackable(const map_t<T, U>& mp) const {
            (void)mp;
            return isMap();
        }

#if ARX_HAVE_LIBSTDCPLUSPLUS >= 201103L  // Have libstdc++11

        template <typename T, typename U>
        bool unpackable(const std::multimap<T, U>& mp) const {
            (void)mp;
            return isMap();
        }

        template <typename T, typename U>
        bool unpackable(const std::unordered_map<T, U>& mp) const {
            (void)mp;
            return isMap();
        }

        template <typename T, typename U>
        bool unpackable(const std::unordered_multimap<T, U>& mp) const {
            (void)mp;
            return isMap();
        }

#endif  // Do not have libstdc++11

        // ---------- EXT format family ----------

        bool unpackable(const object::ext& e) const {
            (void)e;
            return isFixExt() || isExt();
        }

        // ---------- TIMESTAMP format family ----------

        bool unpackable(const object::timespec& t) const {
            (void)t;
            return isTimestamp();
        }

        /////////////////////////////////////////////////
        // ---------- msgpack type checkers ---------- //
        /////////////////////////////////////////////////

        bool isNil() const {
            return getType() == Type::NIL;
        }
        bool isBool() const {
            return getType() == Type::BOOL;
        }
        bool isUInt7() const {
            return getType() == Type::UINT7;
        }
        bool isUInt8() const {
            return getType() == Type::UINT8;
        }
        bool isUInt16() const {
            return getType() == Type::UINT16;
        }
        bool isUInt32() const {
            return getType() == Type::UINT32;
        }
        bool isUInt64() const {
            return getType() == Type::UINT64;
        }
        bool isUInt() const {
            return isUInt7() || isUInt8() || isUInt16() || isUInt32() || isUInt64();
        }
        bool isInt5() const {
            return getType() == Type::INT5;
        }
        bool isInt8() const {
            return getType() == Type::INT8;
        }
        bool isInt16() const {
            return getType() == Type::INT16;
        }
        bool isInt32() const {
            return getType() == Type::INT32;
        }
        bool isInt64() const {
            return getType() == Type::INT64;
        }
        bool isInt() const {
            return isInt5() || isInt8() || isInt16() || isInt32() || isInt64();
        }
        bool isFloat32() const {
            return getType() == Type::FLOAT32;
        }
        bool isFloat64() const {
            return getType() == Type::FLOAT64;
        }
        bool isStr5() const {
            return getType() == Type::STR5;
        }
        bool isStr8() const {
            return getType() == Type::STR8;
        }
        bool isStr16() const {
            return getType() == Type::STR16;
        }
        bool isStr32() const {
            return getType() == Type::STR32;
        }
        bool isStr() const {
            return isStr5() || isStr8() || isStr16() || isStr32();
        }
        bool isBin8() const {
            return getType() == Type::BIN8;
        }
        bool isBin16() const {
            return getType() == Type::BIN16;
        }
        bool isBin32() const {
            return getType() == Type::BIN32;
        }
        bool isBin() const {
            return isBin8() || isBin16() || isBin32();
        }
        bool isArray4() const {
            return getType() == Type::ARRAY4;
        }
        bool isArray16() const {
            return getType() == Type::ARRAY16;
        }
        bool isArray32() const {
            return getType() == Type::ARRAY32;
        }
        bool isArray() const {
            return isArray4() || isArray16() || isArray32();
        }
        bool isMap4() const {
            return getType() == Type::MAP4;
        }
        bool isMap16() const {
            return getType() == Type::MAP16;
        }
        bool isMap32() const {
            return getType() == Type::MAP32;
        }
        bool isMap() const {
            return isMap4() || isMap16() || isMap32();
        }
        bool isFixExt1() const {
            return getType() == Type::FIXEXT1;
        }
        bool isFixExt2() const {
            return getType() == Type::FIXEXT2;
        }
        bool isFixExt4() const {
            return getType() == Type::FIXEXT4;
        }
        bool isFixExt8() const {
            return getType() == Type::FIXEXT8;
        }
        bool isFixExt16() const {
            return getType() == Type::FIXEXT16;
        }
        bool isFixExt() const {
            return isFixExt1() || isFixExt2() || isFixExt4() || isFixExt8() || isFixExt16();
        }
        bool isExt8() const {
            return getType() == Type::EXT8;
        }
        bool isExt16() const {
            return getType() == Type::EXT16;
        }
        bool isExt32() const {
            return getType() == Type::EXT32;
        }
        bool isExt() const {
            return isExt8() || isExt16() || isExt32();
        }
        bool isTimestamp32() const {
            return (getType() == Type::TIMESTAMP32) && (getRawBytes<int8_t>(curr_index, 1) == -1);
        }
        bool isTimestamp64() const {
            return (getType() == Type::TIMESTAMP64) && (getRawBytes<int8_t>(curr_index, 1) == -1);
        }
        bool isTimestamp96() const {
            return (getType() == Type::TIMESTAMP96) && (getRawBytes<int8_t>(curr_index, 2) == -1)
                && (getRawBytes<uint8_t>(curr_index, 1) == 12);
        }
        bool isTimestamp() const {
            return isTimestamp32() || isTimestamp64() || isTimestamp96();
        }

        Type getType() const {
            return getType(curr_index);
        }

    private:
        template <typename DataType>
        DataType getRawBytes(const size_t idx, const size_t offset) const {
            if (idx >= indices.size()) {
                LOG_ERROR(F("index overrun: idx"), idx, F("must be <"), indices.size());
                return DataType();
            }
            DataType data;
            const auto size = sizeof(DataType);
            for (uint8_t b = 0; b < size; ++b) {
                uint8_t distance = size - 1 - b;
                auto index = indices[idx] + offset + distance;
                ((uint8_t*)&data)[b] = raw_data[index];
            }
            return data;
        }

        uint8_t* getRawBytePtr(const size_t idx, const size_t offset) const {
            if (idx >= indices.size()) {
                LOG_ERROR(F("index overrun: idx"), idx, F("must be <"), indices.size());
                return nullptr;
            }
            auto index = indices[idx] + offset;
            return raw_data + index;
        }

        Type getType(const size_t idx) const {
            if (idx >= indices.size()) {
                LOG_ERROR(F("index overrun: idx"), idx, F("must be <"), indices.size());
                return Type::NA;
            }

            uint8_t raw = getRawBytes<uint8_t>(idx, 0);
            if (raw < (uint8_t)Type::MAP4)
                return Type::UINT7;
            else if (raw < (uint8_t)Type::ARRAY4)
                return Type::MAP4;
            else if (raw < (uint8_t)Type::STR5)
                return Type::ARRAY4;
            else if (raw < (uint8_t)Type::NIL)
                return Type::STR5;
            else if (raw == (uint8_t)Type::NIL)
                return Type::NIL;
            else if (raw == (uint8_t)Type::NA)
                return Type::NA;
            else if (raw < (uint8_t)Type::BIN8)
                return Type::BOOL;
            else if (raw < (uint8_t)Type::INT5)
                return (Type)raw;
            else
                return Type::INT5;
        }

        size_t getElementSize(size_t i) const {
            Type type = getType(i);
            switch (type) {
                // size of ARRAY & MAP is size of OBJECTs
                // so the size from header to first object is returned
                case Type::NIL:
                case Type::BOOL:
                case Type::UINT7:
                case Type::INT5:
                case Type::ARRAY4:
                case Type::MAP4:
                    return 1;
                case Type::UINT8:
                case Type::INT8:
                    return 2;
                case Type::UINT16:
                case Type::INT16:
                case Type::ARRAY16:
                case Type::MAP16:
                case Type::FIXEXT1:
                    return 3;
                case Type::FIXEXT2:
                    return 4;
                case Type::UINT32:
                case Type::INT32:
                case Type::FLOAT32:
                case Type::ARRAY32:
                case Type::MAP32:
                    return 5;
                case Type::FIXEXT4:  // includes case Type::TIMESTAMP32:
                    return 6;
                case Type::UINT64:
                case Type::INT64:
                case Type::FLOAT64:
                    return 9;
                case Type::FIXEXT8:  // includes case Type::TIMESTAMP64:
                    return 10;
                case Type::FIXEXT16:
                    return 18;
                case Type::STR5:
                    return (getRawBytes<uint8_t>(i, 0) & (uint8_t)BitMask::STR5) + 1;
                case Type::STR8:
                case Type::BIN8:
                    return getRawBytes<uint8_t>(i, 1) + sizeof(uint8_t) + 1;
                case Type::STR16:
                case Type::BIN16:
                    return (size_t)getRawBytes<uint16_t>(i, 1) + sizeof(uint16_t) + 1;
                case Type::STR32:
                case Type::BIN32:
                    return (size_t)getRawBytes<uint32_t>(i, 1) + sizeof(uint32_t) + 1;
                case Type::EXT8:  // includes case Type::TIMESTAMP96:
                    return (size_t)getRawBytes<uint8_t>(i, 1) + sizeof(uint8_t) + 1 + 1;
                case Type::EXT16:
                    return (size_t)getRawBytes<uint16_t>(i, 1) + sizeof(uint16_t) + 1 + 1;
                case Type::EXT32:
                    return (size_t)getRawBytes<uint32_t>(i, 1) + sizeof(uint32_t) + 1 + 1;
                default:
                    LOG_ERROR(F("undefined type:"), (int)type);
                    return 0;
            }
        }

        template <typename T>
        T type_error(const Type type = Type::NA) {
            if (type == Type::NA)
                LOG_ERROR(F("unknown type in index"), curr_index, F(": type byte"), (int)getType());
            else
                LOG_ERROR(
                    F("unpack type mimatch in index"),
                    curr_index,
                    F(": type byte"),
                    (int)getType(),
                    F("must be"),
                    (int)type);
            b_decode_success = false;
            ++curr_index;
            return T();
        }

        template <typename T, typename U, typename V>
        auto uint_to_int(const U value) -> typename std::enable_if<
            std::is_arithmetic<T>::value && std::is_integral<T>::value && !std::is_same<T, bool>::value
                && !std::is_same<typename std::remove_cv<T>::type, char*>::value && std::is_signed<T>::value,
            T>::type {
            if ((sizeof(T) > sizeof(V)) || (value <= (U)std::numeric_limits<V>::max()))
                return (T)value;
            else
                return T();
        }

#if ARX_HAVE_LIBSTDCPLUSPLUS >= 201103L  // Have libstdc++11
        template <template <typename...> class C, class T>
        void unpackArrayContainerArray(C<T>& arr)
#else   // Do not have libstdc++11
        template <typename T>
        void unpackArrayContainerArray(arr_t<T>& arr)
#endif  // Do not have libstdc++11
        {
            const size_t size = unpackArraySize();
            if (size == 0) {
                LOG_ERROR(F("array size mismatch:"), size, F("must be"), arr.size());
                b_decode_success = false;
            } else if (arr.size() == size)
                for (auto& a : arr) unpack(a);
            else {
                arr.clear();
                for (size_t a = 0; a < size; ++a) {
                    arr.emplace_back(T());
                    unpack(arr.back());
                }
            }
        }

#if ARX_HAVE_LIBSTDCPLUSPLUS >= 201103L  // Have libstdc++11
        template <template <typename...> class C, class T>
        void unpackArrayContainerSet(C<T>& arr)
#else   // Do not have libstdc++11
        template <typename T>
        void unpackArrayContainerSet(arr_t<T>& arr)
#endif  // Do not have libstdc++11
        {
            const size_t size = unpackArraySize();
            if (size == 0) {
                LOG_ERROR(F("array size mismatch:"), size, F("must be"), arr.size());
                b_decode_success = false;
            } else {
                arr.clear();
                for (size_t a = 0; a < size; ++a) {
                    T t;
                    unpack(t);
                    arr.emplace(t);
                }
            }
        }

#if ARX_HAVE_LIBSTDCPLUSPLUS >= 201103L  // Have libstdc++11
        template <template <typename...> class C, class T, class U>
        void unpackMapContainer(C<T, U>& mp) {
#else   // Do not have libstdc++11
        template <typename T, typename U>
        void unpackMapContainer(map_t<T, U>& mp) {
#endif  // Do not have libstdc++11
            const size_t size = unpackMapSize();
            if (size == 0) {
                LOG_ERROR(F("map size mismatch:"), size, F("must be"), mp.size());
                b_decode_success = false;
            } else {
                mp.clear();
                for (size_t a = 0; a < size; ++a) {
                    T t;
                    U u;
                    unpack(t);
                    unpack(u);
                    mp.emplace(std::make_pair(t, u));
                }
            }
        }
    };

}  // namespace msgpack
}  // namespace arduino

#endif  // HT_SERIAL_MSGPACK_UNPACKER_H

#pragma once
#ifndef ARDUINO_MSGPACK_UTILITY_H
#define ARDUINO_MSGPACK_UTILITY_H

#include "Types.h"
#include "Packer.h"
#include "Unpacker.h"

namespace arduino {
namespace msgpack {

    template <typename T>
    inline size_t estimate_size(const T& value) {
        Packer packer;
        packer.serialize(value);
        return packer.size();
    }

#if defined(FILE_WRITE) && defined(ARDUINOJSON_VERSION)

    namespace file {

        template <typename FsType, typename T>
        inline bool save_as_json(FsType& fs, const String& path, const T& value, JsonDocument& doc) {
            // remove file if exists
            if (fs.exists(path.c_str())) {
                if (!fs.remove(path.c_str())) {
                    LOG_ERROR(F("Failed to remove existing file:"), path);
                    return false;
                }
            }

            // open file to write
            File f = fs.open(path.c_str(), FILE_WRITE);
            if (!f) {
                LOG_ERROR(F("Failed to open file:"), path);
                return false;
            }

            // serialize value to msgpack
            Packer packer;
            packer.serialize(value);

            // convert msgpack to json
            DeserializationError err = deserializeMsgPack(doc, packer.data(), packer.size());
            if (err) {
                LOG_ERROR(F("Deserialization from msgpack failed:"), err.f_str());
                return false;
            }

            // serialize json to file
            if (serializeJson(doc, f) == 0) {
                LOG_ERROR(F("Failed to write json to file"));
                return false;
            }

            f.close();
            return true;
        }
        template <size_t N, typename FsType, typename T>
        inline bool save_as_json_static(FsType& fs, const String& path, const T& value) {
            StaticJsonDocument<N> doc;
            return save_as_json(fs, path, value, doc);
        }
        template <typename FsType, typename T>
        inline bool save_as_json_dynamic(
            FsType& fs, const String& path, const T& value, const size_t json_size = 512) {
            DynamicJsonDocument doc(json_size);
            return save_as_json(fs, path, value, doc);
        }

        template <typename FsType, typename T>
        inline bool load_from_json(
            FsType& fs, const String& path, T& value, JsonDocument& doc, const size_t num_max_string_type = 32) {
            // open file as read-only
            File f = fs.open(path.c_str());
            if (!f) {
                LOG_ERROR(F("Failed to open file:"), path);
                return false;
            }

            // deserialize from file to json
            DeserializationError err = deserializeJson(doc, f);
            if (err) {
                LOG_ERROR(F("Deserializing json failed:"), err.f_str());
                return false;
            }

            // serialize json to msgpack
            Packer packer;
            packer.serialize_arduinojson(doc, num_max_string_type);

            // deserialize from msgpack to value
            Unpacker unpacker;
            unpacker.feed(packer.data(), packer.size());
            if (!unpacker.deserialize(value)) {
                LOG_ERROR(F("Failed to deserialize from msgpack"));
                return false;
            }

            f.close();
            return true;
        }
        template <size_t N, typename FsType, typename T>
        inline bool load_from_json_static(FsType& fs, const String& path, T& value) {
            StaticJsonDocument<N> doc;
            return load_from_json(fs, path, value, doc);
        }
        template <typename FsType, typename T>
        inline bool load_from_json_dynamic(FsType& fs, const String& path, T& value, const size_t json_size = 512) {
            DynamicJsonDocument doc(json_size);
            return load_from_json(fs, path, value, doc);
        }

    }  // namespace file

#endif  // defined(FILE_WRITE) && defined(ARDUINOJSON_VERSION)

#ifdef EEPROM_h

    namespace eeprom {

        template <typename T>
        inline bool save(const T& value, const size_t index_offset = 0) {
            Packer packer;
            packer.serialize(value);

            const uint8_t byte_offset = index_offset + 1;  // consider offset of value size

#ifndef ESP_PLATFORM
            // in esp32, EEPROM.length() always returns 0 due to the bug...
            if (byte_offset + packer.size() > EEPROM.length()) {
                LOG_ERROR(
                    F("MsgPack data size + offset is larger the size of EEPROM:"),
                    byte_offset + packer.size(),
                    "should <=",
                    EEPROM.length());

                return false;
            }
#endif

#if defined(ESP_PLATFORM) || defined(ESP8266)
            // write size of value
            EEPROM.write(index_offset, packer.size());

            const uint8_t* data = packer.data();
            for (size_t i = 0; i < packer.size(); ++i) {
                EEPROM.write(byte_offset + i, data[i]);
            }
            EEPROM.commit();
#else
            // write size of value
            EEPROM.update(index_offset, packer.size());

            const uint8_t* data = packer.data();
            for (size_t i = 0; i < packer.size(); ++i) {
                EEPROM.update(byte_offset + i, data[i]);
            }
#endif
            return true;
        }

        template <typename T>
        inline bool load(T& value, const size_t index_offset = 0) {
            // confirm the size of value
            const size_t size = EEPROM.read(index_offset);

            // read data
            bin_t<uint8_t> data;
            for (size_t i = 0; i < size; ++i) {
                data.emplace_back(EEPROM.read(index_offset + 1 + i));  // consider offset of value size
            }

            // deserialize data
            Unpacker unpacker;
            if (unpacker.feed(data.data(), data.size())) {
                if (unpacker.deserialize(value)) {
                    return true;
                } else {
                    LOG_ERROR("EEPROM data deserialization failed");
                    return false;
                }
            } else {
                LOG_ERROR("EEPROM data size was not correct");
                return false;
            }
        }

        inline void clear_size(const size_t size, const size_t index_offset = 0) {
#if defined(ESP_PLATFORM) || defined(ESP8266)
            for (size_t i = 0; i < size; ++i) {
                EEPROM.write(index_offset + i, 0xFF);
            }
            EEPROM.commit();
#else
            for (size_t i = 0; i < size; ++i) {
                EEPROM.update(index_offset + i, 0xFF);
            }
#endif
        }

        template <typename T>
        inline void clear(const T& value, const size_t index_offset = 0) {
            const size_t size = estimate_size(value);
            clear_size(size, index_offset);
        }

    }  // namespace eeprom

#endif  // EEPROM_h

}  // namespace msgpack
}  // namespace arduino

#endif  // ARDUINO_MSGPACK_UTILITY_H

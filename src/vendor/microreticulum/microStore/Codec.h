/*
 * Copyright (c) 2026 Chad Attermann
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 */

#pragma once

#include <vector>
#include <string>
#include <cstring>

namespace microStore {

template<typename T>
struct always_false : std::false_type {};

template<typename T>
struct Codec
{
	static_assert(always_false<T>::value,
		"No Codec<T> specialization for this type — provide one.");
};

template<>
struct Codec<char*>
{
	static std::vector<uint8_t> encode(const char* s)
	{
		return std::vector<uint8_t>(s, s + strlen(s));
	}
	static bool decode(const std::vector<uint8_t>& data, char* s, size_t len)
	{
		memcpy(s, data.data(), std::min(data.size(), len-1));
		s[std::min(data.size(), len-1)] = 0;
		return true;
	}
/*
	static bool decode(const std::vector<uint8_t>& data, char* s)
	{
		memcpy(s, data.data(), data.size());
		s[data.size()] = 0;
		return true;
	}
*/
};

template<>
struct Codec<std::string>
{
	static std::vector<uint8_t> encode(const std::string& s)
	{
		return std::vector<uint8_t>(s.begin(),s.end());
	}
	static bool decode(const std::vector<uint8_t>& data,std::string& out)
	{
		out.assign((const char*)data.data(), data.size());
		return true;
	}
};

template<>
struct Codec<std::vector<uint8_t>>
{
	static std::vector<uint8_t> encode(const std::vector<uint8_t>& vec)
	{
		return std::vector<uint8_t>(vec.begin(), vec.end());
	}
	static bool decode(const std::vector<uint8_t>& data, std::vector<uint8_t>& out)
	{
		out.assign(data.begin(), data.end());
		return true;
	}
};

// Monostate value type for key-only ("set") stores — see TypedKeyStore in TypedStore.h.
struct Empty {};

template<>
struct Codec<Empty>
{
	static std::vector<uint8_t> encode(const Empty&) { return {}; }
	static bool decode(const std::vector<uint8_t>&, Empty&) { return true; }
};

}

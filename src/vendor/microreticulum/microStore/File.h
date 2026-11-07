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

#include "Utility.h"

#ifdef ARDUINO
#include <Stream.h>
#else
#include "Stream.h"
#endif

#include <list>
#include <memory>
#include <cassert>
#include <stdint.h>

namespace microStore {

enum SeekMode {
	SeekModeSet,
	SeekModeCur,
	SeekModeEnd,
};

class FileImpl {

protected:
	FileImpl() {}
public:
	virtual ~FileImpl() {}

protected:
	// File overrides
	virtual const char* name() const = 0;
	virtual size_t size() const = 0;
	virtual void close() = 0;

	// Print overrides
	virtual int read() = 0;
	virtual size_t write(uint8_t ch) = 0;
	virtual size_t read(uint8_t* buffer, size_t size) = 0;
	virtual size_t write(const uint8_t* buffer, size_t size) = 0;

	virtual int available() = 0;
	virtual int peek() = 0;
	virtual size_t tell() = 0;
	virtual long seek(uint32_t pos, SeekMode mode) = 0;
	virtual void flush() = 0;

	// Helper
	virtual bool isValid() const = 0;

friend class File;
};

class File : public Stream {

public:
	enum Mode {
		ModeRead,
		ModeWrite,
		ModeAppend,
		ModeReadWrite,
		ModeReadAppend,
	};

public:
	File() {}
	File(const File& obj) : _impl(obj._impl), _crc(obj._crc) {}
	File(FileImpl* impl) : _impl(impl) {}
	virtual ~File() {}

	inline virtual File& operator = (const File& obj) { _impl = obj._impl; _crc = obj._crc; return *this; }
	inline File& operator = (FileImpl* impl) { _impl.reset(impl); return *this; }
	inline bool operator < (const File& obj) const { return _impl.get() < obj._impl.get(); }
	inline bool operator > (const File& obj) const { return _impl.get() > obj._impl.get(); }
	inline bool operator == (const File& obj) const { return _impl.get() == obj._impl.get(); }
	inline bool operator != (const File& obj) const { return _impl.get() != obj._impl.get(); }
	inline FileImpl* get() { return _impl.get(); }
	inline bool isValid() const { if (_impl.get() == nullptr) return false; return _impl->isValid(); }
	inline operator bool() const { return isValid(); }
	inline void clear() { _impl.reset(); }

public:
	inline uint32_t crc() { return _crc; }
	inline size_t write(const char* str) { return write((const uint8_t*)str, strlen(str)); }

	inline const char* name() const { assert(_impl); return _impl->name(); }
	//NEW inline const char* path() const { assert(_impl); return _impl->path(); }
	inline size_t size() const { assert(_impl); return _impl->size(); }
	//NEW inline size_t position() const { assert(_impl); return _impl->position(); }
	inline void close() { assert(_impl); _impl->close(); }

	inline int read() {
		assert(_impl);
		if (_impl->available() <= 0) return EOF;
		int ch = _impl->read();
		uint8_t uch = (uint8_t)ch;
		_crc = crc32(_crc, uch);
		return ch;
	}
	inline size_t write(uint8_t byte) { assert(_impl); _crc = crc32(_crc, byte); return _impl->write(byte); }
	inline size_t read(uint8_t* buffer, size_t size) { assert(_impl); size_t read = _impl->read(buffer, size); if (read > 0 && read != (size_t)-1) _crc = crc32(_crc, buffer, read); return read; }
	inline size_t read(void* buffer, size_t size) { return read((uint8_t*)buffer, size); }
	inline size_t write(const uint8_t* buffer, size_t size) { assert(_impl); _crc = crc32(_crc, buffer, size); return _impl->write(buffer, size); }
	inline size_t write(const void* buffer, size_t size) { return write((const uint8_t*)buffer, size); }

	inline int available() { assert(_impl); return _impl->available(); }
	inline int peek() { assert(_impl); return _impl->peek(); }
	inline size_t tell() { assert(_impl); return _impl->tell(); }
	inline long seek(uint32_t pos, SeekMode mode) { assert(_impl); return _impl->seek(pos, mode); }
	inline long seek(uint32_t pos) { return seek(pos, SeekModeSet); }
	inline void flush() { assert(_impl); _impl->flush(); }

protected:
public:

#ifndef NDEBUG
	inline std::string debugString() const {
		std::string dump;
		dump = "File object, this: " + std::to_string((uintptr_t)this) + ", data: " + std::to_string((uintptr_t)_impl.get());
		return dump;
	}
#endif

protected:
	std::shared_ptr<FileImpl> _impl;
	uint32_t _crc = 0;
};

}

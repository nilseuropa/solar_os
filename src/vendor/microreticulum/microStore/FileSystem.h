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

#include "File.h"

#include <list>
#include <vector>
#include <memory>
#include <cassert>
#include <functional>
#include <stdint.h>

namespace microStore {

class FileSystemImpl {

public:
	struct Callbacks {
		using DirectoryListing = std::function<void(const char*)>;
	};

protected:
	FileSystemImpl() {}
public:
	virtual ~FileSystemImpl() {}

protected:
	virtual bool format() { return false; }
	virtual bool init(bool reformatOnFail = true) { return true; }
	virtual void loop() {}

	// Factory
	virtual File open(const char* path, File::Mode mode, const bool create = false) = 0;

	//POSIX
	virtual bool exists(const char* path) = 0;
	virtual bool remove(const char* path) = 0;
	virtual bool rename(const char* from_path, const char* to_path) = 0;
	virtual bool mkdir(const char* path) = 0;
	virtual bool rmdir(const char* path) = 0;

	// Helper
	inline virtual size_t size(const char* path) {
		File file = open(path, File::ModeRead);
		if (!file) return false;
		size_t size = file.size();
		file.close();
		return size;
	}
	virtual bool isDirectory(const char* path) = 0;
	virtual std::list<std::string> listDirectory(const char* path, Callbacks::DirectoryListing callback = nullptr) = 0;
	virtual size_t storageSize() = 0;
	virtual size_t storageAvailable() = 0;

	virtual bool isValid() const { return true; }

friend class FileSystem;
};

class FileSystem {

public:
	using Callbacks = FileSystemImpl::Callbacks;

public:
	FileSystem() {}
	FileSystem(const FileSystem& obj) : _impl(obj._impl) {}
	FileSystem(FileSystemImpl* impl) : _impl(impl) {}
	virtual ~FileSystem() {}

	inline FileSystem& operator = (const FileSystem& obj) { _impl = obj._impl; return *this; }
	inline FileSystem& operator = (FileSystemImpl* impl) { _impl.reset(impl); return *this; }
	inline bool operator < (const FileSystem& obj) const { return _impl.get() < obj._impl.get(); }
	inline bool operator > (const FileSystem& obj) const { return _impl.get() > obj._impl.get(); }
	inline bool operator == (const FileSystem& obj) const { return _impl.get() == obj._impl.get(); }
	inline bool operator != (const FileSystem& obj) const { return _impl.get() != obj._impl.get(); }
	inline FileSystemImpl* get() { return _impl.get(); }
	inline void clear() { _impl.reset(); }
	inline bool isValid() const { if (_impl.get() == nullptr) return false; return _impl->isValid(); }
	inline operator bool() const { return isValid(); }

public:
	inline bool format() { assert(_impl); return _impl->format(); }
	inline bool init(bool reformatOnFail = true) { assert(_impl); return _impl->init(reformatOnFail); }
	inline void loop() { assert(_impl); _impl->loop(); }

	// Factory
	inline File open(const char* path, File::Mode mode, const bool create = false) { return _impl->open(path, mode, create); }
	//File open(const char *path, const char *mode = "r", const bool create = false);

	// POSIX
	inline bool exists(const char* path) { assert(_impl); return _impl->exists(path); }
	inline bool remove(const char* path) { assert(_impl); return _impl->remove(path); }
	inline bool rename(const char* from_path, const char* to_path) { assert(_impl); return _impl->rename(from_path, to_path); }
	inline bool mkdir(const char* path) { assert(_impl); return _impl->mkdir(path); }
	inline bool rmdir(const char* path) { assert(_impl); return _impl->rmdir(path); }

	// Helper
	inline size_t size(const char* path) { assert(_impl); return _impl->size(path); }
	inline bool isDirectory(const char* path) { assert(_impl); return _impl->isDirectory(path); }
	inline std::list<std::string> listDirectory(const char* path, Callbacks::DirectoryListing callback = nullptr) { assert(_impl); return _impl->listDirectory(path, callback); }
	inline size_t storageSize() { assert(_impl); return _impl->storageSize(); }
	inline size_t storageAvailable() { assert(_impl); return _impl->storageAvailable(); }

	virtual size_t readFile(const char* path, uint8_t* buffer, size_t size) {
		File file = open(path, microStore::File::ModeRead);
		if (!file) return 0;
		size_t read = file.read(buffer, size);
		file.close();
		return read;
	}
	virtual size_t readFile(const char* path, std::vector<uint8_t>& data) {
		File file = open(path, microStore::File::ModeRead);
		if (!file) return 0;
		size_t size = file.size();
		data.resize(size);
		size_t read = file.read(data.data(), size);
		file.close();
		data.resize(read);
		return read;
	}
	virtual size_t writeFile(const char* path, const uint8_t* buffer, size_t len) {
		// CBA Consider whether remove is necessary/desired (is truncate broken on some platforms?)
		remove(path);
		File file = open(path, microStore::File::ModeWrite);
		if (!file) return 0;
		size_t wrote = file.write(buffer, len);
		file.close();
		return wrote;
	}
	virtual size_t writeFile(const char* path, const std::vector<uint8_t>& data) {
		// CBA Consider whether remove is necessary/desired (is truncate broken on some platforms?)
		remove(path);
		File file = open(path, microStore::File::ModeWrite);
		if (!file) return 0;
		size_t wrote = file.write(data.data(), data.size());
		file.close();
		return wrote;
	}

private:
	std::list<std::string> _empty;
protected:
public:

#ifndef NDEBUG
	inline std::string debugString() const {
		std::string dump;
		dump = "FileSystem object, this: " + std::to_string((uintptr_t)this) + ", data: " + std::to_string((uintptr_t)_impl.get());
		return dump;
	}
#endif

protected:
	std::shared_ptr<FileSystemImpl> _impl;
};

}

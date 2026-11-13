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

#if defined(USTORE_USE_POSIXFS)

#include "../File.h"
#include "../FileSystem.h"
#include "../Log.h"

#if defined(ESP32)
#include <LittleFS.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <string>

namespace microStore { namespace Adapters {

class PosixFileSystem : public microStore::FileSystem {

public:
	PosixFileSystem(const char* basepath = "") : microStore::FileSystem(new FileSystemImpl(basepath)) {}
    virtual ~PosixFileSystem() {}

    // Disable heap allocation
    void* operator new(std::size_t) = delete;
    void* operator new[](std::size_t) = delete;
    void* operator new(std::size_t, void*) = delete;

protected:

	class FileImpl : public microStore::FileImpl {

	private:
		int _fd = -1;
		bool _closed = false;
		size_t _available = 0;
		char _filename[1024];

	public:
		FileImpl(int fd) : microStore::FileImpl(), _fd(fd) { _available = size(); }
		virtual ~FileImpl() { if (!_closed) close(); }

	public:
		inline virtual const char* name() const {
			assert(_fd != -1);
#if 0
			char proclnk[1024];
			snprintf(proclnk, sizeof(proclnk), "/proc/self/fd/%d", fileno(_file));
			int r = ::readlink(proclnk, _filename, sizeof(_filename));
			if (r < 0) {
				return nullptr);
			}
			_filename[r] = '\0';
			return _filename;
#elif 0
			if (::fcntl(fd, F_GETPATH, _filename) < 0) {
				rerturn nullptr;
			}
			return _filename;
#else
			return nullptr;
#endif
		}

		inline virtual size_t size() const {
			assert(_fd != -1);
			struct stat st;
			::fstat(_fd, &st);
			return st.st_size;
		}

		inline virtual void close() {
			assert(_fd != -1);
			::close(_fd);
			_closed = true;
			_fd = -1;
		}

		inline virtual int read() {
			if (_available <= 0) {
				return EOF;
			}
			assert(_fd != -1);
			uint8_t ch;
			int status = ::read(_fd, &ch, 1);
			if (status < 1) {
				return EOF;
			}
			--_available;
			return ch;
		}
		inline virtual size_t write(uint8_t ch) {
			assert(_fd != -1);
			ssize_t status = ::write(_fd, &ch, 1);
			if (status < 1) {
				return status;
			}
			_available = (_available > 0) ? _available - 1 : 0;
			return 1;
		}
		inline virtual size_t read(uint8_t* buffer, size_t size) {
			assert(_fd != -1);
		    ssize_t read = ::read(_fd, buffer, size);
			if (read < 0) {
				return 0;
			}
			_available -= read;
			return (size_t)read;
		}
		inline virtual size_t write(const uint8_t* buffer, size_t size) {
			assert(_fd != -1);
		    ssize_t wrote = ::write(_fd, buffer, size);
			if (wrote < 0) {
				return 0;
			}
			_available = ((size_t)wrote <= _available) ? _available - (size_t)wrote : 0;
			return (size_t)wrote;
		}

		inline virtual int available() {
#if 0
			assert(_fd != -1);
			int size = 0;
			::ioctl(::fileno(_file), FIONREAD, &size);
			return size;
#else
			return _available;
#endif
		}
		inline virtual int peek() {
			if (_available <= 0) {
				return EOF;
			}
			assert(_fd != -1);
			uint8_t ch;
			int status = ::read(_fd, &ch, 1);
			if (status < 1) {
				return EOF;
			}
			::lseek(_fd, -1, SEEK_CUR);
			return ch;
		}
		inline virtual size_t tell() {
			assert(_fd != -1);
		    return ::lseek(_fd, 0, SEEK_CUR);
		}
		inline virtual long seek(uint32_t pos, microStore::SeekMode mode) {
			assert(_fd != -1);
			int whence;
			switch (mode) {
				case microStore::SeekMode::SeekModeCur:
					whence = SEEK_CUR;
					break;
				case microStore::SeekMode::SeekModeEnd:
					whence = SEEK_END;
					break;
				case microStore::SeekMode::SeekModeSet:
				default:
					whence = SEEK_SET;
					break;
			}
			long new_pos = ::lseek(_fd, pos, whence);
			if (new_pos >= 0) {
				size_t file_size = size();
				_available = ((size_t)new_pos <= file_size) ? file_size - (size_t)new_pos : 0;
			}
			return new_pos;
		}
		inline virtual void flush() {
			assert(_fd != -1);
			::fsync(_fd);
		}

		inline virtual bool isValid() const { if (_fd == -1) return false; return !_closed; }

	};

	class FileSystemImpl : public microStore::FileSystemImpl {

	public:
		FileSystemImpl(const char* basepath = "") : _basepath(basepath ? basepath : "") {}
	    virtual ~FileSystemImpl() {}

	public:

		virtual bool format() override {
#if defined(ESP32)
			USTORE_LOG("[ustore] Formatting PosixFileSystem\n");
			if (!LittleFS.format()) {
				USTORE_LOG("[ustore] Failed to format PosixFileSystem!\n");
				return false;
			}
			return true;
#else
			return false;
#endif
		}

		inline virtual bool init(bool reformatOnFail = true) override {
			USTORE_LOG("[ustore] Initializing PosixFileSystem\n");
#if defined(ESP32)
			// Initialize LittleFS for POSIX file access
			if (!LittleFS.begin(true, _basepath.c_str())) {
				USTORE_LOG("[ustore] Failed to initialize PosixFileSystem!\n");
				return false;
			}
#endif
			if (reformatOnFail) {
				// Ensure filesystem is writable and reformat if not
				bool verified = false;
				microStore::File init_test = open("./__init_test__", microStore::File::ModeWrite, true);
				if (init_test) {
					if (init_test.write("test", 4) == 4) {
						verified = true;
					}
					init_test.close();
				}
				if (!verified) {
					USTORE_LOG("[ustore] WARNING: PosixFileSystem check failed, reformatting!\n");
					format();
				}
				else {
					remove("./__init_test__");
					USTORE_LOG("[ustore] PosixFileSystem check passed!\n");
				}
			}
			return true;
		}

		virtual microStore::File open(const char* path_in, microStore::File::Mode mode, const bool create = false) override {
			const std::string full = resolve(path_in);
			const char* path = full.c_str();
			int flags;
			switch (mode) {
				// Read only. File must exist. ("r")
				case microStore::File::ModeRead:
					flags = O_RDONLY;
					break;
				// Write only. Creates file or truncates existing file. ("w")
				case microStore::File::ModeWrite:
					flags = O_WRONLY|O_CREAT|O_TRUNC;
					break;
				// Append only. Creates file if it doesn’t exist. Writes go to end. ("a")
				case microStore::File::ModeAppend:
					flags = O_WRONLY|O_CREAT|O_APPEND;
					break;
				// Read and write. Creates file or truncates existing file. ("w+")
				case microStore::File::ModeReadWrite:
					flags = O_RDWR|O_CREAT|O_TRUNC;
					break;
				// Read and append. Creates file if it doesn’t exist. ("a+")
				case microStore::File::ModeReadAppend:
					flags = O_RDWR|O_CREAT|O_APPEND;
					break;
				// Read and write. File must exist. ("r+") ???
				default:
					//flags = O_RDWR|O_CREAT;
					return {};
			}
			int fd = ::open(path, flags, 0644);
			if (fd == -1) {
				return {};
			}
			return microStore::File(new FileImpl(fd));
		}


		inline virtual bool exists(const char* path_in) override {
			const std::string full = resolve(path_in);
			const char* path = full.c_str();
			int fd = ::open(path, O_RDONLY);
			if (fd != -1) {
				::close(fd);
				return true;
			}
			return false;
		}

		inline virtual bool remove(const char* path_in) override {
			const std::string full = resolve(path_in);
			const char* path = full.c_str();
			return (::unlink(path) == 0);
		}

		inline virtual bool rename(const char* from_path, const char* to_path) override {
			return (::rename(resolve(from_path).c_str(), resolve(to_path).c_str()) == 0);
		}

		inline virtual bool mkdir(const char* path_in) override {
			const std::string full = resolve(path_in);
			const char* path = full.c_str();
			struct stat st = {0};
			if (::stat(path, &st) == 0) {
				if (S_ISDIR(st.st_mode)) {
					//USTORE_LOG("[ustore] mkdir: Directory %s already exists\n", path);
					return true;
				}
				//USTORE_LOG("[ustore] mkdir: Path %s already exists but is not a directory\n", path);
				return false;
			}
			//USTORE_LOG("[ustore] mkdir: Creating directory %s\n", path);
			return (::mkdir(path, 0700) == 0);
		}

		inline virtual bool rmdir(const char* path) override {
			return (::rmdir(resolve(path).c_str()) == 0);
		}


		inline virtual size_t size(const char* path_in) override {
			const std::string full = resolve(path_in);
			const char* path = full.c_str();
			struct stat st;
			if (stat(path, &st) != 0) {
				return 0;
			}
			return st.st_size;
		}

		inline virtual bool isDirectory(const char* path_in) override {
			const std::string full = resolve(path_in);
			const char* path = full.c_str();
			struct stat st = {0};
			return (::stat(path, &st) == 0 && S_ISDIR(st.st_mode));
		}

		virtual std::list<std::string> listDirectory(const char* path_in, Callbacks::DirectoryListing callback = nullptr) override {
			const std::string full = resolve(path_in);
			const char* path = full.c_str();
			std::list<std::string> files;
			DIR *dir = ::opendir(path);
			if (dir == NULL) {
				return files;
			}
			struct dirent *entry;
			while ((entry = ::readdir(dir)) != NULL) {
				if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
					continue;
				}
				char* name = entry->d_name;
				if (callback) callback(name);
				else files.push_back(name);
			}
			::closedir(dir);
			return files;
		}


		inline virtual size_t storageSize() override {
#if defined(ESP32)
			return LittleFS.totalBytes();
#else
			return 0;
#endif
		}

		inline virtual size_t storageAvailable() override {
#if defined(ESP32)
			return (LittleFS.totalBytes() - LittleFS.usedBytes());
#else
			return 0;
#endif
		}

	private:
		std::string resolve(const char* path) const {
			if (path == nullptr || _basepath.empty() || path[0] == '/') {
				return path ? path : "";
			}
			if (path[0] == '.' && path[1] == '/') {
				path += 2;
			}
			return _basepath + "/" + path;
		}
		std::string _basepath;
	};

};

} }

#endif

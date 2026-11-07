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
#include "FileSystem.h"
#include "Log.h"
#include "Utility.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <algorithm>
#include <vector>
#include <unordered_map>

namespace microStore {

/* ---------------- CONFIG ---------------- */

#ifndef USTORE_DEFAULT_SEGMENT_COUNT
#define USTORE_DEFAULT_SEGMENT_COUNT 8
#endif

#ifndef USTORE_DEFAULT_SEGMENT_SIZE
#define USTORE_DEFAULT_SEGMENT_SIZE 65536
#endif

#ifndef USTORE_WRITE_BUFFER_SIZE
//#define USTORE_WRITE_BUFFER_SIZE 4096
#define USTORE_WRITE_BUFFER_SIZE 512
#endif

#ifndef USTORE_MAX_KEY_LEN
#define USTORE_MAX_KEY_LEN 64
#endif

#ifndef USTORE_MAX_VALUE_LEN
#define USTORE_MAX_VALUE_LEN 1024
#endif

#ifndef USTORE_MAX_FILENAME_LEN
#define USTORE_MAX_FILENAME_LEN 64
#endif

#ifndef USTORE_COMPACT_RETRY_MS
//#define USTORE_COMPACT_RETRY_MS 5000   // ms between compact() retries after failure
#define USTORE_COMPACT_RETRY_MS 60000   // ms between compact() retries after failure
#endif

#ifndef USTORE_COMPACT_THRESHOLD
#define USTORE_COMPACT_THRESHOLD 25  // % dead records that trigger auto-compact (0 = disabled)
#endif

#ifndef USTORE_DEFAULT_TTL_SECS
#define USTORE_DEFAULT_TTL_SECS 0
#endif

#ifndef USTORE_DEFAULT_MAX_RECS
#define USTORE_DEFAULT_MAX_RECS 0
#endif

/* ---------------- CONSTANTS ---------------- */

static const uint32_t MAGIC_RECORD  = 0xC0DEC0DE;
static const uint32_t MAGIC_COMMIT  = 0xFACEB00C;
static const uint16_t FLAG_DELETE   = 1;
static const uint32_t JOURNAL_MAGIC = 0x4B564A4E;

enum JournalState { JOURNAL_NONE = 0, JOURNAL_COMPACTING = 1, JOURNAL_COMMIT = 2 };

#pragma pack(push,1)
struct Journal {
	uint32_t magic;
	uint32_t state;           // JOURNAL_NONE / COMPACTING / COMMIT
	uint32_t next_seg;        // segments 0..next_seg-1 are committed to compact.tmp
	uint32_t tmp_valid_size;  // byte count of compact.tmp that is durably valid
};
#pragma pack(pop)


/* ---------------- RECORD STRUCTURES ---------------- */

#pragma pack(push,1)

struct RecordHeader
{
	uint32_t magic;       // framing sentinel — MUST be first
	uint8_t  flags;       // record type (normal/delete) — affects how the rest of the header is interpreted
	uint8_t  key_len;     // length of the key
	uint16_t length;      // lenght of the value
	uint32_t timestamp;   // timestamp at record insertion
	uint32_t ttl;         // per-record TTL in seconds; 0 = use global policy
	uint32_t crc;         // integrity check — MUST be last
};

struct RecordCommit
{
	uint32_t magic;
};

#pragma pack(pop)

/* ---------------- STORAGE ENGINE ---------------- */

template<typename Allocator = std::allocator<uint8_t>>
class BasicFileStore
{
public:

	using allocator_type = Allocator;

	BasicFileStore(uint32_t segment_size = USTORE_DEFAULT_SEGMENT_SIZE, uint8_t segment_count = USTORE_DEFAULT_SEGMENT_COUNT) : BasicFileStore(Allocator{}, segment_size, segment_count) {}
	explicit BasicFileStore(const Allocator& alloc, uint32_t segment_size = USTORE_DEFAULT_SEGMENT_SIZE, uint8_t segment_count = USTORE_DEFAULT_SEGMENT_COUNT)
		: _alloc(alloc), _segment_size(segment_size), _segment_count(segment_count), _index(map_alloc_type(_alloc)) { write_buf_pos = 0; }

	~BasicFileStore()
	{
		if (isValid())
			flush_buffer();
	}

	inline allocator_type get_allocator() const { return _alloc; }

	inline bool isValid() const { if (!_filesystem) return false; return true; }
	inline operator bool() const { return isValid(); }

	bool init(FileSystem& filesystem, const char* prefix, bool clearOnInit = false, uint32_t segment_size = 0, uint8_t segment_count = 0)
	{
		if (segment_size > 0) _segment_size = segment_size;
		if (segment_count > 0) _segment_count = segment_count;
		USTORE_LOG("[ustore] init: Initializing FileStore with prefix=%s, segment_size=%u, segment_count=%u\n", prefix, _segment_size, _segment_count);

		_filesystem = filesystem;
		strncpy(base_prefix, prefix, sizeof(base_prefix) - 1);
		base_prefix[sizeof(base_prefix) - 1] = '\0';

		// Directory-mode prefix: a trailing '/' means base_prefix is a folder
		// and generated names omit the usual leading '_'. Try to create it;
		// failure is fine (typically it already exists).
		{
			size_t n = strlen(base_prefix);
			if (n > 0 && base_prefix[n - 1] == '/') {
				base_prefix[n - 1] = '\0';
				USTORE_LOG("[ustore] init: will create directory %s if necessary\n", base_prefix);
				if (!_filesystem.mkdir(base_prefix)) USTORE_LOG("[ustore] init: Failed to create directory %s\n", base_prefix);
				base_prefix[n - 1] = '/';
			}
		}

		if (clearOnInit) {
			clear();
		}

		recover_if_needed();

		if (!load_index())
			rebuild_index_from_segments();

		// Clean the store at init
		sweep();

		// Enforce max_recs at boot: prune excess entries and rewrite the
		// persistent index so evicted keys don't reappear on the next reboot.
		size_t pruned = prune_index_to_max_recs();
		if (pruned > 0) {
			char iname[USTORE_MAX_FILENAME_LEN]; index_name(iname);
			_filesystem.remove(iname);
			write_index_bulk();
		}

		open_index_for_append();

		// Resume at the highest segment that exists on flash, not always seg 0.
		// Without this, after reset current_segment stays 0 and new writes
		// overwrite previously-live segments, losing all their records.
		uint32_t resume_seg = 0;
		for (uint32_t i = 0; i < _segment_count; i++)
		{
			char name[USTORE_MAX_FILENAME_LEN];
			segment_name(i, name);
			File f = _filesystem.open(name, File::ModeRead);
			if (f) { f.close(); resume_seg = i; }
		}

		open_segment(resume_seg);

		USTORE_LOG("[ustore] init: FileStore initialized with %u active records\n", size());

		return true;
	}

	void close() {
		USTORE_LOG("[ustore] close: Closing FileStore\n");
		flush_buffer();
		if (index_file) index_file.close();
		if (active_file) active_file.close();
	}

	void clear()
	{
		USTORE_LOG("[ustore] clear: Clearing FileStore\n");
        if (!isValid()) {
			USTORE_LOG("[ustore] clear: store is invalid, skipping!\n");
			return;
		}

		flush_buffer();
		bool index_open = false;
		if (index_file) {
			index_file.close();
			index_open = true;
		}
		bool active_open = false;
		if (active_file) {
			active_file.close();
			active_open = true;
		}

		char name[USTORE_MAX_FILENAME_LEN];
		for(uint32_t i = 0; i < _segment_count; i++)
		{
			segment_name(i,name);
			//USTORE_LOG("[ustore] clear: removing segment file: %s\n", name);
//USTORE_LOG("[ustore] Removing file: %s\n", name);
			_filesystem.remove(name);
		}

		index_name(name);
		//USTORE_LOG("[ustore] clear: removing index file: %s\n", name);
		_filesystem.remove(name);

		_index.clear();
		_dead_since_compact = 0;
		current_segment=0;
		current_offset=0;
		write_buf_pos=0;

		if (index_open) open_index_for_append();
		if (active_open) open_segment(0);
	}

	/* -------- PUT -------- */

	bool put(const uint8_t* key, uint8_t key_len, const uint8_t* data, uint16_t len, uint32_t ttl = 0, uint32_t ts = microStore::time())
	{
        if (!isValid()) {
			USTORE_LOG("[ustore] put: store is invalid\n");
			return false;
		}

USTORE_LOG("[ustore] put: storing key=%s ttl=%u, ts=%u with data len %u\n", bin_str(key, key_len), ttl, ts, len);
		if (key_len > USTORE_MAX_KEY_LEN) {
			USTORE_LOG("[ustore] put: failed due to excessive key length: %u\n", key_len);
			return false;
		}
		if (len > USTORE_MAX_VALUE_LEN) {
			USTORE_LOG("[ustore] put: failed due to excessive data length: %u\n", len);
			return false;
		}

		// CBA Don't fail to put just because rotation fails
		//if (!rotate_segment_if_needed(len))
		//    return false;
		rotate_segment_if_needed(len);

		RecordHeader hdr;

		hdr.magic = MAGIC_RECORD;
		hdr.key_len = key_len;
		hdr.timestamp = ts;
		hdr.ttl = ttl;
		hdr.length = len;
		hdr.flags = 0;

		hdr.crc = crc32(0,(uint8_t*)&hdr,sizeof(hdr)-4);
		hdr.crc = crc32(hdr.crc,key,key_len);
		hdr.crc = crc32(hdr.crc,(uint8_t*)data,len);

		uint32_t offset = current_offset;

		append_buffer((uint8_t*)&hdr, sizeof(hdr));
		append_buffer(key, key_len);
		append_buffer(data, len);

		RecordCommit c;
		c.magic = MAGIC_COMMIT;

		append_buffer((uint8_t*)&c, sizeof(c));

		// ensure data is on flash before committing index entry
		// if flush fails then fail put without writing index entry
		if (!flush_buffer()) {
			return false;
		}

		// If an existing record was updated then increment _dead_since_compact
		bool key_exists = index_find(key, key_len) != nullptr;
		if (key_exists) _dead_since_compact++;

		// Enforce max_recs: if inserting a NEW key would push us over the limit,
		// evict the oldest record(s) BEFORE insert so the just-written key can't
		// be selected as its own eviction victim on timestamp ties. Orphaned disk
		// records will be reclaimed at the next compaction.
		if (!key_exists && policy_max_recs > 0 && _index.size() >= policy_max_recs)
			prune_index_to_max_recs(policy_max_recs - 1);

		index_insert(key, key_len, current_segment, offset, ts, ttl);

		persist_index_entry(key, key_len, current_segment, offset, ts, ttl);
//USTORE_LOG("[ustore] put: key %s offset %u\n", bin_str(key, key_len), offset);

		current_offset += sizeof(hdr)+key_len+len+sizeof(c);

		compact_if_threshold();

USTORE_LOG("[ustore] put: wrote key %s with data length %u\n", bin_str(key, key_len), len);
//USTORE_LOG("[ustore] put: %s\n", bin_str((uint8_t*)data, len));
		return true;
	}

	inline bool put(const char* key, const uint8_t* data, uint16_t len, uint32_t ttl = 0, uint32_t ts = microStore::time())
	{
		return put((const uint8_t*)key, (uint8_t)strlen(key), data, len, ttl, ts);
	}

	inline bool put(const char* key, const char* data, uint32_t ttl = 0, uint32_t ts = microStore::time())
	{
		return put((const uint8_t*)key, (uint8_t)strlen(key), (const uint8_t*)data, (uint16_t)strlen(data), ttl, ts);
	}

	inline bool put(const std::vector<uint8_t>& key, const uint8_t* data, uint16_t len, uint32_t ttl = 0, uint32_t ts = microStore::time())
	{
		return put(key.data(), (uint8_t)key.size(), data, len, ttl, ts);
	}

	inline bool put(const std::vector<uint8_t>& key, const std::vector<uint8_t>& data, uint32_t ttl = 0, uint32_t ts = microStore::time())
	{
		return put(key.data(), (uint8_t)key.size(), data.data(), (uint16_t)data.size(), ttl, ts);
	}

	inline bool put(const char* key, const std::string& data, uint32_t ttl = 0, uint32_t ts = microStore::time())
	{
		return put((const uint8_t*)key, (uint8_t)strlen(key), (const uint8_t*)data.c_str(), (uint16_t)data.length(), ttl, ts);
	}

	inline bool put(const std::string& key, const std::string& data, uint32_t ttl = 0, uint32_t ts = microStore::time())
	{
		return put((const uint8_t*)key.c_str(), (uint8_t)key.length(), (const uint8_t*)data.c_str(), (uint16_t)data.length(), ttl, ts);
	}

	/* -------- GET -------- */

	bool get(const uint8_t* key, uint8_t key_len, uint8_t* out, uint16_t* size)
	{
        if (!isValid()) {
			USTORE_LOG("[ustore] get: store is invalid\n");
			return false;
		}

USTORE_LOG("[ustore] get: fetching key %s with data size %u\n", bin_str(key, key_len), *size);
		if (key_len > USTORE_MAX_KEY_LEN) {
			USTORE_LOG("[ustore] get: failed due to excessive key length: %u\n", key_len);
			return false;
		}

		flush_buffer();

		IndexValue* e = index_find(key, key_len);
		if (!e) {
			USTORE_LOG("[ustore] get: key %s not found in index\n", bin_str(key, key_len));
			return false;
		}
//USTORE_LOG("[ustore] get: key %s offset %lu\n", bin_str(key, key_len), e->offset);

		if (is_ttl_expired(e->timestamp, e->ttl)) {
			index_remove(key, key_len);
			USTORE_LOG("[ustore] get: key %s expired by TTL\n", bin_str(key, key_len));
			return false;
		}

		char name[USTORE_MAX_FILENAME_LEN];
		segment_name(e->segment, name);

		File f = _filesystem.open(name, File::ModeRead);
		if (!f) {
			USTORE_LOG("[ustore] get: key %s failed to open file %s\n", bin_str(key, key_len), name);
			return false;
		}

		f.seek(e->offset, SeekModeSet);

		RecordHeader hdr;

		//if (f.read(&hdr, sizeof(hdr)) != sizeof(hdr)) {
		size_t len = f.read(&hdr, sizeof(hdr));
//USTORE_LOG("[ustore] get: key %s read header of size %u\n", bin_str(key, key_len), len);
		if (len != sizeof(hdr)) {
			USTORE_LOG("[ustore] get: key %s header read failed\n", bin_str(key, key_len));
			f.close();
			return false;
		}

		if (hdr.magic != MAGIC_RECORD ||
			hdr.key_len > USTORE_MAX_KEY_LEN ||
			hdr.length > USTORE_MAX_VALUE_LEN)
		{
			USTORE_LOG("[ustore] get: key %s has corrupted record\n", bin_str(key, key_len));
			f.close();
			return false;
		}

		// CBA If this is only a size request then skip reading value
		if (out != nullptr) {
			f.seek((long)(e->offset+sizeof(hdr)+hdr.key_len), SeekModeSet);
			size_t read = std::min(hdr.length, *size);
			if (f.read(out, read) != read) {
				USTORE_LOG("[ustore] get: key %s value read failed\n", bin_str(key, key_len));
				f.close();
				return false;
			}
		}

		*size = hdr.length;

		f.close();

USTORE_LOG("[ustore] get: returning key %s with data length %u\n", bin_str(key, key_len), *size);
//USTORE_LOG("[ustore] get: %s\n", bin_str((uint8_t*)out, *size));
		return true;
	}

	inline bool get(const char* key, uint8_t* out, uint16_t* size)
	{
		return get((const uint8_t*)key, (uint8_t)strlen(key), out, size);
	}

	inline bool get(const char* key, char* out, uint16_t* size)
	{
		return get((const uint8_t*)key, (uint8_t)strlen(key), (uint8_t*)out, size);
	}

	inline bool get(const std::vector<uint8_t>& key, uint8_t* out, uint16_t* size)
	{
		return get(key.data(), (uint8_t)key.size(), out, size);
	}

	inline bool get(const std::vector<uint8_t>& key, std::vector<uint8_t>& out)
	{
		uint16_t size = USTORE_MAX_VALUE_LEN;
		out.resize(size);
		if (!get(key.data(), (uint8_t)key.size(), out.data(), &size)) {
			return false;
		}
		out.resize(size);
		return true;
	}

	inline bool get(const char* key, std::string& out)
	{
		uint16_t size = USTORE_MAX_VALUE_LEN;
		out.resize(size);
		// C++17 supported
		//if (!get(key.data(), (uint8_t)key.size(), out.data(), &size)) {
		// C++14 supported
		if (!get((const uint8_t*)key, (uint8_t)strlen(key), (uint8_t*)&out[0], &size)) {
			return false;
		}
		out.resize(size);
		return true;
	}

	inline bool get(const std::string& key, std::string& out)
	{
		uint16_t size = USTORE_MAX_VALUE_LEN;
		out.resize(size);
		// C++17 supported
		//if (!get(key.data(), (uint8_t)key.size(), out.data(), &size)) {
		// C++14 supported
		if (!get((const uint8_t*)key.c_str(), (uint8_t)key.length(), (uint8_t*)&out[0], &size)) {
			return false;
		}
		out.resize(size);
		return true;
	}

	/* -------- REMOVE -------- */

	bool remove(const uint8_t* key, uint8_t key_len)
	{
        if (!isValid()) {
			USTORE_LOG("[ustore] remove: store is invalid\n");
			return false;
		}

USTORE_LOG("[ustore] remove: removing key %s\n", bin_str(key, key_len));
		if (key_len > USTORE_MAX_KEY_LEN) {
			USTORE_LOG("[ustore] remove: failed due to excessive key length: %u\n", key_len);
			return false;
		}

		// CBA TODO Determine whether we should short-circuit here is the key already does not exist
		IndexValue* e = index_find(key, key_len);
		if (!e) {
			USTORE_LOG("[ustore] remove: key %s not found in index\n", bin_str(key, key_len));
			return false;
		}

		RecordHeader hdr;
		hdr.magic = MAGIC_RECORD;
		hdr.key_len = key_len;
		hdr.timestamp = 0;
		hdr.length = 0;
		hdr.flags = FLAG_DELETE;
		hdr.crc = crc32(0, (uint8_t*)&hdr, sizeof(hdr)-4);
		hdr.crc = crc32(hdr.crc, key, key_len);

		append_buffer((uint8_t*)&hdr, sizeof(hdr));
		append_buffer(key, key_len);

		RecordCommit c;
		c.magic=MAGIC_COMMIT;

		append_buffer((uint8_t*)&c, sizeof(c));

		flush_buffer();  // ensure tombstone is on flash before committing index entry

		// If an existing record was removed then increment _dead_since_compact
		if (index_find(key, key_len)) _dead_since_compact++;

		index_remove(key, key_len);

		persist_index_entry(key, key_len, 0xFFFFFFFF, 0);

		current_offset += sizeof(RecordHeader) + key_len + sizeof(RecordCommit);

		compact_if_threshold();

		return true;
	}

	inline bool remove(const char* key)
	{
		return remove((const uint8_t*)key, (uint8_t)strlen(key));
	}

	inline bool remove(const std::vector<uint8_t>& key)
	{
		return remove(key.data(), (uint8_t)key.size());
	}

	/* -------- EXISTS -------- */

	bool exists(const uint8_t* key, uint8_t key_len)
	{
        if (!isValid()){
			USTORE_LOG("[ustore] exists: store is invalid\n");
			return false;
		}

USTORE_LOG("[ustore] exists: checking key %s\n", bin_str(key, key_len));

		if (key_len > USTORE_MAX_KEY_LEN) {
			USTORE_LOG("[ustore] exists: failed due to excessive key length: %u\n", key_len);
			return false;
		}

		IndexValue* e = index_find(key, key_len);
		if (!e) {
			USTORE_LOG("[ustore] exists: key %s not found in index\n", bin_str(key, key_len));
			return false;
		}

		if (is_ttl_expired(e->timestamp, e->ttl)) {
			index_remove(key, key_len);
			USTORE_LOG("[ustore] exists: key %s expired by TTL\n", bin_str(key, key_len));
			return false;
		}
USTORE_LOG("[ustore] exists: found key %s\n", bin_str(key, key_len));

		return true;
	}

	inline bool exists(const char* key)
	{
		return exists((const uint8_t*)key, (uint8_t)strlen(key));
	}

	inline bool exists(const std::vector<uint8_t>& key)
	{
		return exists(key.data(), (uint8_t)key.size());
	}

	/* -------- SIZE -------- */

	inline size_t size() const
	{
        if (!isValid()) {
			USTORE_LOG("[ustore] size: store is invalid\n");
			return 0;
		}
		return _index.size();
	}

	/* -------- POLICY -------- */

	inline void set_ttl_secs(uint32_t ttl_s)
	{
USTORE_LOG("[ustore] set_ttl_secs: %u\n", ttl_s);
		policy_ttl_secs    = ttl_s;
	}

	inline void set_max_recs(uint32_t max_recs)
	{
USTORE_LOG("[ustore] set_max_recs: %u\n", max_recs);
		policy_max_recs = max_recs;
	}

	/* -------- DUMP INFO -------- */

	void dumpInfo(bool detailed = true)
	{
		flush_buffer();

		uint32_t total_file_size = 0;
		uint32_t total_entries = 0;
		uint32_t total_tomb_entries = 0;
		uint32_t total_tomb_bytes = 0;
		uint32_t total_dead_entries = 0;
		uint32_t total_dead_bytes = 0;

		for(uint32_t seg = 0; seg < _segment_count; seg++) {
			char name[USTORE_MAX_FILENAME_LEN];
			segment_name(seg, name);

			File f = _filesystem.open(name, File::ModeRead);
			if (!f) break;  // segments are contiguous; first missing one ends the scan

			uint32_t file_size = 0;
			uint32_t entries = 0;
			uint32_t tomb_entries = 0;
			uint32_t tomb_bytes = 0;
			uint32_t dead_entries = 0;
			uint32_t dead_bytes = 0;

			{
				f.seek(0, SeekModeEnd);
				file_size = (uint32_t)f.tell();
				f.seek(0, SeekModeSet);

				while(true) {
					uint32_t offset = (uint32_t)f.tell();

					RecordHeader hdr;
					if(f.read(&hdr, sizeof(hdr)) != sizeof(hdr)) break;

					if(hdr.magic != MAGIC_RECORD) break;

					if(hdr.key_len > USTORE_MAX_KEY_LEN) break;

					uint8_t key[USTORE_MAX_KEY_LEN];
					if(f.read(key, hdr.key_len) != hdr.key_len) break;

					f.seek((long)(offset + sizeof(hdr) + hdr.key_len + hdr.length), SeekModeSet);

					RecordCommit c;
					if(f.read(&c, sizeof(c)) != sizeof(c)) break;

					if(c.magic != MAGIC_COMMIT) break;

					uint32_t record_size = sizeof(RecordHeader) + hdr.key_len + hdr.length + sizeof(RecordCommit);

					if (hdr.flags & FLAG_DELETE) {
						tomb_entries++;
						tomb_bytes += record_size;
					}
					else {
						IndexValue* iv = index_find(key, hdr.key_len);
						bool live = (iv && iv->segment == seg && iv->offset == offset);
						if(live) {
							entries++;
						}
						else {
							dead_entries++;
							dead_bytes += record_size;
						}
					}
				}

				f.close();
			}

			total_file_size += file_size;
			total_entries += entries;
			total_tomb_entries += tomb_entries;
			total_tomb_bytes += tomb_bytes;
			total_dead_entries += dead_entries;
			total_dead_bytes += dead_bytes;

			if (detailed) {
				bool active = (seg == current_segment);
				printf("%s%s:\n", name, active ? " (ACTIVE)" : "");
				printf("  Bytes       : %8u\n", (unsigned)file_size);
				printf("  Entries     : %8u\n", (unsigned)entries);
				printf("  Tomb bytes  : %8u\n", (unsigned)tomb_bytes);
				printf("  Tomb entries: %8u\n", (unsigned)tomb_entries);
				printf("  Dead bytes  : %8u\n", (unsigned)dead_bytes);
				printf("  Dead entries: %8u\n", (unsigned)dead_entries);
			}
		}

		printf("TOTAL:\n");
		printf("  Bytes       : %8u\n", (unsigned)total_file_size);
		printf("  Entries     : %8u\n", (unsigned)total_entries);
		printf("  Tomb bytes  : %8u\n", (unsigned)total_tomb_bytes);
		printf("  Tomb entries: %8u\n", (unsigned)total_tomb_entries);
		printf("  Dead bytes  : %8u\n", (unsigned)total_dead_bytes);
		printf("  Dead entries: %8u\n", (unsigned)total_dead_entries);
	}

private:

	/* -------- INDEX TYPES (hoisted so iterator can reference them) -------- */

	struct VectorHash {
		template<typename Alloc>
		size_t operator()(const std::vector<uint8_t, Alloc>& v) const {
			uint32_t h = 2166136261u;
			for(uint8_t b : v) { h ^= b; h *= 16777619u; }
			return h;
		}
	};

	struct IndexValue {
		uint32_t segment;
		uint32_t offset;
		uint32_t timestamp;
		uint32_t ttl;
	};

	template<typename T>
	using rebind_alloc = typename std::allocator_traits<Allocator>::template rebind_alloc<T>;

	using byte_alloc_type = rebind_alloc<uint8_t>;
	using KeyType         = std::vector<uint8_t, byte_alloc_type>;
	using MapPairType     = std::pair<const KeyType, IndexValue>;
	using map_alloc_type  = rebind_alloc<MapPairType>;
	using IndexMap = std::unordered_map<
		KeyType, IndexValue, VectorHash, std::equal_to<KeyType>, map_alloc_type>;

	KeyType make_key(const uint8_t* key, uint8_t key_len) const {
		return KeyType(key, key + key_len, byte_alloc_type(_alloc));
	}

public:

	/* -------- ENTRY -------- */

	struct Entry {
		std::vector<uint8_t> key;
		std::vector<uint8_t> value;
		uint32_t timestamp;
		uint32_t ttl;
	};

	/* -------- ITERATOR -------- */

	// Forward iterator over all live key-value records.
	// Walks the in-memory index; key and timestamp are available with zero disk
	// I/O. The value field is lazy-loaded on first dereference (*it or it->)
	// and cached until operator++ advances to the next record.
	// WARNING: Mutating the store during iteration (put/remove/clear/compact) is
	// undefined behaviour — unordered_map iterator invalidation rules apply.
	class iterator {
	public:
		using iterator_category = std::forward_iterator_tag;
		using value_type        = Entry;
		using difference_type   = std::ptrdiff_t;
		using pointer           = const Entry*;
		using reference         = const Entry&;

		// Default constructor — produces a singular (non-dereferenceable) iterator.
		iterator() : store_(nullptr), value_loaded_(false) {}

		// operator*  — full dereference; loads the value from disk on first call
		//              for this position. Use this when you need entry.value.
		// operator-> — metadata-only; returns key and timestamp (already in memory)
		//              without touching disk. entry.value will be empty unless
		//              operator* was already called for this position.
		reference operator*()  const { if (!value_loaded_) load_value(); return current_; }
		pointer   operator->() const { return &current_; }

		iterator& operator++() {
			++pos_;
			load_meta();
			return *this;
		}

		iterator operator++(int) {
			iterator tmp = *this;
			++(*this);
			return tmp;
		}

		bool operator==(const iterator& o) const { return pos_ == o.pos_; }
		bool operator!=(const iterator& o) const { return pos_ != o.pos_; }

	private:
		friend class BasicFileStore<Allocator>;

		using idx_iter = typename IndexMap::iterator;

		iterator(BasicFileStore* store, idx_iter pos, idx_iter end)
			: store_(store), pos_(pos), end_(end), value_loaded_(false)
		{
			load_meta();
		}

		// Populate key and timestamp from the in-memory index. Zero disk I/O.
		// Resets value_loaded_ so the next dereference re-reads from disk.
		void load_meta() {
			if (pos_ == end_) return;
			iv_                = pos_->second;
			current_.key.assign(pos_->first.begin(), pos_->first.end());
			current_.timestamp = iv_.timestamp;
			current_.ttl       = iv_.ttl;
			current_.value.clear();
			value_loaded_      = false;
		}

		// Read the value from disk into current_.value. Called lazily by
		// operator* / operator->. Marked const so it can mutate mutable members.
		void load_value() const {
			char name[USTORE_MAX_FILENAME_LEN];
			store_->segment_name(iv_.segment, name);

			File f = store_->_filesystem.open(name, File::ModeRead);
			value_loaded_ = true;
			if (!f) { current_.value.clear(); return; }

			f.seek((long)iv_.offset, SeekModeSet);
			RecordHeader hdr;
			f.read(&hdr, sizeof(hdr));
			f.seek((long)(iv_.offset + sizeof(hdr) + hdr.key_len), SeekModeSet);
			current_.value.resize(hdr.length);
			if (hdr.length > 0)
				f.read(current_.value.data(), hdr.length);
			f.close();
		}

		BasicFileStore*      store_;
		idx_iter             pos_;
		idx_iter             end_;
		IndexValue           iv_;
		mutable bool         value_loaded_;
		mutable Entry        current_;
	};

	/* -------- BEGIN / END -------- */

	iterator begin() {
		flush_buffer();   // ensure pending writes are visible before iterating
		return iterator(this, _index.begin(), _index.end());
	}

	iterator end() {
		return iterator(this, _index.end(), _index.end());
	}

private:

	/* -------- BUFFERED WRITES -------- */

	void append_buffer(const uint8_t* data, size_t len)
	{
		const uint8_t* p=(const uint8_t*)data;

		while(len)
		{
			size_t space = USTORE_WRITE_BUFFER_SIZE - write_buf_pos;
			size_t n = (len < space) ? len : space;

			memcpy(write_buf+write_buf_pos, p, n);

			write_buf_pos += n;
			p += n;
			len -= n;

			if(write_buf_pos == USTORE_WRITE_BUFFER_SIZE)
				flush_buffer();
		}
	}

	bool flush_buffer()
	{
		if (write_buf_pos == 0) return true;
		if (!active_file) {
			USTORE_LOG("[ustore] ERROR: Active file is not valid, failed to flush buffer\n");
			// CBA Must reset buffer pos to avoid an infinite flushing loop
			write_buf_pos = 0;
			return false;
		}
		active_file.write(write_buf, write_buf_pos);
		active_file.flush();
		write_buf_pos = 0;
		return true;
	}

	/* -------- INDEX -------- */

	IndexValue* index_find(const uint8_t* key, uint8_t key_len)
	{
		auto it = _index.find(make_key(key, key_len));
		return (it != _index.end()) ? &it->second : nullptr;
	}

	void index_insert(const uint8_t* key, uint8_t key_len, uint32_t seg, uint32_t off, uint32_t ts = 0, uint32_t ttl = 0)
	{
		IndexValue& iv = _index[make_key(key, key_len)];
		iv.segment   = seg;
		iv.offset    = off;
		iv.timestamp = ts;
		iv.ttl       = ttl;
	}

	void index_remove(const uint8_t* key, uint8_t key_len)
	{
		_index.erase(make_key(key, key_len));
	}

	/* -------- POLICY HELPERS -------- */

	// Clean out any records with timestamp in the future.
	size_t sweep()
	{
		size_t evicted = 0;
		uint32_t current_time = microStore::time();
		using KTSAlloc = rebind_alloc<KeyType>;
		KTSAlloc kts_alloc(_alloc);
		std::vector<KeyType, KTSAlloc> evict(kts_alloc);
		evict.reserve(_index.size());
		for (auto& kv : _index) {
			if (kv.second.timestamp > current_time) {
				USTORE_LOG("[ustore] sweep: removing record with timestamp %u seconds in the future\n", kv.second.timestamp - current_time);
				evict.push_back(kv.first);
			}
		}
		for (auto& key : evict) {
			_index.erase(key);
			++evicted;
		}
		// Record(s) evicted so increment _dead_since_compact
		_dead_since_compact += evicted;
		if (evicted > 0) USTORE_LOG("[ustore] sweep: evicted %lu records\n", evicted);
		return evicted;
	}

	bool is_ttl_expired(uint32_t ts, uint32_t record_ttl) const
	{
		uint32_t effective_ttl = (record_ttl > 0) ? record_ttl : policy_ttl_secs;
		return effective_ttl > 0 && microStore::time() > ts && (microStore::time() - ts) >= effective_ttl;
	}

	// Evict the oldest records (by timestamp) until _index.size() <= target.
	// If target is 0 (default) policy_max_recs is used; pass a smaller target
	// from put() to make room for a new key before insert so the new key can't
	// be picked as its own eviction victim on timestamp ties.
	// Returns the number of entries evicted.
	// When to_evict == 1 (the common put() path), a simple O(n) scan is used to
	// avoid heap allocation.  When to_evict > 1 (init / compact), a partial sort
	// is used to evict all excess entries in a single pass.
	size_t prune_index_to_max_recs(uint32_t target = 0)
	{
		if (target == 0) target = policy_max_recs;
		if (target == 0 || _index.size() <= target) return 0;

		size_t to_evict = _index.size() - target;

		if (to_evict == 1) {
			// Fast path: single linear scan for the oldest entry.
			auto oldest = _index.begin();
			for (auto it = _index.begin(); it != _index.end(); ++it) {
				if (it->second.timestamp < oldest->second.timestamp) oldest = it;
			}
			_index.erase(oldest);
		}
		else {
			// Bulk path: collect (timestamp, key) pairs, partial-sort, then erase.
			using KTSPair = std::pair<uint32_t, KeyType>;
			using KTSAlloc = rebind_alloc<KTSPair>;
			KTSAlloc kts_alloc(_alloc);
			std::vector<KTSPair, KTSAlloc> candidates(kts_alloc);
			candidates.reserve(_index.size());
			for (auto& kv : _index)
				candidates.push_back(std::make_pair(kv.second.timestamp, kv.first));
			std::partial_sort(candidates.begin(), candidates.begin() + (long)to_evict, candidates.end(),
				[](const KTSPair& a, const KTSPair& b){ return a.first < b.first; });
			for (size_t i = 0; i < to_evict; i++) {
				_index.erase(candidates[i].second);
			}
		}
		// Record(s) evicted so increment _dead_since_compact
		_dead_since_compact += to_evict;
USTORE_LOG("[ustore] prune_index_to_max_recs: evicted %lu records to policy_max_recs\n", to_evict);

		return to_evict;
	}

	/* -------- INDEX FILE -------- */

	bool persist_index_entry(const uint8_t* key, uint8_t key_len, uint32_t seg, uint32_t off, uint32_t ts = 0, uint32_t ttl = 0)
	{
		if (!index_file) {
			USTORE_LOG("[ustore] ERROR: Index file is not valid\n");
			return false;
		}

		index_file.write(&key_len, 1);
		index_file.write(key, key_len);
		index_file.write(&seg, 4);
		index_file.write(&off, 4);
		index_file.write(&ts, 4);
		index_file.write(&ttl, 4);
		index_file.flush();  // explicit fsync — guarantees entry reaches flash

		return true;
	}

	bool write_index_bulk()
	{
		char name[USTORE_MAX_FILENAME_LEN];
		index_name(name);

		File f = _filesystem.open(name, File::ModeAppend);
		if (!f) return false;

		for (auto& kv : _index)
		{
			uint8_t  key_len = (uint8_t)kv.first.size();
			uint32_t seg     = kv.second.segment;
			uint32_t off     = kv.second.offset;
			uint32_t ts      = kv.second.timestamp;

			uint32_t ttl     = kv.second.ttl;

			f.write(&key_len, 1);
			f.write(kv.first.data(), key_len);
			f.write(&seg, 4);
			f.write(&off, 4);
			f.write(&ts, 4);
			f.write(&ttl, 4);
		}

		f.flush();
		f.close();
		return true;
	}

	bool load_index()
	{
		char name[USTORE_MAX_FILENAME_LEN];
		index_name(name);

		File f = _filesystem.open(name, File::ModeRead);
		if (!f) return false;

		while(true)
		{
			uint8_t key_len;
			uint8_t key[USTORE_MAX_KEY_LEN];
			uint32_t seg;
			uint32_t off;

			if(f.read(&key_len, 1) != 1) break;
			if(key_len > USTORE_MAX_KEY_LEN) break;
			if(f.read(key, key_len) != key_len) break;
			if(f.read(&seg, 4) != 4) break;
			if(f.read(&off, 4) != 4) break;
			uint32_t ts = 0;
			if(f.read(&ts, 4) != 4) break;
			uint32_t ttl = 0;
			if(f.read(&ttl, 4) != 4) break;

			// seg==0xFFFFFFFF is a deletion sentinel written by remove()
			if(seg==0xFFFFFFFF)
				index_remove(key, key_len);
			else
				index_insert(key, key_len, seg, off, ts, ttl);
		}

		f.close();

		return true;
	}

	/* -------- INDEX FILE OPEN HELPER -------- */

	void open_index_for_append()
	{
		char name[USTORE_MAX_FILENAME_LEN];
		index_name(name);
		index_file = _filesystem.open(name, File::ModeAppend);
	}

	const char* char_str(const uint8_t* key, size_t len)
	{
		static char str[USTORE_MAX_VALUE_LEN+1];
		int n = 0;
		for (size_t i = 0; i < len && i < USTORE_MAX_VALUE_LEN; ++i) {
			str[n] = key[i];
			n++;
		}
		str[n] = 0;
		return str;
	}

	const char* bin_str(const uint8_t* key, size_t len)
	{
		static char str[USTORE_MAX_VALUE_LEN*2+1];
		int n = 0;
		for (size_t i = 0; i < len && i < USTORE_MAX_VALUE_LEN; ++i) {
			//if (key[i] > 31 && key[i] < 127) {
			//	str[n++] = key[i];
			//}
			//else {
				snprintf(str+n, sizeof(str)-n, "%02x", key[i]);
				n += 2;
			//}
		}
		str[n] = 0;
		return str;
	}

	/* -------- SEGMENTS -------- */

	// Returns "" when base_prefix ends with '/' (directory mode), else "_".
	inline const char* prefix_sep() const
	{
		size_t n = strlen(base_prefix);
		return (n > 0 && base_prefix[n - 1] == '/') ? "" : "_";
	}

	void segment_name(uint32_t id,char* out)
	{
		snprintf(out, USTORE_MAX_FILENAME_LEN, "%s%sseg%u.dat", base_prefix, prefix_sep(), id);
	}

	void index_name(char* out)
	{
		snprintf(out, USTORE_MAX_FILENAME_LEN, "%s%sindex.dat", base_prefix, prefix_sep());
	}

	void journal_name(char* out)
	{
		snprintf(out, USTORE_MAX_FILENAME_LEN, "%s%sjournal.dat", base_prefix, prefix_sep());
	}

	void tmp_name(char* out)
	{
		snprintf(out, USTORE_MAX_FILENAME_LEN, "%s%scompact.tmp", base_prefix, prefix_sep());
	}

	bool open_segment(uint32_t id)
	{
		if (active_file) {
USTORE_LOG("[ustore] Closing active file\n");
			active_file.close();
		}

		char name[USTORE_MAX_FILENAME_LEN];
		segment_name(id,name);

USTORE_LOG("[ustore] open_segment: opening active file: %s\n", name);
		active_file = _filesystem.open(name, File::ModeReadAppend);
		if (!active_file) {
			USTORE_LOG("[ustore] ERROR: Failed to open active file: %s\n", name);
			return false;
		}
		current_segment=id;
		active_file.seek(0,SeekModeEnd);
		current_offset=active_file.tell();
		return true;
	}

	bool rotate_segment_if_needed(uint32_t write_size)
	{
		if (current_offset + write_size + sizeof(RecordHeader) + sizeof(RecordCommit) < _segment_size)
			return true;

USTORE_LOG("[ustore] rotate_segment_if_needed: rotating segment...\n");
		flush_buffer();

		if (active_file) {
USTORE_LOG("[ustore] rotate_segment_if_needed: closing active file\n");
			active_file.close();
		}

		current_segment++;

		if (current_segment >= _segment_count) {
			if (compact_in_cooldown &&
				(microStore::millis() - compact_cooldown_start_ms) < USTORE_COMPACT_RETRY_MS)
			{
				USTORE_LOG("[ustore] Compact skipped: cooldown active\n");
				current_segment = _segment_count - 1;
				open_segment(current_segment);
				return false;
			}
			if (!compact()) {
				compact_in_cooldown       = true;
				compact_cooldown_start_ms = microStore::millis();
				current_segment = _segment_count - 1;
				open_segment(current_segment);
				return false;
			}
			compact_in_cooldown = false;
			current_segment = 1;  // seg0 = compacted data; fresh writes start at seg1
		}

		open_segment(current_segment);
		return true;
	}

	/* -------- JOURNAL HELPERS -------- */

	void write_journal(uint32_t state, uint32_t next_seg = 0, uint32_t tmp_valid_size = 0)
	{
		char name[USTORE_MAX_FILENAME_LEN]; journal_name(name);
		File f = _filesystem.open(name, File::ModeWrite);
		if (!f) return;
		Journal j;
		j.magic          = JOURNAL_MAGIC;
		j.state          = state;
		j.next_seg       = next_seg;
		j.tmp_valid_size = tmp_valid_size;
		f.write(&j, sizeof(j));
		f.flush();
		f.close();
	}

	void clear_journal()
	{
		char name[USTORE_MAX_FILENAME_LEN];
		journal_name(name);
		_filesystem.remove(name);
	}

	void compact_if_threshold()
	{
#if USTORE_COMPACT_THRESHOLD > 0
		// CBA Using total records can lead to excessive compaction (ie, deleting `1 record when there are only two triggers)
		// Only triggering relative to policy_max_recs instead
		//uint32_t limit = (uint32_t)_index.size() + _dead_since_compact;
		//if (total > 0 && _dead_since_compact * 100 / total >= USTORE_COMPACT_THRESHOLD) {
		if (policy_max_recs > 0 && _dead_since_compact * 100 / policy_max_recs >= USTORE_COMPACT_THRESHOLD) {
USTORE_LOG("[ustore] Compaction triggered by deleted threshold (dead=%u, max=%u)\n", _dead_since_compact, policy_max_recs);
			if (compact()) {
				// After threshold-triggered compaction, seg0 holds the compacted data.
				// Open seg1 for new writes, mirroring what rotate_segment_if_needed() does
				// after a rotation-triggered compaction.
				current_segment = 1;
				open_segment(current_segment);
			}
		}
#endif
	}

	void recover_if_needed()
	{
		char name[USTORE_MAX_FILENAME_LEN];
		journal_name(name);
		File f = _filesystem.open(name, File::ModeRead);
		if (!f) return;

		Journal j;
		size_t n = f.read(&j, sizeof(j));
		f.close();

		if (n != sizeof(j) || j.magic != JOURNAL_MAGIC) {
			_filesystem.remove(name);
			return;
		}

		char tmp_path[USTORE_MAX_FILENAME_LEN]; tmp_name(tmp_path);

		if (j.state == JOURNAL_COMMIT) {
			// Tmp file is complete — finish the swap.
			finalize_compaction();
		} else if (j.next_seg == 0) {
			// COMPACTING, no source segments deleted yet — tmp may be partial, discard it.
			// All original segments are intact; normal boot proceeds.
			_filesystem.remove(tmp_path);
		} else {
			// COMPACTING with next_seg > 0: source segments 0..next_seg-1 have been deleted.
			// compact.tmp holds their live records up to byte tmp_valid_size (bytes beyond
			// that are either extra valid records from a flushed-but-not-journaled segment,
			// or a partial write stopped by the crash — both are handled harmlessly by the
			// segment scan in rebuild_index_from_segments()).
			//
			// Recovery strategy: rename compact.tmp → segment_0.dat so that the normal
			// boot index rebuild picks it up alongside the surviving segments next_seg..7.
			// The next compaction cycle will consolidate everything correctly.
			char seg0[USTORE_MAX_FILENAME_LEN]; segment_name(0, seg0);
			if (!_filesystem.rename(tmp_path, seg0)) {
				// Rename failed (e.g. filesystem error). Best effort: leave compact.tmp
				// in place; the next boot will retry this same recovery path.
				return;  // keep journal so next boot retries
			}
		}

		_filesystem.remove(name);  // clear journal
	}

	/* -------- FINALIZE COMPACTION -------- */

	void finalize_compaction()
	{
		char tmp_path[USTORE_MAX_FILENAME_LEN]; tmp_name(tmp_path);
		char seg0[USTORE_MAX_FILENAME_LEN];
		segment_name(0, seg0);

		// Remove all existing segments, then rename tmp → seg0.
		for (uint32_t i = 0; i < _segment_count; i++) {
			char sname[USTORE_MAX_FILENAME_LEN];
			segment_name(i, sname);
USTORE_LOG("[ustore] Removing file: %s\n", sname);
			_filesystem.remove(sname);
		}
USTORE_LOG("[ustore] Moving temporary file to: %s\n", seg0);
		if (!_filesystem.rename(tmp_path, seg0)) {
			_filesystem.remove(tmp_path);
			return;
		}

		// Rebuild in-memory index by scanning seg0.
		_index.clear();
		File f = _filesystem.open(seg0, File::ModeRead);
		if (f) {
			uint32_t scan_offset = 0;
			uint8_t key_buf[USTORE_MAX_KEY_LEN];
			while (true) {
				RecordHeader hdr;
				if (f.read(&hdr, sizeof(hdr)) != sizeof(hdr)) break;
				if (hdr.magic != MAGIC_RECORD || hdr.key_len == 0 ||
					hdr.key_len > USTORE_MAX_KEY_LEN || hdr.length > USTORE_MAX_VALUE_LEN) break;
				if (f.read(key_buf, hdr.key_len) != hdr.key_len) break;
				f.seek((long)(scan_offset + sizeof(hdr) + hdr.key_len + hdr.length), SeekModeSet);
				RecordCommit c;
				if (f.read(&c, sizeof(c)) != sizeof(c)) break;
				if (c.magic != MAGIC_COMMIT) break;
				if (!(hdr.flags & FLAG_DELETE))
					index_insert(key_buf, hdr.key_len, 0, scan_offset, hdr.timestamp, hdr.ttl);
				scan_offset += sizeof(hdr) + hdr.key_len + hdr.length + sizeof(c);
			}
			f.close();
		}

		// Rebuild persistent index from the now-correct in-memory index.
		// Use write_index_bulk() — single flush for all entries — not persist_index_entry()
		// which flushes after every entry and would cause N × fsync delays on flash.
		char iname[USTORE_MAX_FILENAME_LEN]; index_name(iname);
		if (index_file) index_file.close();
		_filesystem.remove(iname);
		write_index_bulk();
		open_index_for_append();

		current_segment = 0;
		// current_offset is set by open_segment() which the caller will invoke next.
	}

	/* -------- INDEX REBUILD FROM LOG -------- */

	// Called when the index file is missing. Scans all segment files in write
	// order to reconstruct the in-memory index, then persists it so future
	// boots are fast.  Uses the same record-walk logic as finalize_compaction().
	void rebuild_index_from_segments()
	{
		USTORE_LOG("[ustore] Index missing — rebuilding from segment files...\n");
		_index.clear();

		for (uint32_t seg = 0; seg < _segment_count; seg++)
		{
			char sname[USTORE_MAX_FILENAME_LEN];
			segment_name(seg, sname);
			File f = _filesystem.open(sname, File::ModeRead);
			if (!f) continue;

			uint32_t scan_offset = 0;
			uint8_t  key_buf[USTORE_MAX_KEY_LEN];
			while (true)
			{
				RecordHeader hdr;
				if (f.read(&hdr, sizeof(hdr)) != sizeof(hdr)) break;
				if (hdr.magic != MAGIC_RECORD || hdr.key_len == 0 ||
					hdr.key_len > USTORE_MAX_KEY_LEN || hdr.length > USTORE_MAX_VALUE_LEN) break;
				if (f.read(key_buf, hdr.key_len) != hdr.key_len) break;
				f.seek((long)(scan_offset + sizeof(hdr) + hdr.key_len + hdr.length), SeekModeSet);
				RecordCommit c;
				if (f.read(&c, sizeof(c)) != sizeof(c)) break;
				if (c.magic != MAGIC_COMMIT) break;
				if (hdr.flags & FLAG_DELETE)
					index_remove(key_buf, hdr.key_len);
				else
					index_insert(key_buf, hdr.key_len, seg, scan_offset, hdr.timestamp, hdr.ttl);
				scan_offset += sizeof(hdr) + hdr.key_len + hdr.length + sizeof(c);
			}
			f.close();
		}

		// Persist the rebuilt index so the next boot uses the fast path.
		char iname[USTORE_MAX_FILENAME_LEN]; index_name(iname);
		if (index_file) index_file.close();
		_filesystem.remove(iname);
		write_index_bulk();
	}

	/* -------- COMPACTION -------- */

public:
	bool compact()
	{
USTORE_LOG("[ustore] Compacting storage...\n");

		// --- Phase 1: write COMPACTING journal (next_seg=0: no source segments deleted yet) ---
		write_journal(JOURNAL_COMPACTING, 0, 0);

		char tmp_path[USTORE_MAX_FILENAME_LEN]; tmp_name(tmp_path);
USTORE_LOG("[ustore] Opening tmp file: %s\n", tmp_path);
		File outf = _filesystem.open(tmp_path, File::ModeWrite);
		if (!outf) { clear_journal(); return false; }

		// --- Phase 2 + 3 fused: stream live offsets one segment at a time ---
		// For each segment s:
		//   1. Walk _index to collect live offsets in segment s into a single
		//      reusable vector (clear() retains capacity across iterations).
		//   2. Sort by offset for sequential disk access.
		//   3. Copy live records from s to compact.tmp.
		//   4. Flush compact.tmp to disk.
		//   5. Write journal (next_seg=s+1, tmp_valid_size=current byte count).
		//   6. Delete source segment s.
		// Steps 5 and 6 MUST be in this order: the journal is updated BEFORE the source
		// segment is deleted. If power fails between step 5 and step 6, the next boot's
		// recover_if_needed() sees next_seg=s+1 and renames compact.tmp to segment 0, then
		// finalize_compaction() deletes the still-present segment s — no data is lost.
		//
		// _index must not be inserted into or erased from between here and Phase 4
		// (prune_index_to_max_recs_ runs before, finalize_compaction runs after) so
		// the per-segment re-iteration is stable.

		// Apply policies before enumerating live records so that expired and excess
		// records are excluded from the compaction output.
		prune_index_to_max_recs();

		using OffVec = std::vector<uint32_t, rebind_alloc<uint32_t>>;
		rebind_alloc<uint32_t> off_alloc(_alloc);
		OffVec offsets(off_alloc);

		// Static to avoid placing 1 KB on the stack — compact() is not re-entrant.
		uint8_t key_buf[USTORE_MAX_KEY_LEN];
		static uint8_t val_buf[USTORE_MAX_VALUE_LEN];
		bool write_ok = true;
		uint32_t committed_segs = 0;  // number of source segments committed to compact.tmp

		for (uint32_t s = 0; s < _segment_count; s++) {
			offsets.clear();
			for (auto& kv : _index) {
				if (kv.second.segment != s) continue;
				if (is_ttl_expired(kv.second.timestamp, kv.second.ttl)) continue;
				offsets.push_back(kv.second.offset);
			}
			std::sort(offsets.begin(), offsets.end());

USTORE_LOG("[ustore] Processing segment: %u, size: %lu\n", s, (unsigned long)offsets.size());
			char src_name[USTORE_MAX_FILENAME_LEN]; segment_name(s, src_name);
			if (!offsets.empty()) {
USTORE_LOG("[ustore] Opening src file: %s\n", src_name);
				File src = _filesystem.open(src_name, File::ModeRead);
				if (src) {
					for (size_t i = 0; i < offsets.size(); i++) {
						uint32_t off = offsets[i];
//USTORE_LOG("[ustore] Processing record: %u offset: %lu\n", (unsigned)i, (unsigned long)off);
						src.seek((long)off, SeekModeSet);
						RecordHeader hdr;
						if (src.read(&hdr, sizeof(hdr)) != sizeof(hdr)) {
							USTORE_LOG("[ustore] WARNING: Failed to read record header\n");
							continue;
						}
						if (hdr.magic != MAGIC_RECORD || hdr.key_len > USTORE_MAX_KEY_LEN || hdr.length > USTORE_MAX_VALUE_LEN) {
							USTORE_LOG("[ustore] WARNING: Record magic number incorrect\n");
							continue;
						}
						if (src.read(key_buf, hdr.key_len) != hdr.key_len) {
							USTORE_LOG("[ustore] WARNING: Failed to read record key\n");
							continue;
						}
						if (hdr.length > 0 && src.read(val_buf, hdr.length) != hdr.length) {
							USTORE_LOG("[ustore] WARNING: Failed to read record value\n");
							continue;
						}
						RecordCommit c; c.magic = MAGIC_COMMIT;
						uint32_t expected = sizeof(hdr) + hdr.key_len + hdr.length + sizeof(c);
						size_t written = 0;
						written += outf.write(&hdr,    sizeof(hdr));
						written += outf.write(key_buf, hdr.key_len);
						if (hdr.length > 0) written += outf.write(val_buf, hdr.length);
						written += outf.write(&c, sizeof(c));
						if (written != expected) { write_ok = false; break; }
					}
USTORE_LOG("[ustore] Closing src file: %s\n", src_name);
					src.close();
				}
				else {
					USTORE_LOG("[ustore] ERROR: Failed to open src file: %s\n", src_name);
				}
			}

			if (!write_ok) break;

			// Flush compact.tmp, journal BEFORE deleting the source segment (see comment above).
			outf.flush();
			uint32_t valid_size = (uint32_t)outf.tell();
			write_journal(JOURNAL_COMPACTING, s + 1, valid_size);
			_filesystem.remove(src_name);  // no-op if segment had no records / did not exist
			committed_segs++;
		}

		outf.flush();
USTORE_LOG("[ustore] Closing tmp file: %s\n", tmp_path);
		outf.close();

		if (!write_ok) {
			if (committed_segs == 0) {
				// No source segments were deleted — safe to discard compact.tmp entirely.
				USTORE_LOG("[ustore] WARNING: Compact aborted: storage full, all segments preserved\n");
				_filesystem.remove(tmp_path);
				clear_journal();
			} else {
				// Some source segments were already deleted; compact.tmp holds their records.
				// Leave compact.tmp and the journal in place. recover_if_needed() on the
				// next boot will rename compact.tmp to segment 0 and recover cleanly.
				USTORE_LOG("[ustore] WARNING: Compact aborted mid-way after %u segments: recovery on next boot\n",
				       committed_segs);
			}
			return false;
		}

		// --- Phase 4: commit journal → safe to finalize ---
		write_journal(JOURNAL_COMMIT);

		finalize_compaction();   // rename + index rebuild

		clear_journal();

		_dead_since_compact = 0;

		return true;
	}

private:

	FileSystem _filesystem;

	char base_prefix[USTORE_MAX_FILENAME_LEN];
	uint32_t _segment_size = USTORE_DEFAULT_SEGMENT_SIZE;
	uint8_t _segment_count = USTORE_DEFAULT_SEGMENT_COUNT;

	File active_file;
	File index_file;

	uint32_t current_segment = 0;
	uint32_t current_offset = 0;

	bool compact_in_cooldown = false;
	uint32_t compact_cooldown_start_ms = 0;
	uint32_t _dead_since_compact = 0;

	uint32_t policy_ttl_secs = USTORE_DEFAULT_TTL_SECS; // 0 = TTL disabled (seconds)
	uint32_t policy_max_recs = USTORE_DEFAULT_MAX_RECS; // 0 = max-records disabled

	uint8_t write_buf[USTORE_WRITE_BUFFER_SIZE];
	size_t write_buf_pos;

	Allocator _alloc;
	IndexMap _index;
};


using FileStore = BasicFileStore<>;

}

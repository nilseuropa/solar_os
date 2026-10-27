/* Private disk history implementation. Callers own the history mutex and
 * state mutex. Transfers release the state mutex while retaining history
 * ownership. Visitors and Inbox observers run outside both locks. */
static void messaging_restore_record_locked(const messaging_store_record_t *record,
                                             int32_t disk_index);
static bool messaging_message_pending_locked(solar_os_message_key_t key);

static messaging_history_navigation_t *messaging_history_navigation_locked(
    solar_os_conversation_id_t conversation, bool create)
{
    messaging_history_navigation_t *empty = NULL;
    for (size_t i = 0; i < SOLAR_OS_MESSAGING_CONVERSATION_CAPACITY; i++) {
        messaging_history_navigation_t *n = &messaging.navigation[i];
        if (n->conversation == conversation) return n;
        if (!n->conversation) empty = n;
    }
    if (create && empty) { empty->conversation = conversation; return empty; }
    return NULL;
}

static void messaging_history_note_locked(const solar_os_messaging_message_t *message, int32_t index)
{
    messaging_history_navigation_t *n = messaging_history_navigation_locked(message->conversation_id, true);
    if (!n) return;
    n->count++;
    if (message->provider_message_key > n->provider_cursor) n->provider_cursor = message->provider_message_key;
    n->recent[n->recent_head] = index;
    n->recent_head = (n->recent_head + 1) % MESSAGING_HISTORY_PAGE_CAPACITY;
    if (n->recent_count < MESSAGING_HISTORY_PAGE_CAPACITY) n->recent_count++;
    if (message->unread) {
        if (n->unread_count < MESSAGING_HISTORY_PAGE_CAPACITY) n->unread[n->unread_count++] = index;
        else n->unread_overflow = true;
    }
}

static messaging_message_slot_t *messaging_history_cached_index_locked(int32_t index)
{
    size_t first = messaging_message_oldest_locked();
    for (size_t i = 0; i < messaging.message_count; i++) {
        messaging_message_slot_t *slot = &messaging.messages[(first + i) % SOLAR_OS_MESSAGING_MESSAGE_CAPACITY];
        if (slot->disk_index == index) return slot;
    }
    return NULL;
}

static void messaging_history_cache_record_locked(const messaging_store_record_t *record, int32_t index)
{
    messaging_message_slot_t *slot = &messaging.messages[messaging.message_head];
    slot->message = record->message;
    slot->disk_index = index;
    slot->revision = messaging.generation;
    messaging.message_head = (messaging.message_head + 1) % SOLAR_OS_MESSAGING_MESSAGE_CAPACITY;
    if (messaging.message_count < SOLAR_OS_MESSAGING_MESSAGE_CAPACITY) messaging.message_count++;
}

static FILE *messaging_history_fopen(const char *path, const char *mode)
{
    messaging_unlock();
    FILE *file = fopen(path, mode);
    messaging_lock();
    return file;
}

static int messaging_history_fclose(FILE *file)
{
    messaging_unlock();
    int result = fclose(file);
    messaging_lock();
    return result;
}

static int messaging_history_stat(const char *path, struct stat *info)
{
    messaging_unlock();
    int result = stat(path, info);
    messaging_lock();
    return result;
}

static int messaging_history_rename(const char *from, const char *to)
{
    messaging_unlock();
    int result = rename(from, to);
    messaging_lock();
    return result;
}

static bool messaging_generation_newer(uint32_t first, uint32_t second)
{
    return (int32_t)(first - second) > 0;
}

static bool messaging_header_valid(const messaging_store_header_t *header)
{
    return header->magic == MESSAGING_STORE_MAGIC &&
        header->version == MESSAGING_STORE_VERSION && header->capacity > 0 &&
        header->capacity <= SOLAR_OS_MESSAGING_MESSAGE_CAPACITY &&
        header->head < header->capacity && header->count <= header->capacity &&
        header->next_message_key != 0 && header->crc32 ==
            messaging_crc32(header, offsetof(messaging_store_header_t, crc32));
}

static void messaging_make_record_locked(messaging_store_record_t *record,
                                         const messaging_message_slot_t *slot)
{
    memset(record, 0, sizeof(*record));
    record->message = slot->message;
    int index = messaging_conversation_id_index_locked(slot->message.conversation_id);
    if (index >= 0) {
        const solar_os_messaging_conversation_t *c = &messaging.conversations[index];
        strlcpy(record->conversation_key, c->provider_key, sizeof(record->conversation_key));
        strlcpy(record->conversation_title, c->title, sizeof(record->conversation_title));
        record->conversation_kind = c->kind;
        record->group_ref = c->group_ref;
        record->conversation_security = c->security_flags;
    }
    record->crc32 = messaging_crc32(record, offsetof(messaging_store_record_t, crc32));
}

static bool messaging_record_valid(const messaging_store_record_t *record)
{
    if (record->crc32 != messaging_crc32(record, offsetof(messaging_store_record_t, crc32))) return false;
    if (record->message.key == 0) return true;
    return messaging_provider_valid(record->message.provider) &&
        record->conversation_key[0] &&
        memchr(record->conversation_key, 0, sizeof(record->conversation_key)) &&
        memchr(record->conversation_title, 0, sizeof(record->conversation_title)) &&
        memchr(record->message.sender, 0, sizeof(record->message.sender)) &&
        memchr(record->message.body, 0, sizeof(record->message.body)) &&
        memchr(record->message.error, 0, sizeof(record->message.error));
}

static bool messaging_history_copy_valid(const messaging_history_copy_t *copy)
{
    return copy->magic == MESSAGING_HISTORY_MAGIC && copy->version == MESSAGING_HISTORY_VERSION &&
        copy->generation && copy->next_message_key && copy->previous_index >= -1 &&
        (!copy->record.message.key || copy->record.message.key == copy->lookup_key) && copy->crc32 ==
        messaging_crc32(copy, offsetof(messaging_history_copy_t, crc32)) &&
        messaging_record_valid(&copy->record);
}

/* Return the selected copy; short/unwritten alternate copies are harmless. */
static int messaging_history_read_locked(FILE *file, size_t index,
                                         messaging_store_record_t *record)
{
    if (index > (size_t)LONG_MAX / MESSAGING_HISTORY_RECORD_BYTES) return -1;
    memset(messaging.history_scratch, 0, 2 * sizeof(*messaging.history_scratch));
    messaging_unlock();
    if (fseek(file, (long)(index * MESSAGING_HISTORY_RECORD_BYTES), SEEK_SET)) { messaging_lock(); return -1; }
    size_t bytes = fread(messaging.history_scratch, 1, MESSAGING_HISTORY_RECORD_BYTES, file);
    bool failed = ferror(file) || bytes < sizeof(*messaging.history_scratch);
    if (index % 64U == 0) vTaskDelay(1);
    messaging_lock();
    if (failed) return -1;
    bool a = messaging_history_copy_valid(&messaging.history_scratch[0]);
    bool b = messaging_history_copy_valid(&messaging.history_scratch[1]);
    if (!a && !b) return -1;
    int selected = !a || (b && messaging_generation_newer(
        messaging.history_scratch[1].generation, messaging.history_scratch[0].generation));
    if (record) *record = messaging.history_scratch[selected].record;
    return selected;
}

static esp_err_t messaging_history_write_locked(FILE *file, size_t index,
                                                 messaging_store_record_t *record,
                                                 bool append)
{
    if (index >= (size_t)LONG_MAX / MESSAGING_HISTORY_RECORD_BYTES) return ESP_ERR_NO_MEM;
    int previous = append ? -1 : messaging_history_read_locked(file, index, NULL);
    if (!append && previous < 0) return ESP_FAIL;
    uint32_t generation = append ? 1 : messaging.history_scratch[previous].generation + 1;
    uint64_t lookup_key = append ? record->message.key : messaging.history_scratch[previous].lookup_key;
    int32_t previous_index = append ? (lookup_key ? messaging.history_buckets[lookup_key % MESSAGING_HISTORY_BUCKETS] : -1) :
        messaging.history_scratch[previous].previous_index;
    if (!generation) generation = 1;
    int target = previous == 0 ? 1 : 0;
    messaging_history_copy_t *copy = &messaging.history_scratch[target];
    memset(copy, 0, sizeof(*copy));
    copy->magic = MESSAGING_HISTORY_MAGIC;
    copy->version = MESSAGING_HISTORY_VERSION;
    copy->generation = generation;
    copy->next_message_key = messaging.next_message_key;
    copy->lookup_key = lookup_key;
    copy->previous_index = previous_index;
    copy->record = *record;
    copy->record.crc32 = messaging_crc32(&copy->record, offsetof(messaging_store_record_t, crc32));
    copy->crc32 = messaging_crc32(copy, offsetof(messaging_history_copy_t, crc32));
    long offset = (long)(index * MESSAGING_HISTORY_RECORD_BYTES + target * sizeof(*copy));
    messaging_unlock();
    if (fseek(file, offset, SEEK_SET) || fwrite(copy, sizeof(*copy), 1, file) != 1 ||
        messaging_sync_file(file) != ESP_OK) { messaging_lock(); return ESP_FAIL; }
    if (append) {
        /* Reserve the second copy without ever overwriting an earlier record. */
        memset(copy, 0, sizeof(*copy));
        if (fwrite(copy, sizeof(*copy), 1, file) != 1 || messaging_sync_file(file) != ESP_OK) { messaging_lock(); return ESP_FAIL; }
    }
    messaging_lock();
    if (append && lookup_key) messaging.history_buckets[lookup_key % MESSAGING_HISTORY_BUCKETS] = (int32_t)index;
    return ESP_OK;
}

static void messaging_history_normalize_locked(messaging_store_record_t *record)
{
    int index = messaging_conversation_index_locked(record->message.provider, record->conversation_key);
    record->message.conversation_id = index >= 0 ? messaging.conversations[index].id : 0;
    if (record->message.direction == SOLAR_OS_MESSAGE_OUTBOUND &&
        (record->message.delivery == SOLAR_OS_DELIVERY_QUEUED || record->message.delivery == SOLAR_OS_DELIVERY_SENDING) &&
        !messaging_message_pending_locked(record->message.key)) {
        record->message.delivery = SOLAR_OS_DELIVERY_FAILED;
        strlcpy(record->message.error, "Send interrupted by restart; resend explicitly", sizeof(record->message.error));
    }
}

static esp_err_t messaging_history_find_locked(solar_os_message_key_t key,
                                               messaging_store_record_t *record,
                                               int32_t *disk_index)
{
    if (!messaging.persistent) return ESP_ERR_NOT_FOUND;
    FILE *file = messaging_history_fopen(messaging.store_path, "rb");
    if (!file) return messaging.storage_error = ESP_FAIL;
    esp_err_t error = ESP_ERR_NOT_FOUND;
    int32_t index = messaging.history_buckets[key % MESSAGING_HISTORY_BUCKETS];
    while (index >= 0) {
        int selected = messaging_history_read_locked(file, (size_t)index, record);
        if (selected < 0) { error = ESP_FAIL; break; }
        if (record->message.key == key) {
            *disk_index = index;
            messaging_history_normalize_locked(record);
            error = ESP_OK;
            break;
        }
        int32_t previous_index = messaging.history_scratch[selected].previous_index;
        if (previous_index >= index) { error = ESP_FAIL; break; }
        index = previous_index;
    }
    messaging_history_fclose(file);
    messaging.storage_error = error == ESP_ERR_NOT_FOUND ? ESP_OK : error;
    return error;
}

static messaging_message_slot_t *messaging_history_cache_locked(solar_os_message_key_t key)
{
    messaging_message_slot_t *slot = messaging_find_message_locked(key);
    if (slot) return slot;
    int32_t index;
    if (messaging_history_find_locked(key, messaging.record_scratch, &index) != ESP_OK) return NULL;
    slot = &messaging.messages[messaging.message_head];
    slot->message = messaging.record_scratch->message;
    slot->disk_index = index;
    slot->revision = messaging.generation;
    messaging.message_head = (messaging.message_head + 1) % SOLAR_OS_MESSAGING_MESSAGE_CAPACITY;
    if (messaging.message_count < SOLAR_OS_MESSAGING_MESSAGE_CAPACITY) messaging.message_count++;
    return slot;
}

static esp_err_t messaging_history_commit_locked(messaging_message_slot_t *slot)
{
    if (!messaging.persistent) return messaging.storage_error != ESP_OK ? messaging.storage_error : ESP_ERR_INVALID_STATE;
    messaging_make_record_locked(messaging.record_scratch, slot);
    FILE *file = messaging_history_fopen(messaging.store_path, "r+b");
    if (!file) return messaging.storage_error = ESP_FAIL;
    bool append = slot->disk_index < 0;
    size_t index = append ? messaging.disk_head : (size_t)slot->disk_index;
    esp_err_t error = messaging_history_write_locked(file, index, messaging.record_scratch, append);
    if (messaging_history_fclose(file) && error == ESP_OK) error = ESP_FAIL;
    if (error == ESP_OK && append) {
        slot->disk_index = (int32_t)index;
        messaging.disk_head++;
        messaging.disk_count++;
        messaging_history_note_locked(&slot->message, (int32_t)index);
    }
    return messaging.storage_error = error;
}

static esp_err_t messaging_store_update(solar_os_message_key_t key)
{
    messaging_history_lock();
    messaging_message_slot_t *slot = messaging_history_cache_locked(key);
    esp_err_t error = slot ? messaging_history_commit_locked(slot) : ESP_ERR_NOT_FOUND;
    messaging_history_unlock();
    return error;
}

static esp_err_t messaging_history_rebuild_locked(void)
{
    FILE *file = messaging_history_fopen(messaging.store_path, "rb");
    if (!file) return messaging.storage_error = ESP_FAIL;
    struct stat info;
    messaging_unlock();
    int result = fstat(fileno(file), &info);
    messaging_lock();
    if (result || info.st_size < 0 || info.st_size > LONG_MAX) { messaging_history_fclose(file); return ESP_FAIL; }
    size_t count = ((size_t)info.st_size + MESSAGING_HISTORY_RECORD_BYTES - 1) / MESSAGING_HISTORY_RECORD_BYTES;
    memset(messaging.history_buckets, 0xff, MESSAGING_HISTORY_BUCKETS * sizeof(*messaging.history_buckets));
    memset(messaging.messages, 0, SOLAR_OS_MESSAGING_MESSAGE_CAPACITY * sizeof(*messaging.messages));
    memset(messaging.navigation, 0, SOLAR_OS_MESSAGING_CONVERSATION_CAPACITY * sizeof(*messaging.navigation));
    messaging.message_head = messaging.message_count = messaging.message_unread = messaging.disk_count = 0;
    for (size_t i = 0; i < messaging.conversation_count; i++) {
        messaging.conversations[i].unread_count = 0;
        messaging.conversations[i].last_message_ms = 0;
    }
    esp_err_t error = ESP_OK;
    for (size_t i = 0; i < count; i++) {
        int selected = messaging_history_read_locked(file, i, messaging.record_scratch);
        if (selected < 0) { error = ESP_FAIL; break; }
        uint64_t lookup_key = messaging.history_scratch[selected].lookup_key;
        if (lookup_key) {
            size_t bucket = lookup_key % MESSAGING_HISTORY_BUCKETS;
            if (messaging.history_scratch[selected].previous_index != messaging.history_buckets[bucket]) { error = ESP_FAIL; break; }
            messaging.history_buckets[bucket] = (int32_t)i;
        }
        uint64_t next = messaging.history_scratch[selected].next_message_key;
        if (next > messaging.next_message_key) messaging.next_message_key = next;
        if (!messaging.record_scratch->message.key) continue;
        messaging.disk_count++;
        messaging_restore_record_locked(messaging.record_scratch, (int32_t)i);
    }
    messaging_history_fclose(file);
    messaging.disk_head = count;
    return messaging.storage_error = error;
}

static esp_err_t messaging_history_tombstone_locked(int32_t index)
{
    FILE *file = messaging_history_fopen(messaging.store_path, "r+b");
    if (!file) return messaging.storage_error = ESP_FAIL;
    memset(messaging.record_scratch, 0, sizeof(*messaging.record_scratch));
    esp_err_t error = messaging_history_write_locked(file, (size_t)index, messaging.record_scratch, false);
    if (messaging_history_fclose(file) && error == ESP_OK) error = ESP_FAIL;
    if (error == ESP_OK) messaging.disk_count--;
    return messaging.storage_error = error;
}

static esp_err_t messaging_history_compact_locked(void)
{
    char staged[SOLAR_OS_STORAGE_PATH_MAX], backup[SOLAR_OS_STORAGE_PATH_MAX];
    int n = snprintf(staged, sizeof(staged), "%s.compact", messaging.store_path);
    int m = snprintf(backup, sizeof(backup), "%s.bak", messaging.store_path);
    if (n < 0 || m < 0 || (size_t)n >= sizeof(staged) || (size_t)m >= sizeof(backup)) return ESP_ERR_INVALID_SIZE;
    FILE *source = messaging_history_fopen(messaging.store_path, "rb");
    if (!source) return ESP_FAIL;
    FILE *target = messaging_history_fopen(staged, "w+b");
    if (!target) { messaging_history_fclose(source); return ESP_FAIL; }
    memset(messaging.history_buckets, 0xff, MESSAGING_HISTORY_BUCKETS * sizeof(*messaging.history_buckets));
    /* A metadata-only record keeps outbound IDs monotonic even after clear all. */
    memset(messaging.record_scratch, 0, sizeof(*messaging.record_scratch));
    esp_err_t error = messaging_history_write_locked(target, 0, messaging.record_scratch, true);
    size_t count = 1;
    for (size_t i = 0; error == ESP_OK && i < messaging.disk_head; i++) {
        if (messaging_history_read_locked(source, i, messaging.record_scratch) < 0) { error = ESP_FAIL; break; }
        if (!messaging.record_scratch->message.key) continue;
        error = messaging_history_write_locked(target, count++, messaging.record_scratch, true);
    }
    if (messaging_history_fclose(source) && error == ESP_OK) error = ESP_FAIL;
    if (messaging_history_fclose(target) && error == ESP_OK) error = ESP_FAIL;
    messaging_unlock();
    if (error == ESP_OK) error = solar_os_storage_replace_file(staged, messaging.store_path, backup);
    if (error != ESP_OK) (void)unlink(staged);
    messaging_lock();
    return error;
}

static esp_err_t messaging_history_clear_locked(uint32_t provider,
                                                uint32_t conversation, size_t *removed)
{
    *removed = 0;
    if (!messaging.persistent) return ESP_ERR_INVALID_STATE;
    bool any = false;
    size_t indexed = 0;
    for (size_t i = 0; i < messaging.conversation_count; i++) {
        const solar_os_messaging_conversation_t *c = &messaging.conversations[i];
        messaging_history_navigation_t *n = messaging_history_navigation_locked(c->id, false);
        if (n) indexed += n->count;
        if (n && n->count && (!provider || c->provider == provider) && (!conversation || c->id == conversation)) any = true;
    }
    if (!any && indexed == messaging.disk_count) return ESP_OK;
    for (size_t i = 0; i < messaging.outbox_count; i++) {
        size_t first = (messaging.outbox_head + SOLAR_OS_MESSAGING_OUTBOX_CAPACITY -
            messaging.outbox_count) % SOLAR_OS_MESSAGING_OUTBOX_CAPACITY;
        const solar_os_messaging_outbound_t *o = &messaging.outbox[(first + i) % SOLAR_OS_MESSAGING_OUTBOX_CAPACITY];
        if ((!provider || o->provider == provider) && (!conversation || o->conversation_id == conversation))
            return ESP_ERR_INVALID_STATE;
    }
    FILE *file = messaging_history_fopen(messaging.store_path, "r+b");
    if (!file) return messaging.storage_error = ESP_FAIL;
    esp_err_t error = ESP_OK;
    for (size_t i = 0; i < messaging.disk_head; i++) {
        if (messaging_history_read_locked(file, i, messaging.record_scratch) < 0) { error = ESP_FAIL; break; }
        messaging_store_record_t *r = messaging.record_scratch;
        if (!r->message.key || (provider && r->message.provider != provider)) continue;
        messaging_history_normalize_locked(r);
        if (conversation && r->message.conversation_id != conversation) continue;
        memset(r, 0, sizeof(*r));
        error = messaging_history_write_locked(file, i, r, false);
        if (error != ESP_OK) break;
        (*removed)++;
    }
    if (messaging_history_fclose(file) && error == ESP_OK) error = ESP_FAIL;
    if (error == ESP_OK && *removed) error = messaging_history_compact_locked();
    esp_err_t rebuild = *removed ? messaging_history_rebuild_locked() : ESP_OK;
    return messaging.storage_error = error != ESP_OK ? error : rebuild;
}

static esp_err_t messaging_history_inbox_links_locked(uint32_t conversation,
    uint32_t *ids, solar_os_message_key_t *keys, size_t *count)
{
    *count = 0;
    FILE *file = messaging_history_fopen(messaging.store_path, "rb");
    if (!file) return ESP_FAIL;
    esp_err_t error = ESP_OK;
    for (size_t i = messaging.disk_head; i > 0 && *count < SOLAR_OS_INBOX_CAPACITY; i--) {
        if (messaging_history_read_locked(file, i - 1, messaging.record_scratch) < 0) { error = ESP_FAIL; break; }
        messaging_store_record_t *r = messaging.record_scratch;
        if (!r->message.key || !r->message.inbox_id) continue;
        messaging_history_normalize_locked(r);
        if (r->message.conversation_id != conversation) continue;
        ids[*count] = r->message.inbox_id;
        keys[(*count)++] = r->message.key;
    }
    messaging_history_fclose(file);
    return error;
}

static esp_err_t messaging_history_select_path_unlocked(void)
{
    /* Pin the selected volume in internal storage. Mounting an SD card later
     * must never switch an existing archive to a different, empty history. */
    char directory[SOLAR_OS_STORAGE_PATH_MAX];
    if (solar_os_storage_flash_is_mounted()) {
        esp_err_t error = solar_os_storage_join_path(solar_os_storage_flash_mount_point(),
            SOLAR_OS_MESSAGING_STORE_DIR, directory, sizeof(directory));
        if (error != ESP_OK) return error;
        if (solar_os_storage_mkdir(directory) != ESP_OK && errno != EEXIST) return ESP_FAIL;
        error = solar_os_storage_join_path(directory, "location", messaging.location_path, sizeof(messaging.location_path));
        if (error != ESP_OK) return error;
        struct stat info;
        if (stat(messaging.location_path, &info) && errno == ENOENT) {
            char backup[SOLAR_OS_STORAGE_PATH_MAX];
            int n = snprintf(backup, sizeof(backup), "%s.bak", messaging.location_path);
            if (n < 0 || (size_t)n >= sizeof(backup)) return ESP_ERR_INVALID_SIZE;
            if (!stat(backup, &info) && rename(backup, messaging.location_path)) return ESP_FAIL;
        }
        FILE *location = fopen(messaging.location_path, "rb");
        if (location) {
            bool ok = fgets(messaging.store_path, sizeof(messaging.store_path), location) != NULL &&
                messaging.store_path[0] == '/' && !ferror(location);
            fclose(location);
            return ok ? ESP_OK : ESP_FAIL;
        }
        if (errno != ENOENT) return ESP_FAIL;
    }
    esp_err_t error = solar_os_storage_default_path(SOLAR_OS_MESSAGING_STORE_DIR, directory, sizeof(directory));
    if (error != ESP_OK) return error;
    if (solar_os_storage_mkdir(directory) != ESP_OK && errno != EEXIST) return ESP_FAIL;
    return solar_os_storage_join_path(directory, SOLAR_OS_MESSAGING_STORE_FILE, messaging.store_path, sizeof(messaging.store_path));
}

static esp_err_t messaging_history_select_path(void)
{
    messaging_unlock();
    esp_err_t error = messaging_history_select_path_unlocked();
    messaging_lock();
    return error;
}

/* Migration writes a complete staged archive before publishing it. Original
 * ring files are retained. A reset during migration retries from those files. */
static esp_err_t messaging_history_import_ring(FILE *target, const char *path, size_t *count)
{
    FILE *old = messaging_history_fopen(path, "rb");
    if (!old) return errno == ENOENT ? ESP_OK : ESP_FAIL;
    messaging_store_header_t headers[2];
    messaging_unlock();
    bool read_ok = fread(headers, sizeof(headers), 1, old) == 1;
    messaging_lock();
    bool a = read_ok && messaging_header_valid(&headers[0]);
    bool b = read_ok && messaging_header_valid(&headers[1]);
    if (!a && !b) { messaging_history_fclose(old); return ESP_FAIL; }
    messaging_store_header_t *header = &headers[!a || (b && messaging_generation_newer(headers[1].generation, headers[0].generation))];
    if (header->next_message_key > messaging.next_message_key) messaging.next_message_key = header->next_message_key;
    size_t oldest = (header->head + header->capacity - header->count) % header->capacity;
    esp_err_t error = ESP_OK;
    for (size_t i = 0; i < header->count; i++) {
        long offset = (long)(MESSAGING_STORE_RECORDS_OFFSET + ((oldest + i) % header->capacity) * sizeof(*messaging.record_scratch));
        messaging_unlock();
        bool failed = fseek(old, offset, SEEK_SET) || fread(messaging.record_scratch, sizeof(*messaging.record_scratch), 1, old) != 1;
        messaging_lock();
        if (failed || !messaging_record_valid(messaging.record_scratch)) { error = ESP_FAIL; break; }
        if (!messaging.record_scratch->message.key) continue;
        error = messaging_history_write_locked(target, *count, messaging.record_scratch, true);
        if (error != ESP_OK) break;
        (*count)++;
    }
    messaging_history_fclose(old);
    return error;
}

static esp_err_t messaging_history_import_inbox(FILE *target, const char *path, size_t *count)
{
    solar_os_inbox_entry_t *entries = solar_os_memory_calloc(SOLAR_OS_INBOX_CAPACITY, sizeof(*entries),
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "messages.migration");
    if (!entries) return ESP_ERR_NO_MEM;
    size_t n = 0;
    messaging_unlock();
    esp_err_t error = solar_os_inbox_saved_snapshot(path, entries, SOLAR_OS_INBOX_CAPACITY, &n);
    messaging_lock();
    for (size_t i = 0; error == ESP_OK && i < n; i++) {
        const solar_os_inbox_entry_t *e = &entries[i];
        uint32_t provider = 0;
        if (!strcmp(e->source, "messages") || !strcmp(e->source, "chat")) provider = SOLAR_OS_MESSAGING_PROVIDER_GATEWAY;
        else if (!strcmp(e->source, "meshcore")) provider = SOLAR_OS_MESSAGING_PROVIDER_MESHCORE;
        else if (!strcmp(e->source, "link-chat")) provider = SOLAR_OS_MESSAGING_PROVIDER_LINK;
        else if (!strncmp(e->source, "msg-", 4)) {
            char *end; unsigned long value = strtoul(e->source + 4, &end, 10);
            if (!*end && value <= SOLAR_OS_MESSAGING_INSTANCE_ID_MAX) provider = (uint32_t)value;
        }
        if (!messaging_provider_valid(provider) || !e->source_id) continue;
        bool duplicate = false;
        for (size_t j = 0; j < *count; j++) {
            if (messaging_history_read_locked(target, j, messaging.record_scratch) < 0) { error = ESP_FAIL; break; }
            if (messaging.record_scratch->message.key == e->source_id) { duplicate = true; break; }
        }
        if (error != ESP_OK || duplicate) continue;
        messaging_store_record_t *r = messaging.record_scratch;
        memset(r, 0, sizeof(*r));
        r->message.key = e->source_id; r->message.provider = provider;
        r->message.direction = SOLAR_OS_MESSAGE_INBOUND; r->message.delivery = SOLAR_OS_DELIVERY_RECEIVED;
        r->message.timestamp_ms = e->timestamp_ms; r->message.received_ms = e->received_ms;
        r->message.unread = e->unread; r->message.truncated = e->truncated;
        /* Old Inbox IDs may belong to another volume; do not link them. */
        strlcpy(r->message.sender, e->sender, sizeof(r->message.sender));
        strlcpy(r->message.body, e->body, sizeof(r->message.body));
        strlcpy(r->conversation_key, e->topic[0] ? e->topic : "general", sizeof(r->conversation_key));
        strlcpy(r->conversation_title, r->conversation_key, sizeof(r->conversation_title));
        r->conversation_kind = !strncmp(r->conversation_key, "group:", 6) || !strncmp(r->conversation_key, "ble-group:", 10) ?
            SOLAR_OS_CONVERSATION_GROUP : !strncmp(r->conversation_key, "b:", 2) ? SOLAR_OS_CONVERSATION_BROADCAST :
            provider == SOLAR_OS_MESSAGING_PROVIDER_GATEWAY || r->conversation_key[0] == '#' || r->conversation_key[0] == '&' ?
            SOLAR_OS_CONVERSATION_ROOM : SOLAR_OS_CONVERSATION_DIRECT;
        error = messaging_history_write_locked(target, *count, r, true);
        if (error == ESP_OK) (*count)++;
    }
    solar_os_memory_free(entries);
    return error;
}

static esp_err_t messaging_history_migrate(void)
{
    char temporary[SOLAR_OS_STORAGE_PATH_MAX], path[SOLAR_OS_STORAGE_PATH_MAX];
    int n = snprintf(temporary, sizeof(temporary), "%s.tmp", messaging.store_path);
    if (n < 0 || (size_t)n >= sizeof(temporary)) return ESP_ERR_INVALID_SIZE;
    FILE *file = messaging_history_fopen(temporary, "w+b");
    if (!file) return ESP_FAIL;
    size_t count = 0;
    esp_err_t error = solar_os_storage_default_path(SOLAR_OS_MESSAGING_STORE_DIR "/messages.bin", path, sizeof(path));
    if (error == ESP_OK) error = messaging_history_import_ring(file, path, &count);
    if (error == ESP_OK) {
        error = solar_os_storage_default_path(SOLAR_OS_INBOX_STORE_DIR "/messages.bin", path, sizeof(path));
        if (error == ESP_OK) error = messaging_history_import_inbox(file, path, &count);
    }
    /* Also recover the internal fallback Inbox when SD is already mounted. */
    if (error == ESP_OK && solar_os_storage_flash_is_mounted()) {
        error = solar_os_storage_join_path(solar_os_storage_flash_mount_point(),
            SOLAR_OS_INBOX_STORE_DIR "/messages.bin", path, sizeof(path));
        if (error == ESP_OK) error = messaging_history_import_inbox(file, path, &count);
    }
    messaging_unlock();
    if (error == ESP_OK) error = messaging_sync_file(file);
    messaging_lock();
    if (messaging_history_fclose(file) && error == ESP_OK) error = ESP_FAIL;
    if (error == ESP_OK && messaging_history_rename(temporary, messaging.store_path)) error = ESP_FAIL;
    return error;
}

static esp_err_t messaging_store_load(void)
{
    if (!solar_os_storage_is_mounted()) return messaging.storage_error = ESP_ERR_INVALID_STATE;
    esp_err_t error = messaging_history_select_path();
    struct stat info;
    if (error == ESP_OK && messaging_history_stat(messaging.store_path, &info) && errno == ENOENT) {
        char backup[SOLAR_OS_STORAGE_PATH_MAX];
        int n = snprintf(backup, sizeof(backup), "%s.bak", messaging.store_path);
        if (n < 0 || (size_t)n >= sizeof(backup)) error = ESP_ERR_INVALID_SIZE;
        else if (!messaging_history_stat(backup, &info) && messaging_history_rename(backup, messaging.store_path)) error = ESP_FAIL;
    }
    if (error == ESP_OK && messaging_history_stat(messaging.store_path, &info)) {
        /* A pinned archive must exist. Its missing volume is never replaced. */
        if (errno != ENOENT) error = ESP_FAIL;
        else if (messaging.location_path[0] && access(messaging.location_path, F_OK) == 0) error = ESP_FAIL;
        else error = messaging_history_migrate();
    }
    if (error == ESP_OK && messaging.location_path[0] && access(messaging.location_path, F_OK)) {
        char temporary[SOLAR_OS_STORAGE_PATH_MAX];
        int n = snprintf(temporary, sizeof(temporary), "%s.tmp", messaging.location_path);
        if (n < 0 || (size_t)n >= sizeof(temporary)) return messaging.storage_error = ESP_ERR_INVALID_SIZE;
        FILE *location = messaging_history_fopen(temporary, "wb");
        if (!location) error = ESP_FAIL;
        else {
            messaging_unlock();
            if (fputs(messaging.store_path, location) == EOF) error = ESP_FAIL;
            if (error == ESP_OK) error = messaging_sync_file(location);
            messaging_lock();
            if (messaging_history_fclose(location) && error == ESP_OK) error = ESP_FAIL;
            if (error == ESP_OK && messaging_history_rename(temporary, messaging.location_path)) error = ESP_FAIL;
        }
    }
    if (error == ESP_OK) error = messaging_history_rebuild_locked();
    messaging.persistent = error == ESP_OK;
    return messaging.storage_error = error;
}

static esp_err_t messaging_history_ensure_locked(void)
{
    if (messaging.persistent) return ESP_OK;
    esp_err_t error = messaging_store_load();
    /* A volume can become readable after initialization. Honor committed
     * removals before any reader exposes its recovered records. */
    for (messaging_provider_node_t *node = messaging.providers; error == ESP_OK && node; node = node->next) {
        if (!node->status.removed) continue;
        size_t removed;
        error = messaging_history_clear_locked(node->status.id, 0, &removed);
    }
    return messaging.storage_error = error;
}

static esp_err_t messaging_history_relocate_locked(const char *target)
{
    if (!messaging.location_path[0] || !strcmp(target, messaging.store_path)) return ESP_ERR_INVALID_STATE;
    char staged[SOLAR_OS_STORAGE_PATH_MAX], pin[SOLAR_OS_STORAGE_PATH_MAX], backup[SOLAR_OS_STORAGE_PATH_MAX];
    if (snprintf(staged, sizeof(staged), "%s.promote", target) >= (int)sizeof(staged) ||
        snprintf(pin, sizeof(pin), "%s.tmp", messaging.location_path) >= (int)sizeof(pin) ||
        snprintf(backup, sizeof(backup), "%s.bak", messaging.location_path) >= (int)sizeof(backup)) return ESP_ERR_INVALID_SIZE;
    struct stat info;
    if (messaging_history_stat(target, &info) == 0 || errno != ENOENT) return ESP_ERR_INVALID_STATE;
    unsigned char *buffer = solar_os_memory_calloc(1, 4096, SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "messages.move");
    if (!buffer) return ESP_ERR_NO_MEM;
    FILE *source = messaging_history_fopen(messaging.store_path, "rb");
    FILE *destination = source ? messaging_history_fopen(staged, "w+b") : NULL;
    esp_err_t error = source && destination ? ESP_OK : ESP_FAIL;
    if (error == ESP_OK) {
        messaging_unlock();
        size_t n;
        while ((n = fread(buffer, 1, 4096, source)) != 0) {
            if (fwrite(buffer, 1, n, destination) != n) { error = ESP_FAIL; break; }
            vTaskDelay(1);
        }
        if (ferror(source)) error = ESP_FAIL;
        if (error == ESP_OK) error = messaging_sync_file(destination);
        messaging_lock();
        for (size_t i = 0; error == ESP_OK && i < messaging.disk_head; i++) {
            if (messaging_history_read_locked(destination, i, NULL) < 0) error = ESP_FAIL;
        }
    }
    if (source && messaging_history_fclose(source) && error == ESP_OK) error = ESP_FAIL;
    if (destination && messaging_history_fclose(destination) && error == ESP_OK) error = ESP_FAIL;
    solar_os_memory_free(buffer);
    bool published = false;
    if (error == ESP_OK) {
        if (messaging_history_rename(staged, target)) error = ESP_FAIL;
        else published = true;
    }
    if (error == ESP_OK) {
        FILE *location = messaging_history_fopen(pin, "wb");
        if (!location) error = ESP_FAIL;
        else {
            messaging_unlock();
            if (fputs(target, location) == EOF || messaging_sync_file(location) != ESP_OK) error = ESP_FAIL;
            messaging_lock();
            if (messaging_history_fclose(location) && error == ESP_OK) error = ESP_FAIL;
        }
        if (error == ESP_OK) {
            messaging_unlock();
            error = solar_os_storage_replace_file(pin, messaging.location_path, backup);
            messaging_lock();
        }
    }
    if (error == ESP_OK) {
        char previous[SOLAR_OS_STORAGE_PATH_MAX];
        strlcpy(previous, messaging.store_path, sizeof(previous));
        strlcpy(messaging.store_path, target, sizeof(messaging.store_path));
        /* The synced, checked copy and committed location now own history. */
        messaging_unlock();
        (void)unlink(previous);
        messaging_lock();
        messaging.storage_error = ESP_OK;
    }
    /* Failed copies leave the active archive and its location intact. */
    messaging_unlock();
    (void)unlink(staged);
    if (error != ESP_OK && published) (void)unlink(target);
    if (error != ESP_OK) (void)unlink(pin);
    messaging_lock();
    return error;
}

static esp_err_t messaging_history_promote_sd_locked(void)
{
    if (messaging.sd_promotion_checked || !solar_os_storage_sd_is_mounted()) return ESP_OK;
    char directory[SOLAR_OS_STORAGE_PATH_MAX], target[SOLAR_OS_STORAGE_PATH_MAX];
    esp_err_t error = solar_os_storage_join_path(solar_os_storage_sd_mount_point(),
        SOLAR_OS_MESSAGING_STORE_DIR, directory, sizeof(directory));
    if (error != ESP_OK) return error;
    error = solar_os_storage_join_path(directory, SOLAR_OS_MESSAGING_STORE_FILE, target, sizeof(target));
    if (error != ESP_OK) return error;
    if (!strcmp(target, messaging.store_path)) { messaging.sd_promotion_checked = true; return ESP_OK; }
    messaging_unlock();
    error = solar_os_storage_mkdir(directory);
    bool exists = errno == EEXIST;
    messaging_lock();
    if (error != ESP_OK && !exists) return error;
    error = messaging_history_relocate_locked(target);
    if (error == ESP_OK || error == ESP_ERR_INVALID_STATE) messaging.sd_promotion_checked = true;
    if (error == ESP_OK && messaging.disk_head > messaging.disk_count + 1) {
        error = messaging_history_compact_locked();
        esp_err_t rebuilt = messaging_history_rebuild_locked();
        if (error == ESP_OK) error = rebuilt;
    }
    return error;
}

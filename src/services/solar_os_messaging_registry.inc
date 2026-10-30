/* Registry labels never move or disappear. Transport contexts can be destroyed
 * once leased callbacks and workers are excluded. Callbacks run without locks
 * owned by Messaging. */
#define MESSAGING_PROFILE_MAGIC 0x46525053U
#define MESSAGING_PROFILE_MAX 3072U
typedef struct {
    uint32_t magic, version, id, size, crc;
    char name[SOLAR_OS_MESSAGING_LABEL_MAX];
    char class_name[16];
} messaging_profile_header_t;

static const solar_os_messaging_provider_class_t *messaging_class_locked(const char *name)
{
    for (messaging_class_node_t *node = messaging.classes; node; node = node->next)
        if (strcmp(node->type->name, name) == 0) return node->type;
    return NULL;
}

esp_err_t solar_os_messaging_class_register(const solar_os_messaging_provider_class_t *type)
{
    if (!type || !messaging_text_valid(type->name, 16, false)) return ESP_ERR_INVALID_ARG;
    esp_err_t error = solar_os_messaging_init();
    if (error != ESP_OK) return error;
    messaging_class_node_t *fresh = solar_os_memory_calloc(1, sizeof(*fresh),
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "messages.class");
    if (!fresh) return ESP_ERR_NO_MEM;
    messaging_lock();
    const solar_os_messaging_provider_class_t *existing = messaging_class_locked(type->name);
    if (existing) {
        messaging_unlock(); solar_os_memory_free(fresh);
        return existing == type ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    fresh->type = type; fresh->next = messaging.classes; messaging.classes = fresh;
    for (messaging_provider_node_t *node = messaging.providers; node; node = node->next)
        if (strcmp(node->status.class_name, type->name) == 0) {
            node->status.capabilities = type->capabilities;
            node->status.connection_oriented = type->connection_oriented;
        }
    messaging_note_generation_locked();
    messaging_unlock();
    return ESP_OK;
}

bool solar_os_messaging_class_next(const char *after, solar_os_messaging_class_status_t *status)
{
    if (!after || !status || solar_os_messaging_init() != ESP_OK) return false;
    messaging_lock();
    const solar_os_messaging_provider_class_t *found = NULL;
    for (messaging_class_node_t *node = messaging.classes; node; node = node->next) {
        const solar_os_messaging_provider_class_t *type = node->type;
        if (strcmp(type->name, after) > 0 && (!found || strcmp(type->name, found->name) < 0)) found = type;
    }
    if (found) {
        strlcpy(status->name, found->name, sizeof(status->name));
        status->capabilities = found->capabilities; status->creatable = found->create != NULL;
    }
    messaging_unlock(); return found != NULL;
}

esp_err_t solar_os_messaging_instance_register(solar_os_messaging_provider_id_t id,
    const char *class_name, const char *name, void *context)
{
    if (!messaging_provider_valid(id) || !messaging_text_valid(class_name, 16, false) ||
        !messaging_text_valid(name, SOLAR_OS_MESSAGING_LABEL_MAX, false)) return ESP_ERR_INVALID_ARG;
    esp_err_t error = solar_os_messaging_init();
    if (error != ESP_OK) return error;
    messaging_provider_node_t *fresh = solar_os_memory_calloc(1, sizeof(*fresh),
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "messages.instance");
    if (!fresh) return ESP_ERR_NO_MEM;
    messaging_lock();
    messaging_provider_node_t *node = messaging_provider_node_locked(id);
    for (messaging_provider_node_t *other = messaging.providers; other; other = other->next) {
        if (other != node && !other->status.removed && strcmp(other->status.name, name) == 0) {
            messaging_unlock(); solar_os_memory_free(fresh); return ESP_ERR_INVALID_ARG;
        }
    }
    if (node && (node->status.removed || node->removing || strcmp(node->status.class_name, class_name) != 0 ||
                 strcmp(node->status.name, name) != 0 ||
                 (node->context && context && node->context != context))) {
        messaging_unlock(); solar_os_memory_free(fresh); return ESP_ERR_INVALID_STATE;
    }
    if (!node) {
        node = fresh; fresh = NULL;
        node->status.id = id; node->status.registered = true;
        strlcpy(node->status.name, name, sizeof(node->status.name));
        strlcpy(node->status.class_name, class_name, sizeof(node->status.class_name));
        node->next = messaging.providers; messaging.providers = node;
    }
    if (context) node->context = context;
    const solar_os_messaging_provider_class_t *type = messaging_class_locked(class_name);
    node->status.capabilities = type ? type->capabilities : 0;
    node->status.connection_oriented = type ? type->connection_oriented : false;
    messaging_note_generation_locked();
    messaging_unlock(); solar_os_memory_free(fresh);
    return ESP_OK;
}

const char *solar_os_messaging_instance_name(solar_os_messaging_provider_id_t id)
{
    messaging_lock();
    messaging_provider_node_t *node = messaging_provider_node_locked(id);
    const char *name = node ? node->status.name : NULL;
    messaging_unlock(); return name;
}

bool solar_os_messaging_provider_next(solar_os_messaging_provider_id_t *cursor,
    solar_os_messaging_provider_status_t *status)
{
    if (!cursor || !status || solar_os_messaging_init() != ESP_OK) return false;
    messaging_lock();
    messaging_provider_node_t *found = NULL;
    for (messaging_provider_node_t *node = messaging.providers; node; node = node->next)
        if (!node->status.removed && node->status.id > *cursor && (!found || node->status.id < found->status.id)) found = node;
    if (found) { *status = found->status; *cursor = found->status.id; }
    messaging_unlock(); return found != NULL;
}

solar_os_messaging_provider_id_t solar_os_messaging_provider_find(const char *name)
{
    if (!name) return 0;
    solar_os_messaging_provider_id_t cursor = 0;
    solar_os_messaging_provider_status_t status;
    while (solar_os_messaging_provider_next(&cursor, &status))
        if (strcmp(status.name, name) == 0) return cursor;
    return 0;
}

bool solar_os_messaging_provider_is_class(solar_os_messaging_provider_id_t id, const char *name)
{
    solar_os_messaging_provider_status_t status;
    return name && solar_os_messaging_provider_get_status(id, &status) == ESP_OK &&
        strcmp(status.class_name, name) == 0;
}

static esp_err_t messaging_profile_path(solar_os_messaging_provider_id_t id, char *path, size_t size)
{
    if (!solar_os_storage_is_mounted()) return ESP_ERR_INVALID_STATE;
    if (!messaging.profile_directory[0]) {
        esp_err_t error = solar_os_storage_default_path(".chat/profiles",
            messaging.profile_directory, sizeof(messaging.profile_directory));
        if (error != ESP_OK) return error;
    }
    int length = snprintf(path, size, "%s/%03" PRIu32 ".bin", messaging.profile_directory, id);
    return length >= 0 && (size_t)length < size ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t messaging_profile_read(const char *path, messaging_profile_header_t *header,
    void *data, size_t capacity, bool exact)
{
    FILE *file = fopen(path, "rb");
    if (!file) return ESP_ERR_NOT_FOUND;
    bool ok = fread(header, sizeof(*header), 1, file) == 1 &&
        header->magic == MESSAGING_PROFILE_MAGIC && header->version == 1 &&
        messaging_provider_valid(header->id) && header->size <= capacity &&
        (!exact || header->size == capacity) &&
        memchr(header->name, 0, sizeof(header->name)) &&
        memchr(header->class_name, 0, sizeof(header->class_name));
    if (ok) ok = fread(data, 1, header->size, file) == header->size && fgetc(file) == EOF;
    fclose(file);
    if (!ok) return ESP_ERR_INVALID_SIZE;
    uint32_t expected = header->crc; header->crc = 0;
    uint64_t hash = messaging_hash_update(2166136261U, header, sizeof(*header));
    hash = messaging_hash_update(hash, data, header->size);
    header->crc = expected;
    return (uint32_t)hash == expected ? ESP_OK : ESP_ERR_INVALID_CRC;
}

static esp_err_t messaging_profile_write(const solar_os_messaging_provider_status_t *status,
    const void *data, size_t size)
{
    if (!data || size > MESSAGING_PROFILE_MAX) return ESP_ERR_INVALID_SIZE;
    char path[SOLAR_OS_STORAGE_PATH_MAX], temp[SOLAR_OS_STORAGE_PATH_MAX], backup[SOLAR_OS_STORAGE_PATH_MAX];
    esp_err_t error = messaging_profile_path(status->id, path, sizeof(path));
    if (error != ESP_OK) return error;
    char directory[SOLAR_OS_STORAGE_PATH_MAX];
    strlcpy(directory, path, sizeof(directory)); *strrchr(directory, '/') = 0;
    error = solar_os_storage_makedirs(directory, true);
    if (error != ESP_OK) return error;
    if (snprintf(temp, sizeof(temp), "%s.tmp", path) >= (int)sizeof(temp) ||
        snprintf(backup, sizeof(backup), "%s.bak", path) >= (int)sizeof(backup)) return ESP_ERR_INVALID_SIZE;
    messaging_profile_header_t header = {.magic = MESSAGING_PROFILE_MAGIC, .version = 1,
        .id = status->id, .size = size};
    strlcpy(header.name, status->name, sizeof(header.name));
    strlcpy(header.class_name, status->class_name, sizeof(header.class_name));
    uint64_t hash = messaging_hash_update(2166136261U, &header, sizeof(header));
    header.crc = (uint32_t)messaging_hash_update(hash, data, size);
    FILE *file = fopen(temp, "wb");
    if (!file) return ESP_FAIL;
    bool ok = fwrite(&header, sizeof(header), 1, file) == 1 && fwrite(data, 1, size, file) == size;
    if (fflush(file) != 0 || fsync(fileno(file)) != 0) ok = false;
    if (fclose(file) != 0) ok = false;
    if (!ok) { unlink(temp); return ESP_FAIL; }
    void *verify = solar_os_memory_calloc(1, size ? size : 1, SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "messages.profile.verify");
    if (!verify) return ESP_ERR_NO_MEM;
    messaging_profile_header_t checked;
    error = messaging_profile_read(temp, &checked, verify, size, true);
    memset(verify, 0, size); solar_os_memory_free(verify);
    if (error != ESP_OK) return error;
    error = solar_os_storage_replace_file(temp, path, backup);
    if (error != ESP_OK) unlink(temp);
    return error;
}

esp_err_t solar_os_messaging_profile_save(solar_os_messaging_provider_id_t id,
    const void *data, size_t size)
{
    if (!size) return ESP_ERR_INVALID_SIZE;
    solar_os_messaging_provider_status_t status;
    esp_err_t error = solar_os_messaging_provider_get_status(id, &status);
    if (error != ESP_OK) return error;
    xSemaphoreTake(messaging.io_lock, portMAX_DELAY);
    messaging_lock();
    messaging_provider_node_t *node = messaging_provider_node_locked(id);
    bool unavailable = !node || node->status.removed || node->removing;
    messaging_unlock();
    error = unavailable ? ESP_ERR_INVALID_STATE : messaging_profile_write(&status, data, size);
    xSemaphoreGive(messaging.io_lock); return error;
}

esp_err_t solar_os_messaging_profile_load(solar_os_messaging_provider_id_t id, void *data, size_t size)
{
    if (!data || size > MESSAGING_PROFILE_MAX) return ESP_ERR_INVALID_ARG;
    esp_err_t error = solar_os_messaging_init();
    if (error != ESP_OK) return error;
    solar_os_messaging_provider_status_t status;
    if (solar_os_messaging_provider_get_status(id, &status) != ESP_OK || status.removed) return ESP_ERR_NOT_FOUND;
    char path[SOLAR_OS_STORAGE_PATH_MAX], backup[SOLAR_OS_STORAGE_PATH_MAX];
    error = messaging_profile_path(id, path, sizeof(path));
    if (error != ESP_OK) return error;
    messaging_profile_header_t header;
    xSemaphoreTake(messaging.io_lock, portMAX_DELAY);
    messaging_lock();
    messaging_provider_node_t *node = messaging_provider_node_locked(id);
    bool unavailable = !node || node->status.removed || node->removing;
    messaging_unlock();
    if (unavailable) { xSemaphoreGive(messaging.io_lock); return ESP_ERR_NOT_FOUND; }
    error = messaging_profile_read(path, &header, data, size, true);
    if (error != ESP_OK && snprintf(backup, sizeof(backup), "%s.bak", path) < (int)sizeof(backup))
        error = messaging_profile_read(backup, &header, data, size, true);
    if (error == ESP_OK && header.id != id) error = ESP_ERR_INVALID_ARG;
    xSemaphoreGive(messaging.io_lock); return error;
}

esp_err_t solar_os_messaging_instance_create(const char *class_name, const char *name,
    const void *config, size_t size, solar_os_messaging_provider_id_t *id)
{
    if (!id || !size || !messaging_text_valid(class_name, 16, false) ||
        !messaging_text_valid(name, SOLAR_OS_MESSAGING_LABEL_MAX, false)) return ESP_ERR_INVALID_ARG;
    esp_err_t error = solar_os_messaging_init();
    if (error != ESP_OK) return error;
    xSemaphoreTake(messaging.io_lock, portMAX_DELAY);
    messaging_lock();
    uint32_t next = 4;
    bool duplicate = false;
    for (messaging_provider_node_t *node = messaging.providers; node; node = node->next) {
        if (node->status.id >= next) next = node->status.id + 1;
        if (!node->status.removed && strcmp(node->status.name, name) == 0) duplicate = true;
    }
    /* Never reuse a history-only ID whose profile media is unavailable. */
    for (size_t i = 0; i < messaging.message_count; i++)
        if (messaging.messages[i].message.provider >= next) next = messaging.messages[i].message.provider + 1;
    messaging_unlock();
    solar_os_messaging_provider_status_t status = {.id = next};
    strlcpy(status.name, name, sizeof(status.name));
    strlcpy(status.class_name, class_name, sizeof(status.class_name));
    if (duplicate) error = ESP_ERR_INVALID_ARG;
    else if (!messaging_provider_valid(next)) error = ESP_ERR_NO_MEM;
    else error = messaging_profile_write(&status, config, size);
    if (error == ESP_OK) error = solar_os_messaging_instance_register(next, class_name, name, NULL);
    if (error == ESP_OK) *id = next;
    xSemaphoreGive(messaging.io_lock); return error;
}

static void messaging_profiles_restore(void)
{
    (void)solar_os_messaging_instance_register(1, "gateway", "gateway", NULL);
    (void)solar_os_messaging_instance_register(2, "meshcore", "meshcore", NULL);
    (void)solar_os_messaging_instance_register(3, "link", "link", NULL);
    char first[SOLAR_OS_STORAGE_PATH_MAX];
    if (messaging_profile_path(1, first, sizeof(first)) != ESP_OK) return;
    const char *directory = messaging.profile_directory;
    DIR *dir = opendir(directory);
    if (!dir) return;
    void *data = solar_os_memory_calloc(1, MESSAGING_PROFILE_MAX, SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "messages.profiles");
    struct dirent *entry;
    while (data && (entry = readdir(dir)) != NULL) {
        unsigned id = 0; int used = 0;
        if (sscanf(entry->d_name, "%u.bin%n", &id, &used) != 1 || used == 0 ||
            (entry->d_name[used] && strcmp(entry->d_name + used, ".bak") != 0)) continue;
        char path[SOLAR_OS_STORAGE_PATH_MAX];
        if (snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name) >= (int)sizeof(path)) continue;
        messaging_profile_header_t header;
        /* A valid committed file takes precedence over its backup. */
        if (entry->d_name[used]) {
            char active[SOLAR_OS_STORAGE_PATH_MAX];
            if (messaging_profile_path(id, active, sizeof(active)) == ESP_OK &&
                messaging_profile_read(active, &header, data, MESSAGING_PROFILE_MAX, false) == ESP_OK) continue;
        }
        if (messaging_profile_read(path, &header, data, MESSAGING_PROFILE_MAX, false) == ESP_OK && header.id == id) {
            if (header.size) (void)solar_os_messaging_instance_register(id, header.class_name, header.name, NULL);
            else {
                /* Empty profiles are credential-free removal records. Keep
                 * labels and reserve IDs even after their history is cleared. */
                messaging_provider_node_t *candidate = solar_os_memory_calloc(1, sizeof(*candidate),
                    SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "messages.archive");
                messaging_lock();
                messaging_provider_node_t *archived = messaging_provider_node_locked(id);
                bool fresh = archived == NULL;
                if (!archived) archived = candidate;
                if (archived) {
                    archived->status.id = id; archived->status.registered = true;
                    archived->status.removed = true;
                    strlcpy(archived->status.name, header.name, sizeof(archived->status.name));
                    strlcpy(archived->status.class_name, header.class_name, sizeof(archived->status.class_name));
                    strlcpy(archived->status.detail, "removed", sizeof(archived->status.detail));
                    if (fresh) { archived->next = messaging.providers; messaging.providers = archived; candidate = NULL; }
                }
                messaging_unlock();
                solar_os_memory_free(candidate);
            }
        }
    }
    if (data) memset(data, 0, MESSAGING_PROFILE_MAX);
    solar_os_memory_free(data); closedir(dir);
}

static esp_err_t messaging_instance_acquire(solar_os_messaging_provider_id_t id,
    messaging_provider_node_t **instance, const solar_os_messaging_provider_class_t **type, void **context)
{
    esp_err_t error = solar_os_messaging_init();
    if (error != ESP_OK) return error;
    messaging_lock();
    messaging_provider_node_t *node = messaging_provider_node_locked(id);
    if (!node || node->status.removed) error = ESP_ERR_NOT_FOUND;
    else if (node->removing) error = ESP_ERR_INVALID_STATE;
    else {
        *instance = node; *type = messaging_class_locked(node->status.class_name); *context = node->context;
        node->operations++;
    }
    messaging_unlock();
    return error;
}

static void messaging_instance_release(messaging_provider_node_t *node)
{ messaging_lock(); node->operations--; messaging_unlock(); }

void solar_os_messaging_settings_clear(solar_os_messaging_settings_t *settings)
{
    if (!settings) return;
    volatile unsigned char *p = (volatile unsigned char *)settings;
    for (size_t i = 0; i < sizeof(*settings); i++) p[i] = 0;
}

void solar_os_messaging_settings_value(solar_os_messaging_settings_t *settings,
    size_t field, const char *value)
{
    if (!settings || field >= settings->count || field >= SOLAR_OS_MESSAGING_SETTINGS_FIELDS) return;
    settings->present[field] = value && value[0];
    if (!settings->fields[field].secret)
        strlcpy(settings->values[field], value ? value : "", sizeof(settings->values[field]));
}

static esp_err_t messaging_settings_acquire(const char *class_name,
    solar_os_messaging_provider_id_t id, messaging_provider_node_t **node,
    const solar_os_messaging_provider_class_t **type, void **context)
{
    *node = NULL; *context = NULL; *type = NULL;
    if (!class_name) return ESP_ERR_INVALID_ARG;
    esp_err_t error = solar_os_messaging_init();
    if (error != ESP_OK) return error;
    if (id) {
        error = messaging_instance_acquire(id, node, type, context);
        if (error != ESP_OK) return error;
        if (*type && !strcmp((*type)->name, class_name) && *context) return ESP_OK;
        messaging_instance_release(*node); *node = NULL;
        return ESP_ERR_NOT_FOUND;
    }
    messaging_lock(); *type = messaging_class_locked(class_name); messaging_unlock();
    return *type ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static esp_err_t messaging_settings_read_leased(
    const solar_os_messaging_provider_class_t *type, void *context, unsigned form,
    solar_os_messaging_settings_t *settings)
{
    solar_os_messaging_settings_clear(settings);
    esp_err_t error = type->settings_read ? type->settings_read(context, form, settings) : ESP_ERR_NOT_SUPPORTED;
    if (error == ESP_OK && (!settings->fields || !settings->count ||
        settings->count > SOLAR_OS_MESSAGING_SETTINGS_FIELDS)) error = ESP_ERR_INVALID_SIZE;
    for (size_t i = 0; error == ESP_OK && i < settings->count; i++) {
        const solar_os_messaging_setting_t *field = &settings->fields[i];
        if (!field->label || !field->capacity || field->capacity > SOLAR_OS_MESSAGING_SETTING_MAX ||
            !memchr(settings->values[i], 0, field->capacity)) error = ESP_ERR_INVALID_SIZE;
        if (field->secret) memset(settings->values[i], 0, sizeof(settings->values[i]));
        settings->changed[i] = false;
    }
    if (error != ESP_OK) solar_os_messaging_settings_clear(settings);
    return error;
}

esp_err_t solar_os_messaging_settings_read(const char *class_name,
    solar_os_messaging_provider_id_t id, unsigned form, solar_os_messaging_settings_t *settings)
{
    if (!settings) return ESP_ERR_INVALID_ARG;
    solar_os_messaging_settings_clear(settings);
    messaging_provider_node_t *node; const solar_os_messaging_provider_class_t *type; void *context;
    esp_err_t error = messaging_settings_acquire(class_name, id, &node, &type, &context);
    if (error == ESP_OK) error = messaging_settings_read_leased(type, context, form, settings);
    if (node) messaging_instance_release(node);
    return error;
}

esp_err_t solar_os_messaging_settings_write(const char *class_name,
    solar_os_messaging_provider_id_t id, unsigned form, const char *name,
    const solar_os_messaging_settings_t *settings, solar_os_messaging_provider_id_t *result)
{
    if (!settings || !result || (!id && !messaging_text_valid(name, SOLAR_OS_MESSAGING_LABEL_MAX, false))) return ESP_ERR_INVALID_ARG;
    messaging_provider_node_t *node; const solar_os_messaging_provider_class_t *type; void *context;
    esp_err_t error = messaging_settings_acquire(class_name, id, &node, &type, &context);
    if (error != ESP_OK) return error;
    solar_os_messaging_settings_t *schema = solar_os_memory_calloc(1, sizeof(*schema),
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "messages.settings");
    if (!schema) error = ESP_ERR_NO_MEM;
    else error = messaging_settings_read_leased(type, context, form, schema);
    if (error == ESP_OK && schema->count != settings->count) error = ESP_ERR_INVALID_ARG;
    for (size_t i = 0; error == ESP_OK && i < schema->count; i++) {
        const solar_os_messaging_setting_t *field = &schema->fields[i];
        if (!memchr(settings->values[i], 0, field->capacity)) error = ESP_ERR_INVALID_SIZE;
        else if (field->required && (!id || settings->changed[i]) && !settings->values[i][0]) error = ESP_ERR_INVALID_ARG;
    }
    if (error == ESP_OK) error = type->settings_write ?
        type->settings_write(context, form, name, settings, result) : ESP_ERR_NOT_SUPPORTED;
    solar_os_messaging_settings_clear(schema); solar_os_memory_free(schema);
    if (node) messaging_instance_release(node);
    return error;
}

esp_err_t solar_os_messaging_instance_retain(solar_os_messaging_provider_id_t id, void **lease)
{
    if (!lease) return ESP_ERR_INVALID_ARG;
    *lease = NULL;
    messaging_provider_node_t *node; const solar_os_messaging_provider_class_t *type; void *context;
    esp_err_t error = messaging_instance_acquire(id, &node, &type, &context);
    if (error == ESP_OK) *lease = node;
    return error;
}
void solar_os_messaging_instance_release(void *lease)
{ if (lease) messaging_instance_release(lease); }

esp_err_t solar_os_messaging_instance_add(const char *class_name, const char *name,
    int argc, const char *const *argv, solar_os_messaging_provider_id_t *id)
{
    if (!class_name || !name || !id || argc < 0 || (argc && !argv)) return ESP_ERR_INVALID_ARG;
    esp_err_t error = solar_os_messaging_init(); if (error != ESP_OK) return error;
    messaging_lock();
    const solar_os_messaging_provider_class_t *type = messaging_class_locked(class_name);
    messaging_unlock();
    return type && type->create ? type->create(name, argc, argv, id) : ESP_ERR_NOT_SUPPORTED;
}

esp_err_t solar_os_messaging_instance_configure(solar_os_messaging_provider_id_t id,
    int argc, const char *const *argv)
{
    if (argc < 0 || (argc && !argv)) return ESP_ERR_INVALID_ARG;
    messaging_provider_node_t *node; const solar_os_messaging_provider_class_t *type; void *context;
    esp_err_t error = messaging_instance_acquire(id, &node, &type, &context);
    if (error != ESP_OK) return error;
    error = type && type->configure ? type->configure(context, argc, argv) : ESP_ERR_NOT_SUPPORTED;
    messaging_instance_release(node); return error;
}

static esp_err_t messaging_operation_leased(solar_os_messaging_provider_id_t id, const char *target,
    unsigned operation, solar_os_conversation_id_t *conversation,
    const solar_os_messaging_provider_class_t *type, void *context)
{
    if (!type) return ESP_ERR_NOT_SUPPORTED;
    esp_err_t error;
    if (operation == 0 && type->join_open) return type->join_open(context, target, conversation);
    char normalized[SOLAR_OS_MESSAGING_PROVIDER_KEY_MAX];
    if (type->normalize_target) {
        error = type->normalize_target(context, target, normalized, sizeof(normalized));
        if (error != ESP_OK) return error;
        target = normalized;
    }
    if (operation == 2) return type->direct_open ? type->direct_open(context, target, conversation) : ESP_ERR_NOT_SUPPORTED;
    if ((operation == 0 && !type->join) || (operation == 1 && !type->leave)) return ESP_ERR_NOT_SUPPORTED;
    if (operation == 0) {
        messaging_lock();
        int index = messaging_conversation_index_locked(id, target);
        bool already = index >= 0 && (messaging.conversations[index].join_state == SOLAR_OS_MESSAGING_JOIN_JOINED ||
            messaging.conversations[index].join_state == SOLAR_OS_MESSAGING_JOIN_PENDING);
        if (already && conversation) *conversation = messaging.conversations[index].id;
        messaging_unlock();
        if (already) return ESP_OK;
        const solar_os_messaging_conversation_upsert_t request = {.provider = id,
            .provider_key = target, .title = target, .kind = SOLAR_OS_CONVERSATION_ROOM};
        error = solar_os_messaging_conversation_upsert(&request, conversation);
        if (error != ESP_OK) return error;
        (void)solar_os_messaging_join_update(id, target, SOLAR_OS_MESSAGING_JOIN_PENDING, NULL);
    }
    error = operation == 0 ? type->join(context, target) : type->leave(context, target);
    if (error != ESP_OK) (void)solar_os_messaging_join_update(id, target,
        SOLAR_OS_MESSAGING_JOIN_FAILED, esp_err_to_name(error));
    return error;
}

static esp_err_t messaging_operation(solar_os_messaging_provider_id_t id, const char *target,
    unsigned operation, solar_os_conversation_id_t *conversation)
{
    if (!target || !target[0]) return ESP_ERR_INVALID_ARG;
    messaging_provider_node_t *node;
    const solar_os_messaging_provider_class_t *type;
    void *context;
    esp_err_t error = messaging_instance_acquire(id, &node, &type, &context);
    if (error != ESP_OK) return error;
    error = messaging_operation_leased(id, target, operation, conversation, type, context);
    messaging_instance_release(node); return error;
}

esp_err_t solar_os_messaging_join(solar_os_messaging_provider_id_t id, const char *target,
    solar_os_conversation_id_t *conversation)
{ return messaging_operation(id, target, 0, conversation); }

esp_err_t solar_os_messaging_leave(solar_os_conversation_id_t id)
{
    solar_os_messaging_conversation_t conversation;
    esp_err_t error = solar_os_messaging_conversation_get(id, &conversation);
    if (error != ESP_OK) return error;
    return messaging_operation(conversation.provider, conversation.provider_key, 1, NULL);
}

esp_err_t solar_os_messaging_open_direct(solar_os_messaging_provider_id_t id, const char *target,
    solar_os_conversation_id_t *conversation)
{ return messaging_operation(id, target, 2, conversation); }

esp_err_t solar_os_messaging_connect(solar_os_messaging_provider_id_t id, bool enabled)
{
    messaging_provider_node_t *node;
    const solar_os_messaging_provider_class_t *type;
    void *context;
    esp_err_t error = messaging_instance_acquire(id, &node, &type, &context);
    if (error != ESP_OK) return error;
    error = type && type->connect ? type->connect(context, enabled) : ESP_ERR_NOT_SUPPORTED;
    messaging_instance_release(node); return error;
}

esp_err_t solar_os_messaging_join_update(solar_os_messaging_provider_id_t id, const char *target,
    solar_os_messaging_join_state_t state, const char *error)
{
    if (!target) return ESP_ERR_INVALID_ARG;
    messaging_lock();
    messaging_provider_node_t *node = messaging_provider_node_locked(id);
    if (node && node->status.removed) { messaging_unlock(); return ESP_ERR_NOT_FOUND; }
    int index = messaging_conversation_index_locked(id, target);
    if (index < 0) { messaging_unlock(); return ESP_ERR_NOT_FOUND; }
    if (messaging.conversations[index].join_state == state &&
        strcmp(messaging.conversations[index].join_error, error ? error : "") == 0) {
        messaging_unlock(); return ESP_OK;
    }
    messaging.conversations[index].join_state = state;
    strlcpy(messaging.conversations[index].join_error, error ? error : "",
        sizeof(messaging.conversations[index].join_error));
    messaging_note_generation_locked();
    messaging_publish_event_locked(SOLAR_OS_MESSAGING_EVENT_CONVERSATION, id,
        messaging.conversations[index].id, 0, SOLAR_OS_DELIVERY_RECEIVED, ESP_OK);
    messaging_unlock(); return ESP_OK;
}

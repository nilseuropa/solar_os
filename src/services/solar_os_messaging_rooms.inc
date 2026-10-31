/* Explicit room departures are UI/subscription intent, independent of live
 * join state. Keep a checked, bounded preference file; never rewrite history. */
#define MESSAGING_ROOMS_FILE "messaging_rooms"

static bool messaging_room_left_locked(solar_os_messaging_provider_id_t provider, const char *key)
{
    for (size_t i = 0; i < SOLAR_OS_MESSAGING_CONVERSATION_CAPACITY; i++) {
        const messaging_left_room_t *room = &messaging.room_preferences->data.rooms[i];
        if (room->provider == provider && !strcmp(room->key, key)) return true;
    }
    return false;
}

static bool messaging_rooms_valid(const messaging_left_rooms_t *data, size_t length)
{
    if (length != sizeof(*data) || data->version != 1) return false;
    for (size_t i = 0; i < SOLAR_OS_MESSAGING_CONVERSATION_CAPACITY; i++) {
        const messaging_left_room_t *room = &data->rooms[i];
        if (room->provider && (!messaging_provider_valid(room->provider) || !room->key[0] ||
            !memchr(room->key, 0, sizeof(room->key)))) return false;
    }
    return true;
}

static void messaging_rooms_load(void)
{
    messaging_room_preferences_t *prefs = messaging.room_preferences;
    size_t length = 0;
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    esp_err_t error = solar_os_storage_default_path(".settings/" MESSAGING_ROOMS_FILE ".bin", path, sizeof(path));
    if (error == ESP_OK) {
        error = solar_os_saved_config_read(path, &prefs->data, sizeof(prefs->data), &length);
        if (error != ESP_OK || !messaging_rooms_valid(&prefs->data, length)) {
            char backup[SOLAR_OS_STORAGE_PATH_MAX + 5];
            snprintf(backup, sizeof(backup), "%s.bak", path);
            error = solar_os_saved_config_read(backup, &prefs->data, sizeof(prefs->data), &length);
            if (error == ESP_OK && messaging_rooms_valid(&prefs->data, length)) {
                if (unlink(path) != 0 && errno != ENOENT) error = ESP_FAIL;
                else if (rename(backup, path) != 0) error = ESP_FAIL;
            }
        }
    }
    if (error != ESP_OK || !messaging_rooms_valid(&prefs->data, length))
        memset(&prefs->data, 0, sizeof(prefs->data));
    else strlcpy(prefs->file.path, path, sizeof(prefs->file.path));
    prefs->data.version = 1;
}

bool solar_os_messaging_room_left(solar_os_messaging_provider_id_t provider, const char *target)
{
    if (!target || solar_os_messaging_init() != ESP_OK) return false;
    messaging_lock();
    bool left = messaging_room_left_locked(provider, target);
    messaging_unlock();
    return left;
}

static esp_err_t messaging_room_set_left(solar_os_messaging_provider_id_t provider, const char *key, bool left)
{
    xSemaphoreTake(messaging.io_lock, portMAX_DELAY);
    messaging_lock();
    if (messaging_room_left_locked(provider, key) == left) {
        messaging_unlock(); xSemaphoreGive(messaging.io_lock); return ESP_OK;
    }
    messaging_left_rooms_t *next = solar_os_memory_calloc(1, sizeof(*next),
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "messages.rooms.save");
    if (!next) { messaging_unlock(); xSemaphoreGive(messaging.io_lock); return ESP_ERR_NO_MEM; }
    *next = messaging.room_preferences->data;
    size_t empty = SOLAR_OS_MESSAGING_CONVERSATION_CAPACITY;
    for (size_t i = 0; i < SOLAR_OS_MESSAGING_CONVERSATION_CAPACITY; i++) {
        messaging_left_room_t *room = &next->rooms[i];
        messaging_provider_node_t *node = messaging_provider_node_locked(room->provider);
        if ((node && node->status.removed) || (room->provider == provider && !strcmp(room->key, key)))
            memset(room, 0, sizeof(*room));
        if (!room->provider) empty = i;
    }
    esp_err_t error = ESP_OK;
    if (left) {
        if (empty == SOLAR_OS_MESSAGING_CONVERSATION_CAPACITY) error = ESP_ERR_NO_MEM;
        else {
            next->rooms[empty].provider = provider;
            strlcpy(next->rooms[empty].key, key, sizeof(next->rooms[empty].key));
        }
    }
    messaging_unlock();
    if (error == ESP_OK)
        error = solar_os_saved_config_save(&messaging.room_preferences->file, MESSAGING_ROOMS_FILE, next, sizeof(*next));
    messaging_lock();
    if (error == ESP_OK) {
        messaging.room_preferences->data = *next;
        int index = messaging_conversation_index_locked(provider, key);
        if (index >= 0) {
            messaging.conversations[index].left = left;
            messaging_note_generation_locked();
            messaging_publish_event_locked(SOLAR_OS_MESSAGING_EVENT_CONVERSATION, provider,
                messaging.conversations[index].id, 0, SOLAR_OS_DELIVERY_RECEIVED, ESP_OK);
        }
    }
    messaging_unlock();
    solar_os_memory_free(next);
    xSemaphoreGive(messaging.io_lock);
    return error;
}

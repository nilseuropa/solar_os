/*
 * SolarOS SC2 security service: ESP-IDF glue around the SC2 session engine.
 * Platform pieces: hardware RNG mixed into a persisted HMAC pool, A/B record
 * files on internal flash, FreeRTOS mutexes, PSRAM work buffers. The SHA-256
 * and HMAC backend is mbedTLS (SC2_USE_MBEDTLS, set by the package build).
 */
#include "solar_os_sc2.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "solar_os_memory.h"
#include "solar_os_storage.h"

static const char *TAG = "sc2";

#define SC2_SLOT_HEADER 12u          /* generation:u64 | crc32(generation):u32 */
#define SC2_FILE_MAX (4096u + SC2_SLOT_HEADER)
#define SC2_PATH_MAX SOLAR_OS_STORAGE_PATH_MAX
#define SC2_SEED_LEN 32u

typedef struct {
    SemaphoreHandle_t lock;     /* serialises the engine and its store */
    SemaphoreHandle_t rng_lock; /* serialises the entropy pool */
    sc2e_t *engine;
    bool ready;
    bool created;
    char base[SC2_PATH_MAX];
    uint8_t pool[32];
    uint64_t pool_counter;
} sc2_service_t;

static sc2_service_t s_sc2;
static portMUX_TYPE s_sc2_init_mux = portMUX_INITIALIZER_UNLOCKED;

/* ---------------------------------------------------------------------- */
/* Errors                                                                  */
/* ---------------------------------------------------------------------- */

static esp_err_t sc2_esp(int rc)
{
    return rc == SC2_OK ? ESP_OK : SOLAR_OS_SC2_ESP_ERR(rc);
}

const char *solar_os_sc2_err_text(esp_err_t err)
{
    if (err > SOLAR_OS_SC2_ERR_BASE && err <= SOLAR_OS_SC2_ERR_BASE - SC2_ERR_NOT_FOUND) {
        return sc2_strerror((int)(SOLAR_OS_SC2_ERR_BASE - err));
    }
    return esp_err_to_name(err);
}

/* ---------------------------------------------------------------------- */
/* Memory                                                                  */
/* ---------------------------------------------------------------------- */

void *solar_os_sc2_buffer_alloc(size_t len)
{
    return solar_os_memory_calloc(1, len, SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "sc2.work");
}

void solar_os_sc2_buffer_free(void *ptr, size_t len)
{
    if (ptr != NULL) {
        sc2_memzero(ptr, len);
        solar_os_memory_free(ptr);
    }
}

static void *pf_alloc(void *ctx, size_t len)
{
    (void)ctx;
    return solar_os_sc2_buffer_alloc(len);
}

static void pf_release(void *ctx, void *ptr, size_t len)
{
    (void)ctx;
    solar_os_sc2_buffer_free(ptr, len);
}

/* ---------------------------------------------------------------------- */
/* Randomness: HMAC-SHA-256 pool keyed by the hardware RNG and a seed that */
/* is carried across boots. Every output block mixes in fresh esp_fill_random */
/* bytes, so the output is never weaker than the hardware RNG alone.         */
/* ---------------------------------------------------------------------- */

static int pool_block(uint8_t out[32])
{
    uint8_t in[8 + 32 + 8];
    sc2_store_be64(in, ++s_sc2.pool_counter);
    esp_fill_random(in + 8, 32);
    sc2_store_be64(in + 40, (uint64_t)esp_timer_get_time());
    int rc = sc2_hmac_sha256(out, s_sc2.pool, sizeof(s_sc2.pool), in, sizeof(in));
    if (rc == SC2_OK) {
        /* Ratchet the pool key forward so earlier output cannot be rebuilt. */
        uint8_t next[32];
        rc = sc2_hmac_sha256(next, s_sc2.pool, sizeof(s_sc2.pool), out, 32);
        if (rc == SC2_OK) {
            memcpy(s_sc2.pool, next, sizeof(next));
        }
        sc2_memzero(next, sizeof(next));
    }
    sc2_memzero(in, sizeof(in));
    return rc;
}

static int sc2_rng(void *ctx, uint8_t *out, size_t len)
{
    (void)ctx;
    uint8_t block[32];
    int rc = SC2_OK;
    if (s_sc2.rng_lock == NULL || xSemaphoreTake(s_sc2.rng_lock, portMAX_DELAY) != pdTRUE) {
        return -1;
    }
    while (len > 0 && rc == SC2_OK) {
        rc = pool_block(block);
        size_t n = len < sizeof(block) ? len : sizeof(block);
        memcpy(out, block, n);
        out += n;
        len -= n;
    }
    xSemaphoreGive(s_sc2.rng_lock);
    sc2_memzero(block, sizeof(block));
    return rc == SC2_OK ? 0 : -1;
}

static const sc2_rng_t SC2_RNG = {sc2_rng, NULL};

static void seed_path(char *out, size_t cap)
{
    snprintf(out, cap, "%s/seed.bin", s_sc2.base);
}

/* Key the pool from the hardware RNG, the boot timer and the stored seed,
 * then immediately replace the stored seed. */
static void pool_init(void)
{
    uint8_t ikm[64 + SC2_SEED_LEN + 8];
    uint8_t seed[SC2_SEED_LEN];
    char path[SC2_PATH_MAX];
    size_t len = 0;
    esp_fill_random(ikm, 64);
    memset(ikm + 64, 0, SC2_SEED_LEN);
    seed_path(path, sizeof(path));
    if (solar_os_storage_read_file(path, ikm + 64, SC2_SEED_LEN, &len) != ESP_OK || len != SC2_SEED_LEN) {
        memset(ikm + 64, 0, SC2_SEED_LEN);
    }
    sc2_store_be64(ikm + 64 + SC2_SEED_LEN, (uint64_t)esp_timer_get_time());
    (void)sc2_hkdf(s_sc2.pool, sizeof(s_sc2.pool), (const uint8_t *)"SC2-POOL", 8, ikm, sizeof(ikm),
                   (const uint8_t *)"pool", 4);
    if (sc2_rng(NULL, seed, sizeof(seed)) == 0) {
        if (solar_os_storage_write_file(path, seed, sizeof(seed), false) != ESP_OK) {
            ESP_LOGW(TAG, "could not refresh %s", path);
        }
    }
    sc2_memzero(ikm, sizeof(ikm));
    sc2_memzero(seed, sizeof(seed));
}

/* ---------------------------------------------------------------------- */
/* Storage: each record is kept in two slot files, <name>.a and <name>.b.  */
/* A save writes the slot that does not hold the newest generation, so a   */
/* torn write always leaves the previous good copy readable.              */
/* ---------------------------------------------------------------------- */

static bool name_ok(const char *name)
{
    size_t n = strlen(name);
    if (n == 0 || n >= SC2E_NAME_MAX || name[0] == '/' || name[0] == '.') {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || (c == '/' && name[i + 1] != '/');
        if (!ok) {
            return false;
        }
    }
    return true;
}

static void slot_path(char *out, size_t cap, const char *name, char slot)
{
    snprintf(out, cap, "%s/%s.%c", s_sc2.base, name, slot);
}

/* Generation of one slot from its 12-byte header (0 when missing or
 * invalid). Only the header is read. */
static uint64_t slot_gen(const char *name, char slot)
{
    char path[SC2_PATH_MAX];
    uint8_t hdr[SC2_SLOT_HEADER];
    size_t n = 0;
    slot_path(path, sizeof(path), name, slot);
    if (solar_os_storage_read_file(path, hdr, sizeof(hdr), &n) != ESP_OK || n != sizeof(hdr)) {
        return 0;
    }
    if (sc2_crc32(0, hdr, 8) != sc2_load_be32(hdr + 8)) {
        return 0;
    }
    return sc2_load_be64(hdr);
}

/* Read the record of one slot (everything after the header) straight into
 * buf, so loading needs no buffer of its own. SC2_ERR_BOUNDS when it does
 * not fit in cap, SC2_ERR_FORMAT when the slot is short or unreadable. */
static int slot_payload(const char *name, char slot, uint64_t gen, uint8_t *buf, size_t cap, size_t *len)
{
    char path[SC2_PATH_MAX];
    uint8_t hdr[SC2_SLOT_HEADER];
    slot_path(path, sizeof(path), name, slot);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return SC2_ERR_FORMAT;
    }
    int rc = SC2_OK;
    size_t n = 0;
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr) || sc2_load_be64(hdr) != gen) {
        rc = SC2_ERR_FORMAT;
    } else {
        n = fread(buf, 1, cap, f);
        if (ferror(f)) {
            rc = SC2_ERR_FORMAT;
        } else if (n == cap && fgetc(f) != EOF) {
            rc = SC2_ERR_BOUNDS;
        } else if (n == 0) {
            rc = SC2_ERR_FORMAT;
        }
    }
    fclose(f);
    *len = rc == SC2_OK ? n : 0;
    return rc;
}

static int pf_load(void *ctx, const char *name, uint8_t *buf, size_t cap, size_t *len)
{
    (void)ctx;
    if (!name_ok(name)) {
        return SC2_ERR_ARG;
    }
    uint64_t ga = slot_gen(name, 'a');
    uint64_t gb = slot_gen(name, 'b');
    if (ga == 0 && gb == 0) {
        return SC2_ERR_NOT_FOUND;
    }
    /* Prefer the newest slot whose record also unwraps; fall back to the
     * other one if the newest is damaged. */
    const char order[2] = {ga >= gb ? 'a' : 'b', ga >= gb ? 'b' : 'a'};
    const uint64_t gens[2] = {ga >= gb ? ga : gb, ga >= gb ? gb : ga};
    int rc = SC2_ERR_FORMAT;
    for (int i = 0; i < 2; i++) {
        if (gens[i] == 0) {
            continue;
        }
        size_t rec_len = 0;
        int src = slot_payload(name, order[i], gens[i], buf, cap, &rec_len);
        if (src == SC2_ERR_BOUNDS) {
            rc = SC2_ERR_BOUNDS;
            break;
        }
        const uint8_t *payload;
        size_t payload_len;
        if (src != SC2_OK || rec_len < SC2E_RECORD_OVERHEAD || buf[0] != 'S' ||
            sc2e_record_unwrap(buf, rec_len, buf[4], buf[5], &payload, &payload_len) != SC2_OK) {
            continue;
        }
        *len = rec_len;
        rc = SC2_OK;
        if (i == 1) {
            ESP_LOGW(TAG, "%s: newest copy damaged, using previous", name);
        }
        break;
    }
    if (rc != SC2_OK) {
        sc2_memzero(buf, cap);
    }
    return rc;
}

static int ensure_parent(const char *name)
{
    const char *slash = strchr(name, '/');
    if (slash == NULL) {
        return SC2_OK;
    }
    char dir[SC2_PATH_MAX];
    int n = snprintf(dir, sizeof(dir), "%s/%.*s", s_sc2.base, (int)(slash - name), name);
    if (n <= 0 || (size_t)n >= sizeof(dir)) {
        return SC2_ERR_BOUNDS;
    }
    return solar_os_storage_makedirs(dir, true) == ESP_OK ? SC2_OK : SC2_ERR_STATE;
}

static int pf_save(void *ctx, const char *name, const uint8_t *buf, size_t len)
{
    (void)ctx;
    if (!name_ok(name)) {
        return SC2_ERR_ARG;
    }
    if (len + SC2_SLOT_HEADER >= SC2_FILE_MAX) {
        return SC2_ERR_BOUNDS;
    }
    uint64_t ga = slot_gen(name, 'a');
    uint64_t gb = slot_gen(name, 'b');
    char slot = ga > gb ? 'b' : 'a';
    uint64_t gen = (ga > gb ? ga : gb) + 1u;
    uint8_t hdr[SC2_SLOT_HEADER];
    sc2_store_be64(hdr, gen);
    sc2_store_be32(hdr + 8, sc2_crc32(0, hdr, 8));
    char path[SC2_PATH_MAX];
    slot_path(path, sizeof(path), name, slot);
    int rc = ensure_parent(name);
    /* Header, then the record appended: no staging buffer. A write torn in
     * between leaves a slot that does not unwrap, so loads fall back to the
     * other (previous) slot exactly as for any other torn write. */
    if (rc == SC2_OK && (solar_os_storage_write_file(path, hdr, sizeof(hdr), false) != ESP_OK ||
                         solar_os_storage_write_file(path, buf, len, true) != ESP_OK)) {
        ESP_LOGE(TAG, "write %s failed", path);
        rc = SC2_ERR_STATE;
    }
    return rc;
}

/* Overwrite a slot file in place with zeros before unlinking it. This is
 * best effort: the flash wear-levelling layer may still hold older copies of
 * the sectors until they are erased (see the manual page on keys at rest). */
static void slot_scrub(const char *path)
{
    static const uint8_t zeros[256];
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0) {
        return;
    }
    FILE *f = fopen(path, "r+b");
    if (f == NULL) {
        return;
    }
    size_t left = (size_t)st.st_size;
    while (left > 0) {
        size_t n = left < sizeof(zeros) ? left : sizeof(zeros);
        if (fwrite(zeros, 1, n, f) != n) {
            break;
        }
        left -= n;
    }
    (void)solar_os_storage_sync_file(f);
    fclose(f);
}

static int pf_remove(void *ctx, const char *name)
{
    (void)ctx;
    char path[SC2_PATH_MAX];
    bool exists = false;
    if (!name_ok(name)) {
        return SC2_ERR_ARG;
    }
    for (int i = 0; i < 2; i++) {
        slot_path(path, sizeof(path), name, i == 0 ? 'a' : 'b');
        if (solar_os_storage_exists(path, &exists) == ESP_OK && exists) {
            slot_scrub(path);
            if (solar_os_storage_remove(path) != ESP_OK) {
                return SC2_ERR_STATE;
            }
        }
    }
    return SC2_OK;
}

static int pf_count(void *ctx, const char *dir, size_t *count)
{
    (void)ctx;
    char path[SC2_PATH_MAX];
    char sibling[SC2_PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", s_sc2.base, dir);
    *count = 0;
    DIR *d = opendir(path);
    if (d == NULL) {
        return SC2_OK;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        size_t n = strlen(ent->d_name);
        if (n < 3 || ent->d_name[n - 2] != '.') {
            continue;
        }
        char slot = ent->d_name[n - 1];
        if (slot == 'a') {
            (*count)++;
        } else if (slot == 'b') {
            /* Count a .b slot only when there is no matching .a slot. */
            struct stat st;
            snprintf(sibling, sizeof(sibling), "%s/%.*sa", path, (int)(n - 1), ent->d_name);
            if (stat(sibling, &st) != 0) {
                (*count)++;
            }
        }
    }
    closedir(d);
    return SC2_OK;
}

static uint64_t pf_now(void *ctx)
{
    (void)ctx;
    time_t now = time(NULL);
    return now >= (time_t)SOLAR_OS_SC2_MIN_WALL_CLOCK ? (uint64_t)now : 0u;
}

static uint64_t host_clock_us(void *ctx)
{
    (void)ctx;
    return (uint64_t)esp_timer_get_time();
}

/* ---------------------------------------------------------------------- */
/* Init                                                                    */
/* ---------------------------------------------------------------------- */

static esp_err_t sc2_locks(void)
{
    portENTER_CRITICAL(&s_sc2_init_mux);
    bool need = s_sc2.lock == NULL;
    portEXIT_CRITICAL(&s_sc2_init_mux);
    if (!need) {
        return ESP_OK;
    }
    SemaphoreHandle_t lock = xSemaphoreCreateMutex();
    SemaphoreHandle_t rng_lock = xSemaphoreCreateMutex();
    if (lock == NULL || rng_lock == NULL) {
        if (lock) {
            vSemaphoreDelete(lock);
        }
        if (rng_lock) {
            vSemaphoreDelete(rng_lock);
        }
        return ESP_ERR_NO_MEM;
    }
    portENTER_CRITICAL(&s_sc2_init_mux);
    if (s_sc2.lock == NULL) {
        s_sc2.lock = lock;
        s_sc2.rng_lock = rng_lock;
        lock = NULL;
        rng_lock = NULL;
    }
    portEXIT_CRITICAL(&s_sc2_init_mux);
    if (lock) {
        vSemaphoreDelete(lock);
        vSemaphoreDelete(rng_lock);
    }
    return ESP_OK;
}

static esp_err_t sc2_init_locked(void)
{
    if (s_sc2.ready) {
        return ESP_OK;
    }
    if (!solar_os_storage_flash_is_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }
    const char *mount = solar_os_storage_flash_mount_point();
    esp_err_t err = solar_os_storage_join_path(mount, SOLAR_OS_SC2_STORE_DIR, s_sc2.base, sizeof(s_sc2.base));
    if (err != ESP_OK) {
        return err;
    }
    err = solar_os_storage_makedirs(s_sc2.base, true);
    if (err != ESP_OK) {
        return err;
    }
    pool_init();
    sc2e_t *engine = solar_os_sc2_buffer_alloc(sizeof(*engine));
    if (engine == NULL) {
        return ESP_ERR_NO_MEM;
    }
    sc2e_platform_t pf = {
        .load = pf_load,
        .save = pf_save,
        .remove = pf_remove,
        .count = pf_count,
        .alloc = pf_alloc,
        .release = pf_release,
        .now = pf_now,
        .rng = SC2_RNG,
        .ctx = NULL,
    };
    int rc = sc2e_open(engine, &pf);
    if (rc != SC2_OK) {
        ESP_LOGE(TAG, "open %s: %s", s_sc2.base, sc2_strerror(rc));
        solar_os_sc2_buffer_free(engine, sizeof(*engine));
        return sc2_esp(rc);
    }
    s_sc2.engine = engine;
    s_sc2.created = engine->created;
    s_sc2.ready = true;
    ESP_LOGI(TAG, "%s device %s (%s, hash %s)", engine->created ? "created" : "loaded", engine->self_hex,
             SC2_VERSION_STRING, sc2_hash_backend());
    return ESP_OK;
}

static esp_err_t sc2_enter(void)
{
    esp_err_t err = sc2_locks();
    if (err != ESP_OK) {
        return err;
    }
    xSemaphoreTake(s_sc2.lock, portMAX_DELAY);
    err = sc2_init_locked();
    if (err != ESP_OK) {
        xSemaphoreGive(s_sc2.lock);
    }
    return err;
}

static void sc2_leave(void)
{
    xSemaphoreGive(s_sc2.lock);
}

sc2e_t *solar_os_sc2_engine_lock(void)
{
    return sc2_enter() == ESP_OK ? s_sc2.engine : NULL;
}

void solar_os_sc2_engine_unlock(void)
{
    sc2_leave();
}

esp_err_t solar_os_sc2_init(void)
{
    esp_err_t err = sc2_enter();
    if (err == ESP_OK) {
        sc2_leave();
    }
    return err;
}

#define SC2_CALL(expr)                \
    do {                              \
        esp_err_t enter_ = sc2_enter(); \
        if (enter_ != ESP_OK) {       \
            return enter_;            \
        }                             \
        int rc_ = (expr);             \
        sc2_leave();                  \
        return sc2_esp(rc_);          \
    } while (0)

/* ---------------------------------------------------------------------- */
/* API                                                                     */
/* ---------------------------------------------------------------------- */

esp_err_t solar_os_sc2_get_status(solar_os_sc2_status_t *status)
{
    sc2e_identity_info_t info;
    sc2e_counts_t counts;
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(status, 0, sizeof(*status));
    status->core_version = SC2_VERSION_STRING;
    status->hash_backend = sc2_hash_backend();
    esp_err_t err = sc2_enter();
    if (err != ESP_OK) {
        return err;
    }
    int rc = sc2e_identity(s_sc2.engine, &info);
    if (rc == SC2_OK) {
        rc = sc2e_counts(s_sc2.engine, &counts);
    }
    if (rc == SC2_OK) {
        status->ready = true;
        status->created = s_sc2.created;
        memcpy(status->device_id, info.device_id, sizeof(status->device_id));
        memcpy(status->fingerprint, info.fingerprint, sizeof(status->fingerprint));
        status->spk_id = info.spk_id;
        status->spk_created = info.spk_created;
        status->opk_count = info.opk_count;
        status->peers = counts.peers;
        status->sessions = counts.sessions;
        status->rooms = counts.rooms;
        strlcpy(status->store_path, s_sc2.base, sizeof(status->store_path));
        status->work_in_psram = solar_os_memory_is_external(s_sc2.engine);
    }
    sc2_leave();
    return sc2_esp(rc);
}

esp_err_t solar_os_sc2_identity(sc2e_identity_info_t *info)
{
    SC2_CALL(sc2e_identity(s_sc2.engine, info));
}

esp_err_t solar_os_sc2_peer_info(const char *peer, sc2e_peer_info_t *info)
{
    SC2_CALL(sc2e_peer_info(s_sc2.engine, peer, info));
}

esp_err_t solar_os_sc2_safety(const char *peer, sc2e_safety_t *safety)
{
    SC2_CALL(sc2e_safety(s_sc2.engine, peer, safety));
}

esp_err_t solar_os_sc2_forget_peer(const char *peer)
{
    SC2_CALL(sc2e_forget_peer(s_sc2.engine, peer));
}

/* ---------------------------------------------------------------------- */
/* Peer references: full device ID, saved alias or unique hex prefix.      */
/* Aliases are "<name> <device>\n" lines in <base>/alias.txt, verified     */
/* peers "<device> <own device>\n" lines in <base>/verified.txt (public    */
/* data only: the same small record file code serves both).               */
/* ---------------------------------------------------------------------- */

/* "<key> <device>\r\n": key is an alias name or (verified.txt) a device ID. */
#define SC2_ALIAS_LINE (SC2_DEVICE_ID_HEX_LEN + SC2_DEVICE_ID_HEX_LEN + 4u)
_Static_assert(SOLAR_OS_SC2_ALIAS_NAME_MAX <= SC2_DEVICE_ID_HEX_LEN, "alias records reuse the verified line size");

/* Lower-cased copy of an all-hex string; its length, or 0 if not hex/too long. */
static size_t ref_hex(const char *in, char *out, size_t cap)
{
    size_t n = 0;
    for (; in[n] != '\0'; n++) {
        char c = in[n];
        if (c >= 'A' && c <= 'F') {
            c = (char)(c - 'A' + 'a');
        }
        if (n + 1u >= cap || !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return 0;
        }
        out[n] = c;
    }
    out[n] = '\0';
    return n;
}

#define SC2_REC_ALIAS "alias"
#define SC2_REC_VERIFIED "verified"
#define SC2_REC_NOTICE "notice"

static void alias_path(char *out, size_t cap, const char *stem, const char *ext)
{
    snprintf(out, cap, "%s/%s.%s", s_sc2.base, stem, ext);
}

/* Split "<name> <device>" in place; false for a malformed line. */
static bool alias_split(char *line, char **dev)
{
    char *sp = strchr(line, ' ');
    if (sp == NULL) {
        return false;
    }
    *sp = '\0';
    *dev = sp + 1;
    (*dev)[strcspn(*dev, "\r\n")] = '\0';
    return strlen(*dev) == SC2_DEVICE_ID_HEX_LEN;
}

/* Caller holds the service lock. index < 0 looks name up instead. */
static bool alias_read(const char *stem, const char *name, int index, char *name_out,
                       char dev_out[SC2_DEVICE_ID_HEX_LEN + 1])
{
    char path[SC2_PATH_MAX];
    char line[SC2_ALIAS_LINE];
    char *dev;
    bool found = false;
    alias_path(path, sizeof(path), stem, "txt");
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return false;
    }
    while (!found && fgets(line, sizeof(line), f) != NULL) {
        if (!alias_split(line, &dev)) {
            continue;
        }
        found = index < 0 ? strcmp(line, name) == 0 : index-- == 0;
        if (found) {
            memcpy(dev_out, dev, SC2_DEVICE_ID_HEX_LEN + 1u);
            if (name_out != NULL) {
                strlcpy(name_out, line, SOLAR_OS_SC2_ALIAS_NAME_MAX + 1u);
            }
        }
    }
    fclose(f);
    return found;
}

/* Count stored records <dir>/<id>.a|b whose ID starts with prefix: 0, 1 or 2 (= several). */
static int prefix_scan(const char *dir, const char *prefix, size_t len, char out[SC2_DEVICE_ID_HEX_LEN + 1],
                       int found)
{
    char path[SC2_PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", s_sc2.base, dir);
    DIR *d = opendir(path);
    if (d == NULL) {
        return found;
    }
    struct dirent *ent;
    char id[SC2_DEVICE_ID_HEX_LEN + 1];
    while (found < 2 && (ent = readdir(d)) != NULL) {
        if (strlen(ent->d_name) != SC2_DEVICE_ID_HEX_LEN + 2u || ent->d_name[SC2_DEVICE_ID_HEX_LEN] != '.') {
            continue;
        }
        memcpy(id, ent->d_name, SC2_DEVICE_ID_HEX_LEN);
        id[SC2_DEVICE_ID_HEX_LEN] = '\0';
        if (ref_hex(id, id, sizeof(id)) != SC2_DEVICE_ID_HEX_LEN || strncmp(id, prefix, len) != 0) {
            continue;
        }
        if (found == 0) {
            memcpy(out, id, sizeof(id));
            found = 1;
        } else if (strcmp(out, id) != 0) {
            found = 2;
        }
    }
    closedir(d);
    return found;
}

esp_err_t solar_os_sc2_resolve_peer(const char *ref, char out[SC2_DEVICE_ID_HEX_LEN + 1])
{
    if (ref == NULL || out == NULL || ref[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    char hex[SC2_DEVICE_ID_HEX_LEN + 1];
    size_t n = ref_hex(ref, hex, sizeof(hex));
    if (n == SC2_DEVICE_ID_HEX_LEN) {
        memcpy(out, hex, sizeof(hex));
        return ESP_OK;
    }
    esp_err_t err = sc2_enter();
    if (err != ESP_OK) {
        return err;
    }
    if (alias_read(SC2_REC_ALIAS, ref, -1, NULL, out)) {
        err = ESP_OK;
    } else if (n >= SOLAR_OS_SC2_PREFIX_MIN) {
        int found = prefix_scan(SC2E_DIR_PEER, hex, n, out, 0);
        found = prefix_scan(SC2E_DIR_DM, hex, n, out, found);
        err = found == 1 ? ESP_OK : (found == 2 ? ESP_ERR_INVALID_STATE : ESP_ERR_NOT_FOUND);
    } else {
        err = n > 0 ? ESP_ERR_INVALID_SIZE : ESP_ERR_NOT_FOUND;
    }
    sc2_leave();
    return err;
}

const char *solar_os_sc2_peer_ref_error(esp_err_t err)
{
    switch (err) {
    case ESP_ERR_NOT_FOUND:
        return "no known peer or alias by that name (give the 32-hex device id, a unique prefix of a known peer, "
               "or save one with `sc2 alias <name> <device-id>`)";
    case ESP_ERR_INVALID_STATE:
        return "prefix matches more than one known peer; give more hex digits";
    case ESP_ERR_INVALID_SIZE:
        return "device id prefix too short (need at least 6 hex digits)";
    default:
        return solar_os_sc2_err_text(err);
    }
}

bool solar_os_sc2_peer_next(const char *after, char out[SC2_DEVICE_ID_HEX_LEN + 1])
{
    if (!after || !out || sc2_enter() != ESP_OK) return false;
    char best[SC2_DEVICE_ID_HEX_LEN + 1] = "";
    const char *dirs[] = {SC2E_DIR_PEER, SC2E_DIR_DM};
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        char path[SC2_PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", s_sc2.base, dirs[i]);
        DIR *dir = opendir(path);
        if (!dir) continue;
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (strlen(entry->d_name) != SC2_DEVICE_ID_HEX_LEN + 2u ||
                entry->d_name[SC2_DEVICE_ID_HEX_LEN] != '.' ||
                (entry->d_name[SC2_DEVICE_ID_HEX_LEN + 1] != 'a' &&
                 entry->d_name[SC2_DEVICE_ID_HEX_LEN + 1] != 'b')) continue;
            char id[SC2_DEVICE_ID_HEX_LEN + 1];
            memcpy(id, entry->d_name, SC2_DEVICE_ID_HEX_LEN); id[SC2_DEVICE_ID_HEX_LEN] = 0;
            if (ref_hex(id, id, sizeof(id)) == SC2_DEVICE_ID_HEX_LEN && strcmp(id, after) > 0 &&
                (!best[0] || strcmp(id, best) < 0)) memcpy(best, id, sizeof(best));
        }
        closedir(dir);
    }
    sc2_leave();
    if (!best[0]) return false;
    memcpy(out, best, sizeof(best));
    return true;
}

bool solar_os_sc2_alias_name_valid(const char *name)
{
    size_t n = name != NULL ? strlen(name) : 0;
    bool hex = true;
    if (n == 0 || n > SOLAR_OS_SC2_ALIAS_NAME_MAX) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        bool digit = c >= '0' && c <= '9';
        bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        if (!digit && !alpha && c != '-' && c != '_' && c != '.') {
            return false;
        }
        if (!digit && !((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            hex = false;
        }
    }
    return !hex; /* an all-hex name would shadow device id prefixes */
}

esp_err_t solar_os_sc2_alias_get(size_t index, char name[SOLAR_OS_SC2_ALIAS_NAME_MAX + 1],
                                 char device[SC2_DEVICE_ID_HEX_LEN + 1])
{
    if (name == NULL || device == NULL || index > (size_t)SOLAR_OS_SC2_ALIAS_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = sc2_enter();
    if (err != ESP_OK) {
        return err;
    }
    bool found = alias_read(SC2_REC_ALIAS, NULL, (int)index, name, device);
    sc2_leave();
    return found ? ESP_OK : ESP_ERR_NOT_FOUND;
}

/* Caller holds the service lock. Replace (or with hex NULL delete) the
 * record `name` of file `stem`, keeping at most max records. */
static esp_err_t rec_set(const char *stem, size_t max, const char *name, const char *hex)
{
    esp_err_t err;
    char path[SC2_PATH_MAX];
    char tmp[SC2_PATH_MAX];
    char bak[SC2_PATH_MAX];
    char line[SC2_ALIAS_LINE];
    char *dev;
    size_t kept = 0;
    bool removed = false;
    alias_path(path, sizeof(path), stem, "txt");
    alias_path(tmp, sizeof(tmp), stem, "tmp");
    alias_path(bak, sizeof(bak), stem, "bak");
    FILE *in = fopen(path, "r");
    FILE *out = fopen(tmp, "w");
    err = out != NULL ? ESP_OK : ESP_FAIL;
    while (err == ESP_OK && in != NULL && fgets(line, sizeof(line), in) != NULL) {
        if (!alias_split(line, &dev)) {
            continue;
        }
        if (strcmp(line, name) == 0) {
            removed = true;
        } else if (fprintf(out, "%s %s\n", line, dev) < 0) {
            err = ESP_FAIL;
        } else {
            kept++;
        }
    }
    if (in != NULL) {
        fclose(in);
    }
    if (err == ESP_OK && hex != NULL) {
        if (kept >= max) {
            err = ESP_ERR_NO_MEM;
        } else if (fprintf(out, "%s %s\n", name, hex) < 0) {
            err = ESP_FAIL;
        }
    } else if (err == ESP_OK && !removed) {
        err = ESP_ERR_NOT_FOUND;
    }
    if (out != NULL) {
        if (err == ESP_OK && solar_os_storage_sync_file(out) != ESP_OK) {
            err = ESP_FAIL;
        }
        fclose(out);
    }
    if (err == ESP_OK) {
        err = solar_os_storage_replace_file(tmp, path, bak);
    } else {
        (void)remove(tmp);
    }
    return err;
}

esp_err_t solar_os_sc2_alias_set(const char *name, const char *device)
{
    char hex[SC2_DEVICE_ID_HEX_LEN + 1];
    if (!solar_os_sc2_alias_name_valid(name) ||
        (device != NULL && ref_hex(device, hex, sizeof(hex)) != SC2_DEVICE_ID_HEX_LEN)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = sc2_enter();
    if (err != ESP_OK) {
        return err;
    }
    err = rec_set(SC2_REC_ALIAS, SOLAR_OS_SC2_ALIAS_MAX, name, device != NULL ? hex : NULL);
    sc2_leave();
    return err;
}

/* A verified record names the own device that verified the peer, so a new
 * identity (store wiped or restored elsewhere) starts unverified. The peer
 * key itself cannot change under its device ID (the ID hashes the key). */
esp_err_t solar_os_sc2_verified_get(const char *device, bool *verified)
{
    char hex[SC2_DEVICE_ID_HEX_LEN + 1];
    char by[SC2_DEVICE_ID_HEX_LEN + 1];
    if (device == NULL || verified == NULL || ref_hex(device, hex, sizeof(hex)) != SC2_DEVICE_ID_HEX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = sc2_enter();
    if (err != ESP_OK) {
        return err;
    }
    *verified = alias_read(SC2_REC_VERIFIED, hex, -1, NULL, by) && strcmp(by, s_sc2.engine->self_hex) == 0;
    sc2_leave();
    return ESP_OK;
}

esp_err_t solar_os_sc2_verified_set(const char *device, bool verified)
{
    char hex[SC2_DEVICE_ID_HEX_LEN + 1];
    if (device == NULL || ref_hex(device, hex, sizeof(hex)) != SC2_DEVICE_ID_HEX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = sc2_enter();
    if (err != ESP_OK) {
        return err;
    }
    sc2e_peer_info_t info;
    if (verified && (sc2e_peer_info(s_sc2.engine, hex, &info) != SC2_OK || !info.known)) {
        err = ESP_ERR_NOT_FOUND; /* nothing to compare yet: fetch the keys first */
    } else {
        err = rec_set(SC2_REC_VERIFIED, SOLAR_OS_SC2_VERIFIED_MAX, hex, verified ? s_sc2.engine->self_hex : NULL);
        if (!verified && err == ESP_ERR_NOT_FOUND) {
            err = ESP_OK;
        }
    }
    sc2_leave();
    return err;
}

/* First-join notices: the record key is a hash of the room key, so any
 * room name fits the record line. */
static void notice_key(const char *room, char out[SC2_DEVICE_ID_HEX_LEN + 1])
{
    uint8_t h[SC2_SHA256_LEN];
    if (sc2_sha256(h, (const uint8_t *)room, strlen(room)) != SC2_OK) {
        memset(h, 0, sizeof(h));
    }
    for (size_t i = 0; i < SC2_DEVICE_ID_HEX_LEN / 2u; i++) {
        snprintf(out + 2u * i, 3, "%02x", h[i]);
    }
}

bool solar_os_sc2_notice_seen(const char *room)
{
    char key[SC2_DEVICE_ID_HEX_LEN + 1];
    char by[SC2_DEVICE_ID_HEX_LEN + 1];
    if (room == NULL || sc2_enter() != ESP_OK) {
        return false; /* any doubt: show the notice */
    }
    notice_key(room, key);
    bool seen = alias_read(SC2_REC_NOTICE, key, -1, NULL, by);
    sc2_leave();
    return seen;
}

esp_err_t solar_os_sc2_notice_mark(const char *room)
{
    char key[SC2_DEVICE_ID_HEX_LEN + 1];
    if (room == NULL || room[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = sc2_enter();
    if (err != ESP_OK) {
        return err;
    }
    notice_key(room, key);
    err = rec_set(SC2_REC_NOTICE, SOLAR_OS_SC2_NOTICE_MAX, key, s_sc2.engine->self_hex);
    sc2_leave();
    return err;
}

esp_err_t solar_os_sc2_keys_publish(const char *rid, uint32_t opk_target, char *out, size_t cap, size_t *out_len)
{
    SC2_CALL(sc2e_keys_publish(s_sc2.engine, rid, opk_target, out, cap, out_len));
}

esp_err_t solar_os_sc2_add_bundle(const char *line, size_t len, char device[SC2_DEVICE_ID_HEX_LEN + 1])
{
    SC2_CALL(sc2e_add_bundle(s_sc2.engine, line, len, device));
}

esp_err_t solar_os_sc2_dm_encrypt(const char *peer, const char *rid, const char *inner, size_t inner_len, char *out,
                                  size_t cap, size_t *out_len)
{
    SC2_CALL(sc2e_dm_encrypt(s_sc2.engine, peer, rid, inner, inner_len, out, cap, out_len));
}

esp_err_t solar_os_sc2_dm_decrypt(const char *line, size_t len, sc2e_dm_result_t *result, char *pt, size_t pt_cap,
                                  size_t *pt_len)
{
    SC2_CALL(sc2e_dm_decrypt(s_sc2.engine, line, len, result, pt, pt_cap, pt_len));
}

esp_err_t solar_os_sc2_room_create(const char *room, char room_out[SC2_DEVICE_ID_HEX_LEN + 1], uint32_t *epoch)
{
    SC2_CALL(sc2e_room_create(s_sc2.engine, room, room_out, epoch));
}

esp_err_t solar_os_sc2_room_rotate(const char *room, uint32_t *epoch)
{
    SC2_CALL(sc2e_room_rotate(s_sc2.engine, room, epoch));
}

esp_err_t solar_os_sc2_room_share(const char *room, const char *peer, const char *rid, char *out, size_t cap,
                                  size_t *out_len)
{
    SC2_CALL(sc2e_room_share(s_sc2.engine, room, peer, rid, out, cap, out_len));
}

esp_err_t solar_os_sc2_room_encrypt(const char *room, const char *rid, const char *inner, size_t inner_len, char *out,
                                    size_t cap, size_t *out_len)
{
    SC2_CALL(sc2e_room_encrypt(s_sc2.engine, room, rid, inner, inner_len, out, cap, out_len));
}

esp_err_t solar_os_sc2_room_decrypt(const char *line, size_t len, sc2e_room_result_t *result, char *pt, size_t pt_cap,
                                    size_t *pt_len)
{
    SC2_CALL(sc2e_room_decrypt(s_sc2.engine, line, len, result, pt, pt_cap, pt_len));
}

esp_err_t solar_os_sc2_room_info(const char *room, sc2e_room_info_t *info)
{
    SC2_CALL(sc2e_room_info(s_sc2.engine, room, info));
}

esp_err_t solar_os_sc2_room_forget(const char *room)
{
    SC2_CALL(sc2e_room_forget(s_sc2.engine, room));
}

esp_err_t solar_os_sc2_post_sign(const char *relay_id, const char *room, const char *name, const char *body,
                                 size_t body_len, uint64_t ts, const char *ref, bool deleted, const char *rid,
                                 char *out, size_t cap, size_t *out_len)
{
    SC2_CALL(sc2e_post_sign(s_sc2.engine, relay_id, room, name, body, body_len, ts, ref, deleted, rid, out, cap,
                            out_len));
}

esp_err_t solar_os_sc2_post_verify(const char *relay_id, const char *line, size_t len, size_t max_body,
                                   sc2e_post_result_t *result, char *body, size_t body_cap)
{
    return sc2_esp(sc2e_post_verify(relay_id, line, len, max_body, result, body, body_cap));
}

static sc2e_test_host_t test_host(void)
{
    sc2e_test_host_t host = {
        .alloc = pf_alloc,
        .release = pf_release,
        .clock_us = host_clock_us,
        .rng = SC2_RNG,
        .ctx = NULL,
    };
    return host;
}

esp_err_t solar_os_sc2_selftest(sc2e_report_fn report, void *ctx, unsigned *passed, unsigned *failed)
{
    /* The selftest uses its own in-memory devices; only the RNG pool is
     * shared, and it has its own lock. It still needs the pool keyed. */
    esp_err_t err = solar_os_sc2_init();
    if (err != ESP_OK) {
        return err;
    }
    sc2e_test_host_t host = test_host();
    return sc2_esp(sc2e_selftest(&host, report, ctx, passed, failed));
}

esp_err_t solar_os_sc2_bench(uint32_t iterations, sc2e_bench_fn report, void *ctx)
{
    esp_err_t err = solar_os_sc2_init();
    if (err != ESP_OK) {
        return err;
    }
    sc2e_test_host_t host = test_host();
    return sc2_esp(sc2e_bench(&host, iterations, report, ctx));
}

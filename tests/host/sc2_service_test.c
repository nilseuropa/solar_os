/*
 * Host tests for the SolarOS SC2 service (src/services/solar_os_sc2.c) on a
 * real host directory: peer aliases, safety-number verification records and
 * the public-room first-join notice store. A second engine in memory plays
 * the peer. SPDX-License-Identifier: MIT
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <errno.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "esp_random.h"
#include "esp_timer.h"
#include "solar_os_memory.h"
#include "solar_os_sc2.h"
#include "solar_os_sc2_client.h"
#include "solar_os_storage.h"

/* ---------------- host stand-ins for the SolarOS platform ---------------- */

static char root[64];

uint32_t esp_random(void)
{
    uint32_t v;
    esp_fill_random(&v, sizeof(v));
    return v;
}
void esp_fill_random(void *buf, size_t len)
{
    FILE *f = fopen("/dev/urandom", "rb");
    assert(f != NULL && fread(buf, 1, len, f) == len);
    fclose(f);
}
int64_t esp_timer_get_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
const char *esp_err_to_name(esp_err_t err)
{
    static char buf[16];
    snprintf(buf, sizeof(buf), "0x%x", (unsigned)err);
    return buf;
}
void *solar_os_memory_alloc(size_t size, solar_os_memory_class_t memory_class, const char *tag)
{
    (void)memory_class, (void)tag;
    return malloc(size);
}
void *solar_os_memory_calloc(size_t count, size_t size, solar_os_memory_class_t memory_class, const char *tag)
{
    (void)memory_class, (void)tag;
    return calloc(count, size);
}
void solar_os_memory_free(void *ptr) { free(ptr); }
bool solar_os_memory_is_external(const void *ptr) { (void)ptr; return true; }
bool solar_os_storage_flash_is_mounted(void) { return true; }
const char *solar_os_storage_flash_mount_point(void) { return root; }
esp_err_t solar_os_storage_join_path(const char *base, const char *rel, char *path, size_t len)
{
    return (size_t)snprintf(path, len, "%s/%s", base, rel) < len ? ESP_OK : ESP_ERR_INVALID_SIZE;
}
esp_err_t solar_os_storage_exists(const char *path, bool *exists)
{
    struct stat st;
    *exists = stat(path, &st) == 0;
    return ESP_OK;
}
esp_err_t solar_os_storage_makedirs(const char *path, bool exist_ok)
{
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            (void)mkdir(tmp, 0700);
            *p = '/';
        }
    }
    return mkdir(tmp, 0700) == 0 || (exist_ok && errno == EEXIST) ? ESP_OK : ESP_FAIL;
}
esp_err_t solar_os_storage_remove(const char *path) { return remove(path) == 0 ? ESP_OK : ESP_ERR_NOT_FOUND; }
esp_err_t solar_os_storage_sync_file(FILE *file) { return fflush(file) == 0 ? ESP_OK : ESP_FAIL; }
esp_err_t solar_os_storage_replace_file(const char *staged, const char *active, const char *backup)
{
    (void)backup;
    return rename(staged, active) == 0 ? ESP_OK : ESP_FAIL;
}
esp_err_t solar_os_storage_read_file(const char *path, void *buffer, size_t buffer_len, size_t *read_len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    *read_len = fread(buffer, 1, buffer_len, f);
    fclose(f);
    return ESP_OK;
}
esp_err_t solar_os_storage_write_file(const char *path, const void *data, size_t len, bool append)
{
    FILE *f = fopen(path, append ? "ab" : "wb");
    if (f == NULL) {
        return ESP_FAIL;
    }
    size_t n = fwrite(data, 1, len, f);
    fclose(f);
    return n == len ? ESP_OK : ESP_FAIL;
}

/* ---------------- a peer: second engine in memory ---------------- */

static void *t_alloc(void *ctx, size_t len) { (void)ctx; return calloc(1, len); }
static void t_release(void *ctx, void *ptr, size_t len) { (void)ctx; if (ptr) { memset(ptr, 0, len); free(ptr); } }
static uint64_t t_clock(void *ctx) { (void)ctx; return (uint64_t)esp_timer_get_time(); }
static int t_rng(void *ctx, uint8_t *out, size_t len) { (void)ctx; esp_fill_random(out, len); return SC2_OK; }

typedef struct {
    sc2e_memstore_t *store;
    sc2e_t e;
    sc2e_identity_info_t info;
} peer_t;

static char f1[SC2E_FRAME_MAX], f2[SC2E_FRAME_MAX];

static void peer_open(peer_t *p)
{
    sc2e_test_host_t h = {t_alloc, t_release, t_clock, {t_rng, NULL}, NULL};
    sc2e_platform_t pf;
    assert(sc2e_memstore_create(&p->store, &h) == SC2_OK);
    sc2e_memstore_platform(p->store, &pf);
    assert(sc2e_open(&p->e, &pf) == SC2_OK);
    assert(sc2e_identity(&p->e, &p->info) == SC2_OK);
}

/* Hand the peer's bundle to the service, as sc2-sync does after keys.claim. */
static void peer_give(peer_t *p)
{
    size_t n = 0, m = 0;
    char dev[SC2_DEVICE_ID_HEX_LEN + 1];
    assert(sc2e_keys_publish(&p->e, "k", 2, f1, sizeof(f1), &n) == SC2_OK);
    assert(sc2e_relay_bundle(f1, n, &p->info, f2, sizeof(f2), &m) == SC2_OK);
    assert(solar_os_sc2_add_bundle(f2, m, dev) == ESP_OK);
    assert(strcmp(dev, p->info.device_id) == 0);
}

static int rm_entry(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    (void)st, (void)flag, (void)ftw;
    return remove(path);
}

static bool file_has(const char *name, const char *text)
{
    char path[256], buf[4096] = {0};
    snprintf(path, sizeof(path), "%s/%s/%s", root, SOLAR_OS_SC2_STORE_DIR, name);
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return false;
    }
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return strstr(buf, text) != NULL;
}

/* ---------------- tests ---------------- */

static void test_aliases(const peer_t *b)
{
    char out[SC2_DEVICE_ID_HEX_LEN + 1], name[SOLAR_OS_SC2_ALIAS_NAME_MAX + 1];
    assert(solar_os_sc2_alias_set("bob", b->info.device_id) == ESP_OK);
    assert(solar_os_sc2_resolve_peer("bob", out) == ESP_OK && strcmp(out, b->info.device_id) == 0);
    char prefix[8];
    memcpy(prefix, b->info.device_id, 6);
    prefix[6] = '\0';
    assert(solar_os_sc2_resolve_peer(prefix, out) == ESP_OK && strcmp(out, b->info.device_id) == 0);
    assert(solar_os_sc2_alias_get(0, name, out) == ESP_OK && strcmp(name, "bob") == 0);
    assert(solar_os_sc2_alias_set("cafe", b->info.device_id) == ESP_ERR_INVALID_ARG); /* all hex */
    assert(solar_os_sc2_alias_set("bob", NULL) == ESP_OK);
    assert(solar_os_sc2_alias_set("bob", NULL) == ESP_ERR_NOT_FOUND);
    assert(solar_os_sc2_resolve_peer("bob", out) == ESP_ERR_NOT_FOUND);
}

static void test_verified(const peer_t *b)
{
    const char *unknown = "00112233445566778899aabbccddeeff";
    bool v = true;
    assert(solar_os_sc2_verified_get(b->info.device_id, &v) == ESP_OK && !v);
    assert(solar_os_sc2_verified_set(unknown, true) == ESP_ERR_NOT_FOUND); /* no keys to compare */
    assert(solar_os_sc2_verified_set(unknown, false) == ESP_OK);          /* no-op */
    assert(solar_os_sc2_verified_set("xyz", true) == ESP_ERR_INVALID_ARG);
    assert(solar_os_sc2_verified_set(b->info.device_id, true) == ESP_OK);
    assert(solar_os_sc2_verified_set(b->info.device_id, true) == ESP_OK); /* idempotent */
    assert(solar_os_sc2_verified_get(b->info.device_id, &v) == ESP_OK && v);
    assert(file_has("verified.txt", b->info.device_id));
    /* upper case device IDs name the same peer */
    char upper[SC2_DEVICE_ID_HEX_LEN + 1];
    for (size_t i = 0; i <= SC2_DEVICE_ID_HEX_LEN; i++) {
        upper[i] = (char)((b->info.device_id[i] >= 'a' && b->info.device_id[i] <= 'f') ? b->info.device_id[i] - 32
                                                                                        : b->info.device_id[i]);
    }
    assert(solar_os_sc2_verified_get(upper, &v) == ESP_OK && v);
    /* the safety number the screen shows */
    sc2e_safety_t mine;
    assert(solar_os_sc2_safety(b->info.device_id, &mine) == ESP_OK);
    assert(strlen(mine.short_form) == SC2_SAFETY_SHORT_LEN && strlen(mine.full_form) == SC2_SAFETY_FULL_LEN);
    assert(solar_os_sc2_verified_set(b->info.device_id, false) == ESP_OK);
    assert(solar_os_sc2_verified_get(b->info.device_id, &v) == ESP_OK && !v);
    assert(!file_has("verified.txt", b->info.device_id));
}

static void test_notice(void)
{
    assert(!solar_os_sc2_notice_seen("pub:general"));
    assert(solar_os_sc2_notice_mark("pub:general") == ESP_OK);
    assert(solar_os_sc2_notice_seen("pub:general"));
    assert(!solar_os_sc2_notice_seen("pub:other"));
    assert(solar_os_sc2_notice_mark("") == ESP_ERR_INVALID_ARG);
    /* a broken store line is ignored, never counts as seen */
    char path[256];
    snprintf(path, sizeof(path), "%s/%s/notice.txt", root, SOLAR_OS_SC2_STORE_DIR);
    FILE *f = fopen(path, "a");
    assert(f != NULL);
    fputs("garbage\n", f);
    fclose(f);
    assert(solar_os_sc2_notice_seen("pub:general") && !solar_os_sc2_notice_seen("garbage"));
    assert(solar_os_sc2_notice_mark("pub:third") == ESP_OK && solar_os_sc2_notice_seen("pub:third"));
}

static unsigned sent_dms;
static char saved_dm[SC2E_FRAME_MAX];
static int client_send(void *ctx, const char *line, size_t len)
{
    (void)ctx;
    sc2_json_val_t root_value;
    char type[SC2_TYPE_MAX + 1], rid[SC2_RID_MAX + 1];
    assert(sc2_frame_peek(line, len, &root_value, type, rid) == SC2_OK);
    if (!strcmp(type, "dm.send")) {
        if (sent_dms++) assert(strlen(saved_dm) == len && !memcmp(saved_dm, line, len));
        else { assert(len < sizeof(saved_dm)); memcpy(saved_dm, line, len); saved_dm[len] = 0; }
    }
    return 0;
}
static sc2e_t *client_lock(void *ctx) { (void)ctx; return solar_os_sc2_engine_lock(); }
static void client_unlock(void *ctx) { (void)ctx; solar_os_sc2_engine_unlock(); }
static uint64_t client_ms(void *ctx) { return t_clock(ctx) / 1000; }

static void test_file_backed_outbox(const peer_t *peer)
{
    sc2c_host_t host = {.send = client_send, .engine_lock = client_lock, .engine_unlock = client_unlock,
        .alloc = t_alloc, .release = t_release, .mono_ms = client_ms};
    sc2c_config_t config = {.tag = "disk", .nick = "disk client"};
    sc2c_t client;
    assert(sc2c_init(&client, &host, &config) == SC2_OK);
    client.state = SC2C_READY;
    assert(sc2c_submit(&client, SC2C_DM, peer->info.device_id, "persisted", 9, 42, 123456) == SC2_OK);
    assert(sent_dms == 1 && sc2c_outbox_count(&client) == 1);
    sc2c_free(&client);
    assert(sc2c_init(&client, &host, &config) == SC2_OK);
    assert(sc2c_outbox_count(&client) == 1 && client.slots[0].token == 42 &&
           client.slots[0].token_key == 123456 && !strcmp(client.slots[0].target, peer->info.device_id));
    client.state = SC2C_AUTH;
    const char authed[] = "{\"type\":\"authed\"}\n";
    assert(sc2c_feed(&client, (const uint8_t *)authed, sizeof(authed) - 1) == SC2_OK);
    assert(sent_dms == 2); /* Replay the identical sealed frame, without re-encryption. */
    const char ack[] = "{\"type\":\"ack\",\"rid\":\"o0\",\"seq\":\"1\"}\n";
    assert(sc2c_feed(&client, (const uint8_t *)ack, sizeof(ack) - 1) == SC2_OK);
    assert(sc2c_outbox_count(&client) == 0);
    sc2c_free(&client);
}

int main(void)
{
    snprintf(root, sizeof(root), "/tmp/sc2_service_test.%d", (int)getpid());
    assert(mkdir(root, 0700) == 0);
    assert(solar_os_sc2_init() == ESP_OK);
    peer_t b;
    peer_open(&b);
    peer_give(&b);
    test_aliases(&b);
    test_verified(&b);
    test_notice();
    test_file_backed_outbox(&b);
    char peer_id[SC2_DEVICE_ID_HEX_LEN + 1], next_peer[SC2_DEVICE_ID_HEX_LEN + 1];
    assert(solar_os_sc2_peer_next("", peer_id) && !strcmp(peer_id, b.info.device_id));
    assert(!solar_os_sc2_peer_next(peer_id, next_peer)); /* De-duplicates both slots and peer/session directories. */
    sc2e_close(&b.e);
    sc2e_memstore_destroy(b.store);
    nftw(root, rm_entry, 8, FTW_DEPTH | FTW_PHYS);
    printf("sc2_service_test: aliases, verification, notices and file-backed outbox replay OK\n");
    return 0;
}

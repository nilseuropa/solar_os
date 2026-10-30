/* "sc2" Messaging provider class on the real Messaging registry and on-disk
 * profiles (host filesystem), with the SC2 key store stubbed out. */
#define main messaging_existing_tests_main
#include "messaging_service_test.c"
#undef main
#include "../../src/services/solar_os_sc2_provider.c"

#include <ftw.h>

#define PIN_A "sha256/AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="
#define DEVICE_UPPER "00112233445566778899AABBCCDDEEFF"
#define DEVICE_LOWER "00112233445566778899aabbccddeeff"
#define ROOM_HEX "0F0E0D0C0B0A09080706050403020100"

static bool peer_known;
esp_err_t solar_os_sc2_peer_info(const char *peer, sc2e_peer_info_t *info)
{
    memset(info, 0, sizeof(*info));
    info->known = peer_known && strcmp(peer, DEVICE_LOWER) == 0;
    return ESP_OK;
}

static void restart_provider(void)
{
    while (sc2p.relays) {
        sc2p_relay_t *next = sc2p.relays->next;
        free(sc2p.relays); sc2p.relays = next;
    }
    vSemaphoreDelete(sc2p.lock); vSemaphoreDelete(sc2p.edit_lock);
    memset(&sc2p, 0, sizeof(sc2p));
    restart_messages();
    assert(solar_os_sc2_provider_init() == ESP_OK);
}

static void test_parsing(void)
{
    solar_os_sc2_relay_config_t c;
    memset(&c, 0, sizeof(c));
    assert(solar_os_sc2_relay_parse_url("chats://relay.example:7443", &c) == ESP_OK);
    assert(c.tls && c.port == 7443 && !strcmp(c.url, "chats://relay.example:7443") && !c.pin[0]);
    assert(solar_os_sc2_relay_parse_url("chats://relay.example:7778#pin=" PIN_A, &c) == ESP_OK);
    assert(c.tls && c.port == 7778 && !strcmp(c.url, "chats://relay.example:7778") && !strcmp(c.pin, PIN_A));
    assert(solar_os_sc2_relay_parse_url("chat://10.0.0.2:7777", &c) == ESP_OK && !c.tls && !c.pin[0]);
    const char *bad[] = {
        "chat://10.0.0.2:7777#pin=" PIN_A,          /* pin needs TLS */
        "chats://relay.example",                    /* port required */
        "chats://relay.example:0", "chats://relay.example:65536", "chats://:7443",
        "https://relay.example:443", "chats://relay.example:7443/x", "chats://-x:1",
        "chats://relay.example:7443#pin=sha256/short", "chats://relay.example:7443#pin=md5/AAAA",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        assert(solar_os_sc2_relay_parse_url(bad[i], &c) != ESP_OK);

    char key[SOLAR_OS_MESSAGING_PROVIDER_KEY_MAX];
    assert(solar_os_sc2_target_normalize("PUB:Solar-Dev", true, key, sizeof(key)) == SOLAR_OS_SC2_TARGET_PUBLIC_ROOM);
    assert(!strcmp(key, "pub:solar-dev"));
    assert(solar_os_sc2_target_normalize("pub:solar dev", true, key, sizeof(key)) == SOLAR_OS_SC2_TARGET_INVALID);
    assert(solar_os_sc2_target_normalize("pub:", true, key, sizeof(key)) == SOLAR_OS_SC2_TARGET_INVALID);
    assert(solar_os_sc2_target_normalize("pub:general", false, key, sizeof(key)) == SOLAR_OS_SC2_TARGET_INVALID);
    char long_name[80] = "pub:";
    memset(long_name + 4, 'a', SOLAR_OS_SC2_PUBLIC_ROOM_NAME_MAX); long_name[4 + SOLAR_OS_SC2_PUBLIC_ROOM_NAME_MAX] = 0;
    assert(solar_os_sc2_target_normalize(long_name, true, key, sizeof(key)) == SOLAR_OS_SC2_TARGET_PUBLIC_ROOM);
    strcat(long_name, "a");
    assert(solar_os_sc2_target_normalize(long_name, true, key, sizeof(key)) == SOLAR_OS_SC2_TARGET_INVALID);
    assert(solar_os_sc2_target_normalize(DEVICE_UPPER, false, key, sizeof(key)) == SOLAR_OS_SC2_TARGET_DEVICE);
    assert(!strcmp(key, DEVICE_LOWER));
    assert(solar_os_sc2_target_normalize(DEVICE_UPPER, true, key, sizeof(key)) == SOLAR_OS_SC2_TARGET_PRIVATE_ROOM);
    assert(solar_os_sc2_target_normalize("0011", false, key, sizeof(key)) == SOLAR_OS_SC2_TARGET_INVALID);
    assert(solar_os_sc2_target_normalize("#general", true, key, sizeof(key)) == SOLAR_OS_SC2_TARGET_INVALID);

    c.tls = true;
    assert(solar_os_sc2_conversation_security(&c, SOLAR_OS_SC2_TARGET_PUBLIC_ROOM, false) ==
           (SOLAR_OS_SECURITY_PUBLIC_SIGNED | SOLAR_OS_SECURITY_TRANSPORT_SECURED));
    c.tls = false;
    assert(solar_os_sc2_conversation_security(&c, SOLAR_OS_SC2_TARGET_PRIVATE_ROOM, false) ==
           (SOLAR_OS_SECURITY_ENCRYPTED | SOLAR_OS_SECURITY_SHARED_KEY));
    assert(solar_os_sc2_conversation_security(&c, SOLAR_OS_SC2_TARGET_DEVICE, true) ==
           (SOLAR_OS_SECURITY_ENCRYPTED | SOLAR_OS_SECURITY_PEER_KEY_KNOWN));
    assert((solar_os_sc2_conversation_security(&c, SOLAR_OS_SC2_TARGET_PUBLIC_ROOM, false) &
            (SOLAR_OS_SECURITY_ENCRYPTED | SOLAR_OS_SECURITY_LEGACY_INSECURE)) == 0);
}

static solar_os_messaging_conversation_t conversation_of(solar_os_conversation_id_t id)
{
    solar_os_messaging_conversation_t conversation;
    assert(solar_os_messaging_conversation_get(id, &conversation) == ESP_OK);
    return conversation;
}

static void test_registry(void)
{
    assert(solar_os_sc2_provider_init() == ESP_OK);
    assert(solar_os_sc2_provider_init() == ESP_OK);
    solar_os_messaging_class_status_t cls;
    bool listed = false;
    for (char after[16] = ""; solar_os_messaging_class_next(after, &cls); strlcpy(after, cls.name, sizeof(after)))
        if (!strcmp(cls.name, "sc2")) listed = cls.creatable &&
            cls.capabilities == (SOLAR_OS_ENDPOINT_CAP_DIRECT | SOLAR_OS_ENDPOINT_CAP_ROOM);
    assert(listed);

    solar_os_messaging_provider_id_t id = 0, other = 0;
    const char *bad_url[] = {"https://relay.example:443"};
    assert(solar_os_messaging_instance_add("sc2", "broken", 1, bad_url, &other) != ESP_OK);
    const char *too_many[] = {"chats://a.example:1", "code", "extra"};
    assert(solar_os_messaging_instance_add("sc2", "broken", 3, too_many, &other) != ESP_OK);
    const char *bad_admit[] = {"chats://a.example:1", "two words"};
    assert(solar_os_messaging_instance_add("sc2", "broken", 2, bad_admit, &other) != ESP_OK);
    const char *settings[] = {"chats://relay.example:7778#pin=" PIN_A, "invite-42"};
    assert(solar_os_messaging_instance_add("sc2", "relay1", 2, settings, &id) == ESP_OK && id != 0);
    assert(solar_os_messaging_provider_is_class(id, "sc2"));
    solar_os_sc2_relay_config_t config;
    assert(solar_os_sc2_relay_get_config(id, &config) == ESP_OK);
    assert(config.tls && !strcmp(config.pin, PIN_A) && !strcmp(config.admit, "invite-42") && !config.enabled);

    solar_os_conversation_id_t pub = 0, priv = 0, dm = 0;
    assert(solar_os_messaging_join(id, "PUB:General", &pub) == ESP_OK && pub);
    solar_os_messaging_conversation_t c = conversation_of(pub);
    assert(!strcmp(c.provider_key, "pub:general") && c.kind == SOLAR_OS_CONVERSATION_ROOM);
    assert(c.security_flags == (SOLAR_OS_SECURITY_PUBLIC_SIGNED | SOLAR_OS_SECURITY_TRANSPORT_SECURED));
    assert(c.join_state == SOLAR_OS_MESSAGING_JOIN_PENDING);
    assert(solar_os_messaging_join(id, ROOM_HEX, &priv) == ESP_OK && priv && priv != pub);
    c = conversation_of(priv);
    assert(c.security_flags == (SOLAR_OS_SECURITY_ENCRYPTED | SOLAR_OS_SECURITY_SHARED_KEY |
                                SOLAR_OS_SECURITY_TRANSPORT_SECURED));
    {
        char want[16];
        solar_os_sc2_room_title(want, sizeof(want), "0f0e0d0c0b0a09080706050403020100");
        assert(!strcmp(want, "room 0f0e0d0c") && !strcmp(c.title, want));
    }
    solar_os_conversation_id_t again = 0;
    assert(solar_os_messaging_join(id, "pub:general", &again) == ESP_OK && again == pub);
    assert(solar_os_messaging_join(id, "pub:bad name", NULL) != ESP_OK);

    peer_known = true;
    assert(solar_os_messaging_open_direct(id, DEVICE_UPPER, &dm) == ESP_OK && dm);
    c = conversation_of(dm);
    assert(c.kind == SOLAR_OS_CONVERSATION_DIRECT && !strcmp(c.provider_key, DEVICE_LOWER));
    assert(c.security_flags == (SOLAR_OS_SECURITY_ENCRYPTED | SOLAR_OS_SECURITY_PEER_KEY_KNOWN |
                                SOLAR_OS_SECURITY_TRANSPORT_SECURED));
    assert(solar_os_messaging_open_direct(id, "pub:general", NULL) != ESP_OK);

    /* Settings changes; pins need TLS; switching to chat:// drops TRANSPORT_SECURED. */
    const char *admit_off[] = {"admit", "off"};
    assert(solar_os_messaging_instance_configure(id, 2, admit_off) == ESP_OK);
    const char *lan[] = {"chat://lan.local:7777"};
    assert(solar_os_messaging_instance_configure(id, 1, lan) == ESP_OK);
    const char *pin[] = {"pin", PIN_A};
    assert(solar_os_messaging_instance_configure(id, 2, pin) != ESP_OK);
    const char *junk[] = {"colour", "blue"};
    assert(solar_os_messaging_instance_configure(id, 2, junk) != ESP_OK);
    assert(solar_os_sc2_relay_get_config(id, &config) == ESP_OK);
    assert(!config.tls && !config.pin[0] && !config.admit[0] && !strcmp(config.url, "chat://lan.local:7777"));
    assert(conversation_of(pub).security_flags == SOLAR_OS_SECURITY_PUBLIC_SIGNED);

    assert(solar_os_messaging_connect(id, true) == ESP_OK);
    solar_os_messaging_provider_status_t status;
    assert(solar_os_messaging_provider_get_status(id, &status) == ESP_OK);
    assert(!status.running && !status.connected && strstr(status.detail, "not in this build"));

    /* A reboot restores the instance, its settings and its rooms. */
    restart_provider();
    assert(solar_os_sc2_relay_get_config(id, &config) == ESP_OK);
    assert(config.enabled && !config.tls && !strcmp(config.rooms[0], "pub:general") &&
           !strcmp(config.rooms[1], "0f0e0d0c0b0a09080706050403020100"));
    solar_os_conversation_id_t restored = 0;
    assert(solar_os_messaging_join(id, "pub:general", &restored) == ESP_OK && restored == pub);

    assert(solar_os_messaging_leave(priv) == ESP_OK);
    assert(solar_os_sc2_relay_get_config(id, &config) == ESP_OK);
    for (size_t i = 0; i < SOLAR_OS_SC2_RELAY_ROOM_CAPACITY; i++)
        assert(strcmp(config.rooms[i], "0f0e0d0c0b0a09080706050403020100") != 0);
    assert(conversation_of(priv).join_state == SOLAR_OS_MESSAGING_JOIN_LEFT);

    /* Room list capacity. */
    char name[16];
    size_t joined = 1;
    for (; joined < SOLAR_OS_SC2_RELAY_ROOM_CAPACITY; joined++) {
        snprintf(name, sizeof(name), "pub:r%zu", joined);
        assert(solar_os_messaging_join(id, name, NULL) == ESP_OK);
    }
    assert(solar_os_messaging_join(id, "pub:one-too-many", NULL) == ESP_ERR_NO_MEM);

    /* Independent second relay. */
    const char *second[] = {"chats://other.example:7443"};
    assert(solar_os_messaging_instance_add("sc2", "relay2", 1, second, &other) == ESP_OK && other != id);
    solar_os_conversation_id_t pub2 = 0;
    assert(solar_os_messaging_join(other, "pub:general", &pub2) == ESP_OK && pub2 != pub);

    assert(solar_os_messaging_instance_remove(id) == ESP_OK);
    assert(solar_os_sc2_relay_get_config(id, &config) == ESP_ERR_NOT_FOUND);
    assert(solar_os_messaging_conversation_get(pub, &c) == ESP_ERR_NOT_FOUND);
    assert(solar_os_sc2_relay_get_config(other, &config) == ESP_OK && config.tls);
    restart_provider();
    assert(solar_os_sc2_relay_get_config(other, &config) == ESP_OK && !strcmp(config.rooms[0], "pub:general"));
    assert(solar_os_messaging_instance_remove(other) == ESP_OK);
}

/* Fallback relays: parse, replace, off, TLS flag, persistence, v1 profiles. */
static void test_fallbacks(void)
{
    solar_os_messaging_provider_id_t id = 0;
    const char *settings[] = {"chats://primary.example:7778"};
    assert(solar_os_messaging_instance_add("sc2", "multi", 1, settings, &id) == ESP_OK);
    solar_os_conversation_id_t pub = 0;
    assert(solar_os_messaging_join(id, "pub:general", &pub) == ESP_OK);
    assert(conversation_of(pub).security_flags & SOLAR_OS_SECURITY_TRANSPORT_SECURED);

    solar_os_sc2_relay_config_t config;
    solar_os_sc2_relay_endpoint_t ep;
    assert(solar_os_sc2_relay_get_config(id, &config) == ESP_OK);
    assert(solar_os_sc2_relay_endpoint(&config, 0, NULL) == 1);
    assert(solar_os_sc2_relay_endpoint(&config, 1, &ep) == 1);

    const char *fb[] = {"fallback", "chats://b.example:7443#pin=" PIN_A, "chats://c.example:7444"};
    assert(solar_os_messaging_instance_configure(id, 3, fb) == ESP_OK);
    assert(solar_os_sc2_relay_get_config(id, &config) == ESP_OK);
    assert(solar_os_sc2_relay_endpoint(&config, 0, &ep) == 3);
    assert(ep.tls && ep.port == 7778 && !strcmp(ep.url, "chats://primary.example:7778") && !ep.pin[0]);
    assert(solar_os_sc2_relay_endpoint(&config, 1, &ep) == 3);
    assert(ep.tls && ep.port == 7443 && !strcmp(ep.pin, PIN_A) && !strcmp(ep.url, "chats://b.example:7443"));
    assert(solar_os_sc2_relay_endpoint(&config, 2, &ep) == 3 && ep.port == 7444);
    assert(conversation_of(pub).security_flags & SOLAR_OS_SECURITY_TRANSPORT_SECURED);

    /* a plaintext fallback means the transport is no longer always secured */
    const char *plain[] = {"fallback", "chat://lan.local:7777"};
    assert(solar_os_messaging_instance_configure(id, 2, plain) == ESP_OK);
    assert(solar_os_sc2_relay_get_config(id, &config) == ESP_OK);
    assert(solar_os_sc2_relay_endpoint(&config, 0, NULL) == 2 && !config.fallback[1].url[0]);
    assert(!(conversation_of(pub).security_flags & SOLAR_OS_SECURITY_TRANSPORT_SECURED));

    const char *bad[][5] = {
        {"fallback", "https://b.example:1"},
        {"fallback", "chat://b.example:1#pin=" PIN_A},
        {"fallback", "chats://a.example:1", "chats://b.example:2", "chats://c.example:3", "chats://d.example:4"},
        {"fallback"},
    };
    const size_t bad_n[] = {2, 2, 5, 1};
    for (size_t i = 0; i < 4; i++)
        assert(solar_os_messaging_instance_configure(id, bad_n[i], bad[i]) != ESP_OK);
    assert(solar_os_sc2_relay_get_config(id, &config) == ESP_OK && !strcmp(config.fallback[0].url, "chat://lan.local:7777"));

    /* persisted across a reboot */
    restart_provider();
    assert(solar_os_sc2_relay_get_config(id, &config) == ESP_OK);
    assert(config.version == SOLAR_OS_SC2_RELAY_CONFIG_VERSION && !strcmp(config.fallback[0].url, "chat://lan.local:7777"));

    assert(solar_os_messaging_join(id, "pub:general", &pub) == ESP_OK);
    assert(!(conversation_of(pub).security_flags & SOLAR_OS_SECURITY_TRANSPORT_SECURED));
    const char *off[] = {"fallback", "off"};
    assert(solar_os_messaging_instance_configure(id, 2, off) == ESP_OK);
    assert(solar_os_sc2_relay_get_config(id, &config) == ESP_OK && !config.fallback[0].url[0]);
    assert(conversation_of(pub).security_flags & SOLAR_OS_SECURITY_TRANSPORT_SECURED);

    /* Per-relay cursors: sc2-sync tags provider message keys with the relay
     * index (top byte), so each relay resumes from its own sequence. */
    const uint64_t mask = 0xffull << 56;
    const uint64_t keys[] = {5, 9, (1ull << 56) | 3, (1ull << 56) | 2, (2ull << 56) | 40};
    for (size_t i = 0; i < 5; i++) {
        solar_os_messaging_inbound_t in = {.provider = id, .provider_message_key = keys[i],
            .conversation_key = "pub:general", .conversation_kind = SOLAR_OS_CONVERSATION_ROOM,
            .conversation_title = "pub:general", .sender = "x", .body = "post"};
        bool inserted = false;
        assert(solar_os_messaging_publish_inbound(&in, &inserted, NULL) == ESP_OK && inserted);
    }
    assert(solar_os_messaging_provider_cursor_masked(id, "pub:general", mask, 0) == 9);
    assert(solar_os_messaging_provider_cursor_masked(id, "pub:general", mask, 1ull << 56) == 3);
    assert(solar_os_messaging_provider_cursor_masked(id, "pub:general", mask, 2ull << 56) == 40);
    assert(solar_os_messaging_provider_cursor_masked(id, "pub:general", mask, 3ull << 56) == 0);
    assert(solar_os_messaging_provider_cursor(id, "pub:general") == ((2ull << 56) | 40));

    /* a version 1 profile (no fallback list) still loads, as version 2 */
    config.version = 1;
    assert(solar_os_messaging_profile_save(id, &config, offsetof(solar_os_sc2_relay_config_t, fallback)) == ESP_OK);
    restart_provider();
    memset(&config, 0xff, sizeof(config));
    assert(solar_os_sc2_relay_get_config(id, &config) == ESP_OK);
    assert(config.version == SOLAR_OS_SC2_RELAY_CONFIG_VERSION && !strcmp(config.url, "chats://primary.example:7778"));
    for (size_t i = 0; i < SOLAR_OS_SC2_RELAY_FALLBACK_MAX; i++) assert(!config.fallback[i].url[0]);
    /* a short profile claiming version 2 is rejected */
    config.version = 2;
    assert(solar_os_messaging_profile_save(id, &config, offsetof(solar_os_sc2_relay_config_t, fallback)) == ESP_OK);
    restart_provider();
    assert(solar_os_sc2_relay_get_config(id, &config) != ESP_OK);
    assert(solar_os_messaging_instance_remove(id) == ESP_OK);
}

/* Pinned relay keys (profile v3): key= on add, configure key, fallback
 * keys, URL change drops the pin, set_key only replaces the expected pin,
 * version 2 profiles load without pins. */
#define KEY_A "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="
#define KEY_B "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBA="
static void test_relay_keys(void)
{
    solar_os_messaging_provider_id_t id = 0;
    uint8_t k[32], a[32], b[32];
    memset(a, 0, sizeof(a));
    size_t got = 0;
    assert(sc2_b64_decode(b, 32, &got, KEY_B, strlen(KEY_B)) == SC2_OK && got == 32);
    const char *bad_key[] = {"chats://k.example:7778", "key=short"};
    assert(solar_os_messaging_instance_add("sc2", "keyed", 2, bad_key, &id) != ESP_OK);
    const char *two_keys[] = {"chats://k.example:7778", "key=" KEY_A, "key=" KEY_B};
    assert(solar_os_messaging_instance_add("sc2", "keyed", 3, two_keys, &id) != ESP_OK);
    const char *add[] = {"chats://k.example:7778", "code-1", "key=" KEY_A};
    assert(solar_os_messaging_instance_add("sc2", "keyed", 3, add, &id) == ESP_OK);
    solar_os_sc2_relay_config_t config;
    assert(solar_os_sc2_relay_get_config(id, &config) == ESP_OK);
    assert(!strcmp(config.admit, "code-1") && solar_os_sc2_relay_key(&config, 0, k) && !memcmp(k, a, 32));
    assert(!solar_os_sc2_relay_key(&config, 1, k));

    const char *fb[] = {"fallback", "chats://f1.example:1", "key=" KEY_B, "chats://f2.example:2"};
    assert(solar_os_messaging_instance_configure(id, 4, fb) == ESP_OK);
    assert(solar_os_sc2_relay_get_config(id, &config) == ESP_OK);
    assert(solar_os_sc2_relay_key(&config, 1, k) && !memcmp(k, b, 32) && !solar_os_sc2_relay_key(&config, 2, k));
    const char *fb_bad[] = {"fallback", "key=" KEY_B, "chats://f1.example:1"};
    assert(solar_os_messaging_instance_configure(id, 3, fb_bad) != ESP_OK);
    const char *fb_twice[] = {"fallback", "chats://f1.example:1", "key=" KEY_B, "key=" KEY_A};
    assert(solar_os_messaging_instance_configure(id, 4, fb_twice) != ESP_OK);

    /* rotation: only from the pin it replaces */
    assert(solar_os_sc2_relay_set_key(id, 0, b, b) == ESP_ERR_INVALID_STATE);
    assert(solar_os_sc2_relay_set_key(id, 2, a, b) == ESP_ERR_INVALID_STATE); /* no pin there */
    assert(solar_os_sc2_relay_set_key(id, 0, a, b) == ESP_OK);
    restart_provider();
    assert(solar_os_sc2_relay_get_config(id, &config) == ESP_OK);
    assert(solar_os_sc2_relay_key(&config, 0, k) && !memcmp(k, b, 32));

    const char *off[] = {"key", "off"};
    assert(solar_os_messaging_instance_configure(id, 2, off) == ESP_OK);
    assert(solar_os_sc2_relay_get_config(id, &config) == ESP_OK && !solar_os_sc2_relay_key(&config, 0, NULL));
    const char *on[] = {"key", KEY_A};
    assert(solar_os_messaging_instance_configure(id, 2, on) == ESP_OK);
    /* same URL again keeps the pin; a new URL drops it unless key= comes along */
    const char *same[] = {"chats://k.example:7778"};
    assert(solar_os_messaging_instance_configure(id, 1, same) == ESP_OK);
    assert(solar_os_sc2_relay_get_config(id, &config) == ESP_OK && solar_os_sc2_relay_key(&config, 0, NULL));
    const char *moved[] = {"chats://moved.example:7778"};
    assert(solar_os_messaging_instance_configure(id, 1, moved) == ESP_OK);
    assert(solar_os_sc2_relay_get_config(id, &config) == ESP_OK && !solar_os_sc2_relay_key(&config, 0, NULL));
    const char *moved_key[] = {"chats://k.example:7778", "key=" KEY_B};
    assert(solar_os_messaging_instance_configure(id, 2, moved_key) == ESP_OK);
    assert(solar_os_sc2_relay_get_config(id, &config) == ESP_OK && solar_os_sc2_relay_key(&config, 0, k) &&
           !memcmp(k, b, 32) && !strcmp(config.admit, "code-1"));
    const char *fb_off[] = {"fallback", "off"};
    assert(solar_os_messaging_instance_configure(id, 2, fb_off) == ESP_OK);
    assert(solar_os_sc2_relay_get_config(id, &config) == ESP_OK && config.key_set == 1u);

    /* a version 2 profile (no pins) loads as version 3 without pins */
    config.version = 2;
    assert(solar_os_messaging_profile_save(id, &config, offsetof(solar_os_sc2_relay_config_t, key_set)) == ESP_OK);
    restart_provider();
    memset(&config, 0xff, sizeof(config));
    assert(solar_os_sc2_relay_get_config(id, &config) == ESP_OK);
    assert(config.version == SOLAR_OS_SC2_RELAY_CONFIG_VERSION && config.key_set == 0 &&
           !strcmp(config.url, "chats://k.example:7778"));
    assert(solar_os_messaging_instance_remove(id) == ESP_OK);
}

static int remove_entry(const char *path, const struct stat *sb, int flag, struct FTW *ftw)
{
    (void)sb; (void)flag; (void)ftw;
    return remove(path);
}

static unsigned transport_changes;
static bool transport_enabled;
static void record_transport_change(solar_os_messaging_provider_id_t id, bool enabled)
{
    assert(id != 0);
    transport_changes++;
    transport_enabled = enabled;
}

static void test_manual_job_lifecycle(void)
{
    solar_os_sc2_provider_set_transport(record_transport_change);
    solar_os_messaging_provider_id_t id = 0;
    const char *settings[] = {"chat://relay.example:7777"};
    assert(solar_os_messaging_instance_add("sc2", "manual", 1, settings, &id) == ESP_OK);
    solar_os_sc2_relay_config_t config;
    assert(solar_os_sc2_relay_get_config(id, &config) == ESP_OK && !config.enabled);
    assert(transport_changes == 1 && !transport_enabled);

    assert(solar_os_messaging_connect(id, true) == ESP_OK);
    assert(transport_changes == 2 && transport_enabled);
    assert(solar_os_sc2_relay_get_config(id, &config) == ESP_OK && config.enabled);
    const char *admit[] = {"admit", "code"};
    assert(solar_os_messaging_instance_configure(id, 2, admit) == ESP_OK);
    assert(transport_changes == 3 && transport_enabled);

    /* A persisted enabled instance notifies the transport on reload; it
     * still has no worker. The transport callback must not create one. */
    restart_provider();
    assert(transport_changes == 4 && transport_enabled);
    assert(solar_os_sc2_relay_get_config(id, &config) == ESP_OK && config.enabled);

    solar_os_conversation_id_t room = 0;
    assert(solar_os_messaging_join(id, "pub:general", &room) == ESP_OK);
    solar_os_message_key_t key = 0;
    assert(solar_os_messaging_send(room, "queued while stopped", false, &key) == ESP_OK && key);
    assert(transport_changes == 4); /* Chat's send API does not invoke the start hook. */
    solar_os_messaging_provider_status_t status;
    assert(solar_os_messaging_provider_get_status(id, &status) == ESP_OK);
    assert(!status.running && !status.connected);
    solar_os_messaging_outbound_t outbound;
    assert(solar_os_messaging_outbox_peek(id, &outbound) == ESP_OK);
    assert(outbound.message_key == key && !strcmp(outbound.body, "queued while stopped"));
    assert(solar_os_messaging_outbox_update(outbound.id, SOLAR_OS_DELIVERY_SENT, NULL) == ESP_OK);

    assert(solar_os_messaging_connect(id, false) == ESP_OK);
    assert(transport_changes == 5 && !transport_enabled);
    restart_provider();
    assert(transport_changes == 6 && !transport_enabled);
    assert(solar_os_sc2_relay_get_config(id, &config) == ESP_OK && !config.enabled);
    assert(solar_os_messaging_instance_remove(id) == ESP_OK);
    solar_os_sc2_provider_set_transport(NULL);
}

int main(void)
{
    assert(mkdtemp(test_root));
    test_parsing();
    test_registry();
    test_fallbacks();
    test_relay_keys();
    test_manual_job_lifecycle();
    assert(nftw(test_root, remove_entry, 16, FTW_DEPTH | FTW_PHYS) == 0);
    puts("sc2_provider_test: parsing, registry, flags, fallbacks, relay keys, persistence, removal and manual lifecycle OK");
    return 0;
}

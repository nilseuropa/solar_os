/*
 * sc2-sync worker: relay transport for the "sc2" Messaging provider.
 * See solar_os_sc2_sync.h. RAM: one worker stack (SOLAR_OS_SC2_SYNC_STACK), the
 * client struct and its three buffers (PSRAM preferred), one outbound
 * request copy and one private-room request (PSRAM preferred) and the TLS
 * session when chats:// is used.
 *
 * SPDX-License-Identifier: MIT
 */
#include "solar_os_sc2_sync.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <time.h>

#include "esp_crt_bundle.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "mbedtls/sha256.h"
#include "mbedtls/ssl.h"
#include "mbedtls/pk.h"
#include "solar_os_jobs.h"
#include "solar_os_identity.h"
#include "solar_os_log.h"
#include "solar_os_memory.h"
#include "solar_os_messaging.h"
#include "solar_os_sc2.h"
#include "solar_os_sc2_client.h"
#include "solar_os_sc2_provider.h"
#include "solar_os_sc2_relay_policy.h"
#include "solar_os_task.h"

static const char *TAG = "sc2-sync";
_Static_assert(sizeof(((solar_os_sc2_sync_status_t *)0)->host) >= SOLAR_OS_SC2_RELAY_URL_MAX,
               "sc2 relay status must hold the whole host");

#define SC2_SYNC_CONNECT_MS 15000
#define SC2_SYNC_WRITE_MS 8000u
#define SC2_SYNC_CHECK_MS 2000u
#define SC2_SYNC_DEFAULT_ROOM "pub:general"
/* Multi-relay: see solar_os_sc2_relay_policy.h. Inbound provider message keys carry the relay index in the top byte (0
 * for the primary, as before), so per-relay sequence numbers never collide
 * and room cursors stay per relay. */
#define SC2_SYNC_TAG_SHIFT 56u
#define SC2_SYNC_TAG_MASK (0xFFull << SC2_SYNC_TAG_SHIFT)
#define SC2_SYNC_TAG(relay) ((uint64_t)(relay) << SC2_SYNC_TAG_SHIFT)

/* Private-room request from the shell (solar_os_sc2_sync_room). The worker
 * owns the buffers; the caller only copies out after RQ_DONE. */
enum { RQ_IDLE = 0, RQ_CLAIMED, RQ_POSTED, RQ_WORKING, RQ_DONE, RQ_ABANDONED };
typedef struct {
    volatile uint8_t state;
    uint8_t op;
    esp_err_t err;
    char arg[SC2_INVITE_TEXT_MAX + 1];
    char room[SC2_DEVICE_ID_HEX_LEN + 1];
    char creating[SC2_DEVICE_ID_HEX_LEN + 1]; /* created, not yet confirmed (outlives a timed-out caller) */
    char why[96];
    char out[SOLAR_OS_SC2_ROOM_OUT_MAX];
} sc2s_rq_t;
static portMUX_TYPE rq_mux = portMUX_INITIALIZER_UNLOCKED;

typedef struct {
    TaskHandle_t task;
    volatile bool stop;
    volatile bool done;
    volatile bool running;
    bool connected;
    bool ready;
    bool client_ok;
    bool auto_joined;
    bool rebuild;     /* relay switch pending: rebuild the client */
    const char *key_refused; /* login refused: relay key differs from the pin */
    sc2rp_t rp;       /* relay in use, home relay, visits */
    uint32_t attempt;
    uint64_t retry_at;
    uint64_t checked_at;
    uint64_t again_at;
    uint32_t again_id;
    solar_os_messaging_provider_id_t id;
    solar_os_sc2_relay_config_t *cfg;  /* active */
    solar_os_sc2_relay_config_t *next; /* scratch for change detection */
    solar_os_sc2_relay_endpoint_t *ep; /* relay in use (copy from cfg) */
    solar_os_messaging_outbound_t *ob;
    sc2s_rq_t *rq;
    sc2c_t *c;
    esp_tls_t *tls;
    char host[SOLAR_OS_SC2_RELAY_URL_MAX];
    char tag[SC2C_TAG_MAX + 1];
    char nick[SC2C_NICK_MAX + 1];
} sc2s_t;

static sc2s_t s;

static uint64_t mono_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

/* ---------------- client host ---------------- */

static int h_send(void *ctx, const char *line, size_t len)
{
    (void)ctx;
    if (s.tls == NULL) {
        return -1;
    }
    uint64_t deadline = mono_ms() + SC2_SYNC_WRITE_MS;
    for (int part = 0; part < 2; part++) {
        const char *p = part == 0 ? line : "\n";
        size_t left = part == 0 ? len : 1u;
        while (left > 0) {
            int n = esp_tls_conn_write(s.tls, p, left);
            if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE || n == 0) {
                if (mono_ms() > deadline) {
                    return -1;
                }
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            if (n < 0) {
                return -1;
            }
            p += n;
            left -= (size_t)n;
        }
    }
    return 0;
}

static sc2e_t *h_lock(void *ctx)
{
    (void)ctx;
    return solar_os_sc2_engine_lock();
}

static void h_unlock(void *ctx)
{
    (void)ctx;
    solar_os_sc2_engine_unlock();
}

static void *h_alloc(void *ctx, size_t len)
{
    (void)ctx;
    return solar_os_sc2_buffer_alloc(len);
}

static void h_release(void *ctx, void *ptr, size_t len)
{
    (void)ctx;
    solar_os_sc2_buffer_free(ptr, len);
}

static uint64_t h_mono(void *ctx)
{
    (void)ctx;
    return mono_ms();
}

static uint64_t h_wall(void *ctx)
{
    (void)ctx;
    time_t t = time(NULL);
    return t >= (time_t)SOLAR_OS_SC2_MIN_WALL_CLOCK ? (uint64_t)t : 0u;
}

static void status(bool connected, esp_err_t err, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void status(bool connected, esp_err_t err, const char *fmt, ...)
{
    if (s.id == 0) {
        return;
    }
    char detail[SOLAR_OS_MESSAGING_ERROR_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(detail, sizeof(detail), fmt, ap);
    va_end(ap);
    (void)solar_os_messaging_provider_set_status(s.id, true, connected, err, detail);
}

static void short_id(char *out, size_t cap, const char *hex)
{
    snprintf(out, cap, "%.8s", hex != NULL ? hex : "");
}

static void on_message(const sc2c_event_t *ev)
{
    bool dm = ev->kind == SC2C_DM;
    bool priv = ev->kind == SC2C_PRIVATE;
    char title[SOLAR_OS_MESSAGING_TITLE_MAX];
    char sender[SOLAR_OS_MESSAGING_SENDER_MAX];
    char sid[12];
    short_id(sid, sizeof(sid), ev->from);
    if (dm) {
        snprintf(title, sizeof(title), "sc2 %s", sid);
        snprintf(sender, sizeof(sender), "%s", sid);
    } else if (priv) {
        if (sc2c_room_get_title(s.c, ev->conv, title, sizeof(title)) != SC2_OK || !title[0])
            solar_os_sc2_room_title(title, sizeof(title), ev->conv);
        snprintf(sender, sizeof(sender), "%s", sid);
    } else {
        snprintf(title, sizeof(title), "#%s", ev->conv + strlen(SOLAR_OS_SC2_PUBLIC_ROOM_PREFIX));
        /* public names are self-chosen: always show the key prefix too */
        if (ev->name != NULL && ev->name[0] != '\0') {
            snprintf(sender, sizeof(sender), "%.20s~%.6s", ev->name, sid);
        } else {
            snprintf(sender, sizeof(sender), "%s", sid);
        }
    }
    if ((dm || priv) && ev->name && ev->name[0]) {
        snprintf(sender, sizeof(sender), "%.*s", (int)sizeof(sender) - 1, ev->name);
        size_t len = strlen(sender);
        while (len && !sc2_utf8_valid(sender, len)) sender[--len] = 0;
    }
    bool truncated = ev->body_len >= SOLAR_OS_MESSAGING_BODY_MAX;
    const solar_os_messaging_inbound_t in = {
        .provider = s.id,
        .conversation_key = ev->conv,
        .conversation_kind = dm ? SOLAR_OS_CONVERSATION_DIRECT : SOLAR_OS_CONVERSATION_ROOM,
        .conversation_title = title,
        .provider_message_key = (ev->seq & ~SC2_SYNC_TAG_MASK) | SC2_SYNC_TAG(s.rp.relay),
        .timestamp_ms = ev->ts != 0 ? ev->ts * 1000u : (uint64_t)h_wall(NULL) * 1000u,
        .security_flags = solar_os_sc2_conversation_security(
            s.cfg, dm ? SOLAR_OS_SC2_TARGET_DEVICE : priv ? SOLAR_OS_SC2_TARGET_PRIVATE_ROOM
                                                         : SOLAR_OS_SC2_TARGET_PUBLIC_ROOM, dm),
        .sender = sender,
        .body = ev->body,
        .truncated = truncated,
    };
    esp_err_t err = solar_os_messaging_publish_inbound(&in, NULL, NULL);
    if (err != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "inbound message not stored: %s", esp_err_to_name(err));
    }
}

static void rq_finish(esp_err_t err, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void rq_created(const sc2c_event_t *ev);

static void update_room_title(const char *key)
{
    char title[SOLAR_OS_MESSAGING_TITLE_MAX];
    if (sc2c_room_get_title(s.c, key, title, sizeof(title)) != SC2_OK || !title[0]) return;
    const solar_os_messaging_conversation_upsert_t request = {
        .provider = s.id, .provider_key = key, .kind = SOLAR_OS_CONVERSATION_ROOM, .title = title,
        .security_flags = solar_os_sc2_conversation_security(s.cfg, SOLAR_OS_SC2_TARGET_PRIVATE_ROOM, false),
    };
    (void)solar_os_messaging_conversation_upsert(&request, NULL);
}

static void h_event(void *ctx, const sc2c_event_t *ev)
{
    (void)ctx;
    switch (ev->type) {
    case SC2C_EV_MESSAGE:
        on_message(ev);
        break;
    case SC2C_EV_SENT:
        (void)solar_os_messaging_outbox_update(ev->token, SOLAR_OS_DELIVERY_SENT, NULL);
        break;
    case SC2C_EV_FAILED:
        (void)solar_os_messaging_outbox_update(ev->token, SOLAR_OS_DELIVERY_FAILED,
                                               ev->text != NULL && ev->text[0] ? ev->text : "refused by relay");
        break;
    case SC2C_EV_STATE:
        s.ready = ev->code == SC2C_READY;
        if (ev->code == SC2C_READY) {
            s.attempt = 0;
            sc2rp_ready(&s.rp, mono_ms());
            if (s.rp.relay == 0) {
                status(true, ESP_OK, "connected to %s:%u (%s)", s.host, (unsigned)s.ep->port, s.c->relay_id);
            } else {
                status(true, ESP_OK, "connected to %s:%u (%s), fallback %u%s", s.host, (unsigned)s.ep->port,
                       s.c->relay_id, (unsigned)s.rp.relay, s.rp.visiting ? ", visit" : "");
            }
            SOLAR_OS_LOGI(TAG, "ready: relay %u %s as %s", (unsigned)s.rp.relay, s.c->relay_id, s.c->self_hex);
        } else if (ev->code != SC2C_OFFLINE) {
            status(false, ESP_OK, "%s with %s:%u", sc2c_state_name(ev->code), s.host, (unsigned)s.ep->port);
        }
        break;
    case SC2C_EV_ROOM: {
        solar_os_messaging_join_state_t js = SOLAR_OS_MESSAGING_JOIN_PENDING;
        if (ev->code == SC2C_ROOM_JOINED) {
            js = SOLAR_OS_MESSAGING_JOIN_JOINED;
        } else if (ev->code == SC2C_ROOM_FAILED) {
            js = SOLAR_OS_MESSAGING_JOIN_FAILED;
        } else if (ev->code == SC2C_ROOM_LEFT || ev->code == SC2C_ROOM_IDLE) {
            js = SOLAR_OS_MESSAGING_JOIN_LEFT;
        }
        (void)solar_os_messaging_join_update(s.id, ev->conv, js, ev->text);
        if (ev->code == SC2C_ROOM_JOINED && ev->kind == SC2C_PRIVATE) update_room_title(ev->conv);
        if (ev->code == SC2C_ROOM_FAILED && s.rq->creating[0] != '\0' && strcmp(ev->conv, s.rq->creating) == 0) {
            s.rq->creating[0] = '\0';
            rq_finish(ESP_FAIL, "relay refused the room%s%s", ev->text != NULL ? ": " : "",
                      ev->text != NULL ? ev->text : "");
        }
        break;
    }
    case SC2C_EV_CREATED:
        rq_created(ev);
        break;
    case SC2C_EV_NOTICE:
        if (ev->code == SC2C_NOTICE_RELAY_KEY) {
            s.key_refused = ev->text; /* a string literal in the client */
            SOLAR_OS_LOGE(TAG, "relay %u: %s", (unsigned)s.rp.relay, ev->text);
            break;
        }
        SOLAR_OS_LOGI(TAG, "relay notice %d: %s", ev->code, ev->text != NULL ? ev->text : "");
        break;
    case SC2C_EV_RELAY_KEY: {
        /* signed by the pinned key and proven by the relay: keep the new pin */
        const size_t i = s.rp.relay;
        uint8_t old[SOLAR_OS_SC2_RELAY_KEY_LEN];
        if (solar_os_sc2_relay_key(s.cfg, i, old)) {
            esp_err_t err = solar_os_sc2_relay_set_key(s.id, i, old, s.c->relay_key);
            if (err == ESP_OK) {
                memcpy(s.cfg->keys[i], s.c->relay_key, SOLAR_OS_SC2_RELAY_KEY_LEN); /* no reconnect for it */
            }
            SOLAR_OS_LOGW(TAG, "relay %u: %s%s", (unsigned)i, ev->text, err == ESP_OK ? "; pin updated" : "; saving failed");
        }
        break;
    }
    default:
        break;
    }
}

/* ---------------- transport ---------------- */

static bool pin_matches(void)
{
    if (s.ep->pin[0] == '\0') {
        return true;
    }
    mbedtls_ssl_context *ssl = esp_tls_get_ssl_context(s.tls);
    const mbedtls_x509_crt *crt = ssl != NULL ? mbedtls_ssl_get_peer_cert(ssl) : NULL;
    if (crt == NULL) {
        return false;
    }
    unsigned char der[600];
    int n = mbedtls_pk_write_pubkey_der((mbedtls_pk_context *)&crt->pk, der, sizeof(der));
    if (n <= 0) {
        return false;
    }
    unsigned char hash[32];
    if (mbedtls_sha256(der + sizeof(der) - (size_t)n, (size_t)n, hash, 0) != 0) {
        return false;
    }
    unsigned char b64[48];
    size_t bl = 0;
    if (mbedtls_base64_encode(b64, sizeof(b64), &bl, hash, sizeof(hash)) != 0) {
        return false;
    }
    static const char prefix[] = "sha256/";
    const char *pin = s.ep->pin;
    if (strncmp(pin, prefix, sizeof(prefix) - 1u) == 0) {
        pin += sizeof(prefix) - 1u;
    }
    return strlen(pin) == bl && memcmp(pin, b64, bl) == 0;
}

static void drop(esp_err_t err, const char *why)
{
    if (s.tls != NULL) {
        esp_tls_conn_destroy(s.tls);
        s.tls = NULL;
    }
    bool was = s.connected;
    s.connected = false;
    s.ready = false;
    /* the reason the relay gave for closing this session, if any */
    const sc2c_close_t closed = s.client_ok && was ? sc2c_close_reason(s.c) : SC2C_CLOSE_NONE;
    if (s.client_ok && was) {
        sc2c_on_disconnected(s.c);
    }
    if (s.rq != NULL && s.rq->creating[0] != '\0') {
        s.rq->creating[0] = '\0';
        rq_finish(ESP_FAIL, "connection lost before the relay confirmed the room");
    }
    if (why == NULL) {
        return;
    }
    const unsigned from = s.rp.relay;
    /* a close the relay announced is not a failure: no failover for it */
    sc2rp_action_t act = closed != SC2C_CLOSE_NONE ? sc2rp_closed(&s.rp) : sc2rp_failed(&s.rp);
    s.rebuild = act != SC2RP_STAY;
    if (act == SC2RP_SWITCH_NOW) {
        /* a visited relay is not needed now: straight back home, no backoff */
        SOLAR_OS_LOGW(TAG, "%s (relay %u %s:%u); back to relay %u", why, from, s.host, (unsigned)s.ep->port,
                      (unsigned)s.rp.relay);
        s.retry_at = mono_ms();
        return;
    }
    /* the backoff paces the attempts, also across relays; after an idle,
     * evicted or expired session the next attempt comes after a short pause
     * and is not counted; a replaced session backs off on the same relay */
    uint32_t delay = closed != SC2C_CLOSE_NONE ? sc2c_reconnect_ms(s.c, &s.attempt, esp_random())
                                               : sc2c_backoff_ms(&s.attempt, esp_random());
    s.retry_at = mono_ms() + delay;
    const char *next = act == SC2RP_STAY ? "" : s.rp.relay == 0 ? ", next: primary" : ", next: fallback";
    status(false, err, "%s; retry in %u s%s", why, (unsigned)((delay + 999u) / 1000u), next);
    SOLAR_OS_LOGW(TAG, "%s (%s:%u); retry in %u ms%s", why, s.host, (unsigned)s.ep->port, (unsigned)delay, next);
}

/* chats://...#pin=: the pinned SPKI is the trust anchor, so a self-signed
 * relay works. The handshake runs without a CA chain (VERIFY_OPTIONAL) and
 * pin_matches() checks the key before anything is sent. */
static esp_err_t pin_only_attach(void *conf)
{
    mbedtls_ssl_conf_authmode((mbedtls_ssl_config *)conf, MBEDTLS_SSL_VERIFY_OPTIONAL);
    return ESP_OK;
}

static void try_connect(void)
{
    esp_tls_cfg_t tc;
    memset(&tc, 0, sizeof(tc));
    tc.timeout_ms = SC2_SYNC_CONNECT_MS;
    if (s.ep->tls) {
        tc.crt_bundle_attach = s.ep->pin[0] != '\0' ? pin_only_attach : esp_crt_bundle_attach;
    } else {
        tc.is_plain_tcp = true;
    }
    s.tls = esp_tls_init();
    if (s.tls == NULL) {
        drop(ESP_ERR_NO_MEM, "TLS allocation failed");
        return;
    }
    status(false, ESP_OK, "connecting to %s:%u (%s%s)", s.host, (unsigned)s.ep->port,
           s.ep->tls ? (s.ep->pin[0] != '\0' ? "TLS, pinned key" : "TLS") : "plain TCP",
           s.rp.relay == 0 ? "" : s.rp.visiting ? ", visiting fallback" : ", fallback");
    if (esp_tls_conn_new_sync(s.host, (int)strlen(s.host), s.ep->port, &tc, s.tls) != 1) {
        drop(ESP_FAIL, s.ep->tls ? "TLS connection failed" : "TCP connection failed");
        return;
    }
    if (s.ep->tls && !pin_matches()) {
        drop(ESP_ERR_INVALID_RESPONSE, "relay TLS key does not match the pin");
        return;
    }
    s.connected = true;
    if (sc2c_on_connected(s.c) != SC2_OK) {
        drop(ESP_FAIL, "hello failed");
    }
}

/* Wait up to wait_ms for input, then feed what arrived. */
static void pump_input(uint32_t wait_ms)
{
    int fd = -1;
    if (esp_tls_get_conn_sockfd(s.tls, &fd) != ESP_OK || fd < 0) {
        drop(ESP_FAIL, "connection lost");
        return;
    }
    if (esp_tls_get_bytes_avail(s.tls) <= 0) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(fd, &rd);
        struct timeval tv = {.tv_sec = 0, .tv_usec = (suseconds_t)wait_ms * 1000};
        if (select(fd + 1, &rd, NULL, NULL, &tv) <= 0) {
            return;
        }
    }
    uint8_t buf[512];
    for (int pass = 0; pass < 8 && s.tls != NULL; pass++) {
        int n = esp_tls_conn_read(s.tls, buf, sizeof(buf));
        if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) {
            return;
        }
        if (n <= 0) {
            drop(ESP_FAIL, "relay closed the connection");
            return;
        }
        int rc = sc2c_feed(s.c, buf, (size_t)n);
        if (rc != SC2_OK) {
            const char *why = rc == SC2_ERR_AUTH ? "relay refused login" : "protocol error";
            esp_err_t err = ESP_FAIL;
            if (rc == SC2_ERR_AUTH && s.key_refused != NULL) {
                why = s.key_refused; /* hard fail; the pin stays as it is */
                err = ESP_ERR_INVALID_RESPONSE;
            }
            s.key_refused = NULL;
            drop(err, why);
            return;
        }
        if (sc2c_close_reason(s.c) != SC2C_CLOSE_NONE) {
            /* the relay announced the close: do not wait for the socket */
            drop(ESP_OK, sc2c_close_name(sc2c_close_reason(s.c)));
            return;
        }
        if (esp_tls_get_bytes_avail(s.tls) <= 0) {
            return;
        }
    }
}

/* ---------------- Messaging glue ---------------- */

static void push_outbox(void)
{
    /* not while visiting: peers look for us (and our posts) on our home relay */
    if (!s.ready || s.rp.visiting || solar_os_messaging_outbox_peek(s.id, s.ob) != ESP_OK) {
        return;
    }
    uint64_t now = mono_ms();
    if (s.ob->id == s.again_id && now < s.again_at) {
        return;
    }
    solar_os_messaging_conversation_t conv;
    sc2c_kind_t kind;
    if (solar_os_messaging_conversation_get(s.ob->conversation_id, &conv) != ESP_OK) {
        (void)solar_os_messaging_outbox_update(s.ob->id, SOLAR_OS_DELIVERY_FAILED, "conversation not found");
        return;
    }
    if (conv.kind == SOLAR_OS_CONVERSATION_DIRECT) {
        kind = SC2C_DM;
    } else if (strncmp(s.ob->provider_key, SOLAR_OS_SC2_PUBLIC_ROOM_PREFIX,
                       strlen(SOLAR_OS_SC2_PUBLIC_ROOM_PREFIX)) == 0) {
        kind = SC2C_PUBLIC;
    } else if (s.rp.relay == 0) {
        kind = SC2C_PRIVATE;
    } else {
        /* private rooms exist on the relay that hosts them (the primary) */
        (void)solar_os_messaging_outbox_update(s.ob->id, SOLAR_OS_DELIVERY_FAILED,
                                               "private room is on the primary relay, which is not reachable");
        return;
    }
    int rc = sc2c_submit(s.c, kind, s.ob->provider_key, s.ob->body, strlen(s.ob->body), s.ob->id,
                         (uint64_t)s.ob->message_key);
    if (rc == SC2_OK) {
        (void)solar_os_messaging_outbox_update(s.ob->id, SOLAR_OS_DELIVERY_SENDING, NULL);
    } else if (rc == SC2C_AGAIN) {
        s.again_id = s.ob->id;
        s.again_at = now + 500u;
    } else {
        char why[SOLAR_OS_MESSAGING_ERROR_MAX];
        snprintf(why, sizeof(why), "%s", rc == SC2_ERR_NOT_FOUND ? "peer has no keys on this relay" : sc2_strerror(rc));
        (void)solar_os_messaging_outbox_update(s.ob->id, SOLAR_OS_DELIVERY_FAILED, why);
    }
}

static bool room_configured(const char *key)
{
    for (size_t i = 0; i < SOLAR_OS_SC2_RELAY_ROOM_CAPACITY; i++) {
        if (strcmp(s.cfg->rooms[i], key) == 0) {
            return true;
        }
    }
    return false;
}

static void add_rooms(void)
{
    bool have_general = false;
    /* `messages leave` drops a room from the config: leave it on the relay
     * too, or its posts keep arriving for the rest of the session. */
    for (size_t i = 0; i < SC2C_ROOMS_MAX; i++) {
        const sc2c_room_t *r = &s.c->rooms[i];
        if (r->key[0] != '\0' && !room_configured(r->key) && strcmp(r->key, s.rq->creating) != 0) {
            (void)sc2c_room_remove(s.c, r->key); /* a private room's local record goes with it */
        }
    }
    for (size_t i = 0; i < SOLAR_OS_SC2_RELAY_ROOM_CAPACITY; i++) {
        const char *key = s.cfg->rooms[i];
        if (key[0] == '\0') {
            continue;
        }
        if (strcmp(key, SC2_SYNC_DEFAULT_ROOM) == 0) {
            have_general = true;
        }
        if (strncmp(key, SOLAR_OS_SC2_PUBLIC_ROOM_PREFIX, strlen(SOLAR_OS_SC2_PUBLIC_ROOM_PREFIX)) != 0 &&
            s.rp.relay != 0) {
            /* private room: hosted by the primary relay only */
            (void)solar_os_messaging_join_update(s.id, key, SOLAR_OS_MESSAGING_JOIN_PENDING,
                                                 "waits for the primary relay");
            continue;
        }
        (void)sc2c_room_add(s.c, key, solar_os_messaging_provider_cursor_masked(s.id, key, SC2_SYNC_TAG_MASK,
                                                                                 SC2_SYNC_TAG(s.rp.relay)));
        if (strncmp(key, SOLAR_OS_SC2_PUBLIC_ROOM_PREFIX, strlen(SOLAR_OS_SC2_PUBLIC_ROOM_PREFIX))) update_room_title(key);
    }
    if (!have_general && !s.auto_joined) {
        /* #general by default: through Messaging so it shows in the chat UI
         * and is saved; the config change brings it back here. */
        s.auto_joined = true;
        (void)solar_os_messaging_join(s.id, SC2_SYNC_DEFAULT_ROOM, NULL);
    }
}

static void client_close(void)
{
    drop(ESP_OK, NULL);
    if (s.client_ok) {
        sc2c_free(s.c);
        s.client_ok = false;
    }
}

static bool same_link(const solar_os_sc2_relay_config_t *a, const solar_os_sc2_relay_config_t *b)
{
    return a->enabled == b->enabled && a->tls == b->tls && a->port == b->port && strcmp(a->url, b->url) == 0 &&
           strcmp(a->pin, b->pin) == 0 && strcmp(a->admit, b->admit) == 0 &&
           memcmp(a->fallback, b->fallback, sizeof(a->fallback)) == 0 && a->key_set == b->key_set &&
           memcmp(a->keys, b->keys, sizeof(a->keys)) == 0;
}

/* (Re)build the client for relay s.rp.relay. Same store tag on every relay: the
 * persisted outbox follows whichever relay is in use. Fallback relays get the
 * signed prekey only (no one-time prekey may be claimable on two relays). */
static bool build_client(void)
{
    client_close();
    (void)solar_os_sc2_relay_endpoint(s.cfg, s.rp.relay, s.ep);
    const char *h = strstr(s.ep->url, "://");
    h = h != NULL ? h + 3 : s.ep->url;
    size_t hl = strcspn(h, ":");
    if (hl >= sizeof(s.host)) {
        hl = sizeof(s.host) - 1u;
    }
    memcpy(s.host, h, hl);
    s.host[hl] = '\0';
    snprintf(s.tag, sizeof(s.tag), "r%u", (unsigned)(s.id % 10000000u));
    sc2c_host_t host = {h_send, h_event, h_lock, h_unlock, h_alloc, h_release, h_mono, h_wall, NULL};
    sc2c_config_t cc = {
        .tag = s.tag,
        .nick = s.nick,
        .admit = s.cfg->admit[0] != '\0' ? s.cfg->admit : NULL,
        .no_opks = s.rp.relay != 0,
        /* Admission requires the end-to-end invite proof checked by the client. */
        .auto_admit = true,
        .invite_url = s.cfg->url,
    };
    uint8_t key[SOLAR_OS_SC2_RELAY_KEY_LEN];
    if (solar_os_sc2_relay_key(s.cfg, s.rp.relay, key)) {
        cc.relay_key = key; /* the relay must prove this key at login */
    }
    int rc = sc2c_init(s.c, &host, &cc);
    s.rebuild = false;
    if (rc != SC2_OK) {
        status(false, ESP_ERR_NO_MEM, "client init failed: %s", sc2_strerror(rc));
        s.retry_at = mono_ms() + 10000u;
        return false;
    }
    s.client_ok = true;
    s.auto_joined = false;
    add_rooms();
    return true;
}

/* Pick the first enabled sc2 instance; (re)build the client on changes. */
static void check_config(void)
{
    solar_os_messaging_provider_id_t cursor = 0, found = 0;
    solar_os_messaging_provider_status_t st;
    while (solar_os_messaging_provider_next(&cursor, &st)) {
        if (st.removed || !solar_os_messaging_provider_is_class(st.id, "sc2")) {
            continue;
        }
        if (solar_os_sc2_relay_get_config(st.id, s.next) == ESP_OK && s.next->enabled) {
            found = st.id;
            solar_os_identity_format(s.nick, sizeof(s.nick));
            if (s.client_ok) snprintf(s.c->nick, sizeof(s.c->nick), "%s", s.nick);
            break;
        }
    }
    if (found == 0) {
        if (s.id != 0) {
            client_close();
            s.id = 0;
        }
        return;
    }
    bool link_changed = found != s.id || !s.client_ok || !same_link(s.cfg, s.next);
    bool rooms_changed = memcmp(s.cfg->rooms, s.next->rooms, sizeof(s.next->rooms)) != 0;
    if (link_changed) {
        client_close();
    }
    *s.cfg = *s.next;
    s.id = found;
    if (link_changed) {
        sc2rp_reset(&s.rp, (unsigned)solar_os_sc2_relay_endpoint(s.cfg, 0, NULL), mono_ms());
        s.attempt = 0;
        s.retry_at = 0;
        (void)build_client();
        return;
    }
    if (rooms_changed) {
        add_rooms();
    }
}

/* Visits (probe the primary, take keys and mailbox to the fallbacks), only
 * when nothing is waiting to be sent. */
static void schedule_visit(uint64_t now)
{
    bool idle = s.rp.visiting ||
                (sc2c_outbox_count(s.c) == 0 && solar_os_messaging_outbox_peek(s.id, s.ob) != ESP_OK);
    if (sc2rp_tick(&s.rp, now, idle) != SC2RP_STAY) {
        drop(ESP_OK, NULL);
        s.rebuild = true;
        s.retry_at = now;
    }
}

/* ---------------- private rooms ---------------- */

static void rq_finish(esp_err_t err, const char *fmt, ...)
{
    sc2s_rq_t *q = s.rq;
    if (q->state != RQ_WORKING && q->state != RQ_ABANDONED) {
        return;
    }
    if (fmt != NULL) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(q->why, sizeof(q->why), fmt, ap);
        va_end(ap);
    } else {
        q->why[0] = '\0';
    }
    q->err = err;
    portENTER_CRITICAL(&rq_mux);
    bool gone = q->state == RQ_ABANDONED;
    q->state = gone ? RQ_IDLE : RQ_DONE;
    portEXIT_CRITICAL(&rq_mux);
    if (gone) {
        sc2_memzero(q->out, sizeof(q->out));
        sc2_memzero(q->arg, sizeof(q->arg));
    }
}

static size_t rooms_free(void)
{
    size_t n = 0;
    for (size_t i = 0; i < SOLAR_OS_SC2_RELAY_ROOM_CAPACITY; i++) {
        n += s.cfg->rooms[i][0] == '\0';
    }
    return n;
}

static bool is_private_key(const char *key)
{
    return key[0] != '\0' &&
           strncmp(key, SOLAR_OS_SC2_PUBLIC_ROOM_PREFIX, strlen(SOLAR_OS_SC2_PUBLIC_ROOM_PREFIX)) != 0;
}

/* A configured private room: its key, a unique prefix, or an exact title. */
static const char *private_room_find(const char *arg)
{
    const char *hit = NULL;
    size_t len = strlen(arg);
    if (!len) return NULL;
    for (size_t i = 0; i < SOLAR_OS_SC2_RELAY_ROOM_CAPACITY; i++) {
        const char *key = s.cfg->rooms[i];
        char title[SC2E_ROOM_TITLE_MAX + 1];
        bool title_matches = is_private_key(key) &&
            sc2c_room_get_title(s.c, key, title, sizeof(title)) == SC2_OK && title[0] && !strcmp(title, arg);
        if (is_private_key(key) && (title_matches ||
            (len >= 6u && len <= SC2_DEVICE_ID_HEX_LEN && strncasecmp(key, arg, len) == 0))) {
            if (hit != NULL) {
                return NULL;
            }
            hit = key;
        }
    }
    return hit;
}

/* Our room is confirmed: keep it in the instance (also when the shell
 * caller already gave up waiting). */
static void rq_created(const sc2c_event_t *ev)
{
    sc2s_rq_t *q = s.rq;
    if (q->creating[0] == '\0' || strcmp(ev->conv, q->creating) != 0) {
        return;
    }
    q->creating[0] = '\0';
    esp_err_t err = solar_os_messaging_join(s.id, ev->conv, NULL);
    if (err == ESP_OK) update_room_title(ev->conv);
    const sc2c_room_t *r = sc2c_room_get(s.c, ev->conv);
    if (err == ESP_OK && r != NULL && r->state == SC2C_ROOM_JOINED) {
        (void)solar_os_messaging_join_update(s.id, ev->conv, SOLAR_OS_MESSAGING_JOIN_JOINED, NULL);
    }
    SOLAR_OS_LOGI(TAG, "private room %s created%s", ev->conv, err == ESP_OK ? "" : " (not saved)");
    if (q->state == RQ_WORKING) {
        snprintf(q->out, sizeof(q->out), "%s", ev->text != NULL ? ev->text : "");
    }
    if (err != ESP_OK) {
        rq_finish(err, "room created but not saved in the instance: %s", esp_err_to_name(err));
    } else {
        rq_finish(ESP_OK, NULL);
    }
}

static const char *room_state_text(const sc2c_room_t *r)
{
    static const char *const names[] = {"idle",    "joining", "waiting for an admin", "waiting for the room key",
                                        "joined",  "creating", "failed",              "left"};
    return r->state < sizeof(names) / sizeof(names[0]) ? names[r->state] : "?";
}

static void rq_list(sc2s_rq_t *q)
{
    size_t at = 0;
    q->out[0] = '\0';
    char inv[SC2_INVITE_TEXT_MAX];
    for (size_t i = 0; i < SOLAR_OS_SC2_RELAY_ROOM_CAPACITY && at < sizeof(q->out); i++) {
        const char *key = s.cfg->rooms[i];
        if (key[0] == '\0') {
            continue;
        }
        const sc2c_room_t *r = sc2c_room_get(s.c, key);
        bool priv = is_private_key(key);
        bool admin = priv && sc2c_room_invite_text(s.c, key, inv, sizeof(inv)) == SC2_OK;
        char title[SC2E_ROOM_TITLE_MAX + 1] = "";
        if (priv) (void)sc2c_room_get_title(s.c, key, title, sizeof(title));
        int n = snprintf(q->out + at, sizeof(q->out) - at, "%s  %s%s  %s%s%s%s\n", key, priv ? "private" : "public",
                         admin ? ", admin" : "",
                         title[0] ? "\"" : "", title, title[0] ? "\"  " : "",
                         r != NULL ? room_state_text(r) : priv && s.rp.relay != 0 ? "on the primary relay" : "-");
        at = n < 0 ? sizeof(q->out) : at + (size_t)n;
    }
    sc2_memzero(inv, sizeof(inv));
}

static void rq_run(void)
{
    sc2s_rq_t *q = s.rq;
    if (q == NULL || q->state != RQ_POSTED) {
        return;
    }
    portENTER_CRITICAL(&rq_mux);
    q->state = RQ_WORKING;
    portEXIT_CRITICAL(&rq_mux);
    q->room[0] = '\0';
    q->out[0] = '\0';
    if (s.id == 0 || !s.client_ok) {
        rq_finish(ESP_ERR_INVALID_STATE, "no enabled sc2 instance");
        return;
    }
    const bool home = s.ready && s.rp.relay == 0 && !s.rp.visiting;
    switch (q->op) {
    case SOLAR_OS_SC2_ROOM_LIST:
        rq_list(q);
        rq_finish(ESP_OK, NULL);
        return;
    case SOLAR_OS_SC2_ROOM_INVITE: {
        const char *key = private_room_find(q->arg);
        if (key == NULL) {
            rq_finish(ESP_ERR_NOT_FOUND, "no unique private room (give its ID, prefix or exact name)");
            return;
        }
        snprintf(q->room, sizeof(q->room), "%s", key);
        int rc = sc2c_room_invite_text(s.c, key, q->out, sizeof(q->out));
        if (rc == SC2_OK) {
            rq_finish(ESP_OK, NULL);
        } else {
            rq_finish(ESP_ERR_NOT_FOUND, "only the device that created the room can invite");
        }
        return;
    }
    case SOLAR_OS_SC2_ROOM_CREATE:
    case SOLAR_OS_SC2_ROOM_JOIN: {
        if (!home) {
            rq_finish(ESP_ERR_INVALID_STATE, "needs a login on the primary relay (see `sc2 relay`)");
            return;
        }
        if (rooms_free() == 0) {
            rq_finish(ESP_ERR_NO_MEM, "room list is full (%u rooms); leave one first",
                      (unsigned)SOLAR_OS_SC2_RELAY_ROOM_CAPACITY);
            return;
        }
        if (q->op == SOLAR_OS_SC2_ROOM_CREATE) {
            int rc = sc2c_room_create_named(s.c, q->arg[0] ? q->arg : NULL, q->room);
            if (rc != SC2_OK) {
                rq_finish(ESP_FAIL, "create failed: %s", rc == SC2C_AGAIN ? "not logged in" : sc2_strerror(rc));
                return;
            }
            snprintf(q->creating, sizeof(q->creating), "%s", q->room); /* finished by rq_created */
            return;
        }
        int rc = sc2c_room_join_invite(s.c, q->arg, q->room);
        if (rc != SC2_OK) {
            rq_finish(ESP_ERR_INVALID_ARG, "%s", rc == SC2_ERR_FORMAT || rc == SC2_ERR_ARG || rc == SC2_ERR_BOUNDS
                                                     ? "not a valid invite"
                                                     : sc2_strerror(rc));
            return;
        }
        esp_err_t err = solar_os_messaging_join(s.id, q->room, NULL);
        if (err != ESP_OK) {
            (void)sc2c_room_remove(s.c, q->room);
            rq_finish(err, "room not saved in the instance: %s", esp_err_to_name(err));
            return;
        }
        rq_finish(ESP_OK, NULL);
        return;
    }
    default:
        rq_finish(ESP_ERR_INVALID_ARG, "unknown request");
        return;
    }
}

esp_err_t solar_os_sc2_sync_room(solar_os_sc2_room_op_t op, const char *arg, char *room, char *out, size_t cap,
                                 char *why, size_t whycap, uint32_t timeout_ms)
{
    if (why != NULL && whycap > 0) {
        why[0] = '\0';
    }
    sc2s_rq_t *q = s.rq;
    if (!s.running || q == NULL || s.task == NULL) {
        if (why != NULL) {
            snprintf(why, whycap, "sc2-sync is not running");
        }
        return ESP_ERR_INVALID_STATE;
    }
    if (arg != NULL && strlen(arg) >= sizeof(q->arg)) {
        if (why != NULL) {
            snprintf(why, whycap, "argument too long");
        }
        return ESP_ERR_INVALID_SIZE;
    }
    bool busy;
    portENTER_CRITICAL(&rq_mux);
    busy = q->state != RQ_IDLE;
    if (!busy) {
        q->state = RQ_CLAIMED; /* the worker only picks up RQ_POSTED: the fields are ours */
    }
    portEXIT_CRITICAL(&rq_mux);
    if (busy) {
        if (why != NULL) {
            snprintf(why, whycap, "another room request is still running");
        }
        return ESP_ERR_INVALID_STATE;
    }
    q->op = (uint8_t)op;
    snprintf(q->arg, sizeof(q->arg), "%s", arg != NULL ? arg : "");
    portENTER_CRITICAL(&rq_mux);
    q->state = RQ_POSTED;
    portEXIT_CRITICAL(&rq_mux);
    xTaskNotifyGive(s.task);
    const uint64_t end = mono_ms() + timeout_ms;
    while (q->state != RQ_DONE && mono_ms() < end) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    portENTER_CRITICAL(&rq_mux);
    uint8_t st = q->state;
    if (st == RQ_POSTED) {
        q->state = RQ_IDLE;
    } else if (st == RQ_WORKING) {
        q->state = RQ_ABANDONED;
    }
    portEXIT_CRITICAL(&rq_mux);
    if (st != RQ_DONE) {
        if (why != NULL) {
            snprintf(why, whycap, "%s", st == RQ_POSTED ? "sc2-sync did not take the request"
                                                       : "no answer from the relay yet");
        }
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = q->err;
    if (room != NULL) {
        snprintf(room, SC2_DEVICE_ID_HEX_LEN + 1u, "%s", q->room);
    }
    if (out != NULL && cap > 0) {
        snprintf(out, cap, "%s", q->out);
    }
    if (why != NULL && err != ESP_OK) {
        snprintf(why, whycap, "%s", q->why);
    }
    sc2_memzero(q->out, sizeof(q->out)); /* invite texts carry the room secret */
    sc2_memzero(q->arg, sizeof(q->arg));
    portENTER_CRITICAL(&rq_mux);
    q->state = RQ_IDLE;
    portEXIT_CRITICAL(&rq_mux);
    return err;
}

static void worker(void *arg)
{
    (void)arg;
    esp_err_t err = solar_os_sc2_init();
    if (err != ESP_OK) {
        SOLAR_OS_LOGE(TAG, "SC2 service unavailable: %s", solar_os_sc2_err_text(err));
    }
    while (!s.stop && err == ESP_OK) {
        uint64_t now = mono_ms();
        if (now - s.checked_at >= SC2_SYNC_CHECK_MS || s.checked_at == 0) {
            s.checked_at = now;
            check_config();
        }
        if (s.id != 0 && s.rebuild && !s.connected) {
            (void)build_client();
        }
        rq_run();
        if (s.id == 0 || !s.client_ok) {
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(SC2_SYNC_CHECK_MS));
            continue;
        }
        if (!s.connected) {
            if (now < s.retry_at) {
                (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250));
                continue;
            }
            try_connect();
            continue;
        }
        pump_input(100);
        if (!s.connected) {
            continue;
        }
        if (sc2c_tick(s.c) != SC2_OK) {
            drop(ESP_ERR_TIMEOUT, "relay went silent");
            continue;
        }
        push_outbox();
        if (s.ready) {
            schedule_visit(now);
        }
    }
    client_close();
    s.id = 0;
    s.running = false;
    s.done = true;
    solar_os_task_delete(NULL);
}

static bool alloc_state(void)
{
    if (s.c == NULL) {
        s.c = solar_os_memory_calloc(1, sizeof(*s.c), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "sc2.client");
    }
    if (s.cfg == NULL) {
        s.cfg = solar_os_memory_calloc(1, sizeof(*s.cfg), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "sc2.sync.cfg");
    }
    if (s.next == NULL) {
        s.next = solar_os_memory_calloc(1, sizeof(*s.next), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "sc2.sync.cfg");
    }
    if (s.ep == NULL) {
        s.ep = solar_os_memory_calloc(1, sizeof(*s.ep), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "sc2.sync.ep");
    }
    if (s.ob == NULL) {
        s.ob = solar_os_memory_calloc(1, sizeof(*s.ob), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "sc2.sync.ob");
    }
    if (s.rq == NULL) {
        s.rq = solar_os_memory_calloc(1, sizeof(*s.rq), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "sc2.sync.rq");
    }
    return s.c != NULL && s.cfg != NULL && s.next != NULL && s.ep != NULL && s.ob != NULL && s.rq != NULL;
}

esp_err_t solar_os_sc2_sync_start(void)
{
    if (s.running) {
        if (s.task != NULL) {
            xTaskNotifyGive(s.task);
        }
        return ESP_OK;
    }
    if (s.task != NULL && !s.done) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) {
        return err;
    }
    if (!alloc_state()) {
        return ESP_ERR_NO_MEM;
    }
    s.stop = false;
    s.done = false;
    s.running = true;
    s.checked_at = 0;
    if (solar_os_task_create_pinned(worker, "sc2_sync", SOLAR_OS_SC2_SYNC_STACK, NULL, tskIDLE_PRIORITY + 1, &s.task,
                                    tskNO_AFFINITY, SOLAR_OS_TASK_ROLE_BACKGROUND) != pdPASS) {
        s.running = false;
        s.task = NULL;
        return ESP_ERR_NO_MEM;
    }
    (void)solar_os_jobs_note_resource("sc2-sync", SOLAR_OS_JOB_RESOURCE_NET, "SC2 relay", "sc2 messaging instance");
    return ESP_OK;
}

void solar_os_sc2_sync_stop(void)
{
    s.stop = true;
    if (s.task != NULL && !s.done) {
        xTaskNotifyGive(s.task);
    }
    if (solar_os_task_wait_done(s.task, &s.done, 8000)) {
        s.task = NULL;
    } else {
        SOLAR_OS_LOGW(TAG, "worker is still stopping");
    }
}

static void provider_changed(solar_os_messaging_provider_id_t id, bool enabled)
{
    /* Instance settings do not own the job lifecycle. Only wake a worker
     * that was started explicitly (or by configured job autostart). */
    if (s.running && !s.stop && s.task != NULL) {
        s.checked_at = 0;
        xTaskNotifyGive(s.task);
    } else if (enabled) {
        (void)solar_os_messaging_provider_set_status(id, false, false, ESP_OK,
                                                     "enabled; run `job start sc2-sync`");
    }
}

void solar_os_sc2_sync_install(void)
{
    solar_os_sc2_provider_set_transport(provider_changed);
}

void solar_os_sc2_sync_get_status(solar_os_sc2_sync_status_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->running = s.running;
    out->connected = s.connected;
    out->ready = s.ready;
    out->instance = (unsigned)s.id;
    out->attempt = (unsigned)s.attempt;
    if (s.client_ok) {
        out->outbox = (unsigned)sc2c_outbox_count(s.c);
        snprintf(out->relay_id, sizeof(out->relay_id), "%s", s.c->relay_id);
    }
    snprintf(out->host, sizeof(out->host), "%s", s.host);
    if (s.ep != NULL) {
        out->port = s.ep->port;
        out->tls = s.ep->tls;
    }
    out->relay = s.rp.relay;
    out->relays = s.rp.relays;
    out->home = s.rp.home;
    out->visiting = s.rp.visiting;
}

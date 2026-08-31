#include "solar_os_meshcore.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <inttypes.h>
#include <new>

#include "helpers/BaseChatMesh.h"
#include "helpers/SimpleMeshTables.h"
#include "ed_25519.h"

extern "C" {
#include "esp_check.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "solar_os_contacts.h"
#include "solar_os_credentials.h"
#include "solar_os_crypto.h"
#include "solar_os_identity.h"
#include "solar_os_memory.h"
#include "solar_os_messaging.h"
#include "solar_os_meshcore_channel_key.h"
#include "solar_os_meshcore_stream.h"
#include "solar_os_radio.h"
#include "solar_os_time.h"
}

namespace {

constexpr char kNvsNamespace[] = "meshcore";
constexpr char kNvsNameKey[] = "name";
constexpr char kNvsPublicKey[] = "public";
constexpr char kIdentityLabel[] = "identity";
constexpr char kGroupLabelPrefix[] = "group:";
constexpr char kPublicPsk[] = "izOH6cXN6mrJ5e26oRXNcg==";
constexpr uint32_t kPublicGroupId = 0x80000001U;
constexpr uint32_t kSendTimeoutBaseMs = 500U;
constexpr float kFloodTimeoutFactor = 16.0F;
constexpr float kDirectTimeoutFactor = 6.0F;
constexpr uint32_t kDirectPerHopExtraMs = 250U;
constexpr uint32_t kRadioSendTimeoutFloorMs = 3000U;
constexpr uint8_t kMetadataVersion = 1U;
constexpr uint8_t kMetadataPathInbound = 1U << 0;
constexpr uint8_t kMetadataPathOutbound = 1U << 1;
static_assert(SOLAR_OS_MESHCORE_STREAM_ENVELOPE_MAX <= UINT8_MAX,
              "MeshCore stream envelope must fit a request payload");

struct MeshcoreMetadata {
    uint8_t version;
    uint8_t type;
    uint8_t flags;
    uint8_t path_len;
    int32_t gps_lat;
    int32_t gps_lon;
    uint32_t advert_timestamp;
    uint8_t path[32];
};

static_assert(sizeof(MeshcoreMetadata) ==
                  SOLAR_OS_CONTACT_PROVIDER_METADATA_MAX,
              "MeshCore contact metadata must fill the bounded provider blob");

struct ProviderChannel {
    uint32_t id;
    bool builtin;
    char name[SOLAR_OS_MESHCORE_GROUP_NAME_MAX + 1U];
    uint8_t secret[PUB_KEY_SIZE];
    size_t secret_len;
};

class SolarPacketQueue {
public:
    void reset()
    {
        count_ = 0;
    }

    bool add(mesh::Packet *packet, uint8_t priority, uint32_t scheduled)
    {
        if (packet == nullptr || count_ >= SOLAR_OS_MESHCORE_PACKET_POOL_SIZE) {
            return false;
        }
        packets_[count_] = packet;
        priorities_[count_] = priority;
        schedules_[count_] = scheduled;
        count_++;
        return true;
    }

    mesh::Packet *remove(size_t index)
    {
        if (index >= count_) {
            return nullptr;
        }
        mesh::Packet *packet = packets_[index];
        count_--;
        for (size_t move = index; move < count_; move++) {
            packets_[move] = packets_[move + 1U];
            priorities_[move] = priorities_[move + 1U];
            schedules_[move] = schedules_[move + 1U];
        }
        return packet;
    }

    mesh::Packet *next(uint32_t now)
    {
        int best = -1;
        uint8_t priority = UINT8_MAX;
        for (size_t index = 0; index < count_; index++) {
            if ((int32_t)(schedules_[index] - now) > 0) {
                continue;
            }
            if (priorities_[index] < priority) {
                best = (int)index;
                priority = priorities_[index];
            }
        }
        return best >= 0 ? remove((size_t)best) : nullptr;
    }

    size_t ready(uint32_t now) const
    {
        size_t result = 0;
        for (size_t index = 0; index < count_; index++) {
            if ((int32_t)(schedules_[index] - now) <= 0) {
                result++;
            }
        }
        return result;
    }

    size_t count() const
    {
        return count_;
    }

    mesh::Packet *at(size_t index) const
    {
        return index < count_ ? packets_[index] : nullptr;
    }

private:
    mesh::Packet *packets_[SOLAR_OS_MESHCORE_PACKET_POOL_SIZE]{};
    uint8_t priorities_[SOLAR_OS_MESHCORE_PACKET_POOL_SIZE]{};
    uint32_t schedules_[SOLAR_OS_MESHCORE_PACKET_POOL_SIZE]{};
    size_t count_ = 0;
};

class SolarPacketManager final : public mesh::PacketManager {
public:
    SolarPacketManager()
    {
        for (size_t index = 0; index < SOLAR_OS_MESHCORE_PACKET_POOL_SIZE;
             index++) {
            (void)unused_.add(&packets_[index], 0, 0);
        }
    }

    mesh::Packet *allocNew() override
    {
        return unused_.remove(0);
    }

    void free(mesh::Packet *packet) override
    {
        (void)unused_.add(packet, 0, 0);
    }

    void queueOutbound(mesh::Packet *packet,
                       uint8_t priority,
                       uint32_t scheduled_for) override
    {
        if (!outbound_.add(packet, priority, scheduled_for)) {
            free(packet);
        }
    }

    mesh::Packet *getNextOutbound(uint32_t now) override
    {
        return outbound_.next(now);
    }

    int getOutboundCount(uint32_t now) const override
    {
        return (int)outbound_.ready(now);
    }

    int getOutboundTotal() const override
    {
        return (int)outbound_.count();
    }

    int getFreeCount() const override
    {
        return (int)unused_.count();
    }

    mesh::Packet *getOutboundByIdx(int index) override
    {
        return index >= 0 ? outbound_.at((size_t)index) : nullptr;
    }

    mesh::Packet *removeOutboundByIdx(int index) override
    {
        return index >= 0 ? outbound_.remove((size_t)index) : nullptr;
    }

    void queueInbound(mesh::Packet *packet, uint32_t scheduled_for) override
    {
        if (!inbound_.add(packet, 0, scheduled_for)) {
            free(packet);
        }
    }

    mesh::Packet *getNextInbound(uint32_t now) override
    {
        return inbound_.next(now);
    }

private:
    mesh::Packet packets_[SOLAR_OS_MESHCORE_PACKET_POOL_SIZE];
    SolarPacketQueue unused_;
    SolarPacketQueue outbound_;
    SolarPacketQueue inbound_;
};

class SolarMillis final : public mesh::MillisecondClock {
public:
    unsigned long getMillis() override
    {
        return (unsigned long)solar_os_time_uptime_ms();
    }
};

class SolarRtc final : public mesh::RTCClock {
public:
    uint32_t getCurrentTime() override
    {
        uint64_t epoch_ms = 0;
        if (solar_os_time_get_utc_epoch_ms(&epoch_ms) == ESP_OK) {
            return (uint32_t)(epoch_ms / 1000ULL);
        }
        return (uint32_t)(solar_os_time_uptime_ms() / 1000ULL);
    }

    void setCurrentTime(uint32_t time) override
    {
        (void)time;
    }
};

class SolarRng final : public mesh::RNG {
public:
    void random(uint8_t *destination, size_t size) override
    {
        if (destination == nullptr || size == 0U) {
            return;
        }
        if (solar_os_crypto_random(destination, size) != ESP_OK) {
            esp_fill_random(destination, size);
        }
    }
};

class SolarRadio final : public mesh::Radio {
public:
    SolarRadio(const solar_os_radio_handle_t &handle,
               const solar_os_radio_config_t &config)
        : handle_(handle), config_(config)
    {
    }

    void begin() override
    {
        last_error_ =
            solar_os_radio_handle_set_state(&handle_, SOLAR_OS_RADIO_STATE_RX);
        receive_mode_ = last_error_ == ESP_OK;
    }

    int recvRaw(uint8_t *bytes, int size) override
    {
        if (bytes == nullptr || size <= 0 || pending_send_) {
            return 0;
        }
        solar_os_radio_packet_t packet{};
        const esp_err_t error =
            solar_os_radio_handle_receive(&handle_, &packet, 0);
        if (error == ESP_ERR_TIMEOUT) {
            return 0;
        }
        if (error != ESP_OK) {
            last_error_ = error;
            receive_errors_++;
            return 0;
        }
        if (!packet.crc_ok || packet.len == 0U) {
            last_error_ = ESP_ERR_INVALID_CRC;
            receive_errors_++;
            return 0;
        }
        const size_t copied =
            std::min(packet.len, (size_t)size);
        memcpy(bytes, packet.data, copied);
        last_rssi_ = packet.has_rssi ? (float)packet.rssi_dbm : 0.0F;
        last_snr_ = packet.has_snr ? (float)packet.snr_db : 0.0F;
        received_++;
        last_error_ = ESP_OK;
        receive_mode_ = true;
        return (int)copied;
    }

    uint32_t getEstAirtimeFor(int length) override
    {
        if (length <= 0 || config_.modulation !=
                               SOLAR_OS_RADIO_MODULATION_LORA ||
            config_.rx_bandwidth_hz == 0U ||
            config_.spreading_factor < 6U) {
            return 1U;
        }
        const double sf = config_.spreading_factor;
        const double symbol_ms =
            (double)(1UL << config_.spreading_factor) * 1000.0 /
            (double)config_.rx_bandwidth_hz;
        const int low_data_rate = symbol_ms > 16.0 ? 1 : 0;
        const int header_implicit = 0;
        const int crc = config_.crc_enabled ? 1 : 0;
        const int numerator =
            8 * length - 4 * (int)sf + 28 + 16 * crc -
            20 * header_implicit;
        const int denominator =
            4 * ((int)sf - 2 * low_data_rate);
        const int coding =
            std::max(1, (int)config_.coding_rate_denominator - 4);
        double payload_symbols = 8.0;
        if (numerator > 0 && denominator > 0) {
            payload_symbols +=
                std::ceil((double)numerator / (double)denominator) *
                (double)(coding + 4);
        }
        const double preamble =
            ((double)config_.preamble_len + 4.25) * symbol_ms;
        return (uint32_t)std::ceil(
            preamble + payload_symbols * symbol_ms);
    }

    float packetScore(float snr, int packet_length) override
    {
        static constexpr float thresholds[] = {
            -5.0F, -7.5F, -10.0F, -12.5F, -15.0F, -17.5F, -20.0F,
        };
        int sf = config_.spreading_factor;
        if (sf < 6 || sf > 12) {
            return 0.0F;
        }
        const float threshold = thresholds[sf - 6];
        if (snr < threshold) {
            return 0.0F;
        }
        const float snr_success = (snr - threshold) / 10.0F;
        const float collision =
            1.0F - std::min(packet_length, 255) / 256.0F;
        return std::clamp(snr_success * collision, 0.0F, 1.0F);
    }

    bool startSendRaw(const uint8_t *bytes, int length) override
    {
        if (bytes == nullptr || length <= 0 ||
            length > SOLAR_OS_RADIO_PACKET_MAX || pending_send_) {
            last_error_ = ESP_ERR_INVALID_ARG;
            send_errors_++;
            return false;
        }
        solar_os_radio_packet_t packet{};
        packet.len = (size_t)length;
        memcpy(packet.data, bytes, packet.len);
        receive_mode_ = false;
        const uint32_t timeout =
            std::max(kRadioSendTimeoutFloorMs,
                     getEstAirtimeFor(length) * 2U + 500U);
        last_error_ =
            solar_os_radio_handle_send(&handle_, &packet, timeout);
        if (last_error_ != ESP_OK) {
            send_errors_++;
            (void)solar_os_radio_handle_set_state(
                &handle_, SOLAR_OS_RADIO_STATE_RX);
            receive_mode_ = true;
            return false;
        }
        pending_send_ = true;
        completion_armed_ = false;
        started_++;
        return true;
    }

    bool isSendComplete() override
    {
        return pending_send_ && completion_armed_;
    }

    void onSendFinished() override
    {
        if (pending_send_) {
            completed_++;
        }
        pending_send_ = false;
        completion_armed_ = false;
        last_error_ =
            solar_os_radio_handle_set_state(&handle_, SOLAR_OS_RADIO_STATE_RX);
        receive_mode_ = last_error_ == ESP_OK;
    }

    void loop() override
    {
        if (pending_send_) {
            completion_armed_ = true;
        }
    }

    bool isInRecvMode() const override
    {
        return receive_mode_;
    }

    float getLastRSSI() const override
    {
        return last_rssi_;
    }

    float getLastSNR() const override
    {
        return last_snr_;
    }

    uint32_t received() const { return received_; }
    uint32_t started() const { return started_; }
    uint32_t completed() const { return completed_; }
    uint32_t sendErrors() const { return send_errors_; }
    uint32_t receiveErrors() const { return receive_errors_; }
    esp_err_t lastError() const { return last_error_; }
    solar_os_radio_handle_t handle() const { return handle_; }

private:
    solar_os_radio_handle_t handle_;
    solar_os_radio_config_t config_;
    bool receive_mode_ = false;
    bool pending_send_ = false;
    bool completion_armed_ = false;
    float last_rssi_ = 0.0F;
    float last_snr_ = 0.0F;
    uint32_t received_ = 0;
    uint32_t started_ = 0;
    uint32_t completed_ = 0;
    uint32_t send_errors_ = 0;
    uint32_t receive_errors_ = 0;
    esp_err_t last_error_ = ESP_OK;
};

struct PendingDirect {
    bool active;
    uint32_t request_id;
    solar_os_message_key_t message_key;
    uint8_t attempt;
    uint32_t timestamp;
    uint32_t expected_ack;
    solar_os_endpoint_id_t endpoint_id;
    char body[MAX_TEXT_LEN + 1U];
};

struct PendingGroup {
    bool active;
    uint32_t request_id;
    uint32_t completed_before;
};

static uint64_t packet_key(mesh::Packet *packet)
{
    uint8_t hash[MAX_HASH_SIZE]{};
    packet->calculatePacketHash(hash);
    uint64_t result = 0;
    memcpy(&result, hash, sizeof(result));
    return result;
}

static size_t path_bytes(uint8_t encoded_length)
{
    const uint8_t mode = encoded_length >> 6;
    if (mode >= 3U) {
        return 0U;
    }
    return (size_t)(encoded_length & 63U) * (1U << mode);
}

class SolarMesh final : public BaseChatMesh {
public:
    SolarMesh(SolarRadio &radio,
              SolarMillis &millis,
              SolarRng &rng,
              SolarRtc &rtc,
              SolarPacketManager &packets,
              SimpleMeshTables &tables)
        : BaseChatMesh(radio, millis, rng, rtc, packets, tables),
          radio_(radio), packets_(packets), tables_(tables)
    {
    }

    esp_err_t configure(const uint8_t *identity,
                        size_t identity_length,
                        const ProviderChannel *channels,
                        size_t channel_count)
    {
        if (identity == nullptr ||
            (identity_length != PRV_KEY_SIZE &&
             identity_length != PRV_KEY_SIZE + PUB_KEY_SIZE)) {
            return ESP_ERR_INVALID_ARG;
        }
        self_id.readFrom(identity, identity_length);
        ESP_RETURN_ON_ERROR(reloadContacts(true),
                            "meshcore",
                            "contact cache failed");

        channel_count_ = 0;
        for (size_t index = 0;
             index < channel_count &&
             index < SOLAR_OS_MESHCORE_GROUP_CAPACITY;
             index++) {
            ChannelDetails details{};
            strlcpy(details.name,
                    channels[index].name,
                    sizeof(details.name));
            memcpy(details.channel.secret,
                   channels[index].secret,
                   channels[index].secret_len);
            mesh::Utils::sha256(details.channel.hash,
                                sizeof(details.channel.hash),
                                channels[index].secret,
                                (int)channels[index].secret_len);
            if (!setChannel((int)index, details)) {
                return ESP_ERR_INVALID_STATE;
            }
            channel_ids_[index] = channels[index].id;
            strlcpy(channel_names_[index],
                    channels[index].name,
                    sizeof(channel_names_[index]));
            channel_count_++;
        }
        return ESP_OK;
    }

    void beginProvider(const char *name)
    {
        BaseChatMesh::begin();
        sendAdvert(name, false);
    }

    void sendAdvert(const char *name, bool flood)
    {
        mesh::Packet *advert = createSelfAdvert(name);
        if (advert == nullptr) {
            last_error_ = ESP_ERR_NO_MEM;
            return;
        }
        if (flood) {
            sendFlood(advert);
        } else {
            sendZeroHop(advert);
        }
        adverts_sent_++;
    }

    void process()
    {
        const esp_err_t contacts_error = reloadContacts(false);
        if (contacts_error != ESP_OK) {
            last_error_ = contacts_error;
        }
        processStreamOutbound();
        BaseChatMesh::loop();
        finishGroupIfSent();
        if (!direct_.active && !group_.active) {
            processOutbound();
        }
    }

    size_t contactsLoaded() const { return contacts_loaded_; }
    size_t packetPoolFree() const
    {
        return (size_t)packets_.getFreeCount();
    }
    uint32_t advertsReceived() const { return adverts_received_; }
    uint32_t advertsSent() const { return adverts_sent_; }
    uint32_t directReceived() const { return direct_received_; }
    uint32_t groupReceived() const { return group_received_; }
    uint32_t acknowledgements() const { return acknowledgements_; }
    uint32_t retries() const { return retries_; }
    esp_err_t lastError() const { return last_error_; }
    uint32_t duplicateDirect() const { return tables_.getNumDirectDups(); }
    uint32_t duplicateFlood() const { return tables_.getNumFloodDups(); }

protected:
    bool allowPacketForward(const mesh::Packet *packet) override
    {
        (void)packet;
        return false;
    }

    int calcRxDelay(float score, uint32_t air_time) const override
    {
        (void)score;
        (void)air_time;
        return 0;
    }

    bool shouldAutoAddContactType(uint8_t type) const override
    {
        return type == ADV_TYPE_CHAT;
    }

    bool shouldOverwriteWhenFull() const override
    {
        return false;
    }

    void onContactsFull() override
    {
        last_error_ = ESP_ERR_NO_MEM;
    }

    void onDiscoveredContact(ContactInfo &contact,
                             bool is_new,
                             uint8_t path_length,
                             const uint8_t *path) override
    {
        (void)is_new;
        MeshcoreMetadata metadata{};
        metadata.version = kMetadataVersion;
        metadata.type = contact.type;
        metadata.flags = kMetadataPathInbound;
        metadata.path_len = path_length;
        metadata.gps_lat = contact.gps_lat;
        metadata.gps_lon = contact.gps_lon;
        metadata.advert_timestamp = contact.last_advert_timestamp;
        const size_t bytes = path_bytes(path_length);
        if (path != nullptr && bytes <= sizeof(metadata.path)) {
            memcpy(metadata.path, path, bytes);
        } else {
            metadata.path_len = 0;
        }
        solar_os_contact_id_t contact_id = 0;
        solar_os_endpoint_id_t endpoint_id = 0;
        last_error_ = solar_os_contacts_upsert_discovered(
            SOLAR_OS_MESSAGING_PROVIDER_MESHCORE,
            contact.id.pub_key,
            PUB_KEY_SIZE,
            contact.name,
            SOLAR_OS_ENDPOINT_CAP_DIRECT |
                SOLAR_OS_ENDPOINT_CAP_GROUP |
                SOLAR_OS_ENDPOINT_CAP_ACK,
            (uint64_t)contact.lastmod * 1000ULL,
            &metadata,
            sizeof(metadata),
            &contact_id,
            &endpoint_id);
        if (last_error_ == ESP_OK) {
            adverts_received_++;
        }
    }

    void onContactPathUpdated(const ContactInfo &contact) override
    {
        MeshcoreMetadata metadata{};
        metadata.version = kMetadataVersion;
        metadata.type = contact.type;
        metadata.flags = kMetadataPathOutbound;
        metadata.path_len = contact.out_path_len;
        metadata.gps_lat = contact.gps_lat;
        metadata.gps_lon = contact.gps_lon;
        metadata.advert_timestamp = contact.last_advert_timestamp;
        const size_t bytes = path_bytes(contact.out_path_len);
        if (bytes > 0U && bytes <= sizeof(metadata.path)) {
            memcpy(metadata.path, contact.out_path, bytes);
        } else {
            metadata.path_len = 0;
        }
        (void)solar_os_contacts_upsert_discovered(
            SOLAR_OS_MESSAGING_PROVIDER_MESHCORE,
            contact.id.pub_key,
            PUB_KEY_SIZE,
            contact.name,
            SOLAR_OS_ENDPOINT_CAP_DIRECT |
                SOLAR_OS_ENDPOINT_CAP_GROUP |
                SOLAR_OS_ENDPOINT_CAP_ACK,
            (uint64_t)contact.lastmod * 1000ULL,
            &metadata,
            sizeof(metadata),
            nullptr,
            nullptr);
    }

    ContactInfo *processAck(const uint8_t *data) override
    {
        if (!direct_.active || data == nullptr ||
            memcmp(data, &direct_.expected_ack,
                   sizeof(direct_.expected_ack)) != 0) {
            return nullptr;
        }
        ContactInfo *contact =
            lookupContactForEndpoint(direct_.endpoint_id);
        (void)solar_os_messaging_outbox_update(
            direct_.request_id,
            SOLAR_OS_DELIVERY_DELIVERED,
            nullptr);
        direct_.active = false;
        acknowledgements_++;
        return contact;
    }

    void onMessageRecv(const ContactInfo &contact,
                       mesh::Packet *packet,
                       uint32_t sender_timestamp,
                       const char *text) override
    {
        publishDirect(contact, packet, sender_timestamp, text);
    }

    void onCommandDataRecv(const ContactInfo &contact,
                           mesh::Packet *packet,
                           uint32_t sender_timestamp,
                           const char *text) override
    {
        (void)contact;
        (void)packet;
        (void)sender_timestamp;
        (void)text;
    }

    void onSignedMessageRecv(const ContactInfo &contact,
                             mesh::Packet *packet,
                             uint32_t sender_timestamp,
                             const uint8_t *sender_prefix,
                             const char *text) override
    {
        (void)sender_prefix;
        publishDirect(contact, packet, sender_timestamp, text);
    }

    void onChannelMessageRecv(const mesh::GroupChannel &channel,
                              mesh::Packet *packet,
                              uint32_t timestamp,
                              const char *text) override
    {
        const int index = findChannelIdx(channel);
        if (index < 0 || (size_t)index >= channel_count_) {
            return;
        }
        char provider_key[SOLAR_OS_MESSAGING_PROVIDER_KEY_MAX];
        snprintf(provider_key,
                 sizeof(provider_key),
                 "group:%" PRIu32,
                 channel_ids_[index]);
        char sender[SOLAR_OS_MESSAGING_SENDER_MAX]{};
        const char *body = text != nullptr ? text : "";
        const char *separator = strstr(body, ": ");
        if (separator != nullptr) {
            const size_t sender_length =
                std::min((size_t)(separator - body), sizeof(sender) - 1U);
            memcpy(sender, body, sender_length);
            sender[sender_length] = '\0';
            body = separator + 2;
        }
        const solar_os_messaging_inbound_t inbound = {
            .provider = SOLAR_OS_MESSAGING_PROVIDER_MESHCORE,
            .conversation_key = provider_key,
            .conversation_kind = SOLAR_OS_CONVERSATION_GROUP,
            .conversation_title = channel_names_[index],
            .group_ref = channel_ids_[index],
            .provider_message_key = packet_key(packet),
            .timestamp_ms = (uint64_t)timestamp * 1000ULL,
            .security_flags =
                SOLAR_OS_SECURITY_ENCRYPTED |
                SOLAR_OS_SECURITY_SHARED_KEY |
                SOLAR_OS_SECURITY_SENDER_UNVERIFIED,
            .sender = sender,
            .body = body,
        };
        bool inserted = false;
        last_error_ =
            solar_os_messaging_publish_inbound(&inbound, &inserted, nullptr);
        if (last_error_ == ESP_OK && inserted) {
            group_received_++;
        }
    }

    uint8_t onContactRequest(const ContactInfo &contact,
                             uint32_t sender_timestamp,
                             const uint8_t *data,
                             uint8_t length,
                             uint8_t *reply) override
    {
        (void)sender_timestamp;
        (void)reply;
        if (solar_os_meshcore_stream_envelope_matches(data, length)) {
            (void)solar_os_meshcore_stream_ingest(
                contact.id.pub_key,
                data,
                length,
                (uint32_t)solar_os_time_uptime_ms());
        }
        return 0;
    }

    void onContactResponse(const ContactInfo &contact,
                           const uint8_t *data,
                           uint8_t length) override
    {
        (void)contact;
        (void)data;
        (void)length;
    }

    uint32_t calcFloodTimeoutMillisFor(uint32_t airtime) const override
    {
        return kSendTimeoutBaseMs +
            (uint32_t)((float)airtime * kFloodTimeoutFactor);
    }

    uint32_t calcDirectTimeoutMillisFor(uint32_t airtime,
                                        uint8_t path_length) const override
    {
        const uint32_t hops = path_length & 63U;
        return kSendTimeoutBaseMs +
            (uint32_t)((float)airtime * kDirectTimeoutFactor *
                       (float)(hops + 1U)) +
            hops * kDirectPerHopExtraMs;
    }

    void onSendTimeout() override
    {
        if (!direct_.active) {
            return;
        }
        if (direct_.attempt >= 2U) {
            (void)solar_os_messaging_outbox_update(
                direct_.request_id,
                SOLAR_OS_DELIVERY_FAILED,
                "MeshCore acknowledgement timeout");
            direct_.active = false;
            last_error_ = ESP_ERR_TIMEOUT;
            return;
        }
        ContactInfo *contact =
            lookupContactForEndpoint(direct_.endpoint_id);
        if (contact == nullptr) {
            (void)solar_os_messaging_outbox_update(
                direct_.request_id,
                SOLAR_OS_DELIVERY_FAILED,
                "MeshCore contact unavailable");
            direct_.active = false;
            last_error_ = ESP_ERR_NOT_FOUND;
            return;
        }
        direct_.attempt++;
        retries_++;
        (void)solar_os_messaging_outbox_update(
            direct_.request_id,
            SOLAR_OS_DELIVERY_QUEUED,
            "retrying");
        uint32_t timeout = 0;
        const int result = sendMessage(*contact,
                                       direct_.timestamp,
                                       direct_.attempt,
                                       direct_.body,
                                       direct_.expected_ack,
                                       timeout);
        if (result == MSG_SEND_FAILED) {
            (void)solar_os_messaging_outbox_update(
                direct_.request_id,
                SOLAR_OS_DELIVERY_FAILED,
                "MeshCore retry failed");
            direct_.active = false;
            last_error_ = ESP_FAIL;
        } else {
            (void)solar_os_messaging_outbox_update(
                direct_.request_id,
                SOLAR_OS_DELIVERY_SENDING,
                nullptr);
        }
    }

private:
    void processStreamOutbound()
    {
        solar_os_meshcore_stream_process(
            (uint32_t)solar_os_time_uptime_ms());
        solar_os_endpoint_id_t endpoint_id = SOLAR_OS_ENDPOINT_ID_NONE;
        uint8_t envelope[SOLAR_OS_MESHCORE_STREAM_ENVELOPE_MAX]{};
        size_t envelope_len = 0U;
        const esp_err_t take = solar_os_meshcore_stream_take_tx(
            &endpoint_id,
            envelope,
            sizeof(envelope),
            &envelope_len);
        if (take == ESP_ERR_TIMEOUT || take == ESP_ERR_INVALID_STATE) {
            return;
        }
        if (take != ESP_OK) {
            last_error_ = take;
            return;
        }
        ContactInfo *contact = lookupTrustedContactForEndpoint(endpoint_id);
        if (contact == nullptr) {
            solar_os_meshcore_stream_note_tx(
                endpoint_id, ESP_ERR_INVALID_STATE);
            return;
        }
        uint32_t tag = 0U;
        uint32_t timeout = 0U;
        const int result = sendRequest(
            *contact,
            envelope,
            (uint8_t)envelope_len,
            tag,
            timeout);
        (void)tag;
        (void)timeout;
        solar_os_meshcore_stream_note_tx(
            endpoint_id,
            result == MSG_SEND_FAILED ? ESP_ERR_NO_MEM : ESP_OK);
    }

    esp_err_t reloadContacts(bool force)
    {
        solar_os_contacts_status_t before{};
        esp_err_t error = solar_os_contacts_get_status(&before);
        if (error != ESP_OK) {
            return error;
        }
        if (!force &&
            contacts_generation_valid_ &&
            before.generation == contacts_generation_) {
            return ESP_OK;
        }

        solar_os_endpoint_t *endpoints =
            static_cast<solar_os_endpoint_t *>(solar_os_memory_calloc(
                SOLAR_OS_ENDPOINT_CAPACITY,
                sizeof(*endpoints),
                SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                "meshcore.contacts"));
        if (endpoints == nullptr) {
            resetContacts();
            contacts_loaded_ = 0;
            contacts_generation_valid_ = false;
            return ESP_ERR_NO_MEM;
        }

        size_t endpoint_count = 0;
        solar_os_contacts_status_t after{};
        bool stable = false;
        for (unsigned attempt = 0; attempt < 2U; attempt++) {
            error = solar_os_contacts_get_status(&before);
            if (error != ESP_OK) {
                break;
            }
            endpoint_count = solar_os_contacts_endpoint_snapshot(
                SOLAR_OS_CONTACT_ID_NONE,
                endpoints,
                SOLAR_OS_ENDPOINT_CAPACITY);
            error = solar_os_contacts_get_status(&after);
            if (error != ESP_OK) {
                break;
            }
            if (before.generation == after.generation) {
                stable = true;
                break;
            }
        }
        if (!stable) {
            solar_os_memory_free(endpoints);
            resetContacts();
            contacts_loaded_ = 0;
            contacts_generation_valid_ = false;
            return error == ESP_OK ? ESP_ERR_INVALID_STATE : error;
        }

        resetContacts();
        contacts_loaded_ = 0;
        for (size_t index = 0; index < endpoint_count; index++) {
            const solar_os_endpoint_t &endpoint = endpoints[index];
            if (endpoint.provider != SOLAR_OS_MESSAGING_PROVIDER_MESHCORE ||
                endpoint.address.length != PUB_KEY_SIZE ||
                endpoint.trust == SOLAR_OS_CONTACT_TRUST_BLOCKED) {
                continue;
            }
            solar_os_contact_t contact_record{};
            if (solar_os_contacts_get(endpoint.contact_id,
                                      &contact_record) != ESP_OK) {
                continue;
            }
            ContactInfo contact{};
            memcpy(contact.id.pub_key,
                   endpoint.address.bytes,
                   PUB_KEY_SIZE);
            strlcpy(contact.name,
                    contact_record.display_name,
                    sizeof(contact.name));
            contact.type = ADV_TYPE_CHAT;
            contact.out_path_len = OUT_PATH_UNKNOWN;
            contact.lastmod = (uint32_t)(endpoint.last_seen_ms / 1000ULL);
            if (endpoint.provider_metadata_len ==
                sizeof(MeshcoreMetadata)) {
                const auto *metadata =
                    reinterpret_cast<const MeshcoreMetadata *>(
                        endpoint.provider_metadata);
                if (metadata->version == kMetadataVersion) {
                    contact.type = metadata->type;
                    contact.gps_lat = metadata->gps_lat;
                    contact.gps_lon = metadata->gps_lon;
                    contact.last_advert_timestamp =
                        metadata->advert_timestamp;
                    const size_t bytes = path_bytes(metadata->path_len);
                    if ((metadata->flags & kMetadataPathOutbound) != 0U &&
                        bytes > 0U &&
                        bytes <= sizeof(metadata->path)) {
                        contact.out_path_len = metadata->path_len;
                        memcpy(contact.out_path,
                               metadata->path,
                               bytes);
                    }
                }
            }
            if (addContact(contact)) {
                contacts_loaded_++;
            }
        }
        solar_os_memory_free(endpoints);
        contacts_generation_ = after.generation;
        contacts_generation_valid_ = true;
        return ESP_OK;
    }

    ContactInfo *lookupContactForEndpoint(solar_os_endpoint_id_t endpoint_id)
    {
        solar_os_endpoint_t endpoint{};
        if (solar_os_contacts_get_endpoint(endpoint_id, &endpoint) != ESP_OK ||
            endpoint.provider != SOLAR_OS_MESSAGING_PROVIDER_MESHCORE ||
            endpoint.address.length != PUB_KEY_SIZE ||
            endpoint.trust == SOLAR_OS_CONTACT_TRUST_BLOCKED) {
            return nullptr;
        }
        return lookupContactByPubKey(endpoint.address.bytes, PUB_KEY_SIZE);
    }

    ContactInfo *lookupTrustedContactForEndpoint(
        solar_os_endpoint_id_t endpoint_id)
    {
        solar_os_endpoint_t endpoint{};
        if (solar_os_contacts_get_endpoint(endpoint_id, &endpoint) != ESP_OK ||
            endpoint.provider != SOLAR_OS_MESSAGING_PROVIDER_MESHCORE ||
            endpoint.address.length != PUB_KEY_SIZE ||
            endpoint.trust != SOLAR_OS_CONTACT_TRUST_TRUSTED) {
            return nullptr;
        }
        return lookupContactByPubKey(endpoint.address.bytes, PUB_KEY_SIZE);
    }

    void publishDirect(const ContactInfo &contact,
                       mesh::Packet *packet,
                       uint32_t sender_timestamp,
                       const char *text)
    {
        solar_os_endpoint_t endpoint{};
        if (solar_os_contacts_find_endpoint(
                SOLAR_OS_MESSAGING_PROVIDER_MESHCORE,
                contact.id.pub_key,
                PUB_KEY_SIZE,
                &endpoint) != ESP_OK ||
            endpoint.trust == SOLAR_OS_CONTACT_TRUST_BLOCKED) {
            return;
        }
        solar_os_contact_t record{};
        (void)solar_os_contacts_get(endpoint.contact_id, &record);
        char provider_key[SOLAR_OS_MESSAGING_PROVIDER_KEY_MAX];
        snprintf(provider_key,
                 sizeof(provider_key),
                 "direct:%" PRIu32,
                 endpoint.id);
        uint32_t security =
            SOLAR_OS_SECURITY_ENCRYPTED |
            SOLAR_OS_SECURITY_PEER_KEY_KNOWN;
        if (endpoint.trust == SOLAR_OS_CONTACT_TRUST_TRUSTED) {
            security |= SOLAR_OS_SECURITY_PEER_TRUSTED;
        }
        const solar_os_messaging_inbound_t inbound = {
            .provider = SOLAR_OS_MESSAGING_PROVIDER_MESHCORE,
            .conversation_key = provider_key,
            .conversation_kind = SOLAR_OS_CONVERSATION_DIRECT,
            .conversation_title = record.display_name,
            .contact_id = endpoint.contact_id,
            .endpoint_id = endpoint.id,
            .provider_message_key = packet_key(packet),
            .timestamp_ms = (uint64_t)sender_timestamp * 1000ULL,
            .security_flags = security,
            .sender = record.display_name,
            .body = text != nullptr ? text : "",
        };
        bool inserted = false;
        last_error_ =
            solar_os_messaging_publish_inbound(&inbound, &inserted, nullptr);
        if (last_error_ == ESP_OK && inserted) {
            direct_received_++;
        }
    }

    void processOutbound()
    {
        solar_os_messaging_outbound_t outbound{};
        if (solar_os_messaging_outbox_peek(
                SOLAR_OS_MESSAGING_PROVIDER_MESHCORE,
                &outbound) != ESP_OK) {
            return;
        }
        solar_os_messaging_conversation_t conversation{};
        if (solar_os_messaging_conversation_get(
                outbound.conversation_id, &conversation) != ESP_OK) {
            (void)solar_os_messaging_outbox_update(
                outbound.id,
                SOLAR_OS_DELIVERY_FAILED,
                "MeshCore conversation unavailable");
            return;
        }
        if (conversation.kind == SOLAR_OS_CONVERSATION_DIRECT) {
            ContactInfo *contact =
                lookupContactForEndpoint(conversation.endpoint_id);
            if (contact == nullptr) {
                (void)solar_os_messaging_outbox_update(
                    outbound.id,
                    SOLAR_OS_DELIVERY_FAILED,
                    "MeshCore contact unavailable or blocked");
                return;
            }
            if (strnlen(outbound.body, sizeof(outbound.body)) > MAX_TEXT_LEN) {
                (void)solar_os_messaging_outbox_update(
                    outbound.id,
                    SOLAR_OS_DELIVERY_FAILED,
                    "MeshCore text exceeds 160 bytes");
                return;
            }
            memset(&direct_, 0, sizeof(direct_));
            direct_.active = true;
            direct_.request_id = outbound.id;
            direct_.message_key = outbound.message_key;
            direct_.timestamp = getRTCClock()->getCurrentTimeUnique();
            direct_.endpoint_id = conversation.endpoint_id;
            strlcpy(direct_.body, outbound.body, sizeof(direct_.body));
            uint32_t timeout = 0;
            const int result = sendMessage(*contact,
                                           direct_.timestamp,
                                           0,
                                           direct_.body,
                                           direct_.expected_ack,
                                           timeout);
            if (result == MSG_SEND_FAILED) {
                direct_.active = false;
                (void)solar_os_messaging_outbox_update(
                    outbound.id,
                    SOLAR_OS_DELIVERY_FAILED,
                    "MeshCore packet allocation failed");
                last_error_ = ESP_ERR_NO_MEM;
                return;
            }
            (void)solar_os_messaging_outbox_update(
                outbound.id,
                SOLAR_OS_DELIVERY_SENDING,
                nullptr);
            return;
        }
        if (conversation.kind == SOLAR_OS_CONVERSATION_GROUP) {
            int channel_index = -1;
            for (size_t index = 0; index < channel_count_; index++) {
                if (channel_ids_[index] == conversation.group_ref) {
                    channel_index = (int)index;
                    break;
                }
            }
            ChannelDetails channel{};
            char sender[SOLAR_OS_MESHCORE_NAME_MAX + 1U];
            if (channel_index < 0 ||
                !getChannel(channel_index, channel) ||
                solar_os_meshcore_name_get(sender) != ESP_OK) {
                (void)solar_os_messaging_outbox_update(
                    outbound.id,
                    SOLAR_OS_DELIVERY_FAILED,
                    "MeshCore group unavailable");
                last_error_ = ESP_FAIL;
                return;
            }
            if (strlen(sender) + 2U + strlen(outbound.body) >
                MAX_TEXT_LEN) {
                (void)solar_os_messaging_outbox_update(
                    outbound.id,
                    SOLAR_OS_DELIVERY_FAILED,
                    "MeshCore group text and sender exceed 160 bytes");
                last_error_ = ESP_ERR_INVALID_SIZE;
                return;
            }
            if (
                !sendGroupMessage(getRTCClock()->getCurrentTimeUnique(),
                                  channel.channel,
                                  sender,
                                  outbound.body,
                                  (int)strlen(outbound.body))) {
                (void)solar_os_messaging_outbox_update(
                    outbound.id,
                    SOLAR_OS_DELIVERY_FAILED,
                    "MeshCore group send failed");
                last_error_ = ESP_FAIL;
                return;
            }
            group_.active = true;
            group_.request_id = outbound.id;
            group_.completed_before = radio_.completed();
            (void)solar_os_messaging_outbox_update(
                outbound.id,
                SOLAR_OS_DELIVERY_SENDING,
                nullptr);
            return;
        }
        (void)solar_os_messaging_outbox_update(
            outbound.id,
            SOLAR_OS_DELIVERY_FAILED,
            "unsupported MeshCore conversation");
    }

    void finishGroupIfSent()
    {
        if (group_.active &&
            radio_.completed() > group_.completed_before &&
            packets_.getOutboundTotal() == 0) {
            (void)solar_os_messaging_outbox_update(
                group_.request_id,
                SOLAR_OS_DELIVERY_SENT,
                nullptr);
            group_.active = false;
        }
    }

    SolarRadio &radio_;
    SolarPacketManager &packets_;
    SimpleMeshTables &tables_;
    PendingDirect direct_{};
    PendingGroup group_{};
    uint32_t channel_ids_[SOLAR_OS_MESHCORE_GROUP_CAPACITY]{};
    char channel_names_[SOLAR_OS_MESHCORE_GROUP_CAPACITY]
                       [SOLAR_OS_MESHCORE_GROUP_NAME_MAX + 1U]{};
    size_t channel_count_ = 0;
    size_t contacts_loaded_ = 0;
    uint32_t contacts_generation_ = 0;
    bool contacts_generation_valid_ = false;
    uint32_t adverts_received_ = 0;
    uint32_t adverts_sent_ = 0;
    uint32_t direct_received_ = 0;
    uint32_t group_received_ = 0;
    uint32_t acknowledgements_ = 0;
    uint32_t retries_ = 0;
    esp_err_t last_error_ = ESP_OK;
};

struct MeshcoreContext {
    SolarRadio radio;
    SolarMillis millis;
    SolarRng rng;
    SolarRtc rtc;
    SolarPacketManager packets;
    SimpleMeshTables tables;
    SolarMesh mesh;

    MeshcoreContext(const solar_os_radio_handle_t &handle,
                    const solar_os_radio_config_t &config)
        : radio(handle, config),
          mesh(radio, millis, rng, rtc, packets, tables)
    {
    }
};

struct MeshcoreService {
    SemaphoreHandle_t lock;
    MeshcoreContext *context;
    solar_os_meshcore_status_t status;
    solar_os_radio_status_t saved_radio;
    uint8_t zero_adverts;
    uint8_t flood_adverts;
    bool transitioning;
};

MeshcoreService service{};
portMUX_TYPE service_init_mux = portMUX_INITIALIZER_UNLOCKED;
bool service_initializing = false;
bool service_initialized = false;

static void lock_service()
{
    (void)xSemaphoreTake(service.lock, portMAX_DELAY);
}

static void unlock_service()
{
    xSemaphoreGive(service.lock);
}

static bool text_valid(const char *text, size_t max_length, bool allow_empty)
{
    if (text == nullptr) {
        return false;
    }
    const size_t length = strlen(text);
    if ((!allow_empty && length == 0U) || length > max_length) {
        return false;
    }
    for (const unsigned char *cursor =
             reinterpret_cast<const unsigned char *>(text);
         *cursor != '\0';
         cursor++) {
        if (*cursor < 0x20U || *cursor == 0x7fU) {
            return false;
        }
    }
    return true;
}

class StoppedTransition {
public:
    StoppedTransition()
    {
        lock_service();
        if (!service.status.running && !service.transitioning) {
            service.transitioning = true;
            acquired_ = true;
        }
        unlock_service();
    }

    ~StoppedTransition()
    {
        if (!acquired_) {
            return;
        }
        lock_service();
        service.transitioning = false;
        unlock_service();
    }

    bool acquired() const { return acquired_; }

private:
    bool acquired_ = false;
};

static esp_err_t nvs_public_get(bool *enabled)
{
    if (enabled == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *enabled = true;
    nvs_handle_t nvs;
    const esp_err_t error =
        nvs_open(kNvsNamespace, NVS_READONLY, &nvs);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (error != ESP_OK) {
        return error;
    }
    uint8_t value = 1;
    const esp_err_t read = nvs_get_u8(nvs, kNvsPublicKey, &value);
    nvs_close(nvs);
    if (read != ESP_OK && read != ESP_ERR_NVS_NOT_FOUND) {
        return read;
    }
    *enabled = value != 0U;
    return ESP_OK;
}

static esp_err_t identity_read(uint8_t identity[PRV_KEY_SIZE + PUB_KEY_SIZE],
                               size_t *identity_length)
{
    solar_os_credential_info_t record{};
    esp_err_t error = solar_os_credentials_find(
        SOLAR_OS_MESSAGING_PROVIDER_MESHCORE,
        SOLAR_OS_CREDENTIAL_ASYMMETRIC_IDENTITY,
        kIdentityLabel,
        &record);
    if (error != ESP_OK) {
        return error;
    }
    return solar_os_credentials_read_secret(
        record.id,
        identity,
        PRV_KEY_SIZE + PUB_KEY_SIZE,
        identity_length);
}

static esp_err_t identity_store(const uint8_t private_key[PRV_KEY_SIZE],
                                const uint8_t public_key[PUB_KEY_SIZE],
                                bool replace)
{
    uint8_t secret[PRV_KEY_SIZE + PUB_KEY_SIZE];
    memcpy(secret, private_key, PRV_KEY_SIZE);
    memcpy(secret + PRV_KEY_SIZE, public_key, PUB_KEY_SIZE);
    const esp_err_t error = solar_os_credentials_put(
        SOLAR_OS_MESSAGING_PROVIDER_MESHCORE,
        SOLAR_OS_CREDENTIAL_ASYMMETRIC_IDENTITY,
        kIdentityLabel,
        secret,
        sizeof(secret),
        replace,
        nullptr);
    solar_os_credentials_wipe(secret, sizeof(secret));
    return error;
}

static esp_err_t public_psk(uint8_t secret[PUB_KEY_SIZE],
                            size_t *secret_length)
{
    return solar_os_crypto_base64_decode(
        kPublicPsk, secret, PUB_KEY_SIZE, secret_length);
}

static size_t channel_configs(ProviderChannel *channels, size_t capacity)
{
    if (channels == nullptr || capacity == 0U) {
        return 0U;
    }
    size_t count = 0;
    bool public_enabled = true;
    if (nvs_public_get(&public_enabled) == ESP_OK && public_enabled) {
        ProviderChannel &channel = channels[count];
        channel.id = kPublicGroupId;
        channel.builtin = true;
        strlcpy(channel.name, SOLAR_OS_MESHCORE_PUBLIC_GROUP,
                sizeof(channel.name));
        if (public_psk(channel.secret, &channel.secret_len) == ESP_OK) {
            count++;
        }
    }

    solar_os_credential_info_t records[SOLAR_OS_CREDENTIAL_CAPACITY];
    const size_t record_count =
        solar_os_credentials_snapshot(records,
                                      SOLAR_OS_CREDENTIAL_CAPACITY);
    for (size_t index = 0;
         index < record_count && count < capacity;
         index++) {
        const solar_os_credential_info_t &record = records[index];
        if (record.provider != SOLAR_OS_MESSAGING_PROVIDER_MESHCORE ||
            record.kind != SOLAR_OS_CREDENTIAL_SHARED_KEY ||
            strncmp(record.label,
                    kGroupLabelPrefix,
                    strlen(kGroupLabelPrefix)) != 0) {
            continue;
        }
        ProviderChannel &channel = channels[count];
        channel.id = record.id;
        channel.builtin = false;
        strlcpy(channel.name,
                record.label + strlen(kGroupLabelPrefix),
                sizeof(channel.name));
        size_t length = 0;
        if (solar_os_credentials_read_secret(
                record.id,
                channel.secret,
                sizeof(channel.secret),
                &length) == ESP_OK &&
            (length == 16U || length == 32U)) {
            channel.secret_len = length;
            count++;
        } else {
            solar_os_credentials_wipe(channel.secret,
                                      sizeof(channel.secret));
        }
    }
    return count;
}

static void upsert_group_conversations(const ProviderChannel *channels,
                                       size_t count)
{
    for (size_t index = 0; index < count; index++) {
        char provider_key[SOLAR_OS_MESSAGING_PROVIDER_KEY_MAX];
        snprintf(provider_key,
                 sizeof(provider_key),
                 "group:%" PRIu32,
                 channels[index].id);
        const solar_os_messaging_conversation_upsert_t request = {
            .provider = SOLAR_OS_MESSAGING_PROVIDER_MESHCORE,
            .provider_key = provider_key,
            .kind = SOLAR_OS_CONVERSATION_GROUP,
            .title = channels[index].name,
            .group_ref = channels[index].id,
            .security_flags =
                SOLAR_OS_SECURITY_ENCRYPTED |
                SOLAR_OS_SECURITY_SHARED_KEY |
                SOLAR_OS_SECURITY_SENDER_UNVERIFIED,
        };
        (void)solar_os_messaging_conversation_upsert(&request, nullptr);
    }
}

static void update_runtime_status(MeshcoreContext *context)
{
    if (context == nullptr) {
        return;
    }
    solar_os_meshcore_status_t values{};
    values.contacts_loaded = context->mesh.contactsLoaded();
    values.packet_pool_free = context->mesh.packetPoolFree();
    values.transmitted = context->radio.completed();
    values.received = context->radio.received();
    values.send_errors = context->radio.sendErrors();
    values.receive_errors = context->radio.receiveErrors();
    values.adverts_received = context->mesh.advertsReceived();
    values.adverts_sent = context->mesh.advertsSent();
    values.direct_received = context->mesh.directReceived();
    values.group_received = context->mesh.groupReceived();
    values.acknowledgements = context->mesh.acknowledgements();
    values.retries = context->mesh.retries();
    values.duplicate_direct = context->mesh.duplicateDirect();
    values.duplicate_flood = context->mesh.duplicateFlood();
    values.last_error =
        context->mesh.lastError() != ESP_OK ?
            context->mesh.lastError() : context->radio.lastError();

    lock_service();
    service.status.contacts_loaded = values.contacts_loaded;
    service.status.packet_pool_free = values.packet_pool_free;
    service.status.transmitted = values.transmitted;
    service.status.received = values.received;
    service.status.send_errors = values.send_errors;
    service.status.receive_errors = values.receive_errors;
    service.status.adverts_received = values.adverts_received;
    service.status.adverts_sent = values.adverts_sent;
    service.status.direct_received = values.direct_received;
    service.status.group_received = values.group_received;
    service.status.acknowledgements = values.acknowledgements;
    service.status.retries = values.retries;
    service.status.duplicate_direct = values.duplicate_direct;
    service.status.duplicate_flood = values.duplicate_flood;
    if (values.last_error != ESP_OK) {
        service.status.last_error = values.last_error;
    }
    unlock_service();
}

static esp_err_t identity_generate_internal(bool force)
{
    solar_os_credential_info_t existing{};
    if (!force &&
        solar_os_credentials_find(
            SOLAR_OS_MESSAGING_PROVIDER_MESHCORE,
            SOLAR_OS_CREDENTIAL_ASYMMETRIC_IDENTITY,
            kIdentityLabel,
            &existing) == ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t seed[SEED_SIZE]{};
    uint8_t private_key[PRV_KEY_SIZE]{};
    uint8_t public_key[PUB_KEY_SIZE]{};
    esp_err_t error = solar_os_crypto_random(seed, sizeof(seed));
    if (error == ESP_OK) {
        ed25519_create_keypair(public_key, private_key, seed);
        error = identity_store(private_key, public_key, true);
    }
    solar_os_credentials_wipe(seed, sizeof(seed));
    solar_os_credentials_wipe(private_key, sizeof(private_key));
    if (error == ESP_OK) {
        lock_service();
        service.status.identity_set = true;
        (void)solar_os_crypto_bytes_to_hex(
            public_key,
            sizeof(public_key),
            service.status.public_key_hex,
            sizeof(service.status.public_key_hex));
        service.status.generation++;
        unlock_service();
    }
    solar_os_credentials_wipe(public_key, sizeof(public_key));
    return error;
}

}  // namespace

extern "C" esp_err_t solar_os_meshcore_init(void)
{
    bool owner = false;
    while (!owner) {
        portENTER_CRITICAL(&service_init_mux);
        if (service_initialized) {
            portEXIT_CRITICAL(&service_init_mux);
            return ESP_OK;
        }
        if (!service_initializing) {
            service_initializing = true;
            owner = true;
        }
        portEXIT_CRITICAL(&service_init_mux);
        if (!owner) {
            vTaskDelay(1);
        }
    }

    SemaphoreHandle_t lock = xSemaphoreCreateMutex();
    if (lock == nullptr) {
        portENTER_CRITICAL(&service_init_mux);
        service_initializing = false;
        portEXIT_CRITICAL(&service_init_mux);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t error = solar_os_contacts_init();
    if (error == ESP_OK) {
        error = solar_os_credentials_init();
    }
    if (error == ESP_OK) {
        error = solar_os_messaging_init();
    }
    if (error == ESP_OK) {
        error = solar_os_meshcore_stream_init();
    }
    if (error == ESP_OK) {
        error = solar_os_messaging_provider_register(
            SOLAR_OS_MESSAGING_PROVIDER_MESHCORE, "meshcore");
    }
    if (error != ESP_OK) {
        vSemaphoreDelete(lock);
        portENTER_CRITICAL(&service_init_mux);
        service_initializing = false;
        portEXIT_CRITICAL(&service_init_mux);
        return error;
    }

    char name[SOLAR_OS_MESHCORE_NAME_MAX + 1U]{};
    (void)solar_os_meshcore_name_get(name);
    uint8_t identity[PRV_KEY_SIZE + PUB_KEY_SIZE]{};
    size_t identity_length = 0;
    const bool identity_set =
        identity_read(identity, &identity_length) == ESP_OK;
    bool public_enabled = true;
    (void)nvs_public_get(&public_enabled);
    ProviderChannel channels[SOLAR_OS_MESHCORE_GROUP_CAPACITY]{};
    const size_t count =
        channel_configs(channels, SOLAR_OS_MESHCORE_GROUP_CAPACITY);
    upsert_group_conversations(channels, count);

    memset(&service, 0, sizeof(service));
    service.lock = lock;
    service.status.last_error = ESP_OK;
    service.status.context_in_psram = true;
    service.status.identity_set = identity_set;
    service.status.public_channel_enabled = public_enabled;
    service.status.channels = count;
    strlcpy(service.status.name, name, sizeof(service.status.name));
    if (identity_set && identity_length >= PRV_KEY_SIZE + PUB_KEY_SIZE) {
        (void)solar_os_crypto_bytes_to_hex(
            identity + PRV_KEY_SIZE,
            PUB_KEY_SIZE,
            service.status.public_key_hex,
            sizeof(service.status.public_key_hex));
    }
    solar_os_credentials_wipe(identity, sizeof(identity));
    for (size_t index = 0; index < count; index++) {
        solar_os_credentials_wipe(channels[index].secret,
                                  sizeof(channels[index].secret));
    }
    portENTER_CRITICAL(&service_init_mux);
    service.status.initialized = true;
    service_initialized = true;
    service_initializing = false;
    portEXIT_CRITICAL(&service_init_mux);
    return ESP_OK;
}

extern "C" esp_err_t solar_os_meshcore_get_status(
    solar_os_meshcore_status_t *status)
{
    if (status == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = solar_os_meshcore_init();
    if (error != ESP_OK) {
        return error;
    }
    lock_service();
    *status = service.status;
    unlock_service();
    return ESP_OK;
}

extern "C" esp_err_t solar_os_meshcore_identity_generate(bool force)
{
    ESP_RETURN_ON_ERROR(solar_os_meshcore_init(), "meshcore", "init failed");
    StoppedTransition transition;
    if (!transition.acquired()) {
        return ESP_ERR_INVALID_STATE;
    }
    return identity_generate_internal(force);
}

extern "C" esp_err_t solar_os_meshcore_identity_import(
    const char *private_key_hex)
{
    ESP_RETURN_ON_ERROR(solar_os_meshcore_init(), "meshcore", "init failed");
    if (private_key_hex == nullptr ||
        strlen(private_key_hex) != PRV_KEY_SIZE * 2U) {
        return ESP_ERR_INVALID_ARG;
    }
    StoppedTransition transition;
    if (!transition.acquired()) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t private_key[PRV_KEY_SIZE]{};
    uint8_t public_key[PUB_KEY_SIZE]{};
    esp_err_t error = solar_os_crypto_hex_to_bytes(
        private_key_hex, private_key, sizeof(private_key));
    if (error == ESP_OK &&
        !mesh::LocalIdentity::validatePrivateKey(private_key)) {
        error = ESP_ERR_INVALID_ARG;
    }
    if (error == ESP_OK) {
        ed25519_derive_pub(public_key, private_key);
        error = identity_store(private_key, public_key, true);
    }
    solar_os_credentials_wipe(private_key, sizeof(private_key));
    if (error == ESP_OK) {
        lock_service();
        service.status.identity_set = true;
        (void)solar_os_crypto_bytes_to_hex(
            public_key,
            sizeof(public_key),
            service.status.public_key_hex,
            sizeof(service.status.public_key_hex));
        service.status.generation++;
        unlock_service();
    }
    solar_os_credentials_wipe(public_key, sizeof(public_key));
    return error;
}

extern "C" esp_err_t solar_os_meshcore_identity_public(
    char public_key_hex[SOLAR_OS_MESHCORE_PUBLIC_KEY_HEX_LEN])
{
    if (public_key_hex == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t identity[PRV_KEY_SIZE + PUB_KEY_SIZE]{};
    size_t length = 0;
    esp_err_t error = identity_read(identity, &length);
    if (error == ESP_OK && length >= PRV_KEY_SIZE + PUB_KEY_SIZE) {
        error = solar_os_crypto_bytes_to_hex(
            identity + PRV_KEY_SIZE,
            PUB_KEY_SIZE,
            public_key_hex,
            SOLAR_OS_MESHCORE_PUBLIC_KEY_HEX_LEN);
    } else if (error == ESP_OK) {
        error = ESP_ERR_INVALID_SIZE;
    }
    solar_os_credentials_wipe(identity, sizeof(identity));
    return error;
}

extern "C" esp_err_t solar_os_meshcore_identity_export_private(
    char private_key_hex[SOLAR_OS_MESHCORE_PRIVATE_KEY_HEX_LEN])
{
    if (private_key_hex == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t identity[PRV_KEY_SIZE + PUB_KEY_SIZE]{};
    size_t length = 0;
    esp_err_t error = identity_read(identity, &length);
    if (error == ESP_OK && length >= PRV_KEY_SIZE) {
        error = solar_os_crypto_bytes_to_hex(
            identity,
            PRV_KEY_SIZE,
            private_key_hex,
            SOLAR_OS_MESHCORE_PRIVATE_KEY_HEX_LEN);
    } else if (error == ESP_OK) {
        error = ESP_ERR_INVALID_SIZE;
    }
    solar_os_credentials_wipe(identity, sizeof(identity));
    return error;
}

extern "C" esp_err_t solar_os_meshcore_name_get(
    char name[SOLAR_OS_MESHCORE_NAME_MAX + 1U])
{
    if (name == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t nvs;
    esp_err_t error = nvs_open(kNvsNamespace, NVS_READONLY, &nvs);
    if (error == ESP_OK) {
        size_t length = SOLAR_OS_MESHCORE_NAME_MAX + 1U;
        error = nvs_get_str(nvs, kNvsNameKey, name, &length);
        nvs_close(nvs);
        if (error == ESP_OK) {
            return ESP_OK;
        }
    }
    if (error != ESP_ERR_NVS_NOT_FOUND) {
        return error;
    }
    solar_os_identity_format(name, SOLAR_OS_MESHCORE_NAME_MAX + 1U);
    name[SOLAR_OS_MESHCORE_NAME_MAX] = '\0';
    return ESP_OK;
}

extern "C" esp_err_t solar_os_meshcore_name_set(const char *name)
{
    ESP_RETURN_ON_ERROR(solar_os_meshcore_init(), "meshcore", "init failed");
    if (!text_valid(name, SOLAR_OS_MESHCORE_NAME_MAX, true)) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t nvs;
    esp_err_t error = nvs_open(kNvsNamespace, NVS_READWRITE, &nvs);
    if (error != ESP_OK) {
        return error;
    }
    if (name[0] == '\0') {
        error = nvs_erase_key(nvs, kNvsNameKey);
        if (error == ESP_ERR_NVS_NOT_FOUND) {
            error = ESP_OK;
        }
    } else {
        error = nvs_set_str(nvs, kNvsNameKey, name);
    }
    if (error == ESP_OK) {
        error = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (error == ESP_OK) {
        char effective[SOLAR_OS_MESHCORE_NAME_MAX + 1U]{};
        (void)solar_os_meshcore_name_get(effective);
        lock_service();
        strlcpy(service.status.name,
                effective,
                sizeof(service.status.name));
        service.status.generation++;
        unlock_service();
    }
    return error;
}

extern "C" size_t solar_os_meshcore_channel_snapshot(
    solar_os_meshcore_channel_t *channels,
    size_t max_channels)
{
    ProviderChannel configs[SOLAR_OS_MESHCORE_GROUP_CAPACITY]{};
    const size_t count =
        channel_configs(configs, SOLAR_OS_MESHCORE_GROUP_CAPACITY);
    const size_t copied = std::min(count, max_channels);
    for (size_t index = 0; channels != nullptr && index < copied; index++) {
        memset(&channels[index], 0, sizeof(channels[index]));
        channels[index].id = configs[index].id;
        channels[index].builtin = configs[index].builtin;
        channels[index].enabled = true;
        strlcpy(channels[index].name,
                configs[index].name,
                sizeof(channels[index].name));
    }
    for (size_t index = 0; index < count; index++) {
        solar_os_credentials_wipe(configs[index].secret,
                                  sizeof(configs[index].secret));
    }
    return copied;
}

extern "C" esp_err_t solar_os_meshcore_channel_add(
    const char *name,
    const char *base64_psk)
{
    ESP_RETURN_ON_ERROR(solar_os_meshcore_init(), "meshcore", "init failed");
    const bool hashtag = name != nullptr && name[0] == '#';
    if (!text_valid(name, SOLAR_OS_MESHCORE_GROUP_NAME_MAX, false) ||
        strcmp(name, SOLAR_OS_MESHCORE_PUBLIC_GROUP) == 0 ||
        (hashtag && (name[1] == '\0' || base64_psk != nullptr)) ||
        (!hashtag && base64_psk == nullptr)) {
        return ESP_ERR_INVALID_ARG;
    }
    StoppedTransition transition;
    if (!transition.acquired()) {
        return ESP_ERR_INVALID_STATE;
    }
    ProviderChannel existing[SOLAR_OS_MESHCORE_GROUP_CAPACITY]{};
    const size_t existing_count =
        channel_configs(existing, SOLAR_OS_MESHCORE_GROUP_CAPACITY);
    for (size_t index = 0; index < existing_count; index++) {
        if (strcmp(existing[index].name, name) == 0) {
            for (size_t wipe = 0; wipe < existing_count; wipe++) {
                solar_os_credentials_wipe(existing[wipe].secret,
                                          sizeof(existing[wipe].secret));
            }
            return ESP_ERR_INVALID_STATE;
        }
    }
    for (size_t index = 0; index < existing_count; index++) {
        solar_os_credentials_wipe(existing[index].secret,
                                  sizeof(existing[index].secret));
    }
    if (existing_count >= SOLAR_OS_MESHCORE_GROUP_CAPACITY) {
        return ESP_ERR_NO_MEM;
    }
    uint8_t secret[PUB_KEY_SIZE]{};
    size_t secret_length = 0;
    esp_err_t error = ESP_OK;
    if (hashtag) {
        if (!solar_os_meshcore_channel_key_derive_hashtag(name, secret)) {
            error = ESP_ERR_INVALID_ARG;
        } else {
            secret_length = SOLAR_OS_MESHCORE_HASHTAG_KEY_LEN;
        }
    } else {
        error = solar_os_crypto_base64_decode(
            base64_psk, secret, sizeof(secret), &secret_length);
    }
    if (error == ESP_OK && secret_length != 16U && secret_length != 32U) {
        error = ESP_ERR_INVALID_SIZE;
    }
    char label[SOLAR_OS_CREDENTIAL_LABEL_MAX + 1U];
    if (error == ESP_OK &&
        snprintf(label, sizeof(label), "%s%s", kGroupLabelPrefix, name) >=
            (int)sizeof(label)) {
        error = ESP_ERR_INVALID_SIZE;
    }
    solar_os_credential_id_t credential_id = 0;
    if (error == ESP_OK) {
        error = solar_os_credentials_put(
            SOLAR_OS_MESSAGING_PROVIDER_MESHCORE,
            SOLAR_OS_CREDENTIAL_SHARED_KEY,
            label,
            secret,
            secret_length,
            false,
            &credential_id);
    }
    if (error == ESP_OK) {
        ProviderChannel channel{};
        channel.id = credential_id;
        strlcpy(channel.name, name, sizeof(channel.name));
        memcpy(channel.secret, secret, secret_length);
        channel.secret_len = secret_length;
        upsert_group_conversations(&channel, 1);
        lock_service();
        service.status.channels++;
        service.status.generation++;
        unlock_service();
    }
    solar_os_credentials_wipe(secret, sizeof(secret));
    return error;
}

extern "C" esp_err_t solar_os_meshcore_channel_remove(const char *name)
{
    ESP_RETURN_ON_ERROR(solar_os_meshcore_init(), "meshcore", "init failed");
    if (!text_valid(name, SOLAR_OS_MESHCORE_GROUP_NAME_MAX, false) ||
        strcmp(name, SOLAR_OS_MESHCORE_PUBLIC_GROUP) == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    StoppedTransition transition;
    if (!transition.acquired()) {
        return ESP_ERR_INVALID_STATE;
    }
    char label[SOLAR_OS_CREDENTIAL_LABEL_MAX + 1U];
    if (snprintf(label, sizeof(label), "%s%s", kGroupLabelPrefix, name) >=
        (int)sizeof(label)) {
        return ESP_ERR_INVALID_SIZE;
    }
    solar_os_credential_info_t record{};
    esp_err_t error = solar_os_credentials_find(
        SOLAR_OS_MESSAGING_PROVIDER_MESHCORE,
        SOLAR_OS_CREDENTIAL_SHARED_KEY,
        label,
        &record);
    if (error == ESP_OK) {
        error = solar_os_credentials_remove(record.id);
    }
    if (error == ESP_OK) {
        char provider_key[SOLAR_OS_MESSAGING_PROVIDER_KEY_MAX];
        snprintf(provider_key,
                 sizeof(provider_key),
                 "group:%" PRIu32,
                 record.id);
        (void)solar_os_messaging_conversation_remove(
            SOLAR_OS_MESSAGING_PROVIDER_MESHCORE,
            provider_key);
        lock_service();
        if (service.status.channels > 0U) {
            service.status.channels--;
        }
        service.status.generation++;
        unlock_service();
    }
    return error;
}

extern "C" esp_err_t solar_os_meshcore_channel_public_set(bool enabled)
{
    ESP_RETURN_ON_ERROR(solar_os_meshcore_init(), "meshcore", "init failed");
    StoppedTransition transition;
    if (!transition.acquired()) {
        return ESP_ERR_INVALID_STATE;
    }
    bool current = true;
    esp_err_t error = nvs_public_get(&current);
    if (error != ESP_OK || current == enabled) {
        return error;
    }
    nvs_handle_t nvs = 0;
    error = nvs_open(kNvsNamespace, NVS_READWRITE, &nvs);
    if (error == ESP_OK) {
        error = nvs_set_u8(nvs, kNvsPublicKey, enabled ? 1U : 0U);
    }
    if (error == ESP_OK) {
        error = nvs_commit(nvs);
    }
    if (nvs != 0) {
        nvs_close(nvs);
    }
    if (error == ESP_OK) {
        ProviderChannel channels[SOLAR_OS_MESHCORE_GROUP_CAPACITY]{};
        const size_t count =
            channel_configs(channels, SOLAR_OS_MESHCORE_GROUP_CAPACITY);
        if (enabled) {
            upsert_group_conversations(channels, count);
        } else {
            char provider_key[SOLAR_OS_MESSAGING_PROVIDER_KEY_MAX];
            snprintf(provider_key,
                     sizeof(provider_key),
                     "group:%" PRIu32,
                     kPublicGroupId);
            (void)solar_os_messaging_conversation_remove(
                SOLAR_OS_MESSAGING_PROVIDER_MESHCORE,
                provider_key);
        }
        for (size_t index = 0; index < count; index++) {
            solar_os_credentials_wipe(channels[index].secret,
                                      sizeof(channels[index].secret));
        }
        lock_service();
        service.status.public_channel_enabled = enabled;
        service.status.channels = count;
        service.status.generation++;
        unlock_service();
    }
    return error;
}

extern "C" esp_err_t solar_os_meshcore_start(const char *radio,
                                               const char *profile,
                                               const char *owner)
{
    ESP_RETURN_ON_ERROR(solar_os_meshcore_init(), "meshcore", "init failed");
    if (!text_valid(radio, SOLAR_OS_RADIO_NAME_MAX - 1U, false) ||
        !text_valid(profile, SOLAR_OS_RADIO_PROFILE_NAME_MAX - 1U, false) ||
        !text_valid(owner, SOLAR_OS_RADIO_OWNER_MAX - 1U, false)) {
        return ESP_ERR_INVALID_ARG;
    }
    StoppedTransition transition;
    if (!transition.acquired()) {
        return ESP_ERR_INVALID_STATE;
    }
    solar_os_radio_info_t info{};
    ESP_RETURN_ON_ERROR(solar_os_radio_get_info(radio, &info),
                        "meshcore", "radio not found");
    const solar_os_radio_features_t needed =
        SOLAR_OS_RADIO_FEATURE_PACKET |
        SOLAR_OS_RADIO_FEATURE_RSSI |
        SOLAR_OS_RADIO_FEATURE_SNR |
        SOLAR_OS_RADIO_FEATURE_CRC |
        SOLAR_OS_RADIO_FEATURE_VARIABLE_LENGTH;
    if ((info.features & needed) != needed ||
        (info.modulations & SOLAR_OS_RADIO_MODULATION_LORA) == 0U ||
        info.max_packet_len < MAX_TRANS_UNIT) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint8_t identity[PRV_KEY_SIZE + PUB_KEY_SIZE]{};
    size_t identity_length = 0;
    esp_err_t error = identity_read(identity, &identity_length);
    if (error == ESP_ERR_NOT_FOUND) {
        error = identity_generate_internal(false);
        if (error == ESP_OK) {
            error = identity_read(identity, &identity_length);
        }
    }
    if (error != ESP_OK) {
        return error;
    }
    ProviderChannel channels[SOLAR_OS_MESHCORE_GROUP_CAPACITY]{};
    const size_t channel_count =
        channel_configs(channels, SOLAR_OS_MESHCORE_GROUP_CAPACITY);

    solar_os_radio_handle_t handle = SOLAR_OS_RADIO_HANDLE_INIT;
    error = solar_os_radio_claim(radio, owner, &handle);
    solar_os_radio_status_t saved{};
    solar_os_radio_status_t configured{};
    if (error == ESP_OK) {
        error = solar_os_radio_handle_get_status(&handle, &saved);
    }
    if (error == ESP_OK) {
        error = solar_os_radio_handle_profile_apply(&handle, profile);
    }
    if (error == ESP_OK) {
        error = solar_os_radio_handle_get_status(&handle, &configured);
    }
    if (error != ESP_OK) {
        (void)solar_os_radio_release(&handle);
        solar_os_credentials_wipe(identity, sizeof(identity));
        return error;
    }

    void *memory = solar_os_memory_alloc(
        sizeof(MeshcoreContext),
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
        "meshcore.context");
    if (memory == nullptr) {
        (void)solar_os_radio_handle_configure(&handle, &saved.config);
        if (saved.state != SOLAR_OS_RADIO_STATE_UNKNOWN) {
            (void)solar_os_radio_handle_set_state(&handle, saved.state);
        }
        (void)solar_os_radio_release(&handle);
        solar_os_credentials_wipe(identity, sizeof(identity));
        return ESP_ERR_NO_MEM;
    }
    MeshcoreContext *context =
        new (memory) MeshcoreContext(handle, configured.config);
    error = context->mesh.configure(identity,
                                    identity_length,
                                    channels,
                                    channel_count);
    if (error == ESP_OK &&
        identity_length >= PRV_KEY_SIZE + PUB_KEY_SIZE) {
        error = solar_os_meshcore_stream_transport_start(
            identity + PRV_KEY_SIZE);
    } else if (error == ESP_OK) {
        error = ESP_ERR_INVALID_SIZE;
    }
    solar_os_credentials_wipe(identity, sizeof(identity));
    if (error == ESP_OK) {
        upsert_group_conversations(channels, channel_count);
    }
    for (size_t index = 0; index < channel_count; index++) {
        solar_os_credentials_wipe(channels[index].secret,
                                  sizeof(channels[index].secret));
    }
    if (error != ESP_OK) {
        solar_os_meshcore_stream_transport_stop();
        context->~MeshcoreContext();
        solar_os_memory_free(memory);
        (void)solar_os_radio_handle_configure(&handle, &saved.config);
        if (saved.state != SOLAR_OS_RADIO_STATE_UNKNOWN) {
            (void)solar_os_radio_handle_set_state(&handle, saved.state);
        }
        (void)solar_os_radio_release(&handle);
        return error;
    }
    char name[SOLAR_OS_MESHCORE_NAME_MAX + 1U]{};
    (void)solar_os_meshcore_name_get(name);
    context->mesh.beginProvider(name);

    lock_service();
    service.context = context;
    service.saved_radio = saved;
    service.zero_adverts = 0;
    service.flood_adverts = 0;
    service.status.running = true;
    service.status.context_in_psram = true;
    service.status.last_error = ESP_OK;
    service.status.channels = channel_count;
    service.status.contacts_loaded = context->mesh.contactsLoaded();
    service.status.packet_pool_free = context->mesh.packetPoolFree();
    strlcpy(service.status.radio, radio, sizeof(service.status.radio));
    strlcpy(service.status.profile, profile, sizeof(service.status.profile));
    service.status.generation++;
    unlock_service();
    (void)solar_os_messaging_provider_set_status(
        SOLAR_OS_MESSAGING_PROVIDER_MESHCORE,
        true,
        true,
        ESP_OK,
        "radio active");
    return ESP_OK;
}

extern "C" void solar_os_meshcore_loop_once(void)
{
    if (service.lock == nullptr) {
        return;
    }
    MeshcoreContext *context = nullptr;
    uint8_t zero = 0;
    uint8_t flood = 0;
    lock_service();
    context = service.context;
    if (service.zero_adverts > 0U) {
        service.zero_adverts--;
        zero = 1;
    }
    if (service.flood_adverts > 0U) {
        service.flood_adverts--;
        flood = 1;
    }
    unlock_service();
    if (context == nullptr) {
        return;
    }
    char name[SOLAR_OS_MESHCORE_NAME_MAX + 1U]{};
    if ((zero != 0U || flood != 0U) &&
        solar_os_meshcore_name_get(name) == ESP_OK) {
        if (zero != 0U) {
            context->mesh.sendAdvert(name, false);
        }
        if (flood != 0U) {
            context->mesh.sendAdvert(name, true);
        }
    }
    context->mesh.process();
    update_runtime_status(context);
}

extern "C" esp_err_t solar_os_meshcore_request_advert(
    solar_os_meshcore_advert_t advert)
{
    ESP_RETURN_ON_ERROR(solar_os_meshcore_init(), "meshcore", "init failed");
    if (advert != SOLAR_OS_MESHCORE_ADVERT_ZERO &&
        advert != SOLAR_OS_MESHCORE_ADVERT_FLOOD) {
        return ESP_ERR_INVALID_ARG;
    }
    lock_service();
    if (!service.status.running || service.context == nullptr) {
        unlock_service();
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t *queued =
        advert == SOLAR_OS_MESHCORE_ADVERT_ZERO ?
            &service.zero_adverts : &service.flood_adverts;
    if (*queued >= 4U) {
        unlock_service();
        return ESP_ERR_NO_MEM;
    }
    (*queued)++;
    unlock_service();
    return ESP_OK;
}

extern "C" void solar_os_meshcore_note_stack_watermark(uint32_t bytes)
{
    if (service.lock == nullptr) {
        return;
    }
    lock_service();
    if (service.status.stack_watermark_bytes == 0U ||
        bytes < service.status.stack_watermark_bytes) {
        service.status.stack_watermark_bytes = bytes;
    }
    unlock_service();
}

extern "C" void solar_os_meshcore_stop(void)
{
    if (service.lock == nullptr) {
        return;
    }
    MeshcoreContext *context = nullptr;
    solar_os_radio_status_t saved{};
    lock_service();
    if (service.transitioning) {
        unlock_service();
        return;
    }
    context = service.context;
    if (context == nullptr) {
        unlock_service();
        return;
    }
    service.transitioning = true;
    saved = service.saved_radio;
    service.context = nullptr;
    service.status.running = false;
    service.status.generation++;
    unlock_service();
    solar_os_meshcore_stream_transport_stop();
    solar_os_radio_handle_t handle = context->radio.handle();
    (void)solar_os_radio_handle_set_state(
        &handle, SOLAR_OS_RADIO_STATE_STANDBY);
    if (solar_os_radio_handle_configure(&handle, &saved.config) == ESP_OK &&
        saved.state != SOLAR_OS_RADIO_STATE_UNKNOWN) {
        (void)solar_os_radio_handle_set_state(&handle, saved.state);
    }
    context->~MeshcoreContext();
    solar_os_memory_free(context);
    (void)solar_os_radio_release(&handle);
    (void)solar_os_messaging_provider_set_status(
        SOLAR_OS_MESSAGING_PROVIDER_MESHCORE,
        false,
        false,
        ESP_OK,
        "stopped");
    lock_service();
    service.transitioning = false;
    unlock_service();
}

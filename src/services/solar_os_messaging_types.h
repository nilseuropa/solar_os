#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SOLAR_OS_MESSAGING_ADDRESS_MAX 32U
#define SOLAR_OS_MESSAGING_PROVIDER_METADATA_MAX 64U
#define SOLAR_OS_MESSAGING_TITLE_MAX 64U
#define SOLAR_OS_MESSAGING_LABEL_MAX 48U
#define SOLAR_OS_MESSAGING_BODY_MAX 4096U
#define SOLAR_OS_MESSAGING_ERROR_MAX 96U

typedef uint32_t solar_os_contact_id_t;
typedef uint32_t solar_os_endpoint_id_t;
typedef uint32_t solar_os_conversation_id_t;
typedef uint64_t solar_os_message_key_t;
typedef uint32_t solar_os_credential_id_t;

#define SOLAR_OS_CONTACT_ID_NONE ((solar_os_contact_id_t)0U)
#define SOLAR_OS_ENDPOINT_ID_NONE ((solar_os_endpoint_id_t)0U)
#define SOLAR_OS_CONVERSATION_ID_NONE ((solar_os_conversation_id_t)0U)
#define SOLAR_OS_MESSAGE_KEY_NONE ((solar_os_message_key_t)0U)
#define SOLAR_OS_CREDENTIAL_ID_NONE ((solar_os_credential_id_t)0U)

/* Persistent instance IDs. 1..3 retain the original default instances.
 * The byte-sized limit preserves Contacts/Credentials disk compatibility;
 * it is an identity-space bound, not a list of supported protocols. */
typedef uint32_t solar_os_messaging_provider_id_t;
#define SOLAR_OS_MESSAGING_INSTANCE_ID_MAX UINT8_MAX
enum {
    SOLAR_OS_MESSAGING_PROVIDER_GATEWAY = 1,
    SOLAR_OS_MESSAGING_PROVIDER_MESHCORE = 2,
    SOLAR_OS_MESSAGING_PROVIDER_LINK = 3,
};

typedef enum {
    SOLAR_OS_CONTACT_TRUST_DISCOVERED = 0,
    SOLAR_OS_CONTACT_TRUST_TRUSTED,
    SOLAR_OS_CONTACT_TRUST_BLOCKED,
} solar_os_contact_trust_t;

typedef enum {
    SOLAR_OS_CONVERSATION_DIRECT = 0,
    SOLAR_OS_CONVERSATION_GROUP,
    SOLAR_OS_CONVERSATION_ROOM,
    SOLAR_OS_CONVERSATION_BROADCAST,
} solar_os_conversation_kind_t;

typedef enum {
    SOLAR_OS_MESSAGE_INBOUND = 0,
    SOLAR_OS_MESSAGE_OUTBOUND,
} solar_os_message_direction_t;

typedef enum {
    SOLAR_OS_DELIVERY_RECEIVED = 0,
    SOLAR_OS_DELIVERY_QUEUED,
    SOLAR_OS_DELIVERY_SENDING,
    SOLAR_OS_DELIVERY_SENT,
    SOLAR_OS_DELIVERY_DELIVERED,
    SOLAR_OS_DELIVERY_FAILED,
    SOLAR_OS_DELIVERY_CANCELLED,
} solar_os_delivery_state_t;

typedef enum {
    SOLAR_OS_ENDPOINT_CAP_DIRECT = 1U << 0,
    SOLAR_OS_ENDPOINT_CAP_GROUP = 1U << 1,
    SOLAR_OS_ENDPOINT_CAP_ROOM = 1U << 2,
    SOLAR_OS_ENDPOINT_CAP_BROADCAST = 1U << 3,
    SOLAR_OS_ENDPOINT_CAP_ACK = 1U << 4,
} solar_os_endpoint_capability_t;

typedef enum {
    SOLAR_OS_SECURITY_ENCRYPTED = 1U << 0,
    SOLAR_OS_SECURITY_PEER_KEY_KNOWN = 1U << 1,
    SOLAR_OS_SECURITY_PEER_TRUSTED = 1U << 2,
    SOLAR_OS_SECURITY_SHARED_KEY = 1U << 3,
    SOLAR_OS_SECURITY_SENDER_UNVERIFIED = 1U << 4,
    SOLAR_OS_SECURITY_TRANSPORT_SECURED = 1U << 5,
    /* Readable by anyone who can reach the room, but each message carries a
     * sender signature that was checked (e.g. SC2 public rooms). */
    SOLAR_OS_SECURITY_PUBLIC_SIGNED = 1U << 6,
    /* Legacy protocol without end-to-end protection or sender authentication;
     * the server sees and can alter content (gateway v1). */
    SOLAR_OS_SECURITY_LEGACY_INSECURE = 1U << 7,
} solar_os_message_security_flag_t;

typedef enum {
    SOLAR_OS_CONTACT_FLAG_PINNED = 1U << 0,
} solar_os_contact_flag_t;

typedef struct {
    uint8_t bytes[SOLAR_OS_MESSAGING_ADDRESS_MAX];
    uint8_t length;
} solar_os_messaging_address_t;

typedef struct {
    uint8_t bytes[SOLAR_OS_MESSAGING_PROVIDER_METADATA_MAX];
    uint8_t length;
} solar_os_messaging_provider_metadata_t;

const char *solar_os_messaging_provider_name(
    solar_os_messaging_provider_id_t provider);
const char *solar_os_contact_trust_name(solar_os_contact_trust_t trust);
const char *solar_os_conversation_kind_name(solar_os_conversation_kind_t kind);
const char *solar_os_delivery_state_name(solar_os_delivery_state_t state);


#include "solar_os_messaging_types.h"

extern const char *solar_os_messaging_instance_name(solar_os_messaging_provider_id_t)
    __attribute__((weak));

const char *solar_os_messaging_provider_name(
    solar_os_messaging_provider_id_t provider)
{
    if (solar_os_messaging_instance_name != NULL) {
        const char *name = solar_os_messaging_instance_name(provider);
        if (name != NULL) return name;
    }
    switch (provider) {
    case SOLAR_OS_MESSAGING_PROVIDER_GATEWAY:
        return "gateway";
    case SOLAR_OS_MESSAGING_PROVIDER_MESHCORE:
        return "meshcore";
    case SOLAR_OS_MESSAGING_PROVIDER_LINK:
        return "link";
    default:
        return "unknown";
    }
}

const char *solar_os_contact_trust_name(solar_os_contact_trust_t trust)
{
    switch (trust) {
    case SOLAR_OS_CONTACT_TRUST_DISCOVERED:
        return "discovered";
    case SOLAR_OS_CONTACT_TRUST_TRUSTED:
        return "trusted";
    case SOLAR_OS_CONTACT_TRUST_BLOCKED:
        return "blocked";
    default:
        return "unknown";
    }
}

const char *solar_os_conversation_kind_name(solar_os_conversation_kind_t kind)
{
    switch (kind) {
    case SOLAR_OS_CONVERSATION_DIRECT:
        return "direct";
    case SOLAR_OS_CONVERSATION_GROUP:
        return "group";
    case SOLAR_OS_CONVERSATION_ROOM:
        return "room";
    case SOLAR_OS_CONVERSATION_BROADCAST:
        return "broadcast";
    default:
        return "unknown";
    }
}

const char *solar_os_delivery_state_name(solar_os_delivery_state_t state)
{
    switch (state) {
    case SOLAR_OS_DELIVERY_RECEIVED:
        return "received";
    case SOLAR_OS_DELIVERY_QUEUED:
        return "queued";
    case SOLAR_OS_DELIVERY_SENDING:
        return "sending";
    case SOLAR_OS_DELIVERY_SENT:
        return "sent";
    case SOLAR_OS_DELIVERY_DELIVERED:
        return "delivered";
    case SOLAR_OS_DELIVERY_FAILED:
        return "failed";
    case SOLAR_OS_DELIVERY_CANCELLED:
        return "cancelled";
    default:
        return "unknown";
    }
}


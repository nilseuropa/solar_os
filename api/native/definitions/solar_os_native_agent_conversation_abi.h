#pragma once

#include "solar_os_native_service_abi.h"

#define SOLAR_OS_NATIVE_AGENT_CONVERSATION_SERVICE "agent.conversation"
#define SOLAR_OS_NATIVE_AGENT_CONVERSATION_ABI 1U
#define SOLAR_OS_AGENT_CONVERSATION_ID_MAX 24U
#define SOLAR_OS_AGENT_CONVERSATION_TITLE_MAX 64U

#define SOLAR_OS_NATIVE_AGENT_MESSAGE_USER 0U
#define SOLAR_OS_NATIVE_AGENT_MESSAGE_ASSISTANT 1U
#define SOLAR_OS_NATIVE_AGENT_MESSAGE_TOOL 2U

typedef struct {
    char id[SOLAR_OS_AGENT_CONVERSATION_ID_MAX];
    char title[SOLAR_OS_AGENT_CONVERSATION_TITLE_MAX];
    char provider[24];
    char model[96];
    uint64_t created_at, updated_at;
    uint16_t turn_count;
    uint32_t stored_bytes;
} solar_os_native_agent_conversation_info_v1_t;

typedef struct {
    uint32_t role;
    const char *text;
    size_t length;
} solar_os_native_agent_message_v1_t;

typedef struct {
    const char *provider_response_id;
    const solar_os_native_agent_message_v1_t *messages;
    size_t count;
} solar_os_native_agent_history_v1_t;

typedef struct {
    const char *id;
    const char *provider;
    const char *model;
    const char *user_text;
    const char *assistant_text;
    const char *tool_summary;
    const char *provider_response_id;
} solar_os_native_agent_conversation_turn_v1_t;

typedef struct {
    uint32_t abi_version, struct_size;
    /* Metadata is copied. Listing returns at most capacity records. */
    int (*list)(solar_os_native_agent_conversation_info_v1_t *items, size_t capacity,
                size_t *count);
    int (*get)(const char *id, solar_os_native_agent_conversation_info_v1_t *info);
    /* Borrow message text until the visitor returns. A callback error ends
     * the visit. The store owns the order and retention of turns. */
    int (*visit)(const char *id, int (*visitor)(const solar_os_native_agent_message_v1_t *, void *),
                 void *user);
    int (*remove)(const char *id);
    /* Null id starts a conversation. The store assigns the ID. An empty or
     * missing tool summary adds no tool message. The destination must hold
     * SOLAR_OS_AGENT_CONVERSATION_ID_MAX bytes. */
    int (*commit)(const solar_os_native_agent_conversation_turn_v1_t *turn,
                   char *id, size_t capacity);
    /* Borrow the response ID and bounded history used for provider resume.
     * Set include_messages false to skip loading text. The visitor runs on
     * the caller task after reading and before releasing the snapshot. */
    int (*with_history)(const char *id, bool include_messages,
                         int (*visitor)(const solar_os_native_agent_history_v1_t *, void *),
                         void *user);
} solar_os_native_agent_conversation_api_v1_t;

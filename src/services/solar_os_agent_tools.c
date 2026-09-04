#include "solar_os_agent_tools.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "solar_os_agent_reference.h"
#include "solar_os_board.h"
#include "solar_os_board_caps.h"
#include "solar_os_config.h"
#include "solar_os_crypto.h"
#if SOLAR_OS_PACKAGE_SERVICE_BATTERY
#include "solar_os_battery.h"
#endif
#if SOLAR_OS_PACKAGE_SERVICE_RESOURCES
#include "solar_os_buses.h"
#endif
#include "solar_os_display.h"
#if SOLAR_OS_PACKAGE_SERVICE_GPIO
#include "solar_os_gpio.h"
#include "solar_os_pins.h"
#endif
#include "solar_os_jobs.h"
#include "solar_os_json.h"
#include "solar_os_memory.h"
#include "solar_os_task.h"
#if SOLAR_OS_PACKAGE_SERVICE_SENSORS
#include "solar_os_sensors.h"
#endif
#include "solar_os_storage.h"
#if SOLAR_OS_PACKAGE_SERVICE_WIFI
#include "solar_os_wifi.h"
#endif

#ifndef SOLAR_OS_VERSION
#define SOLAR_OS_VERSION "0.0.0"
#endif

#define AGENT_TOOL_STORAGE_ENTRY_MAX 16U
#define AGENT_TOOL_STORAGE_CONTENT_MAX 3072U
#define AGENT_TOOL_STORAGE_RANGE_MAX 2048U
#define AGENT_TOOL_STORAGE_SEARCH_MATCH_MAX 12U
#define AGENT_TOOL_STORAGE_SEARCH_PATH_MAX 32U
#define AGENT_TOOL_STORAGE_SEARCH_BYTES_MAX (64U * 1024U)
#define AGENT_TOOL_STORAGE_SEARCH_LINE_MAX 256U
#define AGENT_TOOL_STORAGE_PATCH_EDIT_MAX 8U
#define AGENT_TOOL_STORAGE_PATCH_INSERT_MAX 2048U
#define AGENT_TOOL_STORAGE_PATCH_FILE_MAX (128U * 1024U)
#define AGENT_TOOL_JSON_SCRATCH_MAX 512U
#define AGENT_TOOL_SCRIPT_SOURCE_MAX 640U
#define AGENT_TOOL_SCRIPT_OUTPUT_MAX 384U
#define AGENT_TOOL_JSON_ESCAPE_FACTOR 6U
#define AGENT_TOOL_SEARCH_QUERY_MAX 159U
#define AGENT_TOOL_SEARCH_MATCH_MAX \
    (SOLAR_OS_AGENT_TOOL_ACTIVE_MAX - 3U)
#define AGENT_TOOL_PROMPT_MATCH_MAX 2U
#define AGENT_TOOL_SEARCH_TOKEN_MAX 31U
#define AGENT_TOOL_JOB_NAME_MAX 32U

#define AGENT_TOOL_SCHEMA_EMPTY \
    "{\"type\":\"object\",\"properties\":{},\"required\":[]," \
    "\"additionalProperties\":false}"
#define AGENT_TOOL_SCHEMA_JOBS_LIST \
    "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"," \
    "\"maxLength\":31}},\"required\":[\"name\"]," \
    "\"additionalProperties\":false}"
#define AGENT_TOOL_SCHEMA_STORAGE_LIST \
    "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"," \
    "\"minLength\":1,\"maxLength\":159}},\"required\":[\"path\"]," \
    "\"additionalProperties\":false}"
#define AGENT_TOOL_SCHEMA_STORAGE_READ AGENT_TOOL_SCHEMA_STORAGE_LIST
#define AGENT_TOOL_SCHEMA_STORAGE_WRITE \
    "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"," \
    "\"minLength\":1,\"maxLength\":159},\"content\":{\"type\":\"string\"," \
    "\"maxLength\":3072}},\"required\":[\"path\",\"content\"]," \
    "\"additionalProperties\":false}"
#define AGENT_TOOL_SCHEMA_STORAGE_SEARCH \
    "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"," \
    "\"minLength\":1,\"maxLength\":159},\"query\":{\"type\":\"string\"," \
    "\"minLength\":1,\"maxLength\":64}},\"required\":[\"path\",\"query\"]," \
    "\"additionalProperties\":false}"
#define AGENT_TOOL_SCHEMA_STORAGE_READ_RANGE \
    "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"," \
    "\"minLength\":1,\"maxLength\":159},\"offset\":{\"type\":\"integer\"," \
    "\"minimum\":0,\"maximum\":524288},\"length\":{\"type\":\"integer\"," \
    "\"minimum\":1,\"maximum\":2048}},\"required\":[\"path\",\"offset\"," \
    "\"length\"],\"additionalProperties\":false}"
#define AGENT_TOOL_SCHEMA_STORAGE_PATCH \
    "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"," \
    "\"minLength\":1,\"maxLength\":159},\"expected_sha256\":{\"type\":\"string\"," \
    "\"minLength\":64,\"maxLength\":64},\"edits\":{\"type\":\"array\"," \
    "\"minItems\":1,\"maxItems\":8,\"items\":{\"type\":\"object\"," \
    "\"properties\":{\"offset\":{\"type\":\"integer\",\"minimum\":0," \
    "\"maximum\":131072},\"delete_bytes\":{\"type\":\"integer\",\"minimum\":0," \
    "\"maximum\":131072},\"insert\":{\"type\":\"string\",\"maxLength\":2048}}," \
    "\"required\":[\"offset\",\"delete_bytes\",\"insert\"]," \
    "\"additionalProperties\":false}}},\"required\":[\"path\"," \
    "\"expected_sha256\",\"edits\"],\"additionalProperties\":false}"
#define AGENT_TOOL_SCHEMA_SCRIPT_RUN \
    "{\"type\":\"object\",\"properties\":{\"source\":{\"type\":\"string\"," \
    "\"minLength\":1,\"maxLength\":640}},\"required\":[\"source\"]," \
    "\"additionalProperties\":false}"
#define AGENT_TOOL_SCHEMA_SCRIPT_RUN_FILE \
    "{\"type\":\"object\",\"properties\":{\"language\":{\"type\":\"string\"," \
    "\"enum\":[\"python\",\"lua\"]},\"path\":{\"type\":\"string\"," \
    "\"minLength\":1,\"maxLength\":159},\"args\":{\"type\":\"array\"," \
    "\"maxItems\":7,\"items\":{\"type\":\"string\",\"maxLength\":159}}}," \
    "\"required\":[\"language\",\"path\",\"args\"]," \
    "\"additionalProperties\":false}"
#define AGENT_TOOL_SCHEMA_REFERENCE \
    "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"," \
    "\"minLength\":1,\"maxLength\":64}},\"required\":[\"query\"]," \
    "\"additionalProperties\":false}"
#define AGENT_TOOL_SCHEMA_SEARCH \
    "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"," \
    "\"minLength\":1,\"maxLength\":159}},\"required\":[\"query\"]," \
    "\"additionalProperties\":false}"
#define AGENT_TOOL_SCHEMA_GPIO_READ \
    "{\"type\":\"object\",\"properties\":{\"pin\":{\"type\":\"integer\"," \
    "\"minimum\":0,\"maximum\":63}},\"required\":[\"pin\"]," \
    "\"additionalProperties\":false}"

#define AGENT_TOOL_OUTPUT_SYSTEM_STATUS \
    "{\"type\":\"object\",\"properties\":{\"board\":{\"type\":\"string\"}," \
    "\"version\":{\"type\":\"string\"},\"uptime_ms\":{\"type\":\"integer\"}," \
    "\"internal_free_bytes\":{\"type\":\"integer\"}," \
    "\"internal_largest_block_bytes\":{\"type\":\"integer\"}," \
    "\"psram_free_bytes\":{\"type\":\"integer\"}}," \
    "\"additionalProperties\":false}"
#define AGENT_TOOL_OUTPUT_STORAGE_LIST \
    "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}," \
    "\"entries\":{\"type\":\"array\"},\"truncated\":{\"type\":\"boolean\"}}," \
    "\"additionalProperties\":false}"
#define AGENT_TOOL_OUTPUT_STORAGE_STAT \
    "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}," \
    "\"exists\":{\"type\":\"boolean\"},\"type\":{\"type\":\"string\"}," \
    "\"size_bytes\":{\"type\":\"integer\"}},\"required\":[\"path\",\"exists\"," \
    "\"type\",\"size_bytes\"],\"additionalProperties\":false}"
#define AGENT_TOOL_OUTPUT_STORAGE_READ \
    "{\"type\":\"object\",\"properties\":{\"ok\":{\"type\":\"boolean\"}," \
    "\"path\":{\"type\":\"string\"},\"size_bytes\":{\"type\":\"integer\"}," \
    "\"content\":{\"type\":\"string\"},\"truncated\":{\"type\":\"boolean\"}}," \
    "\"required\":[\"ok\",\"path\",\"size_bytes\",\"content\",\"truncated\"]," \
    "\"additionalProperties\":false}"
#define AGENT_TOOL_OUTPUT_STORAGE_WRITE \
    "{\"type\":\"object\",\"properties\":{\"ok\":{\"type\":\"boolean\"}," \
    "\"path\":{\"type\":\"string\"},\"bytes_written\":{\"type\":\"integer\"}}," \
    "\"required\":[\"ok\",\"path\",\"bytes_written\"]," \
    "\"additionalProperties\":false}"
#define AGENT_TOOL_OUTPUT_STORAGE_SEARCH \
    "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}," \
    "\"query\":{\"type\":\"string\"},\"matches\":{\"type\":\"array\"}," \
    "\"files_scanned\":{\"type\":\"integer\"},\"bytes_scanned\":" \
    "{\"type\":\"integer\"},\"truncated\":{\"type\":\"boolean\"}}," \
    "\"additionalProperties\":false}"
#define AGENT_TOOL_OUTPUT_STORAGE_READ_RANGE \
    "{\"type\":\"object\",\"properties\":{\"ok\":{\"type\":\"boolean\"}," \
    "\"path\":{\"type\":\"string\"},\"size_bytes\":{\"type\":\"integer\"}," \
    "\"sha256\":{\"type\":\"string\"},\"offset\":{\"type\":\"integer\"}," \
    "\"bytes_returned\":{\"type\":\"integer\"},\"next_offset\":" \
    "{\"type\":\"integer\"},\"eof\":{\"type\":\"boolean\"},\"content\":" \
    "{\"type\":\"string\"},\"truncated\":{\"type\":\"boolean\"}}," \
    "\"additionalProperties\":false}"
#define AGENT_TOOL_OUTPUT_STORAGE_PATCH \
    "{\"type\":\"object\",\"properties\":{\"ok\":{\"type\":\"boolean\"}," \
    "\"conflict\":{\"type\":\"boolean\"},\"path\":{\"type\":\"string\"}," \
    "\"expected_sha256\":{\"type\":\"string\"},\"current_sha256\":" \
    "{\"type\":\"string\"},\"new_sha256\":{\"type\":\"string\"}," \
    "\"bytes_written\":{\"type\":\"integer\"}},\"additionalProperties\":false}"
#define AGENT_TOOL_OUTPUT_JOBS_LIST \
    "{\"type\":\"object\",\"properties\":{\"memory\":{\"type\":\"object\"," \
    "\"properties\":{\"internal_free_bytes\":{\"type\":\"integer\"}," \
    "\"internal_largest_block_bytes\":{\"type\":\"integer\"}," \
    "\"psram_free_bytes\":{\"type\":\"integer\"}," \
    "\"psram_largest_block_bytes\":{\"type\":\"integer\"}," \
    "\"background_internal_reserve_bytes\":{\"type\":\"integer\"}," \
    "\"task_internal_overhead_bytes\":{\"type\":\"integer\"}," \
    "\"external_stacks_supported\":{\"type\":\"boolean\"}}," \
    "\"additionalProperties\":false},\"jobs\":{\"type\":\"array\",\"items\":" \
    "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"}," \
    "\"kind\":{\"type\":\"string\"},\"state\":{\"type\":\"string\"}," \
    "\"generation\":{\"type\":\"integer\"},\"start_disposition\":" \
    "{\"type\":\"string\"},\"start_reason\":{\"type\":\"string\"}," \
    "\"last_error\":{\"type\":\"string\"},\"worker_stack_bytes\":" \
    "{\"type\":\"integer\"},\"worker_stack_region\":{\"type\":\"string\"}," \
    "\"owner\":{\"type\":\"string\"},\"resources_current\":{\"type\":" \
    "\"boolean\"},\"resources\":{\"type\":\"array\",\"items\":{\"type\":" \
    "\"object\",\"properties\":{\"type\":{\"type\":\"string\"},\"name\":" \
    "{\"type\":\"string\"},\"detail\":{\"type\":\"string\"}}," \
    "\"additionalProperties\":false}}},\"additionalProperties\":false}}," \
    "\"count\":{\"type\":\"integer\"},\"total\":{\"type\":\"integer\"}," \
    "\"truncated\":{\"type\":\"boolean\"}},\"additionalProperties\":false}"
#define AGENT_TOOL_OUTPUT_DISPLAY_LIST \
    "{\"type\":\"object\",\"properties\":{\"count\":{\"type\":\"integer\"}," \
    "\"displays\":{\"type\":\"array\",\"items\":{\"type\":\"object\"," \
    "\"properties\":{\"name\":{\"type\":\"string\"},\"source\":" \
    "{\"type\":\"string\"},\"driver\":{\"type\":\"string\"},\"controller\":" \
    "{\"type\":\"string\"},\"width\":{\"type\":\"integer\"},\"height\":" \
    "{\"type\":\"integer\"},\"role\":{\"type\":\"string\"},\"ready\":" \
    "{\"type\":\"boolean\"},\"brightness_supported\":{\"type\":\"boolean\"}," \
    "\"owner\":{\"type\":\"string\"}},\"additionalProperties\":false}}," \
    "\"truncated\":{\"type\":\"boolean\"}},\"additionalProperties\":false}"
#define AGENT_TOOL_OUTPUT_SCRIPT_RUN \
    "{\"type\":\"object\",\"properties\":{\"ok\":{\"type\":\"boolean\"}," \
    "\"status\":{\"type\":\"string\"},\"output\":{\"type\":\"string\"}," \
    "\"output_truncated\":{\"type\":\"boolean\"}," \
    "\"cancelled\":{\"type\":\"boolean\"}," \
    "\"timed_out\":{\"type\":\"boolean\"},\"error\":{\"type\":\"string\"}}," \
    "\"required\":[\"ok\",\"status\",\"output\",\"output_truncated\"," \
    "\"cancelled\",\"timed_out\",\"error\"]," \
    "\"additionalProperties\":false}"
#define AGENT_TOOL_OUTPUT_REFERENCE \
    "{\"type\":\"object\",\"properties\":{\"guidance\":{\"type\":\"string\"}," \
    "\"count\":{\"type\":\"integer\"}," \
    "\"matches\":{\"type\":\"array\",\"items\":{\"type\":\"object\"," \
    "\"properties\":{\"topic\":{\"type\":\"string\"},\"section\":" \
    "{\"type\":\"string\"},\"part\":{\"type\":\"integer\"},\"parts\":" \
    "{\"type\":\"integer\"},\"reference\":{\"type\":\"string\"}}," \
    "\"required\":[\"topic\",\"reference\"]," \
    "\"additionalProperties\":false}}},\"required\":[\"guidance\",\"count\"," \
    "\"matches\"]," \
    "\"additionalProperties\":false}"
#define AGENT_TOOL_OUTPUT_SEARCH \
    "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"}," \
    "\"count\":{\"type\":\"integer\"},\"tools\":{\"type\":\"array\"," \
    "\"items\":{\"type\":\"object\",\"properties\":{\"name\":" \
    "{\"type\":\"string\"},\"domain\":{\"type\":\"string\"}," \
    "\"description\":{\"type\":\"string\"},\"risk\":{\"type\":\"string\"}}," \
    "\"required\":[\"name\",\"domain\",\"description\",\"risk\"]," \
    "\"additionalProperties\":false}}},\"required\":[\"query\",\"count\"," \
    "\"tools\"],\"additionalProperties\":false}"
#define AGENT_TOOL_OUTPUT_HARDWARE_DESCRIBE \
    "{\"type\":\"object\",\"properties\":{\"board\":{\"type\":\"string\"}," \
    "\"name\":{\"type\":\"string\"},\"capabilities\":{\"type\":\"string\"}," \
    "\"psram_bytes\":{\"type\":\"integer\"}},\"required\":[\"board\",\"name\"," \
    "\"capabilities\",\"psram_bytes\"],\"additionalProperties\":false}"
#define AGENT_TOOL_OUTPUT_GPIO_LIST \
    "{\"type\":\"object\",\"properties\":{\"count\":{\"type\":\"integer\"}," \
    "\"pins\":{\"type\":\"array\"},\"truncated\":{\"type\":\"boolean\"}}," \
    "\"required\":[\"count\",\"pins\",\"truncated\"]," \
    "\"additionalProperties\":false}"
#define AGENT_TOOL_OUTPUT_GPIO_READ \
    "{\"type\":\"object\",\"properties\":{\"pin\":{\"type\":\"integer\"}," \
    "\"configured\":{\"type\":\"boolean\"},\"readable\":{\"type\":\"boolean\"}," \
    "\"level\":{\"type\":[\"boolean\",\"null\"]},\"owner\":{\"type\":\"string\"}}," \
    "\"required\":[\"pin\",\"configured\",\"readable\",\"level\",\"owner\"]," \
    "\"additionalProperties\":false}"
#define AGENT_TOOL_OUTPUT_BUSES_LIST \
    "{\"type\":\"object\",\"properties\":{\"count\":{\"type\":\"integer\"}," \
    "\"buses\":{\"type\":\"array\"},\"truncated\":{\"type\":\"boolean\"}}," \
    "\"required\":[\"count\",\"buses\",\"truncated\"]," \
    "\"additionalProperties\":false}"
#define AGENT_TOOL_OUTPUT_NETWORK_STATUS \
    "{\"type\":\"object\",\"properties\":{\"state\":{\"type\":\"string\"}," \
    "\"started\":{\"type\":\"boolean\"},\"connected\":{\"type\":\"boolean\"}," \
    "\"ssid\":{\"type\":\"string\"},\"ip\":{\"type\":\"string\"}," \
    "\"gateway\":{\"type\":\"string\"},\"rssi\":{\"type\":\"integer\"}," \
    "\"channel\":{\"type\":\"integer\"},\"ap_running\":{\"type\":\"boolean\"}," \
    "\"ap_station_count\":{\"type\":\"integer\"},\"nat_active\":" \
    "{\"type\":\"boolean\"}},\"additionalProperties\":false}"
#define AGENT_TOOL_OUTPUT_SENSORS_READ \
    "{\"type\":\"object\",\"properties\":{\"battery\":{\"type\":[\"object\"," \
    "\"null\"]},\"environment\":{\"type\":[\"object\",\"null\"]}}," \
    "\"required\":[\"battery\",\"environment\"]," \
    "\"additionalProperties\":false}"

typedef esp_err_t (*agent_tool_execute_fn)(const char *arguments,
                                           const solar_os_agent_request_t *request,
                                           char *result,
                                           size_t result_len);
typedef bool (*agent_tool_available_fn)(void);

typedef struct {
    solar_os_agent_tool_descriptor_t provider;
    const char *domain;
    const char *search_terms;
    const char *output_schema_json;
    const char *required_capability;
    solar_os_agent_tool_risk_t risk;
    uint32_t required_script_language;
    bool bootstrap;
    bool activate_on_demand;
    agent_tool_available_fn available;
    agent_tool_execute_fn execute;
} agent_tool_definition_t;

typedef struct {
    char *buffer;
    size_t capacity;
    size_t length;
} agent_tool_output_t;

typedef struct {
    const agent_tool_definition_t *definition;
    unsigned score;
} agent_tool_search_match_t;

typedef struct {
    uint32_t offset;
    uint32_t delete_bytes;
    char insert[AGENT_TOOL_STORAGE_PATCH_INSERT_MAX + 1U];
} agent_tool_storage_patch_edit_t;

static esp_err_t agent_tool_system_status(const char *arguments,
                                           const solar_os_agent_request_t *request,
                                           char *result,
                                           size_t result_len);
static esp_err_t agent_tool_solaros_reference(
    const char *arguments,
    const solar_os_agent_request_t *request,
    char *result,
    size_t result_len);
static esp_err_t agent_tool_search(const char *arguments,
                                   const solar_os_agent_request_t *request,
                                   char *result,
                                   size_t result_len);
static esp_err_t agent_tool_storage_list(const char *arguments,
                                         const solar_os_agent_request_t *request,
                                         char *result,
                                         size_t result_len);
static esp_err_t agent_tool_storage_stat(const char *arguments,
                                         const solar_os_agent_request_t *request,
                                         char *result,
                                         size_t result_len);
static esp_err_t agent_tool_storage_read(const char *arguments,
                                         const solar_os_agent_request_t *request,
                                         char *result,
                                         size_t result_len);
static esp_err_t agent_tool_storage_write(const char *arguments,
                                          const solar_os_agent_request_t *request,
                                          char *result,
                                          size_t result_len);
static esp_err_t agent_tool_storage_search(
    const char *arguments,
    const solar_os_agent_request_t *request,
    char *result,
    size_t result_len);
static esp_err_t agent_tool_storage_read_range(
    const char *arguments,
    const solar_os_agent_request_t *request,
    char *result,
    size_t result_len);
static esp_err_t agent_tool_storage_patch(
    const char *arguments,
    const solar_os_agent_request_t *request,
    char *result,
    size_t result_len);
static esp_err_t agent_tool_jobs_list(const char *arguments,
                                      const solar_os_agent_request_t *request,
                                      char *result,
                                      size_t result_len);
static esp_err_t agent_tool_display_list(const char *arguments,
                                         const solar_os_agent_request_t *request,
                                         char *result,
                                         size_t result_len);
static esp_err_t agent_tool_hardware_describe(
    const char *arguments,
    const solar_os_agent_request_t *request,
    char *result,
    size_t result_len);
static esp_err_t agent_tool_gpio_list(const char *arguments,
                                      const solar_os_agent_request_t *request,
                                      char *result,
                                      size_t result_len);
static esp_err_t agent_tool_gpio_read(const char *arguments,
                                      const solar_os_agent_request_t *request,
                                      char *result,
                                      size_t result_len);
static esp_err_t agent_tool_buses_list(const char *arguments,
                                       const solar_os_agent_request_t *request,
                                       char *result,
                                       size_t result_len);
static esp_err_t agent_tool_network_status(
    const char *arguments,
    const solar_os_agent_request_t *request,
    char *result,
    size_t result_len);
static esp_err_t agent_tool_sensors_read(const char *arguments,
                                         const solar_os_agent_request_t *request,
                                         char *result,
                                         size_t result_len);
static esp_err_t agent_tool_script_run_python(
    const char *arguments,
    const solar_os_agent_request_t *request,
    char *result,
    size_t result_len);
static esp_err_t agent_tool_script_run_lua(
    const char *arguments,
    const solar_os_agent_request_t *request,
    char *result,
    size_t result_len);
static esp_err_t agent_tool_script_run_file(
    const char *arguments,
    const solar_os_agent_request_t *request,
    char *result,
    size_t result_len);

static bool agent_tool_storage_available(void)
{
    return solar_os_storage_is_mounted();
}

static bool agent_tool_display_available(void)
{
    return solar_os_board_has(SOLAR_OS_BOARD_CAP_GFX);
}

static bool agent_tool_gpio_available(void)
{
#if SOLAR_OS_PACKAGE_SERVICE_GPIO
    return solar_os_board_has(SOLAR_OS_BOARD_CAP_GPIO);
#else
    return false;
#endif
}

static bool agent_tool_buses_available(void)
{
#if SOLAR_OS_PACKAGE_SERVICE_RESOURCES
    return true;
#else
    return false;
#endif
}

static bool agent_tool_network_available(void)
{
#if SOLAR_OS_PACKAGE_SERVICE_WIFI
    return solar_os_board_has(SOLAR_OS_BOARD_CAP_WIFI);
#else
    return false;
#endif
}

static bool agent_tool_sensors_available(void)
{
#if SOLAR_OS_PACKAGE_SERVICE_BATTERY || SOLAR_OS_PACKAGE_SERVICE_SENSORS
    return true;
#else
    return false;
#endif
}

static bool agent_tool_python_available(void)
{
#if SOLAR_OS_PACKAGE_APP_PYTHON
    return true;
#else
    return false;
#endif
}

static bool agent_tool_lua_available(void)
{
#if SOLAR_OS_PACKAGE_APP_LUA
    return true;
#else
    return false;
#endif
}

static bool agent_tool_script_file_available(void)
{
    return agent_tool_python_available() || agent_tool_lua_available();
}

static const agent_tool_definition_t AGENT_TOOL_REGISTRY[] = {
    {
        .provider = {
            .name = "system_status",
            .description =
                "Read the SolarOS board identity, uptime, firmware version, "
                "and current internal RAM and PSRAM availability.",
            .parameters_json = AGENT_TOOL_SCHEMA_EMPTY,
            .strict = true,
        },
        .domain = "system",
        .search_terms = "status memory ram psram uptime board version",
        .output_schema_json = AGENT_TOOL_OUTPUT_SYSTEM_STATUS,
        .risk = SOLAR_OS_AGENT_TOOL_RISK_READ_ONLY,
        .bootstrap = true,
        .execute = agent_tool_system_status,
    },
    {
        .provider = {
            .name = "solaros_reference",
            .description =
                "Search focused firmware-matched SolarOS Python and Lua manual "
                "excerpts and mandatory coding guidance. Pass exactly one "
                "field named query that combines the language and task, for "
                "example {\"query\":\"lua gfx drawing\"}. Make one "
                "comprehensive query before writing or running code, and "
                "follow returned constants, patterns, targets, and capability "
                "constraints exactly.",
            .parameters_json = AGENT_TOOL_SCHEMA_REFERENCE,
            .strict = true,
        },
        .domain = "reference",
        .search_terms = "manual documentation api python lua coding help",
        .output_schema_json = AGENT_TOOL_OUTPUT_REFERENCE,
        .risk = SOLAR_OS_AGENT_TOOL_RISK_READ_ONLY,
        .bootstrap = true,
        .execute = agent_tool_solaros_reference,
    },
    {
        .provider = {
            .name = "tool_search",
            .description =
                "Find and activate up to five installed SolarOS tools relevant "
                "to a task. Call this before attempting storage, display, "
                "hardware, GPIO, bus, network, sensor, or script operations "
                "whose tools are not currently visible.",
            .parameters_json = AGENT_TOOL_SCHEMA_SEARCH,
            .strict = true,
        },
        .domain = "tools",
        .search_terms = "discover find activate capabilities operations",
        .output_schema_json = AGENT_TOOL_OUTPUT_SEARCH,
        .risk = SOLAR_OS_AGENT_TOOL_RISK_READ_ONLY,
        .bootstrap = true,
        .execute = agent_tool_search,
    },
    {
        .provider = {
            .name = "storage_list",
            .description =
                "List up to 16 entries in one SolarOS storage directory. "
                "Relative paths use the invoking shell directory. This reads "
                "names, types, and sizes only; use storage_stat to check one "
                "exact path.",
            .parameters_json = AGENT_TOOL_SCHEMA_STORAGE_LIST,
            .strict = true,
        },
        .domain = "storage",
        .search_terms = "files directories filesystem inspect browse",
        .output_schema_json = AGENT_TOOL_OUTPUT_STORAGE_LIST,
        .required_capability = "storage",
        .risk = SOLAR_OS_AGENT_TOOL_RISK_READ_ONLY,
        .available = agent_tool_storage_available,
        .execute = agent_tool_storage_list,
    },
    {
        .provider = {
            .name = "storage_stat",
            .description =
                "Check whether one exact SolarOS path exists and return its "
                "resolved path, type, and size. Relative paths use the "
                "invoking shell directory. Use this for file-existence "
                "questions; it does not search file contents.",
            .parameters_json = AGENT_TOOL_SCHEMA_STORAGE_LIST,
            .strict = true,
        },
        .domain = "storage",
        .search_terms =
            "see exists existence locate exact filename path stat metadata size",
        .output_schema_json = AGENT_TOOL_OUTPUT_STORAGE_STAT,
        .required_capability = "storage",
        .risk = SOLAR_OS_AGENT_TOOL_RISK_READ_ONLY,
        .available = agent_tool_storage_available,
        .execute = agent_tool_storage_stat,
    },
    {
        .provider = {
            .name = "storage_read",
            .description =
                "Read one SolarOS text file, except files below .ssh. "
                "Relative paths use the invoking shell directory. The result "
                "is bounded and reports truncation.",
            .parameters_json = AGENT_TOOL_SCHEMA_STORAGE_READ,
            .strict = true,
        },
        .domain = "storage",
        .search_terms = "file content open inspect load text",
        .output_schema_json = AGENT_TOOL_OUTPUT_STORAGE_READ,
        .required_capability = "storage",
        .risk = SOLAR_OS_AGENT_TOOL_RISK_SENSITIVE_READ,
        .available = agent_tool_storage_available,
        .execute = agent_tool_storage_read,
    },
    {
        .provider = {
            .name = "storage_write",
            .description =
                "Replace or create one SolarOS text file with up to 3072 "
                "bytes, except files below .ssh. Relative paths use the "
                "invoking shell directory.",
            .parameters_json = AGENT_TOOL_SCHEMA_STORAGE_WRITE,
            .strict = true,
        },
        .domain = "storage",
        .search_terms = "file content edit modify create save replace",
        .output_schema_json = AGENT_TOOL_OUTPUT_STORAGE_WRITE,
        .required_capability = "storage",
        .risk = SOLAR_OS_AGENT_TOOL_RISK_MUTATING,
        .available = agent_tool_storage_available,
        .execute = agent_tool_storage_write,
    },
    {
        .provider = {
            .name = "storage_search",
            .description =
                "Search file contents under one file or directory and return "
                "matching paths, line numbers, and excerpts. This does not "
                "search file names or test whether an exact path exists. "
                "Relative paths use the invoking shell directory.",
            .parameters_json = AGENT_TOOL_SCHEMA_STORAGE_SEARCH,
            .strict = true,
        },
        .domain = "storage",
        .search_terms =
            "search find grep text code configuration files directories debug",
        .output_schema_json = AGENT_TOOL_OUTPUT_STORAGE_SEARCH,
        .required_capability = "storage",
        .risk = SOLAR_OS_AGENT_TOOL_RISK_SENSITIVE_READ,
        .available = agent_tool_storage_available,
        .execute = agent_tool_storage_search,
    },
    {
        .provider = {
            .name = "storage_read_range",
            .description =
                "Read a bounded byte range from one text file and return its "
                "complete-file SHA-256 for conflict-safe editing. Relative "
                "paths use the invoking shell directory.",
            .parameters_json = AGENT_TOOL_SCHEMA_STORAGE_READ_RANGE,
            .strict = true,
        },
        .domain = "storage",
        .search_terms =
            "read inspect range chunk offset large file code sha256 edit debug",
        .output_schema_json = AGENT_TOOL_OUTPUT_STORAGE_READ_RANGE,
        .required_capability = "storage",
        .risk = SOLAR_OS_AGENT_TOOL_RISK_SENSITIVE_READ,
        .available = agent_tool_storage_available,
        .execute = agent_tool_storage_read_range,
    },
    {
        .provider = {
            .name = "storage_patch",
            .description =
                "Apply ascending non-overlapping byte-offset edits to one "
                "text file using its expected SHA-256 version. Relative paths "
                "use the invoking shell directory. Stale edits return a "
                "recoverable conflict and replacement retains a rollback "
                "copy until the new path is installed.",
            .parameters_json = AGENT_TOOL_SCHEMA_STORAGE_PATCH,
            .strict = true,
        },
        .domain = "storage",
        .search_terms =
            "patch edit modify replace insert delete code file sha256 safe",
        .output_schema_json = AGENT_TOOL_OUTPUT_STORAGE_PATCH,
        .required_capability = "storage",
        .risk = SOLAR_OS_AGENT_TOOL_RISK_MUTATING,
        .available = agent_tool_storage_available,
        .execute = agent_tool_storage_patch,
    },
    {
        .provider = {
            .name = "jobs_list",
            .description =
                "Inspect SolarOS background workloads with current memory "
                "headroom, start admission, wait/failure reasons, generation, "
                "worker-stack requirements, and current resource claims. Pass "
                "an empty name to list jobs or a job name for one complete "
                "record.",
            .parameters_json = AGENT_TOOL_SCHEMA_JOBS_LIST,
            .strict = true,
        },
        .domain = "jobs",
        .search_terms = "background workers tasks process memory state",
        .output_schema_json = AGENT_TOOL_OUTPUT_JOBS_LIST,
        .risk = SOLAR_OS_AGENT_TOOL_RISK_READ_ONLY,
        .execute = agent_tool_jobs_list,
    },
    {
        .provider = {
            .name = "display_list",
            .description =
                "List registered SolarOS displays with their real target "
                "names, size, readiness, role, driver, and current owner. "
                "Call this before writing graphics code for an attached "
                "display, and use only a returned ready target name.",
            .parameters_json = AGENT_TOOL_SCHEMA_EMPTY,
            .strict = true,
        },
        .domain = "display",
        .search_terms = "graphics gfx screen oled lcd target attached",
        .output_schema_json = AGENT_TOOL_OUTPUT_DISPLAY_LIST,
        .required_capability = "gfx",
        .risk = SOLAR_OS_AGENT_TOOL_RISK_READ_ONLY,
        .available = agent_tool_display_available,
        .execute = agent_tool_display_list,
    },
    {
        .provider = {
            .name = "hardware_describe",
            .description =
                "Describe the compiled SolarOS board and its actual hardware "
                "capabilities. Use this before assuming peripherals exist.",
            .parameters_json = AGENT_TOOL_SCHEMA_EMPTY,
            .strict = true,
        },
        .domain = "hardware",
        .search_terms =
            "board capabilities peripherals gpio buses wifi battery sensors",
        .output_schema_json = AGENT_TOOL_OUTPUT_HARDWARE_DESCRIBE,
        .risk = SOLAR_OS_AGENT_TOOL_RISK_READ_ONLY,
        .execute = agent_tool_hardware_describe,
    },
    {
        .provider = {
            .name = "gpio_list",
            .description =
                "List real board GPIO slots, runtime policy, current claims, "
                "configuration, and readable levels without changing pins.",
            .parameters_json = AGENT_TOOL_SCHEMA_EMPTY,
            .strict = true,
        },
        .domain = "gpio",
        .search_terms = "pins digital input output level claims expansion",
        .output_schema_json = AGENT_TOOL_OUTPUT_GPIO_LIST,
        .required_capability = "gpio",
        .risk = SOLAR_OS_AGENT_TOOL_RISK_READ_ONLY,
        .available = agent_tool_gpio_available,
        .execute = agent_tool_gpio_list,
    },
    {
        .provider = {
            .name = "gpio_read",
            .description =
                "Inspect one real GPIO slot and return its level only when it "
                "is already configured and readable. This never configures or "
                "claims a pin.",
            .parameters_json = AGENT_TOOL_SCHEMA_GPIO_READ,
            .strict = true,
        },
        .domain = "gpio",
        .search_terms = "pin digital input level state inspect",
        .output_schema_json = AGENT_TOOL_OUTPUT_GPIO_READ,
        .required_capability = "gpio",
        .risk = SOLAR_OS_AGENT_TOOL_RISK_READ_ONLY,
        .available = agent_tool_gpio_available,
        .execute = agent_tool_gpio_read,
    },
    {
        .provider = {
            .name = "buses_list",
            .description =
                "List registered I2C, SPI, UART, MIDI, OneWire, and PS/2 buses with their "
                "real names, pins, readiness, origin, sharing, and leases.",
            .parameters_json = AGENT_TOOL_SCHEMA_EMPTY,
            .strict = true,
        },
        .domain = "buses",
        .search_terms =
            "bus i2c spi uart midi onewire ps2 serial pins names leases hardware",
        .output_schema_json = AGENT_TOOL_OUTPUT_BUSES_LIST,
        .required_capability = "resources",
        .risk = SOLAR_OS_AGENT_TOOL_RISK_READ_ONLY,
        .available = agent_tool_buses_available,
        .execute = agent_tool_buses_list,
    },
    {
        .provider = {
            .name = "network_status",
            .description =
                "Read current Wi-Fi station, IP, access-point, signal, and NAT "
                "state without changing the active provider connection.",
            .parameters_json = AGENT_TOOL_SCHEMA_EMPTY,
            .strict = true,
        },
        .domain = "network",
        .search_terms =
            "wifi wireless connection ip gateway ssid rssi access point nat",
        .output_schema_json = AGENT_TOOL_OUTPUT_NETWORK_STATUS,
        .required_capability = "wifi",
        .risk = SOLAR_OS_AGENT_TOOL_RISK_READ_ONLY,
        .available = agent_tool_network_available,
        .execute = agent_tool_network_status,
    },
    {
        .provider = {
            .name = "sensors_read",
            .description =
                "Read installed battery and environmental sensors. Missing "
                "package-gated sensor families are returned as null.",
            .parameters_json = AGENT_TOOL_SCHEMA_EMPTY,
            .strict = true,
        },
        .domain = "sensors",
        .search_terms =
            "battery voltage percent power temperature humidity environment",
        .output_schema_json = AGENT_TOOL_OUTPUT_SENSORS_READ,
        .required_capability = "battery-or-environment",
        .risk = SOLAR_OS_AGENT_TOOL_RISK_READ_ONLY,
        .available = agent_tool_sensors_available,
        .execute = agent_tool_sensors_read,
    },
    {
        .provider = {
            .name = "script_run_python",
            .description =
                "Execute a bounded MicroPython source string locally with "
                "SolarOS APIs. This can read or change device state and must "
                "be locally confirmed unless unrestricted tools are enabled.",
            .parameters_json = AGENT_TOOL_SCHEMA_SCRIPT_RUN,
            .strict = true,
        },
        .domain = "script",
        .search_terms = "python micropython execute run code program",
        .output_schema_json = AGENT_TOOL_OUTPUT_SCRIPT_RUN,
        .required_capability = "python",
        .risk = SOLAR_OS_AGENT_TOOL_RISK_DISRUPTIVE,
        .required_script_language = SOLAR_OS_AGENT_SCRIPT_PYTHON,
        .available = agent_tool_python_available,
        .execute = agent_tool_script_run_python,
    },
    {
        .provider = {
            .name = "script_run_lua",
            .description =
                "Execute a bounded Lua source string locally with SolarOS "
                "APIs. This can read or change device state and must be "
                "locally confirmed unless unrestricted tools are enabled.",
            .parameters_json = AGENT_TOOL_SCHEMA_SCRIPT_RUN,
            .strict = true,
        },
        .domain = "script",
        .search_terms = "lua execute run code program",
        .output_schema_json = AGENT_TOOL_OUTPUT_SCRIPT_RUN,
        .required_capability = "lua",
        .risk = SOLAR_OS_AGENT_TOOL_RISK_DISRUPTIVE,
        .required_script_language = SOLAR_OS_AGENT_SCRIPT_LUA,
        .available = agent_tool_lua_available,
        .execute = agent_tool_script_run_lua,
    },
    {
        .provider = {
            .name = "script_run_file",
            .description =
                "Run one saved Python or Lua file with bounded arguments, "
                "captured output, cancellation, and a 30-second deadline.",
            .parameters_json = AGENT_TOOL_SCHEMA_SCRIPT_RUN_FILE,
            .strict = true,
        },
        .domain = "script",
        .search_terms =
            "run execute saved file python lua test debug program arguments",
        .output_schema_json = AGENT_TOOL_OUTPUT_SCRIPT_RUN,
        .required_capability = "python-or-lua",
        .risk = SOLAR_OS_AGENT_TOOL_RISK_DISRUPTIVE,
        .required_script_language =
            SOLAR_OS_AGENT_SCRIPT_PYTHON | SOLAR_OS_AGENT_SCRIPT_LUA,
        .activate_on_demand = true,
        .available = agent_tool_script_file_available,
        .execute = agent_tool_script_run_file,
    },
};

static const size_t AGENT_TOOL_COUNT =
    sizeof(AGENT_TOOL_REGISTRY) / sizeof(AGENT_TOOL_REGISTRY[0]);
_Static_assert(
    sizeof(AGENT_TOOL_REGISTRY) / sizeof(AGENT_TOOL_REGISTRY[0]) <=
        SOLAR_OS_AGENT_TOOL_REGISTRY_MAX,
    "agent tool registry exceeds SOLAR_OS_AGENT_TOOL_REGISTRY_MAX");

static bool agent_tool_is_available(
    const agent_tool_definition_t *definition,
    const solar_os_agent_request_t *request)
{
    if (definition == NULL ||
        (definition->available != NULL && !definition->available())) {
        return false;
    }
    if (definition->required_script_language == 0U || request == NULL) {
        return true;
    }
    return request->run_script != NULL &&
        (request->script_languages & definition->required_script_language) != 0U;
}

static bool agent_tool_contains_ci(const char *text, const char *needle)
{
    if (text == NULL || needle == NULL || needle[0] == '\0') {
        return false;
    }
    const size_t needle_len = strlen(needle);
    for (const char *cursor = text; *cursor != '\0'; cursor++) {
        if (strncasecmp(cursor, needle, needle_len) == 0) {
            return true;
        }
    }
    return false;
}

static bool agent_tool_search_stop_word(const char *token)
{
    static const char *const words[] = {
        "a", "an", "and", "for", "how", "in", "of", "on", "the", "to",
        "tool", "tools", "use", "with",
    };
    for (size_t i = 0U; i < sizeof(words) / sizeof(words[0]); i++) {
        if (strcmp(token, words[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool agent_tool_query_has_name(const char *query, const char *name)
{
    if (query == NULL || name == NULL || name[0] == '\0') {
        return false;
    }
    const size_t name_len = strlen(name);
    for (const char *cursor = query; *cursor != '\0'; cursor++) {
        if (strncasecmp(cursor, name, name_len) != 0) {
            continue;
        }
        const unsigned char before = cursor == query
            ? '\0'
            : (unsigned char)cursor[-1];
        const unsigned char after = (unsigned char)cursor[name_len];
        const bool before_boundary =
            before == '\0' || (!isalnum(before) && before != '_');
        const bool after_boundary =
            after == '\0' || (!isalnum(after) && after != '_');
        if (before_boundary && after_boundary) {
            return true;
        }
    }
    return false;
}

static bool agent_tool_prompt_has_path(const char *prompt)
{
    if (prompt == NULL) {
        return false;
    }
    for (const unsigned char *cursor = (const unsigned char *)prompt;
         *cursor != '\0';
         cursor++) {
        if (*cursor == '/') {
            return true;
        }
        if (*cursor == '.' && cursor > (const unsigned char *)prompt &&
            isalnum(cursor[-1]) && isalnum(cursor[1])) {
            return true;
        }
    }
    return false;
}

static bool agent_tool_append_named(
    const char *name,
    const solar_os_agent_request_t *request,
    solar_os_agent_tool_policy_t policy,
    solar_os_agent_tool_descriptor_t *descriptors,
    size_t *count,
    size_t capacity)
{
    if (name == NULL || descriptors == NULL || count == NULL ||
        *count >= capacity) {
        return false;
    }
    for (size_t i = 0U; i < *count; i++) {
        if (descriptors[i].name != NULL &&
            strcmp(descriptors[i].name, name) == 0) {
            return true;
        }
    }
    for (size_t i = 0U; i < AGENT_TOOL_COUNT; i++) {
        const agent_tool_definition_t *definition = &AGENT_TOOL_REGISTRY[i];
        if (strcmp(definition->provider.name, name) != 0) {
            continue;
        }
        if (!agent_tool_is_available(definition, request) ||
            solar_os_agent_tools_policy_decision(policy, definition->risk) ==
                SOLAR_OS_AGENT_TOOL_POLICY_DENY) {
            return false;
        }
        descriptors[(*count)++] = definition->provider;
        return true;
    }
    return false;
}

static unsigned agent_tool_search_score(
    const agent_tool_definition_t *definition,
    const char *query)
{
    if (definition == NULL || query == NULL || query[0] == '\0') {
        return 0U;
    }
    if (agent_tool_query_has_name(query, definition->provider.name)) {
        return 100000U;
    }
    if (strcasecmp(definition->domain, query) == 0) {
        return 50000U;
    }

    unsigned score = 0U;
    char token[AGENT_TOOL_SEARCH_TOKEN_MAX + 1U];
    size_t token_len = 0U;
    for (const unsigned char *cursor = (const unsigned char *)query;; cursor++) {
        const bool separator = *cursor == '\0' || !isalnum(*cursor);
        if (!separator && token_len < AGENT_TOOL_SEARCH_TOKEN_MAX) {
            token[token_len++] = (char)tolower(*cursor);
        }
        if (separator && token_len > 0U) {
            token[token_len] = '\0';
            if (!agent_tool_search_stop_word(token)) {
                if (agent_tool_contains_ci(definition->provider.name, token)) {
                    score += 900U;
                }
                if (agent_tool_contains_ci(definition->domain, token)) {
                    score += 700U;
                }
                if (agent_tool_contains_ci(definition->search_terms, token)) {
                    score += 500U;
                }
                if (agent_tool_contains_ci(definition->provider.description,
                                           token)) {
                    score += 100U;
                }
            }
            token_len = 0U;
        }
        if (*cursor == '\0') {
            break;
        }
    }
    return score;
}

static size_t agent_tool_search_matches(
    const char *query,
    const solar_os_agent_request_t *request,
    solar_os_agent_tool_policy_t policy,
    agent_tool_search_match_t *matches,
    size_t capacity)
{
    if (query == NULL || query[0] == '\0' ||
        matches == NULL || capacity == 0U) {
        return 0U;
    }

    size_t count = 0U;
    for (size_t candidate = 0U; candidate < AGENT_TOOL_COUNT; candidate++) {
        const agent_tool_definition_t *definition =
            &AGENT_TOOL_REGISTRY[candidate];
        if (definition->bootstrap ||
            !agent_tool_is_available(definition, request) ||
            solar_os_agent_tools_policy_decision(policy, definition->risk) ==
                SOLAR_OS_AGENT_TOOL_POLICY_DENY) {
            continue;
        }
        const unsigned score = agent_tool_search_score(definition, query);
        if (score == 0U) {
            continue;
        }
        size_t insert = 0U;
        while (insert < count &&
               (matches[insert].score > score ||
                (matches[insert].score == score &&
                 strcmp(matches[insert].definition->provider.name,
                        definition->provider.name) < 0))) {
            insert++;
        }
        if (insert >= capacity) {
            continue;
        }
        if (count < capacity) {
            count++;
        }
        for (size_t move = count - 1U; move > insert; move--) {
            matches[move] = matches[move - 1U];
        }
        matches[insert] = (agent_tool_search_match_t){
            .definition = definition,
            .score = score,
        };
    }
    return count;
}

static esp_err_t agent_tool_parse_object(const char *arguments,
                                         solar_os_json_doc_t **doc,
                                         const solar_os_json_value_t **root)
{
    if (arguments == NULL || doc == NULL || root == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *doc = NULL;
    *root = NULL;
    const char *source = arguments[0] == '\0' ? "{}" : arguments;
    esp_err_t err = solar_os_json_parse_cstr(source, doc);
    if (err != ESP_OK) {
        return err;
    }
    *root = solar_os_json_root(*doc);
    if (!solar_os_json_is_object(*root)) {
        solar_os_json_free(*doc);
        *doc = NULL;
        *root = NULL;
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static esp_err_t agent_tool_parse_query(const char *arguments,
                                        char *query,
                                        size_t query_len)
{
    if (query == NULL || query_len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    solar_os_json_doc_t *doc = NULL;
    const solar_os_json_value_t *root = NULL;
    esp_err_t err = agent_tool_parse_object(arguments, &doc, &root);
    if (err == ESP_OK) {
        err = solar_os_json_get_string(
            solar_os_json_object_get(root, "query"), query, query_len);
    }
    solar_os_json_free(doc);
    if (err != ESP_OK || query[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static esp_err_t agent_tool_output_append(agent_tool_output_t *output,
                                          const char *format,
                                          ...)
{
    if (output == NULL || output->buffer == NULL ||
        output->length >= output->capacity) {
        return ESP_ERR_INVALID_ARG;
    }
    va_list args;
    va_start(args, format);
    const int written = vsnprintf(output->buffer + output->length,
                                  output->capacity - output->length,
                                  format,
                                  args);
    va_end(args);
    if (written < 0 ||
        (size_t)written >= output->capacity - output->length) {
        return ESP_ERR_INVALID_SIZE;
    }
    output->length += (size_t)written;
    return ESP_OK;
}

static bool agent_tool_output_append_json_byte(agent_tool_output_t *output,
                                               uint8_t byte,
                                               size_t tail_reserve)
{
    char escaped[7];
    size_t length = 1U;
    escaped[0] = (char)byte;
    switch (byte) {
    case '"':
        memcpy(escaped, "\\\"", 2U);
        length = 2U;
        break;
    case '\\':
        memcpy(escaped, "\\\\", 2U);
        length = 2U;
        break;
    case '\b':
        memcpy(escaped, "\\b", 2U);
        length = 2U;
        break;
    case '\f':
        memcpy(escaped, "\\f", 2U);
        length = 2U;
        break;
    case '\n':
        memcpy(escaped, "\\n", 2U);
        length = 2U;
        break;
    case '\r':
        memcpy(escaped, "\\r", 2U);
        length = 2U;
        break;
    case '\t':
        memcpy(escaped, "\\t", 2U);
        length = 2U;
        break;
    default:
        if (byte < 0x20U) {
            snprintf(escaped, sizeof(escaped), "\\u%04x", byte);
            length = 6U;
        }
        break;
    }
    if (output == NULL || output->buffer == NULL ||
        output->length + length + tail_reserve >= output->capacity) {
        return false;
    }
    memcpy(output->buffer + output->length, escaped, length);
    output->length += length;
    output->buffer[output->length] = '\0';
    return true;
}

static esp_err_t agent_tool_output_append_json_string(
    agent_tool_output_t *output,
    const char *text)
{
    esp_err_t err = agent_tool_output_append(output, "\"");
    if (err != ESP_OK) {
        return err;
    }
    const uint8_t *cursor =
        (const uint8_t *)(text != NULL ? text : "");
    while (*cursor != '\0') {
        if (!agent_tool_output_append_json_byte(output, *cursor++, 1U)) {
            return ESP_ERR_INVALID_SIZE;
        }
    }
    return agent_tool_output_append(output, "\"");
}

static bool agent_tool_path_has_segment(const char *path,
                                        const char *segment)
{
    if (path == NULL || segment == NULL || segment[0] == '\0') {
        return false;
    }
    const size_t segment_len = strlen(segment);
    const char *cursor = path;
    while (*cursor != '\0') {
        while (*cursor == '/') {
            cursor++;
        }
        const char *start = cursor;
        while (*cursor != '\0' && *cursor != '/') {
            cursor++;
        }
        if ((size_t)(cursor - start) == segment_len &&
            memcmp(start, segment, segment_len) == 0) {
            return true;
        }
    }
    return false;
}

static esp_err_t agent_tool_storage_path(
    const solar_os_json_value_t *root,
    const solar_os_agent_request_t *request,
    char *path,
    size_t path_len)
{
    char requested[SOLAR_OS_STORAGE_PATH_MAX];
    const solar_os_json_value_t *path_value =
        solar_os_json_object_get(root, "path");
    esp_err_t err = solar_os_json_get_string(path_value,
                                              requested,
                                              sizeof(requested));
    if (err != ESP_OK || requested[0] == '\0') {
        return err != ESP_OK ? err : ESP_ERR_INVALID_ARG;
    }
    err = solar_os_storage_resolve_path_at(
        request != NULL ? request->storage_cwd : NULL,
        requested,
        path,
        path_len);
    if (err == ESP_OK && agent_tool_path_has_segment(path, ".ssh")) {
        return ESP_ERR_NOT_ALLOWED;
    }
    return err;
}

static esp_err_t agent_tool_sha256_file(
    FILE *file,
    char hex[SOLAR_OS_CRYPTO_SHA256_HEX_LEN])
{
    if (file == NULL || hex == NULL || fseek(file, 0L, SEEK_SET) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    solar_os_crypto_sha256_t sha256;
    solar_os_crypto_sha256_init(&sha256);
    esp_err_t err = solar_os_crypto_sha256_start(&sha256);
    uint8_t buffer[512];
    while (err == ESP_OK) {
        const size_t read_len = fread(buffer, 1U, sizeof(buffer), file);
        if (read_len > 0U) {
            err = solar_os_crypto_sha256_update(&sha256,
                                                buffer,
                                                read_len);
        }
        if (read_len < sizeof(buffer)) {
            if (ferror(file)) {
                err = ESP_FAIL;
            }
            break;
        }
    }
    uint8_t digest[SOLAR_OS_CRYPTO_SHA256_LEN];
    if (err == ESP_OK) {
        err = solar_os_crypto_sha256_finish(&sha256, digest);
    }
    if (err == ESP_OK) {
        err = solar_os_crypto_bytes_to_hex(digest,
                                           sizeof(digest),
                                           hex,
                                           SOLAR_OS_CRYPTO_SHA256_HEX_LEN);
    }
    solar_os_crypto_sha256_free(&sha256);
    return err;
}

static bool agent_tool_write_complete_file(const char *path,
                                           const uint8_t *content,
                                           size_t content_size)
{
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        return false;
    }
    const size_t written = fwrite(content, 1U, content_size, file);
    const int flush_result = fflush(file);
    const int close_result = fclose(file);
    return written == content_size &&
           flush_result == 0 &&
           close_result == 0;
}

static esp_err_t agent_tool_replace_from_memory(
    const char *path,
    const uint8_t *replacement,
    size_t replacement_size,
    const uint8_t *rollback,
    size_t rollback_size)
{
    if (agent_tool_write_complete_file(path,
                                       replacement,
                                       replacement_size)) {
        return ESP_OK;
    }
    return agent_tool_write_complete_file(path, rollback, rollback_size)
        ? ESP_FAIL
        : ESP_ERR_INVALID_STATE;
}

static esp_err_t agent_tool_validate_result(const char *result)
{
    solar_os_json_doc_t *doc = NULL;
    esp_err_t err = solar_os_json_parse_cstr(result, &doc);
    if (err == ESP_OK && !solar_os_json_is_object(solar_os_json_root(doc))) {
        err = ESP_ERR_INVALID_RESPONSE;
    }
    solar_os_json_free(doc);
    return err;
}

static esp_err_t agent_tool_system_status(const char *arguments,
                                           const solar_os_agent_request_t *request,
                                           char *result,
                                           size_t result_len)
{
    (void)request;
    solar_os_json_doc_t *doc = NULL;
    const solar_os_json_value_t *root = NULL;
    esp_err_t err = agent_tool_parse_object(arguments, &doc, &root);
    if (err != ESP_OK) {
        return err;
    }
    (void)root;
    solar_os_json_free(doc);

    const uint64_t uptime_ms = (uint64_t)(esp_timer_get_time() / 1000);
    const uint32_t internal_free = (uint32_t)heap_caps_get_free_size(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const uint32_t internal_largest = (uint32_t)heap_caps_get_largest_free_block(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const uint32_t psram_free = (uint32_t)heap_caps_get_free_size(
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const int written = snprintf(
        result,
        result_len,
        "{\"board\":\"%s\",\"version\":\"%s\",\"uptime_ms\":%" PRIu64 ","
        "\"internal_free_bytes\":%" PRIu32 ","
        "\"internal_largest_block_bytes\":%" PRIu32 ","
        "\"psram_free_bytes\":%" PRIu32 "}",
        SOLAR_OS_BOARD_ID,
        SOLAR_OS_VERSION,
        uptime_ms,
        internal_free,
        internal_largest,
        psram_free);
    return written >= 0 && (size_t)written < result_len ?
        ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t agent_tool_solaros_reference(
    const char *arguments,
    const solar_os_agent_request_t *request,
    char *result,
    size_t result_len)
{
    (void)request;
    char query[65];
    const esp_err_t err =
        agent_tool_parse_query(arguments, query, sizeof(query));
    if (err != ESP_OK) {
        return err;
    }
    return solar_os_agent_reference_search(query, result, result_len);
}

static esp_err_t agent_tool_search_with_policy(
    const char *arguments,
    const solar_os_agent_request_t *request,
    solar_os_agent_tool_policy_t policy,
    char *result,
    size_t result_len)
{
    char query[AGENT_TOOL_SEARCH_QUERY_MAX + 1U];
    esp_err_t err =
        agent_tool_parse_query(arguments, query, sizeof(query));
    if (err != ESP_OK) {
        return err;
    }

    agent_tool_search_match_t matches[AGENT_TOOL_SEARCH_MATCH_MAX] = {0};
    const size_t count =
        agent_tool_search_matches(query,
                                  request,
                                  policy,
                                  matches,
                                  AGENT_TOOL_SEARCH_MATCH_MAX);
    char escaped_query[(AGENT_TOOL_SEARCH_QUERY_MAX *
                        AGENT_TOOL_JSON_ESCAPE_FACTOR) + 1U];
    err = solar_os_json_escape_string(query,
                                      escaped_query,
                                      sizeof(escaped_query));
    agent_tool_output_t output = {
        .buffer = result,
        .capacity = result_len,
    };
    if (err == ESP_OK) {
        err = agent_tool_output_append(&output,
                                       "{\"query\":\"%s\",\"count\":%u,"
                                       "\"tools\":[",
                                       escaped_query,
                                       (unsigned)count);
    }
    for (size_t i = 0U; err == ESP_OK && i < count; i++) {
        const agent_tool_definition_t *definition = matches[i].definition;
        char escaped_name[128];
        char escaped_domain[128];
        char escaped_description[1024];
        if (solar_os_json_escape_string(definition->provider.name,
                                        escaped_name,
                                        sizeof(escaped_name)) != ESP_OK ||
            solar_os_json_escape_string(definition->domain,
                                        escaped_domain,
                                        sizeof(escaped_domain)) != ESP_OK ||
            solar_os_json_escape_string(definition->provider.description,
                                        escaped_description,
                                        sizeof(escaped_description)) != ESP_OK) {
            return ESP_ERR_INVALID_SIZE;
        }
        err = agent_tool_output_append(
            &output,
            "%s{\"name\":\"%s\",\"domain\":\"%s\","
            "\"description\":\"%s\",\"risk\":\"%s\"}",
            i == 0U ? "" : ",",
            escaped_name,
            escaped_domain,
            escaped_description,
            solar_os_agent_tool_risk_name(definition->risk));
    }
    if (err == ESP_OK) {
        err = agent_tool_output_append(&output, "]}");
    }
    return err;
}

static esp_err_t agent_tool_search(const char *arguments,
                                   const solar_os_agent_request_t *request,
                                   char *result,
                                   size_t result_len)
{
    return agent_tool_search_with_policy(arguments,
                                         request,
                                         SOLAR_OS_AGENT_TOOL_POLICY_ALL,
                                         result,
                                         result_len);
}

static esp_err_t agent_tool_storage_list(const char *arguments,
                                         const solar_os_agent_request_t *request,
                                         char *result,
                                         size_t result_len)
{
    solar_os_json_doc_t *doc = NULL;
    const solar_os_json_value_t *root = NULL;
    esp_err_t err = agent_tool_parse_object(arguments, &doc, &root);
    if (err != ESP_OK) {
        return err;
    }

    char path[SOLAR_OS_STORAGE_PATH_MAX];
    err = agent_tool_storage_path(root, request, path, sizeof(path));
    solar_os_json_free(doc);
    if (err != ESP_OK) {
        return err;
    }

    DIR *directory = opendir(path);
    if (directory == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    char escaped_path[AGENT_TOOL_JSON_SCRATCH_MAX];
    err = solar_os_json_escape_string(path,
                                      escaped_path,
                                      sizeof(escaped_path));
    agent_tool_output_t output = {
        .buffer = result,
        .capacity = result_len,
    };
    if (err == ESP_OK) {
        err = agent_tool_output_append(&output,
                                       "{\"path\":\"%s\",\"entries\":[",
                                       escaped_path);
    }

    bool first = true;
    bool truncated = false;
    size_t count = 0;
    struct dirent *entry = NULL;
    while (err == ESP_OK && (entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (count >= AGENT_TOOL_STORAGE_ENTRY_MAX) {
            truncated = true;
            break;
        }

        char escaped_name[AGENT_TOOL_JSON_SCRATCH_MAX];
        if (solar_os_json_escape_string(entry->d_name,
                                        escaped_name,
                                        sizeof(escaped_name)) != ESP_OK) {
            truncated = true;
            break;
        }
        char full_path[SOLAR_OS_STORAGE_PATH_MAX];
        const int path_written = strcmp(path, "/") == 0 ?
            snprintf(full_path,
                     sizeof(full_path),
                     "/%s",
                     entry->d_name) :
            snprintf(full_path,
                     sizeof(full_path),
                     "%s/%s",
                     path,
                     entry->d_name);
        struct stat status;
        memset(&status, 0, sizeof(status));
        const bool has_status =
            path_written >= 0 &&
            (size_t)path_written < sizeof(full_path) &&
            stat(full_path, &status) == 0;
        const char *type = !has_status ? "unknown" :
            (S_ISDIR(status.st_mode) ? "directory" :
             (S_ISREG(status.st_mode) ? "file" : "other"));

        char item[AGENT_TOOL_JSON_SCRATCH_MAX];
        const int item_written = snprintf(
            item,
            sizeof(item),
            "%s{\"name\":\"%s\",\"type\":\"%s\",\"size_bytes\":%" PRIu64 "}",
            first ? "" : ",",
            escaped_name,
            type,
            has_status ? (uint64_t)status.st_size : 0);
        const size_t tail_reserve = sizeof("],\"truncated\":true}");
        if (item_written < 0 ||
            (size_t)item_written >= sizeof(item) ||
            output.length + (size_t)item_written + tail_reserve >=
                output.capacity) {
            truncated = true;
            break;
        }
        err = agent_tool_output_append(&output, "%s", item);
        first = false;
        count++;
    }
    closedir(directory);
    if (err == ESP_OK) {
        err = agent_tool_output_append(&output,
                                       "],\"truncated\":%s}",
                                       truncated ? "true" : "false");
    }
    return err;
}

static esp_err_t agent_tool_storage_stat(const char *arguments,
                                         const solar_os_agent_request_t *request,
                                         char *result,
                                         size_t result_len)
{
    solar_os_json_doc_t *doc = NULL;
    const solar_os_json_value_t *root = NULL;
    esp_err_t err = agent_tool_parse_object(arguments, &doc, &root);
    if (err != ESP_OK) {
        return err;
    }

    char path[SOLAR_OS_STORAGE_PATH_MAX];
    err = agent_tool_storage_path(root, request, path, sizeof(path));
    solar_os_json_free(doc);
    if (err != ESP_OK) {
        return err;
    }

    struct stat status;
    errno = 0;
    const bool exists = stat(path, &status) == 0;
    if (!exists && errno != ENOENT && errno != ENOTDIR) {
        return ESP_FAIL;
    }
    const char *type = !exists ? "missing" :
        (S_ISREG(status.st_mode) ? "file" :
            (S_ISDIR(status.st_mode) ? "directory" : "other"));
    const uint64_t size =
        exists && status.st_size > 0 ? (uint64_t)status.st_size : 0U;

    char escaped_path[AGENT_TOOL_JSON_SCRATCH_MAX];
    err = solar_os_json_escape_string(path,
                                      escaped_path,
                                      sizeof(escaped_path));
    if (err != ESP_OK) {
        return err;
    }
    const int written = snprintf(
        result,
        result_len,
        "{\"path\":\"%s\",\"exists\":%s,\"type\":\"%s\","
        "\"size_bytes\":%" PRIu64 "}",
        escaped_path,
        exists ? "true" : "false",
        type,
        size);
    return written >= 0 && (size_t)written < result_len ?
        ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t agent_tool_storage_read(const char *arguments,
                                         const solar_os_agent_request_t *request,
                                         char *result,
                                         size_t result_len)
{
    solar_os_json_doc_t *doc = NULL;
    const solar_os_json_value_t *root = NULL;
    esp_err_t err = agent_tool_parse_object(arguments, &doc, &root);
    if (err != ESP_OK) {
        return err;
    }

    char path[SOLAR_OS_STORAGE_PATH_MAX];
    err = agent_tool_storage_path(root, request, path, sizeof(path));
    solar_os_json_free(doc);
    if (err != ESP_OK) {
        return err;
    }

    struct stat status;
    if (stat(path, &status) != 0 || !S_ISREG(status.st_mode)) {
        return ESP_ERR_NOT_FOUND;
    }
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return ESP_FAIL;
    }

    char escaped_path[AGENT_TOOL_JSON_SCRATCH_MAX];
    err = solar_os_json_escape_string(path,
                                      escaped_path,
                                      sizeof(escaped_path));
    agent_tool_output_t output = {
        .buffer = result,
        .capacity = result_len,
    };
    if (err == ESP_OK) {
        err = agent_tool_output_append(
            &output,
            "{\"ok\":true,\"path\":\"%s\",\"size_bytes\":%" PRIu64
            ",\"content\":\"",
            escaped_path,
            status.st_size > 0 ? (uint64_t)status.st_size : 0U);
    }

    const size_t tail_reserve = sizeof("\",\"truncated\":true}");
    size_t bytes_read = 0;
    bool truncated = false;
    while (err == ESP_OK && bytes_read < AGENT_TOOL_STORAGE_CONTENT_MAX) {
        const int byte = fgetc(file);
        if (byte == EOF) {
            if (ferror(file)) {
                err = ESP_FAIL;
            }
            break;
        }
        if (!agent_tool_output_append_json_byte(&output,
                                                (uint8_t)byte,
                                                tail_reserve)) {
            truncated = true;
            break;
        }
        bytes_read++;
    }
    if (err == ESP_OK && !truncated) {
        const int byte = fgetc(file);
        truncated = byte != EOF;
        if (byte == EOF && ferror(file)) {
            err = ESP_FAIL;
        }
    }
    fclose(file);
    if (err == ESP_OK) {
        err = agent_tool_output_append(&output,
                                       "\",\"truncated\":%s}",
                                       truncated ? "true" : "false");
    }
    return err;
}

static esp_err_t agent_tool_storage_write(const char *arguments,
                                          const solar_os_agent_request_t *request,
                                          char *result,
                                          size_t result_len)
{
    solar_os_json_doc_t *doc = NULL;
    const solar_os_json_value_t *root = NULL;
    esp_err_t err = agent_tool_parse_object(arguments, &doc, &root);
    if (err != ESP_OK) {
        return err;
    }

    char path[SOLAR_OS_STORAGE_PATH_MAX];
    err = agent_tool_storage_path(root, request, path, sizeof(path));
    char *content = solar_os_memory_calloc(
        1,
        AGENT_TOOL_STORAGE_CONTENT_MAX + 1U,
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
        "agent.tool.storage-write");
    if (err == ESP_OK && content == NULL) {
        err = ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK) {
        const solar_os_json_value_t *content_value =
            solar_os_json_object_get(root, "content");
        err = solar_os_json_get_string(content_value,
                                       content,
                                       AGENT_TOOL_STORAGE_CONTENT_MAX + 1U);
    }
    solar_os_json_free(doc);
    if (err != ESP_OK) {
        solar_os_memory_free(content);
        return err;
    }

    struct stat status;
    if (stat(path, &status) == 0 && S_ISDIR(status.st_mode)) {
        solar_os_memory_free(content);
        return ESP_ERR_INVALID_ARG;
    }

    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        solar_os_memory_free(content);
        return ESP_FAIL;
    }
    const size_t content_len = strlen(content);
    const size_t written = fwrite(content, 1U, content_len, file);
    const int flush_result = fflush(file);
    const int close_result = fclose(file);
    const bool write_ok =
        written == content_len && flush_result == 0 && close_result == 0;
    if (!write_ok) {
        solar_os_memory_free(content);
        return ESP_FAIL;
    }

    char escaped_path[AGENT_TOOL_JSON_SCRATCH_MAX];
    err = solar_os_json_escape_string(path,
                                      escaped_path,
                                      sizeof(escaped_path));
    if (err == ESP_OK) {
        const int result_written = snprintf(
            result,
            result_len,
            "{\"ok\":true,\"path\":\"%s\",\"bytes_written\":%u}",
            escaped_path,
            (unsigned)written);
        err = result_written >= 0 && (size_t)result_written < result_len ?
            ESP_OK : ESP_ERR_INVALID_SIZE;
    }
    memset(content, 0, AGENT_TOOL_STORAGE_CONTENT_MAX + 1U);
    solar_os_memory_free(content);
    return err;
}

static esp_err_t agent_tool_storage_search(
    const char *arguments,
    const solar_os_agent_request_t *request,
    char *result,
    size_t result_len)
{
    solar_os_json_doc_t *doc = NULL;
    const solar_os_json_value_t *root = NULL;
    esp_err_t err = agent_tool_parse_object(arguments, &doc, &root);
    if (err != ESP_OK) {
        return err;
    }

    char path[SOLAR_OS_STORAGE_PATH_MAX];
    char query[65];
    err = agent_tool_storage_path(root, request, path, sizeof(path));
    if (err == ESP_OK) {
        err = solar_os_json_get_string(
            solar_os_json_object_get(root, "query"),
            query,
            sizeof(query));
    }
    solar_os_json_free(doc);
    if (err != ESP_OK || query[0] == '\0') {
        return err != ESP_OK ? err : ESP_ERR_INVALID_ARG;
    }

    struct stat root_status;
    if (stat(path, &root_status) != 0 ||
        (!S_ISREG(root_status.st_mode) && !S_ISDIR(root_status.st_mode))) {
        return ESP_ERR_NOT_FOUND;
    }

    char (*paths)[SOLAR_OS_STORAGE_PATH_MAX] = solar_os_memory_calloc(
        AGENT_TOOL_STORAGE_SEARCH_PATH_MAX,
        SOLAR_OS_STORAGE_PATH_MAX,
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
        "agent.tool.storage-search");
    if (paths == NULL) {
        return ESP_ERR_NO_MEM;
    }
    strlcpy(paths[0], path, SOLAR_OS_STORAGE_PATH_MAX);
    size_t queue_head = 0U;
    size_t queue_tail = 1U;

    char escaped_path[(SOLAR_OS_STORAGE_PATH_MAX *
                       AGENT_TOOL_JSON_ESCAPE_FACTOR) + 1U];
    char escaped_query[(64U * AGENT_TOOL_JSON_ESCAPE_FACTOR) + 1U];
    err = solar_os_json_escape_string(path,
                                      escaped_path,
                                      sizeof(escaped_path));
    if (err == ESP_OK) {
        err = solar_os_json_escape_string(query,
                                          escaped_query,
                                          sizeof(escaped_query));
    }
    agent_tool_output_t output = {
        .buffer = result,
        .capacity = result_len,
    };
    if (err == ESP_OK) {
        err = agent_tool_output_append(&output,
                                       "{\"path\":\"%s\",\"query\":\"%s\","
                                       "\"matches\":[",
                                       escaped_path,
                                       escaped_query);
    }

    size_t files_scanned = 0U;
    size_t bytes_scanned = 0U;
    size_t matches = 0U;
    bool first = true;
    bool truncated = false;
    bool stop = false;
    while (err == ESP_OK && queue_head < queue_tail && !stop) {
        const char *current = paths[queue_head++];
        struct stat status;
        if (stat(current, &status) != 0) {
            continue;
        }
        if (S_ISDIR(status.st_mode)) {
            DIR *directory = opendir(current);
            if (directory == NULL) {
                continue;
            }
            struct dirent *entry = NULL;
            while ((entry = readdir(directory)) != NULL) {
                if (strcmp(entry->d_name, ".") == 0 ||
                    strcmp(entry->d_name, "..") == 0) {
                    continue;
                }
                char child[SOLAR_OS_STORAGE_PATH_MAX];
                const int written = strcmp(current, "/") == 0 ?
                    snprintf(child,
                             sizeof(child),
                             "/%s",
                             entry->d_name) :
                    snprintf(child,
                             sizeof(child),
                             "%s/%s",
                             current,
                             entry->d_name);
                if (written < 0 || (size_t)written >= sizeof(child)) {
                    truncated = true;
                    break;
                }
                if (agent_tool_path_has_segment(child, ".ssh")) {
                    continue;
                }
                if (queue_tail >= AGENT_TOOL_STORAGE_SEARCH_PATH_MAX) {
                    truncated = true;
                    break;
                }
                strlcpy(paths[queue_tail++],
                        child,
                        SOLAR_OS_STORAGE_PATH_MAX);
            }
            closedir(directory);
            continue;
        }
        if (!S_ISREG(status.st_mode)) {
            continue;
        }

        FILE *file = fopen(current, "rb");
        if (file == NULL) {
            continue;
        }
        files_scanned++;
        char line[AGENT_TOOL_STORAGE_SEARCH_LINE_MAX];
        uint32_t line_number = 0U;
        bool continuing_line = false;
        while (fgets(line, sizeof(line), file) != NULL) {
            if (!continuing_line) {
                line_number++;
            }
            const size_t line_len = strlen(line);
            continuing_line =
                line_len > 0U && line[line_len - 1U] != '\n';
            if (bytes_scanned + line_len >
                AGENT_TOOL_STORAGE_SEARCH_BYTES_MAX) {
                truncated = true;
                stop = true;
                break;
            }
            bytes_scanned += line_len;
            if (!agent_tool_contains_ci(line, query)) {
                continue;
            }

            size_t excerpt_len = line_len;
            while (excerpt_len > 0U &&
                   (line[excerpt_len - 1U] == '\n' ||
                    line[excerpt_len - 1U] == '\r')) {
                excerpt_len--;
            }
            if (excerpt_len > 160U) {
                excerpt_len = 160U;
            }
            char excerpt[161];
            memcpy(excerpt, line, excerpt_len);
            excerpt[excerpt_len] = '\0';
            char escaped_current[(SOLAR_OS_STORAGE_PATH_MAX *
                                  AGENT_TOOL_JSON_ESCAPE_FACTOR) + 1U];
            char escaped_excerpt[(160U *
                                  AGENT_TOOL_JSON_ESCAPE_FACTOR) + 1U];
            if (solar_os_json_escape_string(current,
                                            escaped_current,
                                            sizeof(escaped_current)) != ESP_OK ||
                solar_os_json_escape_string(excerpt,
                                            escaped_excerpt,
                                            sizeof(escaped_excerpt)) != ESP_OK) {
                err = ESP_ERR_INVALID_SIZE;
                break;
            }
            char item[1400];
            const int item_written = snprintf(
                item,
                sizeof(item),
                "%s{\"path\":\"%s\",\"line\":%" PRIu32
                ",\"excerpt\":\"%s\"}",
                first ? "" : ",",
                escaped_current,
                line_number,
                escaped_excerpt);
            const size_t tail_reserve = 160U;
            if (item_written < 0 ||
                (size_t)item_written >= sizeof(item) ||
                output.length + (size_t)item_written + tail_reserve >=
                    output.capacity) {
                truncated = true;
                stop = true;
                break;
            }
            err = agent_tool_output_append(&output, "%s", item);
            first = false;
            matches++;
            if (matches >= AGENT_TOOL_STORAGE_SEARCH_MATCH_MAX) {
                truncated = true;
                stop = true;
                break;
            }
        }
        if (ferror(file) && err == ESP_OK) {
            err = ESP_FAIL;
        }
        fclose(file);
    }
    if (queue_head < queue_tail) {
        truncated = true;
    }
    if (err == ESP_OK) {
        err = agent_tool_output_append(
            &output,
            "],\"files_scanned\":%u,\"bytes_scanned\":%u,"
            "\"truncated\":%s}",
            (unsigned)files_scanned,
            (unsigned)bytes_scanned,
            truncated ? "true" : "false");
    }
    solar_os_memory_free(paths);
    return err;
}

static esp_err_t agent_tool_storage_read_range(
    const char *arguments,
    const solar_os_agent_request_t *request,
    char *result,
    size_t result_len)
{
    solar_os_json_doc_t *doc = NULL;
    const solar_os_json_value_t *root = NULL;
    esp_err_t err = agent_tool_parse_object(arguments, &doc, &root);
    if (err != ESP_OK) {
        return err;
    }

    char path[SOLAR_OS_STORAGE_PATH_MAX];
    uint32_t offset = 0U;
    uint32_t requested_length = 0U;
    err = agent_tool_storage_path(root, request, path, sizeof(path));
    if (err == ESP_OK) {
        err = solar_os_json_get_uint32(
            solar_os_json_object_get(root, "offset"),
            &offset);
    }
    if (err == ESP_OK) {
        err = solar_os_json_get_uint32(
            solar_os_json_object_get(root, "length"),
            &requested_length);
    }
    solar_os_json_free(doc);
    if (err != ESP_OK || requested_length == 0U ||
        requested_length > AGENT_TOOL_STORAGE_RANGE_MAX) {
        return err != ESP_OK ? err : ESP_ERR_INVALID_ARG;
    }

    struct stat status;
    if (stat(path, &status) != 0 || !S_ISREG(status.st_mode)) {
        return ESP_ERR_NOT_FOUND;
    }
    if (status.st_size < 0 || (uint64_t)status.st_size > UINT32_MAX ||
        offset > (uint64_t)status.st_size) {
        return ESP_ERR_INVALID_ARG;
    }
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return ESP_FAIL;
    }
    char sha256[SOLAR_OS_CRYPTO_SHA256_HEX_LEN];
    err = agent_tool_sha256_file(file, sha256);
    if (err == ESP_OK && fseek(file, (long)offset, SEEK_SET) != 0) {
        err = ESP_FAIL;
    }

    char escaped_path[(SOLAR_OS_STORAGE_PATH_MAX *
                       AGENT_TOOL_JSON_ESCAPE_FACTOR) + 1U];
    if (err == ESP_OK) {
        err = solar_os_json_escape_string(path,
                                          escaped_path,
                                          sizeof(escaped_path));
    }
    agent_tool_output_t output = {
        .buffer = result,
        .capacity = result_len,
    };
    if (err == ESP_OK) {
        err = agent_tool_output_append(
            &output,
            "{\"ok\":true,\"path\":\"%s\",\"size_bytes\":%" PRIu64
            ",\"sha256\":\"%s\",\"offset\":%" PRIu32 ",\"content\":\"",
            escaped_path,
            (uint64_t)status.st_size,
            sha256,
            offset);
    }

    size_t bytes_returned = 0U;
    bool truncated = false;
    const size_t tail_reserve = 160U;
    while (err == ESP_OK && bytes_returned < requested_length) {
        const int byte = fgetc(file);
        if (byte == EOF) {
            if (ferror(file)) {
                err = ESP_FAIL;
            }
            break;
        }
        if (!agent_tool_output_append_json_byte(&output,
                                                (uint8_t)byte,
                                                tail_reserve)) {
            truncated = true;
            break;
        }
        bytes_returned++;
    }
    fclose(file);
    const uint32_t next_offset = offset + (uint32_t)bytes_returned;
    const bool eof = next_offset >= (uint64_t)status.st_size;
    if (bytes_returned < requested_length && !eof) {
        truncated = true;
    }
    if (err == ESP_OK) {
        err = agent_tool_output_append(
            &output,
            "\",\"bytes_returned\":%u,\"next_offset\":%" PRIu32
            ",\"eof\":%s,\"truncated\":%s}",
            (unsigned)bytes_returned,
            next_offset,
            eof ? "true" : "false",
            truncated ? "true" : "false");
    }
    return err;
}

static esp_err_t agent_tool_storage_patch(
    const char *arguments,
    const solar_os_agent_request_t *request,
    char *result,
    size_t result_len)
{
    solar_os_json_doc_t *doc = NULL;
    const solar_os_json_value_t *root = NULL;
    esp_err_t err = agent_tool_parse_object(arguments, &doc, &root);
    if (err != ESP_OK) {
        return err;
    }

    char path[SOLAR_OS_STORAGE_PATH_MAX];
    char expected_sha256[SOLAR_OS_CRYPTO_SHA256_HEX_LEN];
    err = agent_tool_storage_path(root, request, path, sizeof(path));
    if (err == ESP_OK) {
        err = solar_os_json_get_string(
            solar_os_json_object_get(root, "expected_sha256"),
            expected_sha256,
            sizeof(expected_sha256));
    }
    const solar_os_json_value_t *edits_value =
        solar_os_json_object_get(root, "edits");
    const size_t edit_count = solar_os_json_array_size(edits_value);
    if (err == ESP_OK &&
        (!solar_os_crypto_sha256_hex_is_valid(expected_sha256) ||
         !solar_os_json_is_array(edits_value) || edit_count == 0U ||
         edit_count > AGENT_TOOL_STORAGE_PATCH_EDIT_MAX)) {
        err = ESP_ERR_INVALID_ARG;
    }

    agent_tool_storage_patch_edit_t *edits = NULL;
    if (err == ESP_OK) {
        edits = solar_os_memory_calloc(
            edit_count,
            sizeof(*edits),
            SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
            "agent.tool.storage-patch-edits");
        if (edits == NULL) {
            err = ESP_ERR_NO_MEM;
        }
    }
    for (size_t i = 0U; err == ESP_OK && i < edit_count; i++) {
        const solar_os_json_value_t *edit =
            solar_os_json_array_get(edits_value, i);
        if (!solar_os_json_is_object(edit)) {
            err = ESP_ERR_INVALID_ARG;
            break;
        }
        err = solar_os_json_get_uint32(
            solar_os_json_object_get(edit, "offset"),
            &edits[i].offset);
        if (err == ESP_OK) {
            err = solar_os_json_get_uint32(
                solar_os_json_object_get(edit, "delete_bytes"),
                &edits[i].delete_bytes);
        }
        if (err == ESP_OK) {
            err = solar_os_json_get_string(
                solar_os_json_object_get(edit, "insert"),
                edits[i].insert,
                sizeof(edits[i].insert));
        }
    }
    solar_os_json_free(doc);
    if (err != ESP_OK) {
        solar_os_memory_free(edits);
        return err;
    }

    struct stat status;
    if (stat(path, &status) != 0 || !S_ISREG(status.st_mode)) {
        solar_os_memory_free(edits);
        return ESP_ERR_NOT_FOUND;
    }
    if (status.st_size < 0 ||
        (uint64_t)status.st_size > AGENT_TOOL_STORAGE_PATCH_FILE_MAX) {
        solar_os_memory_free(edits);
        return ESP_ERR_INVALID_SIZE;
    }
    const size_t original_size = (size_t)status.st_size;
    uint8_t *original = solar_os_memory_calloc(
        original_size + 1U,
        1U,
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
        "agent.tool.storage-patch-original");
    if (original == NULL) {
        solar_os_memory_free(edits);
        return ESP_ERR_NO_MEM;
    }
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        err = ESP_FAIL;
        goto cleanup_patch;
    }
    const size_t original_read = fread(original, 1U, original_size, file);
    const int original_close = fclose(file);
    if (original_read != original_size || original_close != 0) {
        err = ESP_FAIL;
        goto cleanup_patch;
    }

    uint8_t digest[SOLAR_OS_CRYPTO_SHA256_LEN];
    char current_sha256[SOLAR_OS_CRYPTO_SHA256_HEX_LEN];
    err = solar_os_crypto_sha256_once(original, original_size, digest);
    if (err == ESP_OK) {
        err = solar_os_crypto_bytes_to_hex(digest,
                                           sizeof(digest),
                                           current_sha256,
                                           sizeof(current_sha256));
    }
    char escaped_path[(SOLAR_OS_STORAGE_PATH_MAX *
                       AGENT_TOOL_JSON_ESCAPE_FACTOR) + 1U];
    if (err == ESP_OK) {
        err = solar_os_json_escape_string(path,
                                          escaped_path,
                                          sizeof(escaped_path));
    }
    if (err != ESP_OK) {
        goto cleanup_patch;
    }
    if (!solar_os_crypto_sha256_matches_hex(digest, expected_sha256)) {
        const int written = snprintf(
            result,
            result_len,
            "{\"ok\":false,\"conflict\":true,\"path\":\"%s\","
            "\"expected_sha256\":\"%s\",\"current_sha256\":\"%s\","
            "\"new_sha256\":\"\",\"bytes_written\":0}",
            escaped_path,
            expected_sha256,
            current_sha256);
        err = written >= 0 && (size_t)written < result_len ?
            ESP_OK : ESP_ERR_INVALID_SIZE;
        goto cleanup_patch;
    }

    size_t new_size = original_size;
    size_t previous_end = 0U;
    for (size_t i = 0U; i < edit_count; i++) {
        const size_t offset = edits[i].offset;
        const size_t delete_bytes = edits[i].delete_bytes;
        const size_t insert_bytes = strlen(edits[i].insert);
        if (offset < previous_end || offset > original_size ||
            delete_bytes > original_size - offset ||
            new_size < delete_bytes ||
            new_size - delete_bytes >
                AGENT_TOOL_STORAGE_PATCH_FILE_MAX - insert_bytes) {
            err = ESP_ERR_INVALID_ARG;
            goto cleanup_patch;
        }
        new_size = new_size - delete_bytes + insert_bytes;
        previous_end = offset + delete_bytes;
    }

    uint8_t *patched = solar_os_memory_calloc(
        new_size + 1U,
        1U,
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
        "agent.tool.storage-patch-output");
    if (patched == NULL) {
        err = ESP_ERR_NO_MEM;
        goto cleanup_patch;
    }
    size_t source_offset = 0U;
    size_t output_offset = 0U;
    for (size_t i = 0U; i < edit_count; i++) {
        const size_t unchanged = edits[i].offset - source_offset;
        memcpy(patched + output_offset,
               original + source_offset,
               unchanged);
        output_offset += unchanged;
        const size_t insert_bytes = strlen(edits[i].insert);
        memcpy(patched + output_offset, edits[i].insert, insert_bytes);
        output_offset += insert_bytes;
        source_offset = edits[i].offset + edits[i].delete_bytes;
    }
    memcpy(patched + output_offset,
           original + source_offset,
           original_size - source_offset);

    char temp_path[SOLAR_OS_STORAGE_PATH_MAX];
    char backup_path[SOLAR_OS_STORAGE_PATH_MAX];
    bool staging_paths_available = false;
    for (unsigned i = 0U; i < 4U; i++) {
        const int temp_written = snprintf(temp_path,
                                          sizeof(temp_path),
                                          "%s.agent%u.tmp",
                                          path,
                                          i);
        const int backup_written = snprintf(backup_path,
                                            sizeof(backup_path),
                                            "%s.agent%u.bak",
                                            path,
                                            i);
        if (temp_written < 0 ||
            (size_t)temp_written >= sizeof(temp_path) ||
            backup_written < 0 ||
            (size_t)backup_written >= sizeof(backup_path)) {
            err = ESP_ERR_INVALID_SIZE;
            break;
        }
        struct stat temp_status;
        struct stat backup_status;
        if (stat(temp_path, &temp_status) != 0 &&
            stat(backup_path, &backup_status) != 0) {
            staging_paths_available = true;
            break;
        }
    }
    if (err != ESP_OK || !staging_paths_available) {
        if (err == ESP_OK) {
            err = ESP_ERR_INVALID_STATE;
        }
        solar_os_memory_free(patched);
        goto cleanup_patch;
    }

    bool staging_no_space = false;
    errno = 0;
    file = fopen(temp_path, "wb");
    if (file == NULL) {
        const int open_errno = errno;
        if (open_errno == ENOSPC) {
            (void)remove(temp_path);
            staging_no_space = true;
        } else {
            err = ESP_FAIL;
        }
        if (err != ESP_OK) {
            solar_os_memory_free(patched);
            goto cleanup_patch;
        }
    }
    if (!staging_no_space) {
        errno = 0;
        const size_t written_bytes = fwrite(patched, 1U, new_size, file);
        const int write_errno = written_bytes == new_size ? 0 : errno;
        const int flush_result = fflush(file);
        const int flush_errno = flush_result == 0 ? 0 : errno;
        const int close_result = fclose(file);
        const int close_errno = close_result == 0 ? 0 : errno;
        if (written_bytes != new_size ||
            flush_result != 0 ||
            close_result != 0) {
            const bool no_space =
                write_errno == ENOSPC ||
                flush_errno == ENOSPC ||
                close_errno == ENOSPC;
            (void)remove(temp_path);
            if (no_space) {
                staging_no_space = true;
            } else {
                err = ESP_FAIL;
            }
        }
    }
    if (err != ESP_OK) {
        (void)remove(temp_path);
        solar_os_memory_free(patched);
        goto cleanup_patch;
    }
    file = fopen(path, "rb");
    if (file == NULL) {
        (void)remove(temp_path);
        solar_os_memory_free(patched);
        err = ESP_FAIL;
        goto cleanup_patch;
    }
    err = agent_tool_sha256_file(file, current_sha256);
    const int verify_close = fclose(file);
    if (err != ESP_OK || verify_close != 0) {
        (void)remove(temp_path);
        solar_os_memory_free(patched);
        if (err == ESP_OK) {
            err = ESP_FAIL;
        }
        goto cleanup_patch;
    }
    if (strcasecmp(current_sha256, expected_sha256) != 0) {
        (void)remove(temp_path);
        solar_os_memory_free(patched);
        const int written = snprintf(
            result,
            result_len,
            "{\"ok\":false,\"conflict\":true,\"path\":\"%s\","
            "\"expected_sha256\":\"%s\",\"current_sha256\":\"%s\","
            "\"new_sha256\":\"\",\"bytes_written\":0}",
            escaped_path,
            expected_sha256,
            current_sha256);
        err = written >= 0 && (size_t)written < result_len ?
            ESP_OK : ESP_ERR_INVALID_SIZE;
        goto cleanup_patch;
    }
    if (staging_no_space) {
        err = agent_tool_replace_from_memory(path,
                                             patched,
                                             new_size,
                                             original,
                                             original_size);
        if (err != ESP_OK) {
            solar_os_memory_free(patched);
            goto cleanup_patch;
        }
    } else {
        /*
         * ESP VFS filesystems do not consistently replace an existing rename
         * destination. Preserve the verified original until the staged file
         * has acquired the public path so a failed second rename can be
         * rolled back.
         */
        if (rename(path, backup_path) != 0) {
            (void)remove(temp_path);
            solar_os_memory_free(patched);
            err = ESP_FAIL;
            goto cleanup_patch;
        }
        if (rename(temp_path, path) != 0) {
            const int rollback_result = rename(backup_path, path);
            (void)remove(temp_path);
            solar_os_memory_free(patched);
            err = rollback_result == 0 ? ESP_FAIL : ESP_ERR_INVALID_STATE;
            goto cleanup_patch;
        }
        (void)remove(backup_path);
    }

    char new_sha256[SOLAR_OS_CRYPTO_SHA256_HEX_LEN];
    err = solar_os_crypto_sha256_once(patched, new_size, digest);
    if (err == ESP_OK) {
        err = solar_os_crypto_bytes_to_hex(digest,
                                           sizeof(digest),
                                           new_sha256,
                                           sizeof(new_sha256));
    }
    solar_os_memory_free(patched);
    if (err == ESP_OK) {
        const int written = snprintf(
            result,
            result_len,
            "{\"ok\":true,\"conflict\":false,\"path\":\"%s\","
            "\"expected_sha256\":\"%s\",\"current_sha256\":\"%s\","
            "\"new_sha256\":\"%s\",\"bytes_written\":%u}",
            escaped_path,
            expected_sha256,
            current_sha256,
            new_sha256,
            (unsigned)new_size);
        err = written >= 0 && (size_t)written < result_len ?
            ESP_OK : ESP_ERR_INVALID_SIZE;
    }

cleanup_patch:
    memset(original, 0, original_size + 1U);
    solar_os_memory_free(original);
    if (edits != NULL) {
        memset(edits, 0, edit_count * sizeof(*edits));
    }
    solar_os_memory_free(edits);
    return err;
}

static esp_err_t agent_tool_append_job(
    agent_tool_output_t *output,
    const solar_os_job_inspection_t *inspection,
    bool first)
{
    if (output == NULL || inspection == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t start = output->length;
    const solar_os_job_status_t *status = &inspection->status;
    esp_err_t err = agent_tool_output_append(output,
                                             "%s{\"name\":",
                                             first ? "" : ",");
    if (err == ESP_OK) {
        err = agent_tool_output_append_json_string(output, status->name);
    }
    if (err == ESP_OK) {
        err = agent_tool_output_append(
            output,
            ",\"kind\":\"%s\",\"state\":\"%s\",\"generation\":%" PRIu32
            ",\"start_disposition\":\"%s\",\"start_reason\":\"%s\","
            "\"last_error\":\"%s\",\"worker_stack_bytes\":%" PRIu32
            ",\"worker_stack_region\":\"%s\",\"owner\":",
            solar_os_job_kind_name(status->kind),
            solar_os_job_state_name(status->state),
            status->generation,
            solar_os_job_start_disposition_name(inspection->disposition),
            solar_os_job_start_reason_name(inspection->reason),
            esp_err_to_name(status->last_error),
            status->worker_stack_bytes,
            status->worker_stack_bytes == 0 ? "none" :
                (status->worker_stack_external ? "psram" : "internal"));
    }
    if (err == ESP_OK) {
        err = agent_tool_output_append_json_string(output, status->owner);
    }
    if (err == ESP_OK) {
        err = agent_tool_output_append(output,
                                       ",\"resources_current\":true,"
                                       "\"resources\":[");
    }
    for (size_t i = 0; err == ESP_OK && i < status->resource_count; i++) {
        const solar_os_job_resource_t *resource = &status->resources[i];
        err = agent_tool_output_append(
            output,
            "%s{\"type\":\"%s\",\"name\":",
            i == 0 ? "" : ",",
            solar_os_job_resource_type_name(resource->type));
        if (err == ESP_OK) {
            err = agent_tool_output_append_json_string(output,
                                                       resource->name);
        }
        if (err == ESP_OK) {
            err = agent_tool_output_append(output, ",\"detail\":");
        }
        if (err == ESP_OK) {
            err = agent_tool_output_append_json_string(output,
                                                       resource->detail);
        }
        if (err == ESP_OK) {
            err = agent_tool_output_append(output, "}");
        }
    }
    if (err == ESP_OK) {
        err = agent_tool_output_append(output, "]}");
    }
    if (err != ESP_OK) {
        output->length = start;
        output->buffer[start] = '\0';
    }
    return err;
}

static esp_err_t agent_tool_jobs_list(const char *arguments,
                                      const solar_os_agent_request_t *request,
                                      char *result,
                                      size_t result_len)
{
    (void)request;
    solar_os_json_doc_t *doc = NULL;
    const solar_os_json_value_t *root = NULL;
    esp_err_t err = agent_tool_parse_object(arguments, &doc, &root);
    char filter[AGENT_TOOL_JOB_NAME_MAX] = {0};
    if (err == ESP_OK) {
        const solar_os_json_value_t *name =
            solar_os_json_object_get(root, "name");
        if (name != NULL) {
            err = solar_os_json_get_string(name,
                                           filter,
                                           sizeof(filter));
        }
    }
    solar_os_json_free(doc);
    if (err != ESP_OK) {
        return err;
    }

    solar_os_task_admission_status_t memory;
    solar_os_task_get_admission_status(&memory);
    agent_tool_output_t output = {
        .buffer = result,
        .capacity = result_len > 96U ? result_len - 96U : 0U,
    };
    err = agent_tool_output_append(
        &output,
        "{\"memory\":{\"internal_free_bytes\":%" PRIu32
        ",\"internal_largest_block_bytes\":%" PRIu32
        ",\"psram_free_bytes\":%" PRIu32
        ",\"psram_largest_block_bytes\":%" PRIu32
        ",\"background_internal_reserve_bytes\":%" PRIu32
        ",\"task_internal_overhead_bytes\":%" PRIu32
        ",\"external_stacks_supported\":%s},\"jobs\":[",
        memory.internal_free_bytes,
        memory.internal_largest_block_bytes,
        memory.external_free_bytes,
        memory.external_largest_block_bytes,
        memory.background_internal_reserve_bytes,
        memory.task_internal_overhead_bytes,
        memory.external_stacks_supported ? "true" : "false");

    size_t total = 0;
    size_t returned = 0;
    bool truncated = false;
    if (err == ESP_OK && filter[0] != '\0') {
        solar_os_job_inspection_t inspection;
        if (solar_os_jobs_inspect_by_name(filter, &inspection)) {
            total = 1;
            err = agent_tool_append_job(&output, &inspection, true);
            if (err == ESP_OK) {
                returned = 1;
            } else if (err == ESP_ERR_INVALID_SIZE) {
                truncated = true;
                err = ESP_OK;
            }
        }
    } else if (err == ESP_OK) {
        total = solar_os_jobs_count();
        for (size_t i = 0; i < total; i++) {
            solar_os_job_inspection_t inspection;
            if (!solar_os_jobs_inspect(i, &inspection)) {
                continue;
            }
            err = agent_tool_append_job(&output,
                                        &inspection,
                                        returned == 0);
            if (err == ESP_ERR_INVALID_SIZE) {
                truncated = true;
                err = ESP_OK;
                break;
            }
            if (err != ESP_OK) {
                break;
            }
            returned++;
        }
    }
    if (err == ESP_OK) {
        output.capacity = result_len;
        err = agent_tool_output_append(
            &output,
            "],\"count\":%u,\"total\":%u,\"truncated\":%s}",
            (unsigned)returned,
            (unsigned)total,
            truncated ? "true" : "false");
    }
    return err;
}

static esp_err_t agent_tool_display_list(const char *arguments,
                                         const solar_os_agent_request_t *request,
                                         char *result,
                                         size_t result_len)
{
    (void)request;
    solar_os_json_doc_t *doc = NULL;
    const solar_os_json_value_t *root = NULL;
    esp_err_t err = agent_tool_parse_object(arguments, &doc, &root);
    if (err != ESP_OK) {
        return err;
    }
    (void)root;
    solar_os_json_free(doc);

    const size_t total = solar_os_display_target_count();
    agent_tool_output_t output = {
        .buffer = result,
        .capacity = result_len,
    };
    err = agent_tool_output_append(&output,
                                   "{\"count\":%u,\"displays\":[",
                                   (unsigned)total);
    bool first = true;
    bool truncated = false;
    for (size_t i = 0; err == ESP_OK && i < total; i++) {
        solar_os_display_target_t target;
        if (!solar_os_display_get_target(i, &target)) {
            continue;
        }

        char name[SOLAR_OS_DISPLAY_TARGET_NAME_MAX *
                      AGENT_TOOL_JSON_ESCAPE_FACTOR +
                  1U];
        char source[SOLAR_OS_DISPLAY_TARGET_SOURCE_MAX *
                        AGENT_TOOL_JSON_ESCAPE_FACTOR +
                    1U];
        char driver[SOLAR_OS_DISPLAY_TARGET_DRIVER_MAX *
                        AGENT_TOOL_JSON_ESCAPE_FACTOR +
                    1U];
        char controller[SOLAR_OS_DISPLAY_TARGET_CONTROLLER_MAX *
                            AGENT_TOOL_JSON_ESCAPE_FACTOR +
                        1U];
        char role[SOLAR_OS_DISPLAY_TARGET_ROLE_MAX *
                      AGENT_TOOL_JSON_ESCAPE_FACTOR +
                  1U];
        char owner[SOLAR_OS_DISPLAY_TARGET_OWNER_MAX *
                       AGENT_TOOL_JSON_ESCAPE_FACTOR +
                   1U];
        if (solar_os_json_escape_string(target.name,
                                        name,
                                        sizeof(name)) != ESP_OK ||
            solar_os_json_escape_string(target.source,
                                        source,
                                        sizeof(source)) != ESP_OK ||
            solar_os_json_escape_string(target.driver,
                                        driver,
                                        sizeof(driver)) != ESP_OK ||
            solar_os_json_escape_string(target.controller,
                                        controller,
                                        sizeof(controller)) != ESP_OK ||
            solar_os_json_escape_string(target.role,
                                        role,
                                        sizeof(role)) != ESP_OK ||
            solar_os_json_escape_string(target.owner,
                                        owner,
                                        sizeof(owner)) != ESP_OK) {
            truncated = true;
            break;
        }

        char item[1024];
        const int written = snprintf(
            item,
            sizeof(item),
            "%s{\"name\":\"%s\",\"source\":\"%s\",\"driver\":\"%s\","
            "\"controller\":\"%s\",\"width\":%u,\"height\":%u,"
            "\"role\":\"%s\",\"ready\":%s,\"brightness_supported\":%s,"
            "\"owner\":\"%s\"}",
            first ? "" : ",",
            name,
            source,
            driver,
            controller,
            (unsigned)target.width,
            (unsigned)target.height,
            role,
            target.ready ? "true" : "false",
            target.brightness_supported ? "true" : "false",
            owner);
        const size_t tail_reserve = sizeof("],\"truncated\":true}");
        if (written < 0 ||
            (size_t)written >= sizeof(item) ||
            output.length + (size_t)written + tail_reserve >= output.capacity) {
            truncated = true;
            break;
        }
        err = agent_tool_output_append(&output, "%s", item);
        first = false;
    }
    if (err == ESP_OK) {
        err = agent_tool_output_append(&output,
                                       "],\"truncated\":%s}",
                                       truncated ? "true" : "false");
    }
    return err;
}

static esp_err_t agent_tool_hardware_describe(
    const char *arguments,
    const solar_os_agent_request_t *request,
    char *result,
    size_t result_len)
{
    (void)request;
    solar_os_json_doc_t *doc = NULL;
    const solar_os_json_value_t *root = NULL;
    esp_err_t err = agent_tool_parse_object(arguments, &doc, &root);
    if (err != ESP_OK) {
        return err;
    }
    (void)root;
    solar_os_json_free(doc);

    char capabilities[SOLAR_OS_BOARD_CAPABILITIES_TEXT_MAX];
    char escaped_capabilities[sizeof(capabilities) *
                              AGENT_TOOL_JSON_ESCAPE_FACTOR + 1U];
    solar_os_board_capabilities_format(capabilities, sizeof(capabilities));
    err = solar_os_json_escape_string(capabilities,
                                      escaped_capabilities,
                                      sizeof(escaped_capabilities));
    if (err != ESP_OK) {
        return err;
    }
    const int written = snprintf(
        result,
        result_len,
        "{\"board\":\"%s\",\"name\":\"%s\",\"capabilities\":\"%s\","
        "\"psram_bytes\":%u}",
        SOLAR_OS_BOARD_ID,
        SOLAR_OS_BOARD_NAME,
        escaped_capabilities,
        (unsigned)SOLAR_OS_BOARD_PSRAM_BYTES);
    return written >= 0 && (size_t)written < result_len ?
        ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t agent_tool_gpio_list(const char *arguments,
                                      const solar_os_agent_request_t *request,
                                      char *result,
                                      size_t result_len)
{
    (void)request;
#if !SOLAR_OS_PACKAGE_SERVICE_GPIO
    (void)arguments;
    (void)result;
    (void)result_len;
    return ESP_ERR_NOT_SUPPORTED;
#else
    solar_os_json_doc_t *doc = NULL;
    const solar_os_json_value_t *root = NULL;
    esp_err_t err = agent_tool_parse_object(arguments, &doc, &root);
    if (err != ESP_OK) {
        return err;
    }
    (void)root;
    solar_os_json_free(doc);

    const size_t total = solar_os_gpio_pin_count();
    agent_tool_output_t output = {
        .buffer = result,
        .capacity = result_len,
    };
    err = agent_tool_output_append(&output,
                                   "{\"count\":%u,\"pins\":[",
                                   (unsigned)total);
    bool first = true;
    bool truncated = false;
    for (size_t i = 0U; err == ESP_OK && i < total; i++) {
        solar_os_gpio_pin_info_t info;
        if (!solar_os_gpio_get_pin_info(i, &info)) {
            continue;
        }
        char escaped_role[128];
        char escaped_owner[(SOLAR_OS_GPIO_OWNER_MAX *
                            AGENT_TOOL_JSON_ESCAPE_FACTOR) + 1U];
        if (solar_os_json_escape_string(info.role != NULL ? info.role : "",
                                        escaped_role,
                                        sizeof(escaped_role)) != ESP_OK ||
            solar_os_json_escape_string(info.owner,
                                        escaped_owner,
                                        sizeof(escaped_owner)) != ESP_OK) {
            truncated = true;
            break;
        }
        char item[768];
        const int written = snprintf(
            item,
            sizeof(item),
            "%s{\"pin\":%d,\"role\":\"%s\",\"policy\":\"%s\","
            "\"expansion\":%s,\"runtime_allowed\":%s,\"available\":%s,"
            "\"claimed\":%s,\"owner\":\"%s\",\"configured\":%s,"
            "\"mode\":\"%s\",\"pull\":\"%s\",\"level\":%s}",
            first ? "" : ",",
            info.pin,
            escaped_role,
            solar_os_pin_policy_name(info.policy),
            info.expansion ? "true" : "false",
            info.runtime_allowed ? "true" : "false",
            info.available ? "true" : "false",
            info.claimed ? "true" : "false",
            escaped_owner,
            info.configured ? "true" : "false",
            solar_os_gpio_mode_name(info.mode),
            solar_os_gpio_pull_name(info.pull),
            info.level_valid ? (info.level ? "true" : "false") : "null");
        const size_t tail_reserve = sizeof("],\"truncated\":true}");
        if (written < 0 ||
            (size_t)written >= sizeof(item) ||
            output.length + (size_t)written + tail_reserve >= output.capacity) {
            truncated = true;
            break;
        }
        err = agent_tool_output_append(&output, "%s", item);
        first = false;
    }
    if (err == ESP_OK) {
        err = agent_tool_output_append(&output,
                                       "],\"truncated\":%s}",
                                       truncated ? "true" : "false");
    }
    return err;
#endif
}

static esp_err_t agent_tool_gpio_read(const char *arguments,
                                      const solar_os_agent_request_t *request,
                                      char *result,
                                      size_t result_len)
{
    (void)request;
#if !SOLAR_OS_PACKAGE_SERVICE_GPIO
    (void)arguments;
    (void)result;
    (void)result_len;
    return ESP_ERR_NOT_SUPPORTED;
#else
    solar_os_json_doc_t *doc = NULL;
    const solar_os_json_value_t *root = NULL;
    esp_err_t err = agent_tool_parse_object(arguments, &doc, &root);
    if (err != ESP_OK) {
        return err;
    }
    int64_t pin = -1;
    err = solar_os_json_get_int64(solar_os_json_object_get(root, "pin"), &pin);
    solar_os_json_free(doc);
    if (err != ESP_OK || pin < 0 || pin > 63) {
        return ESP_ERR_INVALID_ARG;
    }

    solar_os_gpio_pin_info_t info;
    if (!solar_os_gpio_get_pin_info_by_pin((int)pin, &info)) {
        return ESP_ERR_NOT_FOUND;
    }
    char escaped_owner[(SOLAR_OS_GPIO_OWNER_MAX *
                        AGENT_TOOL_JSON_ESCAPE_FACTOR) + 1U];
    err = solar_os_json_escape_string(info.owner,
                                      escaped_owner,
                                      sizeof(escaped_owner));
    if (err != ESP_OK) {
        return err;
    }
    const int written = snprintf(
        result,
        result_len,
        "{\"pin\":%d,\"configured\":%s,\"readable\":%s,\"level\":%s,"
        "\"owner\":\"%s\"}",
        info.pin,
        info.configured ? "true" : "false",
        info.level_valid ? "true" : "false",
        info.level_valid ? (info.level ? "true" : "false") : "null",
        escaped_owner);
    return written >= 0 && (size_t)written < result_len ?
        ESP_OK : ESP_ERR_INVALID_SIZE;
#endif
}

static esp_err_t agent_tool_buses_list(const char *arguments,
                                       const solar_os_agent_request_t *request,
                                       char *result,
                                       size_t result_len)
{
    (void)request;
#if !SOLAR_OS_PACKAGE_SERVICE_RESOURCES
    (void)arguments;
    (void)result;
    (void)result_len;
    return ESP_ERR_NOT_SUPPORTED;
#else
    solar_os_json_doc_t *doc = NULL;
    const solar_os_json_value_t *root = NULL;
    esp_err_t err = agent_tool_parse_object(arguments, &doc, &root);
    if (err != ESP_OK) {
        return err;
    }
    (void)root;
    solar_os_json_free(doc);

    const size_t total = solar_os_bus_count();
    agent_tool_output_t output = {
        .buffer = result,
        .capacity = result_len,
    };
    err = agent_tool_output_append(&output,
                                   "{\"count\":%u,\"buses\":[",
                                   (unsigned)total);
    bool first = true;
    bool truncated = false;
    for (size_t i = 0U; err == ESP_OK && i < total; i++) {
        solar_os_bus_info_t info;
        if (!solar_os_bus_get(i, &info)) {
            continue;
        }
        char escaped_name[(SOLAR_OS_BUS_NAME_MAX *
                           AGENT_TOOL_JSON_ESCAPE_FACTOR) + 1U];
        if (solar_os_json_escape_string(info.name,
                                        escaped_name,
                                        sizeof(escaped_name)) != ESP_OK) {
            truncated = true;
            break;
        }
        char pins[256];
        int pins_written = -1;
        switch (info.protocol) {
        case SOLAR_OS_BUS_PROTOCOL_I2C:
            pins_written = snprintf(
                pins,
                sizeof(pins),
                "{\"sda\":%d,\"scl\":%d,\"speed_hz\":%" PRIu32 "}",
                info.config.i2c.sda_pin,
                info.config.i2c.scl_pin,
                info.config.i2c.speed_hz);
            break;
        case SOLAR_OS_BUS_PROTOCOL_SPI:
            pins_written = snprintf(
                pins,
                sizeof(pins),
                "{\"sclk\":%d,\"miso\":%d,\"mosi\":%d,\"cs_count\":%u}",
                info.config.spi.sclk_pin,
                info.config.spi.miso_pin,
                info.config.spi.mosi_pin,
                (unsigned)info.config.spi.cs_count);
            break;
        case SOLAR_OS_BUS_PROTOCOL_UART:
        case SOLAR_OS_BUS_PROTOCOL_MIDI:
            pins_written = snprintf(
                pins,
                sizeof(pins),
                "{\"tx\":%d,\"rx\":%d,\"baud\":%" PRIu32 "}",
                info.config.uart.tx_pin,
                info.config.uart.rx_pin,
                info.config.uart.baud_rate);
            break;
        case SOLAR_OS_BUS_PROTOCOL_ONEWIRE:
            pins_written = snprintf(pins,
                                    sizeof(pins),
                                    "{\"pin\":%d}",
                                    info.config.onewire.pin);
            break;
        case SOLAR_OS_BUS_PROTOCOL_PS2:
            pins_written = snprintf(pins,
                                    sizeof(pins),
                                    "{\"clock\":%d,\"data\":%d}",
                                    info.config.ps2.clock_pin,
                                    info.config.ps2.data_pin);
            break;
        default:
            pins_written = snprintf(pins, sizeof(pins), "{}");
            break;
        }
        char item[768];
        const int written = pins_written < 0 ||
                                    (size_t)pins_written >= sizeof(pins) ?
            -1 :
            snprintf(
                item,
                sizeof(item),
                "%s{\"name\":\"%s\",\"protocol\":\"%s\","
                "\"origin\":\"%s\",\"sharing\":\"%s\",\"ready\":%s,"
                "\"attached\":%s,\"detachable\":%s,\"lease_count\":%u,"
                "\"config\":%s}",
                first ? "" : ",",
                escaped_name,
                solar_os_bus_protocol_name(info.protocol),
                solar_os_bus_origin_name(info.origin),
                solar_os_bus_sharing_name(info.sharing),
                info.ready ? "true" : "false",
                info.attached ? "true" : "false",
                info.detachable ? "true" : "false",
                (unsigned)info.lease_count,
                pins);
        const size_t tail_reserve = sizeof("],\"truncated\":true}");
        if (written < 0 ||
            (size_t)written >= sizeof(item) ||
            output.length + (size_t)written + tail_reserve >= output.capacity) {
            truncated = true;
            break;
        }
        err = agent_tool_output_append(&output, "%s", item);
        first = false;
    }
    if (err == ESP_OK) {
        err = agent_tool_output_append(&output,
                                       "],\"truncated\":%s}",
                                       truncated ? "true" : "false");
    }
    return err;
#endif
}

static esp_err_t agent_tool_network_status(
    const char *arguments,
    const solar_os_agent_request_t *request,
    char *result,
    size_t result_len)
{
    (void)request;
#if !SOLAR_OS_PACKAGE_SERVICE_WIFI
    (void)arguments;
    (void)result;
    (void)result_len;
    return ESP_ERR_NOT_SUPPORTED;
#else
    solar_os_json_doc_t *doc = NULL;
    const solar_os_json_value_t *root = NULL;
    esp_err_t err = agent_tool_parse_object(arguments, &doc, &root);
    if (err != ESP_OK) {
        return err;
    }
    (void)root;
    solar_os_json_free(doc);

    solar_os_wifi_status_t status;
    solar_os_wifi_get_status(&status);
    char escaped_ssid[(SOLAR_OS_WIFI_SSID_MAX *
                       AGENT_TOOL_JSON_ESCAPE_FACTOR) + 1U];
    if (solar_os_json_escape_string(status.ssid,
                                    escaped_ssid,
                                    sizeof(escaped_ssid)) != ESP_OK) {
        return ESP_ERR_INVALID_SIZE;
    }
    const int written = snprintf(
        result,
        result_len,
        "{\"state\":\"%s\",\"started\":%s,\"connected\":%s,"
        "\"ssid\":\"%s\",\"ip\":\"%s\",\"gateway\":\"%s\","
        "\"rssi\":%d,\"channel\":%u,\"ap_running\":%s,"
        "\"ap_station_count\":%u,\"nat_active\":%s}",
        solar_os_wifi_state_name(status.state),
        status.started ? "true" : "false",
        status.connected ? "true" : "false",
        escaped_ssid,
        status.ip,
        status.gateway,
        (int)status.rssi,
        (unsigned)status.channel,
        status.ap_running ? "true" : "false",
        (unsigned)status.ap_station_count,
        status.nat_active ? "true" : "false");
    return written >= 0 && (size_t)written < result_len ?
        ESP_OK : ESP_ERR_INVALID_SIZE;
#endif
}

static esp_err_t agent_tool_sensors_read(const char *arguments,
                                         const solar_os_agent_request_t *request,
                                         char *result,
                                         size_t result_len)
{
    (void)request;
    solar_os_json_doc_t *doc = NULL;
    const solar_os_json_value_t *root = NULL;
    esp_err_t err = agent_tool_parse_object(arguments, &doc, &root);
    if (err != ESP_OK) {
        return err;
    }
    (void)root;
    solar_os_json_free(doc);

    agent_tool_output_t output = {
        .buffer = result,
        .capacity = result_len,
    };
    err = agent_tool_output_append(&output, "{\"battery\":");
#if SOLAR_OS_PACKAGE_SERVICE_BATTERY
    solar_os_battery_status_t battery;
    const esp_err_t battery_err = solar_os_battery_get_status(&battery);
    if (err == ESP_OK && battery_err == ESP_OK) {
        err = agent_tool_output_append(
            &output,
            "{\"ok\":true,\"voltage_mv\":%u,\"percent\":%u,"
            "\"percent_estimated\":%s,\"adc_calibrated\":%s,"
            "\"external_power\":%s,\"charging\":%s,"
            "\"charging_known\":%s}",
            (unsigned)battery.voltage_mv,
            (unsigned)battery.percent,
            battery.percent_estimated ? "true" : "false",
            battery.adc_calibrated ? "true" : "false",
            battery.external_power ? "true" : "false",
            battery.charging ? "true" : "false",
            battery.charging_known ? "true" : "false");
    } else if (err == ESP_OK) {
        err = agent_tool_output_append(
            &output,
            "{\"ok\":false,\"error\":\"%s\"}",
            esp_err_to_name(battery_err));
    }
#else
    if (err == ESP_OK) {
        err = agent_tool_output_append(&output, "null");
    }
#endif
    if (err == ESP_OK) {
        err = agent_tool_output_append(&output, ",\"environment\":");
    }
#if SOLAR_OS_PACKAGE_SERVICE_SENSORS
    solar_os_environment_t environment;
    const esp_err_t environment_err =
        solar_os_sensors_read_environment(&environment);
    if (err == ESP_OK && environment_err == ESP_OK) {
        err = agent_tool_output_append(
            &output,
            "{\"ok\":true,\"temperature_c\":%.2f,"
            "\"humidity_percent\":%.2f}",
            (double)environment.temperature_c,
            (double)environment.humidity_percent);
    } else if (err == ESP_OK) {
        err = agent_tool_output_append(
            &output,
            "{\"ok\":false,\"error\":\"%s\"}",
            esp_err_to_name(environment_err));
    }
#else
    if (err == ESP_OK) {
        err = agent_tool_output_append(&output, "null");
    }
#endif
    if (err == ESP_OK) {
        err = agent_tool_output_append(&output, "}");
    }
    return err;
}

static esp_err_t agent_tool_script_execute(
    solar_os_agent_script_language_t language,
    solar_os_script_input_t input_type,
    const char *input,
    int argc,
    const char *const *argv,
    const solar_os_agent_request_t *request,
    char *result,
    size_t result_len)
{
    if (request == NULL || request->run_script == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (input == NULL || argc < 0 || argc > SOLAR_OS_APP_ARG_MAX ||
        (argc > 0 && argv == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    char *output = solar_os_memory_calloc(
        1,
        AGENT_TOOL_SCRIPT_OUTPUT_MAX,
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
        "agent.tool.script-output");
    char *escaped_output = solar_os_memory_alloc(
        AGENT_TOOL_SCRIPT_OUTPUT_MAX * AGENT_TOOL_JSON_ESCAPE_FACTOR + 1U,
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
        "agent.tool.script-json");
    char *escaped_error = solar_os_memory_alloc(
        SOLAR_OS_SCRIPT_ERROR_MAX * AGENT_TOOL_JSON_ESCAPE_FACTOR + 1U,
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
        "agent.tool.script-error");
    if (output == NULL || escaped_output == NULL || escaped_error == NULL) {
        solar_os_memory_free(escaped_error);
        solar_os_memory_free(escaped_output);
        solar_os_memory_free(output);
        return ESP_ERR_NO_MEM;
    }

    solar_os_script_run_result_t run_result = {0};
    esp_err_t err = request->run_script(language,
                                        input_type,
                                        input,
                                        argc,
                                        argv,
                                        output,
                                        AGENT_TOOL_SCRIPT_OUTPUT_MAX,
                                        &run_result,
                                        request->user_data);
    if (err != ESP_OK) {
        goto cleanup;
    }
    err = solar_os_json_escape_string(output,
                                      escaped_output,
                                      AGENT_TOOL_SCRIPT_OUTPUT_MAX *
                                          AGENT_TOOL_JSON_ESCAPE_FACTOR + 1U);
    if (err == ESP_OK) {
        err = solar_os_json_escape_string(
            run_result.error,
            escaped_error,
            SOLAR_OS_SCRIPT_ERROR_MAX * AGENT_TOOL_JSON_ESCAPE_FACTOR + 1U);
    }
    if (err != ESP_OK) {
        goto cleanup;
    }

    const int written = snprintf(
        result,
        result_len,
        "{\"ok\":%s,\"status\":\"%s\",\"output\":\"%s\","
        "\"output_truncated\":%s,\"cancelled\":%s,\"timed_out\":%s,"
        "\"error\":\"%s\"}",
        run_result.success ? "true" : "false",
        esp_err_to_name(run_result.status),
        escaped_output,
        run_result.output_truncated ? "true" : "false",
        run_result.cancelled ? "true" : "false",
        run_result.timed_out ? "true" : "false",
        escaped_error);
    err = written >= 0 && (size_t)written < result_len ?
        ESP_OK : ESP_ERR_INVALID_SIZE;

cleanup:
    solar_os_memory_free(escaped_error);
    solar_os_memory_free(escaped_output);
    solar_os_memory_free(output);
    return err;
}

static esp_err_t agent_tool_script_run(
    solar_os_agent_script_language_t language,
    const char *arguments,
    const solar_os_agent_request_t *request,
    char *result,
    size_t result_len)
{
    solar_os_json_doc_t *doc = NULL;
    const solar_os_json_value_t *root = NULL;
    esp_err_t err = agent_tool_parse_object(arguments, &doc, &root);
    if (err != ESP_OK) {
        return err;
    }
    char *source = solar_os_memory_calloc(
        1,
        AGENT_TOOL_SCRIPT_SOURCE_MAX + 1U,
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
        "agent.tool.script-source");
    if (source == NULL) {
        solar_os_json_free(doc);
        return ESP_ERR_NO_MEM;
    }
    err = solar_os_json_get_string(
        solar_os_json_object_get(root, "source"),
        source,
        AGENT_TOOL_SCRIPT_SOURCE_MAX + 1U);
    solar_os_json_free(doc);
    if (err == ESP_OK && source[0] == '\0') {
        err = ESP_ERR_INVALID_ARG;
    }
    if (err == ESP_OK) {
        const char *argv[] = {"<agent-tool>"};
        err = agent_tool_script_execute(language,
                                        SOLAR_OS_SCRIPT_INPUT_SOURCE,
                                        source,
                                        1,
                                        argv,
                                        request,
                                        result,
                                        result_len);
    }
    memset(source, 0, AGENT_TOOL_SCRIPT_SOURCE_MAX + 1U);
    solar_os_memory_free(source);
    return err;
}

static esp_err_t agent_tool_script_run_python(
    const char *arguments,
    const solar_os_agent_request_t *request,
    char *result,
    size_t result_len)
{
    return agent_tool_script_run(SOLAR_OS_AGENT_SCRIPT_PYTHON,
                                 arguments,
                                 request,
                                 result,
                                 result_len);
}

static esp_err_t agent_tool_script_run_lua(
    const char *arguments,
    const solar_os_agent_request_t *request,
    char *result,
    size_t result_len)
{
    return agent_tool_script_run(SOLAR_OS_AGENT_SCRIPT_LUA,
                                 arguments,
                                 request,
                                 result,
                                 result_len);
}

static esp_err_t agent_tool_script_run_file(
    const char *arguments,
    const solar_os_agent_request_t *request,
    char *result,
    size_t result_len)
{
    if (request == NULL || request->run_script == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    solar_os_json_doc_t *doc = NULL;
    const solar_os_json_value_t *root = NULL;
    esp_err_t err = agent_tool_parse_object(arguments, &doc, &root);
    if (err != ESP_OK) {
        return err;
    }

    char language_name[8];
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    err = solar_os_json_get_string(
        solar_os_json_object_get(root, "language"),
        language_name,
        sizeof(language_name));
    if (err == ESP_OK) {
        err = agent_tool_storage_path(root, request, path, sizeof(path));
    }
    solar_os_agent_script_language_t language = 0U;
    if (err == ESP_OK && strcmp(language_name, "python") == 0) {
        language = SOLAR_OS_AGENT_SCRIPT_PYTHON;
    } else if (err == ESP_OK && strcmp(language_name, "lua") == 0) {
        language = SOLAR_OS_AGENT_SCRIPT_LUA;
    } else if (err == ESP_OK) {
        err = ESP_ERR_INVALID_ARG;
    }
    if (err == ESP_OK &&
        (request->script_languages & (uint32_t)language) == 0U) {
        err = ESP_ERR_NOT_SUPPORTED;
    }

    const solar_os_json_value_t *args_value =
        solar_os_json_object_get(root, "args");
    const size_t arg_count = solar_os_json_array_size(args_value);
    if (err == ESP_OK &&
        (!solar_os_json_is_array(args_value) ||
         arg_count >= SOLAR_OS_APP_ARG_MAX)) {
        err = ESP_ERR_INVALID_ARG;
    }
    char argv_storage[SOLAR_OS_APP_ARG_MAX][SOLAR_OS_APP_ARG_LEN] = {{0}};
    const char *argv[SOLAR_OS_APP_ARG_MAX] = {0};
    if (err == ESP_OK) {
        strlcpy(argv_storage[0], path, sizeof(argv_storage[0]));
        argv[0] = argv_storage[0];
    }
    for (size_t i = 0U; err == ESP_OK && i < arg_count; i++) {
        err = solar_os_json_get_string(
            solar_os_json_array_get(args_value, i),
            argv_storage[i + 1U],
            sizeof(argv_storage[i + 1U]));
        argv[i + 1U] = argv_storage[i + 1U];
    }
    solar_os_json_free(doc);
    if (err != ESP_OK) {
        return err;
    }

    struct stat status;
    if (stat(path, &status) != 0 || !S_ISREG(status.st_mode)) {
        return ESP_ERR_NOT_FOUND;
    }
    return agent_tool_script_execute(language,
                                     SOLAR_OS_SCRIPT_INPUT_FILE,
                                     path,
                                     (int)arg_count + 1,
                                     argv,
                                     request,
                                     result,
                                     result_len);
}

solar_os_agent_tool_policy_decision_t solar_os_agent_tools_policy_decision(
    solar_os_agent_tool_policy_t policy,
    solar_os_agent_tool_risk_t risk)
{
    switch (policy) {
    case SOLAR_OS_AGENT_TOOL_POLICY_OFF:
        return SOLAR_OS_AGENT_TOOL_POLICY_DENY;
    case SOLAR_OS_AGENT_TOOL_POLICY_READONLY:
        return risk == SOLAR_OS_AGENT_TOOL_RISK_READ_ONLY ?
            SOLAR_OS_AGENT_TOOL_POLICY_ALLOW :
            SOLAR_OS_AGENT_TOOL_POLICY_DENY;
    case SOLAR_OS_AGENT_TOOL_POLICY_CONFIRM:
        return risk == SOLAR_OS_AGENT_TOOL_RISK_READ_ONLY ?
            SOLAR_OS_AGENT_TOOL_POLICY_ALLOW :
            SOLAR_OS_AGENT_TOOL_POLICY_CONFIRM_ONCE;
    case SOLAR_OS_AGENT_TOOL_POLICY_ALL:
        return SOLAR_OS_AGENT_TOOL_POLICY_ALLOW;
    default:
        return SOLAR_OS_AGENT_TOOL_POLICY_DENY;
    }
}

static size_t agent_tool_collect_bootstrap(
    const solar_os_agent_request_t *request,
    solar_os_agent_tool_policy_t policy,
    solar_os_agent_tool_descriptor_t *descriptors,
    size_t capacity)
{
    if (descriptors == NULL || capacity == 0) {
        return 0;
    }
    if (capacity > SOLAR_OS_AGENT_TOOL_ACTIVE_MAX) {
        capacity = SOLAR_OS_AGENT_TOOL_ACTIVE_MAX;
    }
    size_t count = 0;
    for (size_t i = 0; i < AGENT_TOOL_COUNT && count < capacity; i++) {
        const agent_tool_definition_t *definition = &AGENT_TOOL_REGISTRY[i];
        if (!definition->bootstrap ||
            !agent_tool_is_available(definition, request) ||
            solar_os_agent_tools_policy_decision(policy, definition->risk) ==
                SOLAR_OS_AGENT_TOOL_POLICY_DENY) {
            continue;
        }
        descriptors[count++] = definition->provider;
    }
    return count;
}

size_t solar_os_agent_tools_collect(
    const solar_os_agent_request_t *request,
    solar_os_agent_tool_policy_t policy,
    solar_os_agent_tool_descriptor_t *descriptors,
    size_t capacity)
{
    if (descriptors == NULL || capacity == 0U) {
        return 0U;
    }
    if (capacity > SOLAR_OS_AGENT_TOOL_ACTIVE_MAX) {
        capacity = SOLAR_OS_AGENT_TOOL_ACTIVE_MAX;
    }
    size_t count =
        agent_tool_collect_bootstrap(request, policy, descriptors, capacity);

    if (request != NULL && request->prompt != NULL &&
        request->prompt[0] != '\0' && count < capacity) {
        if (agent_tool_prompt_has_path(request->prompt)) {
            (void)agent_tool_append_named("storage_stat",
                                          request,
                                          policy,
                                          descriptors,
                                          &count,
                                          capacity);
        }
        agent_tool_search_match_t matches[AGENT_TOOL_PROMPT_MATCH_MAX] = {0};
        size_t match_capacity = capacity - count;
        if (match_capacity > AGENT_TOOL_PROMPT_MATCH_MAX) {
            match_capacity = AGENT_TOOL_PROMPT_MATCH_MAX;
        }
        const size_t match_count =
            agent_tool_search_matches(request->prompt,
                                      request,
                                      policy,
                                      matches,
                                      match_capacity);
        for (size_t i = 0U; i < match_count; i++) {
            (void)agent_tool_append_named(
                matches[i].definition->provider.name,
                request,
                policy,
                descriptors,
                &count,
                capacity);
        }
    }
    return count;
}

size_t solar_os_agent_tools_collect_discovered(
    const char *arguments,
    const solar_os_agent_request_t *request,
    solar_os_agent_tool_policy_t policy,
    solar_os_agent_tool_descriptor_t *descriptors,
    size_t capacity)
{
    if (arguments == NULL || descriptors == NULL || capacity == 0U) {
        return 0U;
    }
    if (capacity > SOLAR_OS_AGENT_TOOL_ACTIVE_MAX) {
        capacity = SOLAR_OS_AGENT_TOOL_ACTIVE_MAX;
    }
    size_t count =
        agent_tool_collect_bootstrap(request,
                                     policy,
                                     descriptors,
                                     capacity);
    char query[AGENT_TOOL_SEARCH_QUERY_MAX + 1U];
    if (agent_tool_parse_query(arguments, query, sizeof(query)) != ESP_OK ||
        count >= capacity) {
        return count;
    }

    agent_tool_search_match_t matches[AGENT_TOOL_SEARCH_MATCH_MAX] = {0};
    const size_t match_count =
        agent_tool_search_matches(query,
                                  request,
                                  policy,
                                  matches,
                                  AGENT_TOOL_SEARCH_MATCH_MAX);
    for (size_t i = 0U; i < match_count; i++) {
        bool duplicate = false;
        for (size_t j = 0U; j < count; j++) {
            if (strcmp(descriptors[j].name,
                       matches[i].definition->provider.name) == 0) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate && count < capacity) {
            descriptors[count++] = matches[i].definition->provider;
        }
    }
    return count;
}

size_t solar_os_agent_tools_activate(
    const char *name,
    const solar_os_agent_request_t *request,
    solar_os_agent_tool_policy_t policy,
    solar_os_agent_tool_descriptor_t *descriptors,
    size_t count,
    size_t capacity)
{
    if (name == NULL || name[0] == '\0' || descriptors == NULL ||
        capacity == 0U || count > capacity) {
        return count;
    }
    if (capacity > SOLAR_OS_AGENT_TOOL_ACTIVE_MAX) {
        capacity = SOLAR_OS_AGENT_TOOL_ACTIVE_MAX;
    }
    if (count >= capacity) {
        return count;
    }
    for (size_t i = 0U; i < count; i++) {
        if (descriptors[i].name != NULL &&
            strcmp(descriptors[i].name, name) == 0) {
            return count;
        }
    }
    for (size_t i = 0U; i < AGENT_TOOL_COUNT; i++) {
        const agent_tool_definition_t *definition = &AGENT_TOOL_REGISTRY[i];
        if (strcmp(definition->provider.name, name) != 0) {
            continue;
        }
        if (!definition->activate_on_demand ||
            !agent_tool_is_available(definition, request) ||
            solar_os_agent_tools_policy_decision(policy, definition->risk) ==
                SOLAR_OS_AGENT_TOOL_POLICY_DENY) {
            return count;
        }
        descriptors[count++] = definition->provider;
        return count;
    }
    return count;
}

bool solar_os_agent_tools_is_discovery(const char *name)
{
    return name != NULL && strcmp(name, "tool_search") == 0;
}

size_t solar_os_agent_tools_count(void)
{
    return AGENT_TOOL_COUNT;
}

bool solar_os_agent_tools_get(size_t index, solar_os_agent_tool_info_t *info)
{
    if (index >= AGENT_TOOL_COUNT || info == NULL) {
        return false;
    }
    const agent_tool_definition_t *definition = &AGENT_TOOL_REGISTRY[index];
    *info = (solar_os_agent_tool_info_t){
        .provider = definition->provider,
        .domain = definition->domain,
        .output_schema_json = definition->output_schema_json,
        .required_capability = definition->required_capability,
        .risk = definition->risk,
        .available = agent_tool_is_available(definition, NULL),
    };
    return true;
}

esp_err_t solar_os_agent_tools_execute(const char *name,
                                       const char *arguments,
                                       const solar_os_agent_request_t *request,
                                       solar_os_agent_tool_policy_t policy,
                                       bool confirmed,
                                       char *result,
                                       size_t result_len)
{
    if (name == NULL || arguments == NULL ||
        result == NULL || result_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < AGENT_TOOL_COUNT; i++) {
        const agent_tool_definition_t *definition = &AGENT_TOOL_REGISTRY[i];
        if (strcmp(name, definition->provider.name) != 0) {
            continue;
        }
        if (!agent_tool_is_available(definition, request)) {
            return ESP_ERR_NOT_SUPPORTED;
        }
        const solar_os_agent_tool_policy_decision_t decision =
            solar_os_agent_tools_policy_decision(policy, definition->risk);
        if (decision == SOLAR_OS_AGENT_TOOL_POLICY_DENY ||
            (decision == SOLAR_OS_AGENT_TOOL_POLICY_CONFIRM_ONCE &&
             !confirmed)) {
            return ESP_ERR_NOT_ALLOWED;
        }
        result[0] = '\0';
        esp_err_t err = solar_os_agent_tools_is_discovery(name) ?
            agent_tool_search_with_policy(arguments,
                                          request,
                                          policy,
                                          result,
                                          result_len) :
            definition->execute(arguments, request, result, result_len);
        if (err == ESP_OK) {
            err = agent_tool_validate_result(result);
        }
        return err;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

const char *solar_os_agent_tool_risk_name(solar_os_agent_tool_risk_t risk)
{
    switch (risk) {
    case SOLAR_OS_AGENT_TOOL_RISK_READ_ONLY:
        return "read-only";
    case SOLAR_OS_AGENT_TOOL_RISK_SENSITIVE_READ:
        return "sensitive-read";
    case SOLAR_OS_AGENT_TOOL_RISK_MUTATING:
        return "mutating";
    case SOLAR_OS_AGENT_TOOL_RISK_DISRUPTIVE:
        return "disruptive";
    default:
        return "unknown";
    }
}

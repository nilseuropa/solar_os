#include "solar_os_slip_job.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/err.h"
#include "lwip/esp_netif_net_stack.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/opt.h"
#include "lwip/pbuf.h"
#include "solar_os_jobs.h"
#include "solar_os_log.h"
#include "solar_os_memory.h"
#include "solar_os_port.h"
#include "solar_os_uart.h"

#define SLIP_JOB_TASK_STACK 6144
#define SLIP_JOB_TASK_PRIORITY (tskIDLE_PRIORITY + 3)
#define SLIP_JOB_READ_BUFFER_SIZE 128
#define SLIP_JOB_FRAME_SIZE 1600
#define SLIP_JOB_MTU 1500
#define SLIP_JOB_READ_TIMEOUT_MS 10U
#define SLIP_JOB_TX_CHUNK_SIZE 256

#define SLIP_END 0xc0
#define SLIP_ESC 0xdb
#define SLIP_ESC_END 0xdc
#define SLIP_ESC_ESC 0xdd

#define SLIP_DEFAULT_PORT SOLAR_OS_UART_PORT_NAME
#define SLIP_DEFAULT_BAUD SOLAR_OS_UART_DEFAULT_BAUD_RATE
#define SLIP_DEFAULT_LOCAL_IP "192.168.7.1"
#define SLIP_DEFAULT_PEER_IP "192.168.7.2"
#define SLIP_DEFAULT_NETMASK "255.255.255.252"
#define SLIP_GOT_IP_EVENT 0x534c4950U

static const char *TAG = "solar_os_slip";

typedef struct {
    esp_netif_driver_base_t base;
} slip_driver_t;

typedef struct {
    char port_name[SOLAR_OS_PORT_NAME_MAX];
    uint32_t baud_rate;
    bool baud_explicit;
    ip4_addr_t local_ip;
    ip4_addr_t peer_ip;
    ip4_addr_t netmask;
} slip_job_config_t;

typedef struct {
    bool running;
    volatile bool stop_requested;
    TaskHandle_t task;
    solar_os_port_handle_t port;
    char port_name[SOLAR_OS_PORT_NAME_MAX];
    uint32_t baud_rate;
    esp_netif_t *esp_netif;
    slip_driver_t driver;
    bool nat_active;
    bool link_connected;
    ip4_addr_t local_ip;
    ip4_addr_t peer_ip;
    ip4_addr_t netmask;
    uint8_t *frame;
    size_t frame_len;
    bool escaped;
    bool dropping;
    uint32_t rx_packets;
    uint32_t rx_bytes;
    uint32_t tx_packets;
    uint32_t tx_bytes;
    uint32_t rx_errors;
    uint32_t rx_oversize;
    uint32_t tx_errors;
    esp_err_t last_error;
} slip_job_state_t;

static slip_job_state_t slip_job = {
    .port = SOLAR_OS_PORT_HANDLE_INIT,
    .last_error = ESP_OK,
};

#ifdef CONFIG_ESP_NETIF_RECEIVE_REPORT_ERRORS
#define SLIP_INPUT_RETURN(err) return (err)
#else
#define SLIP_INPUT_RETURN(err) return
#endif

static esp_err_t slip_lwip_err_to_esp(err_t err)
{
    switch (err) {
    case ERR_OK:
        return ESP_OK;
    case ERR_MEM:
        return ESP_ERR_NO_MEM;
    case ERR_TIMEOUT:
        return ESP_ERR_TIMEOUT;
    case ERR_VAL:
        return ESP_ERR_INVALID_ARG;
    case ERR_RTE:
        return ESP_ERR_NOT_FOUND;
    default:
        return ESP_FAIL;
    }
}

static bool slip_parse_u32(const char *text,
                           uint32_t min,
                           uint32_t max,
                           uint32_t *value)
{
    if (text == NULL || text[0] == '\0' || value == NULL) {
        return false;
    }

    char *end = NULL;
    errno = 0;
    const unsigned long parsed = strtoul(text, &end, 0);
    if (errno != 0 ||
        end == text ||
        *end != '\0' ||
        parsed < min ||
        parsed > max) {
        return false;
    }

    *value = (uint32_t)parsed;
    return true;
}

static bool slip_parse_ip(const char *text, ip4_addr_t *addr)
{
    return text != NULL && addr != NULL && ip4addr_aton(text, addr) != 0;
}

static bool slip_config_defaults(slip_job_config_t *config)
{
    if (config == NULL) {
        return false;
    }

    memset(config, 0, sizeof(*config));
    strlcpy(config->port_name, SLIP_DEFAULT_PORT, sizeof(config->port_name));
    config->baud_rate = SLIP_DEFAULT_BAUD;
    return slip_parse_ip(SLIP_DEFAULT_LOCAL_IP, &config->local_ip) &&
           slip_parse_ip(SLIP_DEFAULT_PEER_IP, &config->peer_ip) &&
           slip_parse_ip(SLIP_DEFAULT_NETMASK, &config->netmask);
}

static bool slip_parse_args(int argc, char **argv, slip_job_config_t *config)
{
    if (!slip_config_defaults(config) || argc < 0) {
        return false;
    }

    int first_arg = 0;
    if (argc > 0 && argv != NULL && argv[0] != NULL &&
        strcmp(argv[0], solar_os_slip_job.name) == 0) {
        first_arg = 1;
    }

    const int positional_count = argc - first_arg;
    if (positional_count > 5) {
        return false;
    }

    const char *positional[5] = {0};
    for (int i = 0; i < positional_count; i++) {
        if (argv == NULL ||
            argv[first_arg + i] == NULL ||
            argv[first_arg + i][0] == '\0') {
            return false;
        }
        positional[i] = argv[first_arg + i];
    }

    if (positional_count >= 1) {
        if (strlen(positional[0]) >= sizeof(config->port_name)) {
            return false;
        }
        strlcpy(config->port_name, positional[0], sizeof(config->port_name));
    }
    if (positional_count >= 2) {
        if (!slip_parse_u32(positional[1],
                            SOLAR_OS_UART_MIN_BAUD_RATE,
                            SOLAR_OS_UART_MAX_BAUD_RATE,
                            &config->baud_rate)) {
            return false;
        }
        config->baud_explicit = true;
    }
    if (positional_count >= 3 && !slip_parse_ip(positional[2], &config->local_ip)) {
        return false;
    }
    if (positional_count >= 4 && !slip_parse_ip(positional[3], &config->peer_ip)) {
        return false;
    }
    if (positional_count >= 5 && !slip_parse_ip(positional[4], &config->netmask)) {
        return false;
    }

    return true;
}

static esp_err_t slip_validate_port(const slip_job_config_t *config)
{
    solar_os_port_info_t info;

    const esp_err_t err = solar_os_port_get_info(config->port_name, &info);
    if (err != ESP_OK) {
        return err;
    }
    if (info.claimed) {
        return ESP_ERR_INVALID_STATE;
    }
    if ((info.capabilities & (SOLAR_OS_PORT_CAP_READ | SOLAR_OS_PORT_CAP_WRITE)) !=
        (SOLAR_OS_PORT_CAP_READ | SOLAR_OS_PORT_CAP_WRITE)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return ESP_OK;
}

static esp_err_t slip_configure_port(const slip_job_config_t *config)
{
    if (strcmp(config->port_name, SOLAR_OS_UART_PORT_NAME) != 0) {
        return ESP_OK;
    }

    esp_err_t err = solar_os_uart_set_baud_rate(config->baud_rate);
    if (err != ESP_OK) {
        return err;
    }
    return solar_os_uart_set_mode(SOLAR_OS_UART_MODE_RAW);
}

static esp_err_t slip_write_all(const uint8_t *data, size_t len)
{
    size_t offset = 0;
    while (!slip_job.stop_requested && offset < len) {
        size_t written = 0;
        const esp_err_t err = solar_os_port_write(&slip_job.port,
                                                  &data[offset],
                                                  len - offset,
                                                  &written);
        if (err != ESP_OK) {
            slip_job.tx_errors++;
            slip_job.last_error = err;
            return err;
        }
        if (written == 0) {
            slip_job.tx_errors++;
            slip_job.last_error = ESP_FAIL;
            return ESP_FAIL;
        }
        offset += written;
    }
    return ESP_OK;
}

static esp_err_t slip_tx_flush(uint8_t *buffer, size_t *len)
{
    if (len == NULL || *len == 0) {
        return ESP_OK;
    }

    const esp_err_t err = slip_write_all(buffer, *len);
    *len = 0;
    return err;
}

static esp_err_t slip_tx_put(uint8_t *buffer, size_t *len, uint8_t value)
{
    if (buffer == NULL || len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (*len >= SLIP_JOB_TX_CHUNK_SIZE) {
        const esp_err_t err = slip_tx_flush(buffer, len);
        if (err != ESP_OK) {
            return err;
        }
    }
    buffer[(*len)++] = value;
    return ESP_OK;
}

static esp_err_t slip_tx_encode_bytes(uint8_t *out,
                                      size_t *out_len,
                                      const uint8_t *payload,
                                      size_t len)
{
    if (payload == NULL && len > 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ESP_OK;
    for (size_t i = 0; err == ESP_OK && i < len; i++) {
        switch (payload[i]) {
        case SLIP_END:
            err = slip_tx_put(out, out_len, SLIP_ESC);
            if (err == ESP_OK) {
                err = slip_tx_put(out, out_len, SLIP_ESC_END);
            }
            break;
        case SLIP_ESC:
            err = slip_tx_put(out, out_len, SLIP_ESC);
            if (err == ESP_OK) {
                err = slip_tx_put(out, out_len, SLIP_ESC_ESC);
            }
            break;
        default:
            err = slip_tx_put(out, out_len, payload[i]);
            break;
        }
    }
    return err;
}

static esp_err_t slip_driver_output(void *handle, void *buffer, size_t len)
{
    (void)handle;

    if (buffer == NULL && len > 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len == 0 ||
        !slip_job.running ||
        slip_job.stop_requested ||
        !solar_os_port_handle_valid(&slip_job.port)) {
        return ESP_OK;
    }

    uint8_t out[SLIP_JOB_TX_CHUNK_SIZE];
    size_t out_len = 0;
    esp_err_t err = slip_tx_put(out, &out_len, SLIP_END);
    if (err == ESP_OK) {
        err = slip_tx_encode_bytes(out, &out_len, (const uint8_t *)buffer, len);
    }
    if (err == ESP_OK) {
        err = slip_tx_put(out, &out_len, SLIP_END);
    }
    if (err == ESP_OK) {
        err = slip_tx_flush(out, &out_len);
    }

    if (err == ESP_OK) {
        slip_job.tx_packets++;
        slip_job.tx_bytes += (uint32_t)len;
    }
    return err;
}

static err_t slip_netif_output(struct netif *netif, struct pbuf *p, const ip4_addr_t *ipaddr)
{
    (void)ipaddr;

    if (netif == NULL ||
        p == NULL ||
        !slip_job.running ||
        slip_job.stop_requested ||
        !solar_os_port_handle_valid(&slip_job.port)) {
        return ERR_IF;
    }

    esp_netif_t *esp_netif = (esp_netif_t *)netif->state;
    if (esp_netif == NULL || esp_netif != slip_job.esp_netif) {
        return ERR_IF;
    }

    uint8_t out[SLIP_JOB_TX_CHUNK_SIZE];
    size_t out_len = 0;
    esp_err_t err = slip_tx_put(out, &out_len, SLIP_END);
    for (struct pbuf *q = p; err == ESP_OK && q != NULL; q = q->next) {
        err = slip_tx_encode_bytes(out, &out_len, (const uint8_t *)q->payload, q->len);
    }
    if (err == ESP_OK) {
        err = slip_tx_put(out, &out_len, SLIP_END);
    }
    if (err == ESP_OK) {
        err = slip_tx_flush(out, &out_len);
    }
    if (err != ESP_OK) {
        return err == ESP_ERR_NO_MEM ? ERR_MEM : ERR_IF;
    }

    slip_job.tx_packets++;
    slip_job.tx_bytes += p->tot_len;
    return ERR_OK;
}

static err_t slip_netif_init(struct netif *netif)
{
    if (netif == NULL) {
        return ERR_ARG;
    }

    netif->name[0] = 's';
    netif->name[1] = 'l';
    netif->output = slip_netif_output;
    netif->mtu = SLIP_JOB_MTU;
    netif->flags = 0;
    return ERR_OK;
}

static esp_netif_recv_ret_t slip_lwip_input(void *handle, void *buffer, size_t len, void *rx_buffer)
{
    (void)rx_buffer;

    struct netif *netif = (struct netif *)handle;
    if (netif == NULL || buffer == NULL || len == 0 || len > UINT16_MAX) {
        SLIP_INPUT_RETURN(ESP_ERR_INVALID_ARG);
    }

    struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)len, PBUF_RAM);
    if (p == NULL) {
        SLIP_INPUT_RETURN(ESP_ERR_NO_MEM);
    }

    memcpy(p->payload, buffer, len);
    const err_t err = netif->input(p, netif);
    if (err != ERR_OK) {
        pbuf_free(p);
        SLIP_INPUT_RETURN(slip_lwip_err_to_esp(err));
    }

    SLIP_INPUT_RETURN(ESP_OK);
}

static esp_err_t slip_driver_post_attach(esp_netif_t *esp_netif,
                                         esp_netif_iodriver_handle driver_handle)
{
    slip_driver_t *driver = (slip_driver_t *)driver_handle;
    if (esp_netif == NULL || driver == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    driver->base.netif = esp_netif;
    esp_netif_action_start(esp_netif, 0, 0, NULL);
    return ESP_OK;
}

static esp_err_t slip_set_nat(bool enable)
{
    if (slip_job.esp_netif == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t err = enable ? esp_netif_napt_enable(slip_job.esp_netif)
                                 : esp_netif_napt_disable(slip_job.esp_netif);
    if (err == ESP_OK) {
        slip_job.nat_active = enable;
    }
    return err;
}

static esp_err_t slip_add_netif(const slip_job_config_t *config)
{
    esp_netif_ip_info_t ip_info = {
        .ip.addr = config->local_ip.addr,
        .netmask.addr = config->netmask.addr,
        .gw.addr = config->peer_ip.addr,
    };
    esp_netif_driver_ifconfig_t driver_config = {
        .handle = &slip_job.driver,
        .transmit = slip_driver_output,
    };
    esp_netif_netstack_config_t stack_config = {
        .lwip = {
            .init_fn = slip_netif_init,
            .input_fn = slip_lwip_input,
        },
    };
    esp_netif_inherent_config_t base_config = {
        .flags = (esp_netif_flags_t)(ESP_NETIF_IPV4_ONLY_FLAGS(ESP_NETIF_FLAG_AUTOUP) |
                                     ESP_NETIF_FLAG_EVENT_IP_MODIFIED),
        .ip_info = &ip_info,
        .get_ip_event = SLIP_GOT_IP_EVENT,
        .if_key = "SLIP",
        .if_desc = "slip",
        .route_prio = 5,
    };
    esp_netif_config_t netif_config = {
        .base = &base_config,
        .driver = &driver_config,
        .stack = &stack_config,
    };

    slip_job.driver = (slip_driver_t){
        .base.post_attach = slip_driver_post_attach,
    };

    slip_job.esp_netif = esp_netif_new(&netif_config);
    if (slip_job.esp_netif == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_netif_attach(slip_job.esp_netif, &slip_job.driver);
    if (err == ESP_OK) {
        esp_netif_action_connected(slip_job.esp_netif, 0, 0, NULL);
        slip_job.link_connected = true;
    }
    if (err != ESP_OK) {
        esp_netif_destroy(slip_job.esp_netif);
        slip_job.esp_netif = NULL;
        return err;
    }

    err = slip_set_nat(true);
    if (err == ESP_OK) {
        SOLAR_OS_LOGI(TAG, "NAT enabled on SLIP interface");
    } else {
        SOLAR_OS_LOGW(TAG, "NAT enable failed: %s", esp_err_to_name(err));
    }
    return err;
}

static void slip_remove_netif(void)
{
    if (slip_job.nat_active) {
        (void)slip_set_nat(false);
    }

    if (slip_job.esp_netif != NULL) {
        if (slip_job.link_connected) {
            esp_netif_action_disconnected(slip_job.esp_netif, 0, 0, NULL);
            slip_job.link_connected = false;
        }
        esp_netif_destroy(slip_job.esp_netif);
        slip_job.esp_netif = NULL;
    }
}

static void slip_reset_rx_frame(void)
{
    slip_job.frame_len = 0;
    slip_job.escaped = false;
    slip_job.dropping = false;
}

static void slip_deliver_frame(void)
{
    if (slip_job.frame == NULL || slip_job.frame_len == 0) {
        slip_reset_rx_frame();
        return;
    }

    const esp_err_t err = esp_netif_receive(slip_job.esp_netif,
                                            slip_job.frame,
                                            slip_job.frame_len,
                                            NULL);
    if (err != ESP_OK) {
        slip_job.rx_errors++;
        slip_job.last_error = err;
        slip_reset_rx_frame();
        return;
    }

    slip_job.rx_packets++;
    slip_job.rx_bytes += (uint32_t)slip_job.frame_len;
    slip_reset_rx_frame();
}

static void slip_rx_byte(uint8_t value)
{
    if (value == SLIP_END) {
        if (!slip_job.dropping) {
            slip_deliver_frame();
        } else {
            slip_reset_rx_frame();
        }
        return;
    }

    if (slip_job.dropping) {
        return;
    }

    if (slip_job.escaped) {
        if (value == SLIP_ESC_END) {
            value = SLIP_END;
        } else if (value == SLIP_ESC_ESC) {
            value = SLIP_ESC;
        } else {
            slip_job.rx_errors++;
            slip_job.last_error = ESP_ERR_INVALID_RESPONSE;
            slip_reset_rx_frame();
            slip_job.dropping = true;
            return;
        }
        slip_job.escaped = false;
    } else if (value == SLIP_ESC) {
        slip_job.escaped = true;
        return;
    }

    if (slip_job.frame_len >= SLIP_JOB_FRAME_SIZE) {
        slip_job.rx_oversize++;
        slip_job.last_error = ESP_ERR_NO_MEM;
        slip_job.dropping = true;
        return;
    }

    slip_job.frame[slip_job.frame_len++] = value;
}

static void slip_cleanup(void)
{
    slip_remove_netif();

    if (solar_os_port_handle_valid(&slip_job.port)) {
        (void)solar_os_port_release(&slip_job.port);
    }

    if (slip_job.frame != NULL) {
        solar_os_memory_free(slip_job.frame);
        slip_job.frame = NULL;
    }

    slip_job.running = false;
    slip_job.stop_requested = false;
    slip_job.task = NULL;
    slip_job.port_name[0] = '\0';
    slip_job.frame_len = 0;
    slip_job.escaped = false;
    slip_job.dropping = false;
}

static void slip_task(void *arg)
{
    (void)arg;

    uint8_t buffer[SLIP_JOB_READ_BUFFER_SIZE];
    char local_ip[IP4ADDR_STRLEN_MAX];
    char peer_ip[IP4ADDR_STRLEN_MAX];
    char netmask[IP4ADDR_STRLEN_MAX];
    ip4addr_ntoa_r(&slip_job.local_ip, local_ip, sizeof(local_ip));
    ip4addr_ntoa_r(&slip_job.peer_ip, peer_ip, sizeof(peer_ip));
    ip4addr_ntoa_r(&slip_job.netmask, netmask, sizeof(netmask));

    SOLAR_OS_LOGI(TAG,
                  "started: port=%s baud=%" PRIu32 " local=%s peer=%s netmask=%s nat=%s",
                  slip_job.port_name,
                  slip_job.baud_rate,
                  local_ip,
                  peer_ip,
                  netmask,
                  slip_job.nat_active ? "on" : "off");

    while (!slip_job.stop_requested) {
        size_t read_len = 0;
        const esp_err_t err = solar_os_port_read(&slip_job.port,
                                                 buffer,
                                                 sizeof(buffer),
                                                 SLIP_JOB_READ_TIMEOUT_MS,
                                                 &read_len);
        if (err != ESP_OK) {
            if (err != ESP_ERR_TIMEOUT) {
                slip_job.rx_errors++;
                slip_job.last_error = err;
            }
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        for (size_t i = 0; i < read_len; i++) {
            slip_rx_byte(buffer[i]);
        }
        if (read_len == 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    SOLAR_OS_LOGI(TAG,
                  "stopped: rx=%" PRIu32 "/%" PRIu32 " tx=%" PRIu32 "/%" PRIu32 " rx_err=%" PRIu32 " over=%" PRIu32 " tx_err=%" PRIu32,
                  slip_job.rx_packets,
                  slip_job.rx_bytes,
                  slip_job.tx_packets,
                  slip_job.tx_bytes,
                  slip_job.rx_errors,
                  slip_job.rx_oversize,
                  slip_job.tx_errors);

    slip_cleanup();
    vTaskDelete(NULL);
}

static esp_err_t slip_job_start(solar_os_context_t *ctx, int argc, char **argv)
{
    (void)ctx;

    slip_job_config_t config;
    if (!slip_parse_args(argc, argv, &config)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (slip_job.running || slip_job.task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = slip_validate_port(&config);
    if (err != ESP_OK) {
        return err;
    }
    err = slip_configure_port(&config);
    if (err != ESP_OK) {
        return err;
    }

    err = solar_os_jobs_claim_port(solar_os_slip_job.name, config.port_name, &slip_job.port);
    if (err != ESP_OK) {
        return err;
    }

    slip_job.frame = solar_os_memory_alloc(SLIP_JOB_FRAME_SIZE,
                                           SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                           "slip.frame");
    if (slip_job.frame == NULL) {
        slip_cleanup();
        return ESP_ERR_NO_MEM;
    }

    slip_job.running = true;
    slip_job.stop_requested = false;
    slip_job.baud_rate = config.baud_rate;
    slip_job.local_ip = config.local_ip;
    slip_job.peer_ip = config.peer_ip;
    slip_job.netmask = config.netmask;
    slip_job.rx_packets = 0;
    slip_job.rx_bytes = 0;
    slip_job.tx_packets = 0;
    slip_job.tx_bytes = 0;
    slip_job.rx_errors = 0;
    slip_job.rx_oversize = 0;
    slip_job.tx_errors = 0;
    slip_job.last_error = ESP_OK;
    slip_reset_rx_frame();
    strlcpy(slip_job.port_name, config.port_name, sizeof(slip_job.port_name));

    err = slip_add_netif(&config);
    if (err != ESP_OK) {
        slip_cleanup();
        return err;
    }

    char network[96];
    char local_ip[IP4ADDR_STRLEN_MAX];
    char peer_ip[IP4ADDR_STRLEN_MAX];
    ip4addr_ntoa_r(&config.local_ip, local_ip, sizeof(local_ip));
    ip4addr_ntoa_r(&config.peer_ip, peer_ip, sizeof(peer_ip));
    snprintf(network, sizeof(network), "%s->%s", local_ip, peer_ip);
    (void)solar_os_jobs_note_resource(solar_os_slip_job.name,
                                      SOLAR_OS_JOB_RESOURCE_NET,
                                      network,
                                      "ipv4");

    if (xTaskCreate(slip_task,
                    "slip_job",
                    SLIP_JOB_TASK_STACK,
                    NULL,
                    SLIP_JOB_TASK_PRIORITY,
                    &slip_job.task) != pdPASS) {
        slip_cleanup();
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static void slip_job_stop(solar_os_context_t *ctx)
{
    (void)ctx;

    if (!slip_job.running && slip_job.task == NULL) {
        return;
    }

    slip_job.stop_requested = true;
    if (slip_job.task != NULL && slip_job.task != xTaskGetCurrentTaskHandle()) {
        for (uint32_t i = 0; i < 80 && slip_job.task != NULL; i++) {
            vTaskDelay(pdMS_TO_TICKS(25));
        }
    }
}

const solar_os_job_t solar_os_slip_job = {
    .name = "slip",
    .summary = "SLIP IPv4 gateway on a port",
    .start = slip_job_start,
    .stop = slip_job_stop,
    .event = NULL,
};

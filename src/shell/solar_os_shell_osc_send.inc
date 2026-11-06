typedef struct {
    solar_os_context_t *ctx;
    solar_os_shell_io_t *io;
    bool cancelled;
} osc_send_context_t;

static bool osc_send_cancelled(void *user)
{
    osc_send_context_t *s = user;
    if (s->cancelled) return true;
    if (s->ctx->exit_requested || s->ctx->sleep_requested || s->ctx->deep_sleep_requested)
        return s->cancelled = true;
    char chars[16]; size_t count = 0;
    if (solar_os_shell_io_kind(s->io) == SOLAR_OS_SHELL_IO_KIND_PORT) {
        esp_err_t err = solar_os_port_read(&s->io->port, (uint8_t *)chars, sizeof(chars), 0, &count);
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) return s->cancelled = true;
    } else count = solar_os_input_read_chars(chars, sizeof(chars));
    for (size_t i = 0; i < count; ++i)
        if ((uint8_t)chars[i] == SOLAR_OS_KEY_APP_EXIT || chars[i] == 0x1d || chars[i] == 0x03 || chars[i] == 0x1b)
            return s->cancelled = true;
    return false;
}

static void osc_send_command(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *io = solar_os_shell_command_io(ctx);
    if (argc < 5) {
        solar_os_shell_io_printf(io, "osc send: %s\n", argc < 3 ? "missing destination host:port" :
            argc < 4 ? "missing OSC address" : "at least one value is required");
        solar_os_shell_io_writeln(io, osc_usage); return;
    }
    char host[SOLAR_OS_NET_HOST_MAX], ip[SOLAR_OS_NET_ADDR_MAX];
    const char *colon = strrchr(argv[2], ':'); size_t port;
    if (!colon || colon == argv[2] || strchr(argv[2], ':') != colon ||
        (size_t)(colon - argv[2]) >= sizeof(host) ||
        !solar_os_shell_parse_size_arg(colon + 1, 1, UINT16_MAX, &port)) {
        solar_os_shell_io_printf(io, "osc send: invalid destination '%s'; expected IPv4-or-hostname:1..65535\n", argv[2]); return;
    }
    memcpy(host, argv[2], (size_t)(colon - argv[2])); host[colon - argv[2]] = '\0';
    float values[SOLAR_OS_OSC_ARGUMENT_MAX]; size_t count = 0;
    unsigned rate = 0; uint64_t duration_ms = 0;
    for (int i = 4; i < argc; ++i) {
        if (!strcmp(argv[i], "--rate")) {
            size_t parsed;
            if (rate || i + 1 >= argc) { solar_os_shell_io_writeln(io, "osc send: --rate requires one integer from 1 to 100 Hz"); return; }
            if (!solar_os_shell_parse_size_arg(argv[++i], 1, 100, &parsed)) {
                solar_os_shell_io_printf(io, "osc send: invalid rate '%s'; expected an integer from 1 to 100 Hz\n", argv[i]); return;
            }
            rate = (unsigned)parsed; continue;
        }
        if (!strcmp(argv[i], "--duration")) {
            if (duration_ms || i + 1 >= argc) { solar_os_shell_io_writeln(io, "osc send: --duration requires one value in seconds"); return; }
            char *end; errno = 0; double seconds = strtod(argv[++i], &end);
            if (errno || end == argv[i] || *end || !isfinite(seconds) || seconds < .001 || seconds > 86400) {
                solar_os_shell_io_printf(io, "osc send: invalid duration '%s'; expected 0.001..86400 seconds\n", argv[i]); return;
            }
            duration_ms = (uint64_t)(seconds * 1000 + .5); continue;
        }
        if (!strncmp(argv[i], "--", 2)) { solar_os_shell_io_printf(io, "osc send: unknown option '%s'\n", argv[i]); return; }
        if (count == SOLAR_OS_OSC_ARGUMENT_MAX) {
            solar_os_shell_io_printf(io, "osc send: at most %u values per message\n", (unsigned)SOLAR_OS_OSC_ARGUMENT_MAX); return;
        }
        if (!osc_parse_float(argv[i], &values[count])) {
            solar_os_shell_io_printf(io, "osc send: invalid value %u '%s'; expected a finite float32 number\n", (unsigned)count + 1, argv[i]); return;
        }
        ++count;
    }
    if (!count) { solar_os_shell_io_writeln(io, "osc send: at least one value is required"); return; }
    if (duration_ms && !rate) { solar_os_shell_io_writeln(io, "osc send: --duration requires --rate"); return; }
    uint8_t packet[SOLAR_OS_OSC_PACKET_MAX]; size_t bytes = 0;
    esp_err_t err = solar_os_osc_encode_floats(argv[3], values, count, packet, sizeof(packet), &bytes);
    if (err != ESP_OK) {
        solar_os_shell_io_printf(io, "osc send: invalid address '%s'; expected an exact OSC address starting with /\n", argv[3]); return;
    }
    const char *operation = "resolve host";
    err = solar_os_net_resolve_host(host, ip, sizeof(ip));
    solar_os_net_session_t *session = NULL; uint32_t handle = 0; uint64_t sent = 0;
    osc_send_context_t state = {.ctx=ctx, .io=io};
    if (err == ESP_OK) {
        operation = "create UDP session";
        err = solar_os_net_session_create("osc-send", osc_send_cancelled, &state, &session);
    }
    if (err == ESP_OK) { operation = "open UDP socket"; err = solar_os_net_session_udp_open(session, 0, &handle); }
    if (err == ESP_OK && rate)
        solar_os_shell_io_printf(io, "Publishing OSC %s to %s at %u Hz; Ctrl+C stops\n", argv[3], argv[2], rate);
    uint64_t started = solar_os_time_uptime_ms();
    operation = err == ESP_OK ? "send UDP message" : operation;
    while (err == ESP_OK && !osc_send_cancelled(&state)) {
        if (duration_ms && solar_os_time_uptime_ms() - started >= duration_ms) break;
        err = solar_os_net_session_udp_send(session, handle, ip, (uint16_t)port, packet, bytes, 100);
        if (err != ESP_OK) break;
        ++sent;
        if (!rate) break;
        uint64_t elapsed = solar_os_time_uptime_ms() - started;
        /* Match stream write: skip missed periods instead of sending a burst. */
        uint64_t due = started + (((elapsed * rate / 1000 + 1) * 1000 + rate - 1) / rate);
        if (duration_ms && due > started + duration_ms) due = started + duration_ms;
        uint64_t current;
        while ((current = solar_os_time_uptime_ms()) < due && !osc_send_cancelled(&state)) {
            uint32_t chunk = due - current > 10 ? 10 : (uint32_t)(due - current);
            TickType_t ticks = pdMS_TO_TICKS(chunk); vTaskDelay(ticks ? ticks : 1);
        }
    }
    solar_os_net_session_destroy(session);
    if (err != ESP_OK && !state.cancelled)
        solar_os_shell_io_printf(io, "osc send: %s to %s failed: %s\n", operation, argv[2], solar_os_shell_error_text(err));
    else solar_os_shell_io_printf(io, "OSC sent %llu message%s to %s%s\n",
        (unsigned long long)sent, sent == 1 ? "" : "s", argv[2], state.cancelled ? "; cancelled" : "");
}

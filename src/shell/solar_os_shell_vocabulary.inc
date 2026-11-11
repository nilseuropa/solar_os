/* Reincluded with SOLAR_OS_SHELL_WORDS(name, ...). Keep capability gates
 * identical for completion and invalid-argument help. Root command dispatch,
 * completion and help already share shell_builtin_commands. */
#if SOLAR_OS_PACKAGE_SERVICE_BLE
#define SOLAR_OS_SHELL_BLE_WORD "ble",
#else
#define SOLAR_OS_SHELL_BLE_WORD
#endif
#if SOLAR_OS_PACKAGE_SERVICE_BLE_STREAM
#define SOLAR_OS_SHELL_BLE_STREAM_WORD "ble",
#else
#define SOLAR_OS_SHELL_BLE_STREAM_WORD
#endif
#if SOLAR_OS_PACKAGE_SERVICE_TCP_STREAM
#define SOLAR_OS_SHELL_TCP_STREAM_WORD "tcp",
#else
#define SOLAR_OS_SHELL_TCP_STREAM_WORD
#endif
SOLAR_OS_SHELL_WORDS(display,
    "list", "layouts", "test", "mode", "orientation", "scale", "relax",
    "join", "unjoin", "split", "unsplit")
SOLAR_OS_SHELL_WORDS(job, "status", "start", "stop")
SOLAR_OS_SHELL_WORDS(setterm,
    "--display", "orientation", "scale", "font", "textsize", "cursor", "palette",
    "foreground", "background", "statusbar", "brightness", "backlight",
    "profile", "charset", "keyboard", "keymap", "powerkey", "key", "keyrate",
    "typerate", SOLAR_OS_SHELL_BLE_WORD "repeat", "timezone", "startup", "otaurl", "ota")
SOLAR_OS_SHELL_WORDS(stream,
    SOLAR_OS_SHELL_BLE_STREAM_WORD SOLAR_OS_SHELL_TCP_STREAM_WORD "list", "status", "echo", "write")
SOLAR_OS_SHELL_WORDS(log, "status", "show", "follow", "clear", "level", "sink")
SOLAR_OS_SHELL_WORDS(port, "list", "status")
SOLAR_OS_SHELL_WORDS(xfer, "protocols", "send", "recv")
SOLAR_OS_SHELL_WORDS(xfer_options,
    "--raw", "--zmodem", "--kermit", "--protocol", "-d", "--delay-ms",
    "--idle-ms", "--append", "--replace")
#if SOLAR_OS_PACKAGE_SERVICE_OTA
SOLAR_OS_SHELL_WORDS(ota, "status", "check", "upgrade", "url", "flavor", "boot")
#endif
#if SOLAR_OS_PACKAGE_SERVICE_SSH
SOLAR_OS_SHELL_WORDS(sshkey, "status", "gen", "pub", "rm")
#endif
#undef SOLAR_OS_SHELL_BLE_WORD
#undef SOLAR_OS_SHELL_BLE_STREAM_WORD
#undef SOLAR_OS_SHELL_TCP_STREAM_WORD

+++
id = "compatibility.io"
title = "Compatibility I/O modules"
section = "api"
summary = "Use the legacy single-bus I2C, SPI, UART, and OneWire APIs"
aliases = ["i2c", "spi", "uart", "onewire"]
keywords = "python lua i2c spi uart onewire serial transfer register probe scan compatibility"
packages_any = ["service_i2c", "service_spi", "service_uart", "service_onewire"]
+++
# Compatibility I/O modules

The compatibility modules expose the board's traditional single I2C, SPI,
UART, and OneWire services. They are useful for simple scripts and existing
code. New applications that need multiple or dynamically attached buses should
prefer `solaros.buses`.

The default I2C bus uses the board's primary controller and SDA/SCL pins.
Starting another named I2C bus does not change this default. Board-defined
buses on different controllers can operate independently through `solaros.buses`.

## Safe workflow

1. Inspect `status()` or `allowed(pin)`.
2. Use a board-supported bus, pin, chip select, address, mode, and speed.
3. Check transfer results.
4. Leave the service in a known state when the script exits.

For external hardware, `man buses` and `man expansion` describe the
resource-owned path.

## Quick reference

Compatibility modules are solaros.i2c info, probe, scan, read_reg, write_reg;
solaros.spi status, xfer, read, write; solaros.uart status, baud,
is_valid_baud, mode, write, read; solaros.onewire allowed, reset, scan, xfer.
Inspect status or allowed first. New multi-bus code should prefer
solaros.buses. Modules are package-gated.

#!/usr/bin/env python3
"""Build a copy of usb_host_hid 1.2.1 with lifecycle mutex cleanup.

SolarOS joins its connection worker before uninstalling the HID driver. Once
the driver's event task has also exited, no application open/close waiters can
remain. Version 1.2.1 omits deletion of open_close_mutex on uninstall and on
install failure; repeated USB role switches would otherwise leak it.
The managed component and its checksums are left unchanged.
"""

from pathlib import Path
import sys


def patched_source(source: str) -> str:
    uninstall = """    s_hid_driver = NULL;
    xSemaphoreGive(open_close_mutex); // Unblock any waiting tasks
    return ESP_OK;"""
    install_failure = """    if (driver->all_events_handled) {
        vSemaphoreDelete(driver->all_events_handled);
    }
    free(driver);
    return ret;"""
    if source.count(uninstall) != 1 or source.count(install_failure) != 1:
        raise ValueError("usb_host_hid lifecycle changed; review the SolarOS cleanup patch")
    source = source.replace(uninstall, """    s_hid_driver = NULL;
    /* SolarOS has joined all application users before uninstall. */
    vSemaphoreDelete(open_close_mutex);
    return ESP_OK;""")
    return source.replace(install_failure, """    if (driver->all_events_handled) {
        vSemaphoreDelete(driver->all_events_handled);
    }
    if (driver->open_close_mutex) {
        vSemaphoreDelete(driver->open_close_mutex);
    }
    free(driver);
    return ret;""")


if __name__ == "__main__":
    src, dest = map(Path, sys.argv[1:])
    result = patched_source(src.read_text())
    if not dest.exists() or dest.read_text() != result:
        dest.write_text(result)

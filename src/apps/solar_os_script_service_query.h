#pragma once

#include "solar_os_native_ble.h"
#include "solar_os_native_ble_keyboard.h"
#include "solar_os_native_ble_stream.h"
#include "solar_os_native_dsp.h"
#include "solar_os_native_hid_device.h"
#include "solar_os_native_tcp_stream.h"

/* The VM bindings use public ABI clients. Built-in VMs can resolve known
 * services without keeping the firmware's dynamic dispatcher reachable. */
#define SOLAR_OS_SCRIPT_SERVICE_QUERY(id) solar_os_script_query_##id
#define solar_os_script_query_bluetooth solar_os_native_ble_get_service
#define solar_os_script_query_ble_keyboard solar_os_native_ble_keyboard_get_service
#define solar_os_script_query_ble_stream solar_os_native_ble_stream_get_service
#define solar_os_script_query_dsp solar_os_native_dsp_get_service
#define solar_os_script_query_hid_device solar_os_native_hid_device_get_service
#define solar_os_script_query_tcp_stream solar_os_native_tcp_stream_get_service

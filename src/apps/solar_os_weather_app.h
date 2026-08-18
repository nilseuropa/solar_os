#pragma once

#include "solar_os.h"

/*
 * "weather" — native SolarOS foreground app.
 *
 * Displays an Open-Meteo forecast for a configured place alongside the
 * on-board temperature/humidity sensor reading (solar_os_sensors_read_environment).
 * Modeled on solar_os_curl.c (async HTTP worker + event queue) and
 * solar_os_clock.c (foreground gfx app skeleton). Persists the last
 * resolved place/coordinates and the last successful forecast snapshot
 * in NVS, following the pattern in solar_os_webradio_catalog.c.
 */
extern const solar_os_app_t solar_os_weather_app;

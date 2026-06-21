#include "solar_os_job_registry.h"

#include <stddef.h>
#include <string.h>

#include "solar_os_batmon_job.h"
#include "solar_os_bridge_job.h"
#include "solar_os_httpd_job.h"
#include "solar_os_log_job.h"
#include "solar_os_ntp_sync_job.h"
#include "solar_os_shell_job.h"
#include "solar_os_slip_job.h"

static const solar_os_job_registry_entry_t registered_jobs[] = {
    {"batmon", "battery voltage trend monitor", &solar_os_batmon_job},
    {"bridge", "raw bidirectional port bridge", &solar_os_bridge_job},
    {"httpd", "static HTTP file server", &solar_os_httpd_job},
    {"log", "stream SolarOS logs to a port or file", &solar_os_log_job},
    {"ntp-sync", "periodic RTC NTP sync", &solar_os_ntp_sync_job},
    {"shell", "VT100 shell on a byte-stream port", &solar_os_shell_job},
    {"slip", "SLIP IPv4 gateway on a byte-stream port", &solar_os_slip_job},
};

static const size_t registered_job_count = sizeof(registered_jobs) / sizeof(registered_jobs[0]);

size_t solar_os_job_registry_count(void)
{
    return registered_job_count;
}

const solar_os_job_registry_entry_t *solar_os_job_registry_get(size_t index)
{
    if (index >= registered_job_count) {
        return NULL;
    }

    return &registered_jobs[index];
}

const solar_os_job_registry_entry_t *solar_os_job_registry_find(const char *name)
{
    if (name == NULL) {
        return NULL;
    }

    for (size_t i = 0; i < registered_job_count; i++) {
        if (registered_jobs[i].name != NULL && strcmp(registered_jobs[i].name, name) == 0) {
            return &registered_jobs[i];
        }
    }

    return NULL;
}

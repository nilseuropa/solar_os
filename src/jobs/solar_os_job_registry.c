#include "solar_os_job_registry.h"

#include <stddef.h>
#include <string.h>

#include "solar_os_config.h"
#if SOLAR_OS_PACKAGE_JOB_BATMON
#include "solar_os_batmon_job.h"
#endif
#if SOLAR_OS_PACKAGE_JOB_BRIDGE
#include "solar_os_bridge_job.h"
#endif
#if SOLAR_OS_PACKAGE_JOB_DAQ
#include "solar_os_daq_job.h"
#endif
#if SOLAR_OS_PACKAGE_JOB_EMAIL_SYNC
#include "solar_os_email_sync_job.h"
#endif
#if SOLAR_OS_PACKAGE_JOB_HTTPD
#include "solar_os_httpd_job.h"
#endif
#if SOLAR_OS_PACKAGE_JOB_CHATD
#include "solar_os_chatd_job.h"
#endif
#if SOLAR_OS_PACKAGE_JOB_LOG
#include "solar_os_log_job.h"
#endif
#if SOLAR_OS_PACKAGE_JOB_NTP_SYNC
#include "solar_os_ntp_sync_job.h"
#endif
#if SOLAR_OS_PACKAGE_JOB_POCSAG
#include "solar_os_pocsag_job.h"
#endif
#if SOLAR_OS_PACKAGE_JOB_SLIP
#include "solar_os_slip_job.h"
#endif
#if SOLAR_OS_PACKAGE_JOB_SUMP
#include "solar_os_sump_job.h"
#endif

static const solar_os_job_registry_entry_t registered_jobs[] = {
#if SOLAR_OS_PACKAGE_JOB_BATMON
    {"batmon", "battery voltage trend monitor", &solar_os_batmon_job},
#endif
#if SOLAR_OS_PACKAGE_JOB_BRIDGE
    {"bridge", "raw bidirectional port bridge", &solar_os_bridge_job},
#endif
#if SOLAR_OS_PACKAGE_JOB_DAQ
    {"daq", "capture data streams to CSV", &solar_os_daq_job},
#endif
#if SOLAR_OS_PACKAGE_JOB_EMAIL_SYNC
    {"email-sync", "periodic IMAP email synchronization", &solar_os_email_sync_job},
#endif
#if SOLAR_OS_PACKAGE_JOB_HTTPD
    {"httpd", "static HTTP file server", &solar_os_httpd_job},
#endif
#if SOLAR_OS_PACKAGE_JOB_CHATD
    {"chatd", "local chat gateway server", &solar_os_chatd_job},
#endif
#if SOLAR_OS_PACKAGE_JOB_LOG
    {"log", "stream SolarOS logs to a port or file", &solar_os_log_job},
#endif
#if SOLAR_OS_PACKAGE_JOB_NTP_SYNC
    {"ntp-sync", "periodic RTC NTP sync", &solar_os_ntp_sync_job},
#endif
#if SOLAR_OS_PACKAGE_JOB_POCSAG
    {"pocsag", "POCSAG pager receiver", &solar_os_pocsag_job},
#endif
#if SOLAR_OS_PACKAGE_JOB_SLIP
    {"slip", "SLIP IPv4 gateway on a port", &solar_os_slip_job},
#endif
#if SOLAR_OS_PACKAGE_JOB_SUMP
    {"sump", "SUMP logic analyzer on cdc0", &solar_os_sump_job},
#endif
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

#pragma once

#include "esp_http_server.h"
#include "solar_os.h"

extern const solar_os_job_t solar_os_remote_job;

/* Running server handle (NULL while the job is stopped), with the
 * listening port in *port when non-NULL. Lets other packages register
 * their pages on this server instead of starting a second httpd
 * instance -- two instances contend for sockets and starve each other
 * on congested links. */
httpd_handle_t solar_os_remote_job_server(uint16_t *port);

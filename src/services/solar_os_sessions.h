#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "solar_os.h"
#include "solar_os_shell_io.h"
#include "solar_os_terminal.h"
#include "u8g2.h"

typedef void (*solar_os_sessions_terminal_fn)(solar_os_terminal_t *terminal, void *user);
typedef void (*solar_os_sessions_overlay_fn)(const char *title, void *user);

esp_err_t solar_os_sessions_init(solar_os_context_t *ctx,
                                 solar_os_terminal_t *shell_terminal,
                                 u8g2_t *display_u8g2,
                                 solar_os_sessions_terminal_fn terminal_fn,
                                 solar_os_sessions_overlay_fn overlay_fn,
                                 void *user);
void solar_os_sessions_set_display(solar_os_terminal_t *shell_terminal, u8g2_t *display_u8g2);

const solar_os_app_t *solar_os_sessions_foreground_app(void);
solar_os_terminal_t *solar_os_sessions_foreground_terminal(void);
bool solar_os_sessions_foreground_is_shell(void);
bool solar_os_sessions_has_display_shell(void);
void solar_os_sessions_set_status_bar(const solar_os_status_bar_t *status);
esp_err_t solar_os_sessions_set_terminal_orientation(solar_os_terminal_t *terminal,
                                                     uint16_t degrees);
esp_err_t solar_os_sessions_set_terminal_font(solar_os_terminal_t *terminal,
                                              solar_os_terminal_font_t font);
esp_err_t solar_os_sessions_set_terminal_text_size(solar_os_terminal_t *terminal,
                                                   solar_os_terminal_text_size_t text_size);
esp_err_t solar_os_sessions_set_terminal_palette_inverted(solar_os_terminal_t *terminal,
                                                          bool inverted);
esp_err_t solar_os_sessions_set_terminal_status_bar_visible(solar_os_terminal_t *terminal,
                                                            bool visible);
esp_err_t solar_os_sessions_set_display_terminal_profile(
    const char *target_name,
    const solar_os_terminal_profile_t *profile);

bool solar_os_sessions_switch_to_app(const solar_os_app_t *app);
bool solar_os_sessions_switch_to_app_with_policy(const solar_os_app_t *app,
                                                 solar_os_launch_policy_t policy);
void solar_os_sessions_cycle_next(void);
bool solar_os_sessions_cycle_input_focus(void);
bool solar_os_sessions_cycle_input_focus_previous(void);
void solar_os_sessions_mark_foreground_dirty(void);

void solar_os_sessions_dispatch_foreground_event(const solar_os_event_t *event);
bool solar_os_sessions_dispatch_input_event(const solar_os_event_t *event);
const solar_os_app_t *solar_os_sessions_input_app(void);
uint32_t solar_os_sessions_requested_tick_interval_ms(void);
void solar_os_sessions_dispatch_tick(uint32_t now_ms);
void solar_os_sessions_dispatch_resume(uint32_t now_ms);
void solar_os_sessions_process_requests(void);
void solar_os_sessions_prompt_if_shell_active(void);

esp_err_t solar_os_sessions_create_display_shell(const char *target_name,
                                                 uint8_t *session_id,
                                                 char *busy_owner,
                                                 size_t busy_owner_len);
esp_err_t solar_os_sessions_create_detached_display_shell(const char *target_name,
                                                          uint8_t *session_id,
                                                          char *busy_owner,
                                                          size_t busy_owner_len);
esp_err_t solar_os_sessions_create_display_app(const solar_os_app_t *app,
                                               const char *target_name,
                                               int argc,
                                               char **argv,
                                               uint8_t *session_id,
                                               char *busy_owner,
                                               size_t busy_owner_len);
esp_err_t solar_os_sessions_focus_display(const char *target_name);
bool solar_os_sessions_input_focus(char *target_name, size_t target_name_len);
bool solar_os_sessions_active_for_display(const char *target_name, uint8_t *session_id);
bool solar_os_sessions_context_uses_display(solar_os_context_t *ctx,
                                            const char *target_name);
bool solar_os_sessions_dispatch_session_event(uint8_t session_id,
                                              const solar_os_event_t *event);
esp_err_t solar_os_sessions_send_command(uint8_t session_id,
                                         const char *command,
                                         solar_os_shell_io_t *caller_io);
esp_err_t solar_os_sessions_close_display(const char *target_name);
esp_err_t solar_os_sessions_close_session(uint8_t session_id, solar_os_shell_io_t *io);
esp_err_t solar_os_sessions_close_any(uint8_t session_id, solar_os_shell_io_t *io);
size_t solar_os_sessions_active_count(void);
size_t solar_os_sessions_shell_count(void);
bool solar_os_sessions_get_active_id(size_t index, uint8_t *session_id);
void solar_os_sessions_print_list(solar_os_shell_io_t *io, void *user);

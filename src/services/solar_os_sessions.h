#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "solar_os.h"
#include "solar_os_display_types_v1.h"
#include "solar_os_display_layout_types_v1.h"
#include "solar_os_native_sessions_abi.h"
#include "solar_os_shell_io.h"
#include "solar_os_terminal.h"
#include "u8g2.h"

typedef struct solar_os_tui solar_os_tui_t;

typedef void (*solar_os_sessions_terminal_fn)(solar_os_terminal_t *terminal, void *user);
typedef void (*solar_os_sessions_overlay_fn)(const char *title,
                                             const char *target_name,
                                             bool after_next_frame,
                                             void *user);

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
esp_err_t solar_os_sessions_set_display_preference(const char *target, uint32_t field, uint32_t value, bool save);
esp_err_t solar_os_sessions_set_terminal_preference(solar_os_terminal_t *terminal, uint32_t field, uint32_t value);
esp_err_t solar_os_sessions_set_terminal_orientation(solar_os_terminal_t *terminal,
                                                     uint16_t degrees);
esp_err_t solar_os_sessions_set_terminal_scale(solar_os_terminal_t *terminal, uint8_t scale);
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
/* Set the runtime canvas and reflow its terminals on the session owner. */
esp_err_t solar_os_sessions_set_display_canvas(const char *target_name,
                                               const solar_os_display_rect_t *canvas);
esp_err_t solar_os_sessions_set_display_orientation(const char *target, uint16_t degrees, bool save);
esp_err_t solar_os_sessions_set_display_scale(const char *target, uint8_t scale, bool save);
void solar_os_sessions_attach_tui(solar_os_shell_io_t *io, solar_os_tui_t *tui);
void solar_os_sessions_detach_tui(solar_os_shell_io_t *io, const solar_os_tui_t *tui);

bool solar_os_sessions_switch_to_app(const solar_os_app_t *app);
bool solar_os_sessions_switch_to_app_with_policy(const solar_os_app_t *app,
                                                 solar_os_launch_policy_t policy);
bool solar_os_sessions_cycle_input_focus(void);
bool solar_os_sessions_cycle_input_focus_previous(void);
esp_err_t solar_os_sessions_move_display_focus(uint32_t direction, bool *moved);
bool solar_os_sessions_cycle_display_focus(void);
bool solar_os_sessions_cycle_display_focus_previous(void);
bool solar_os_sessions_cycle_display_focus_down(void);
bool solar_os_sessions_cycle_display_focus_up(void);
void solar_os_sessions_show_input_focus_overlay(void);
void solar_os_sessions_mark_foreground_dirty(void);

void solar_os_sessions_dispatch_foreground_event(const solar_os_event_t *event);
bool solar_os_sessions_dispatch_input_event(const solar_os_event_t *event);
const solar_os_app_t *solar_os_sessions_input_app(void);
uint32_t solar_os_sessions_requested_tick_interval_ms(void);
uint32_t solar_os_sessions_next_tick_wait_ms(uint32_t now_ms);
void solar_os_sessions_dispatch_tick(uint32_t now_ms);
void solar_os_sessions_dispatch_resume(uint32_t now_ms);
void solar_os_sessions_process_requests(void);
void solar_os_sessions_drain_operations(void);
/* Synchronous UI work on the session task. The callback must not wait for
 * its caller. Teardown is allowed while shutdown requests are draining. */
esp_err_t solar_os_sessions_run_on_owner(esp_err_t (*operation)(void *), void *context);
/* Runtime owner only; keeps contexts alive if an app has not stopped. */
esp_err_t solar_os_sessions_prepare_shutdown(void);
void solar_os_sessions_prompt_if_shell_active(void);

esp_err_t solar_os_sessions_open_shell(solar_os_context_t *caller,
    const solar_os_native_shell_session_request_v1_t *request,
    solar_os_native_shell_session_result_v1_t *result);
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
/* Create a display app retaining the caller's directory for relative paths. */
esp_err_t solar_os_sessions_create_display_app_at(const solar_os_app_t *app,
    const char *target_name, int argc, char **argv, const char *cwd,
    uint8_t *session_id, char *busy_owner, size_t busy_owner_len);
esp_err_t solar_os_sessions_focus_display(const char *target_name);
bool solar_os_sessions_input_focus(char *target_name, size_t target_name_len);
bool solar_os_sessions_active_for_display(const char *target_name, uint8_t *session_id);
bool solar_os_sessions_display_accepts_pointer_events(const char *target_name);
bool solar_os_sessions_context_uses_display(solar_os_context_t *ctx,
                                            const char *target_name);
/* Display layout changes run with the session owner and keep the built-in shell attached. */
esp_err_t solar_os_sessions_create_display_layout(const solar_os_display_layout_request_v1_t *request,
                                                   char *busy_owner, size_t capacity);
esp_err_t solar_os_sessions_remove_display_layout(solar_os_display_layout_kind_t kind,
                                                   const char *name);
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
esp_err_t solar_os_sessions_snapshot(uint32_t kinds, uint32_t id,
    solar_os_native_session_info_v1_t *entries, size_t capacity, size_t *total);
esp_err_t solar_os_sessions_get_display_focus(char *name, size_t capacity);
void solar_os_sessions_print_list(solar_os_shell_io_t *io, void *user);

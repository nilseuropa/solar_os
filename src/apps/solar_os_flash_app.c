#include "solar_os_flash_app.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "solar_os_bus_types.h"
#include "solar_os_buses.h"
#include "solar_os_flash.h"
#include "solar_os_keys.h"
#include "solar_os_log.h"
#include "solar_os_memory.h"
#include "solar_os_queue.h"
#include "solar_os_shell_io.h"
#include "solar_os_storage.h"
#include "solar_os_task.h"
#include "solar_os_terminal.h"
#include "solar_os_tui.h"
#include "solar_os_tui_widgets.h"
#include "solar_os_uart.h"

#define FLASH_APP_TASK_STACK 16384U
#define FLASH_APP_TASK_PRIORITY (tskIDLE_PRIORITY + 2U)
#define FLASH_APP_EVENT_QUEUE_LEN 12U

static const char *TAG = "solar_os_flash";

SOLAR_OS_TASK_REQUIRE_FOREGROUND_STACK(FLASH_APP_TASK_STACK);

typedef enum {
  FLASH_APP_OPERATION_NONE,
  FLASH_APP_OPERATION_REFRESH,
  FLASH_APP_OPERATION_DOWNLOAD,
  FLASH_APP_OPERATION_PROGRAM,
} flash_app_operation_t;

typedef enum {
  FLASH_APP_EVENT_PROGRESS,
  FLASH_APP_EVENT_DONE,
} flash_app_event_type_t;

typedef enum {
  FLASH_APP_TAB_CATALOG,
  FLASH_APP_TAB_SETTINGS,
} flash_app_tab_t;

typedef enum {
  FLASH_APP_NODE_BOARD,
  FLASH_APP_NODE_FLAVOR,
  FLASH_APP_NODE_ARTIFACT,
} flash_app_node_kind_t;

typedef enum {
  FLASH_APP_MODAL_NONE,
  FLASH_APP_MODAL_DOWNLOAD,
  FLASH_APP_MODAL_PROGRAM,
  FLASH_APP_MODAL_DELETE,
  FLASH_APP_MODAL_RESULT,
  FLASH_APP_MODAL_EDIT_BOOT,
  FLASH_APP_MODAL_EDIT_RESET,
} flash_app_modal_t;

typedef struct {
  flash_app_node_kind_t kind;
  size_t artifact_index;
  size_t parent;
  uint8_t depth;
  bool expanded;
} flash_app_node_t;

typedef struct {
  flash_app_node_kind_t kind;
  char board_id[SOLAR_OS_FLASH_BOARD_ID_MAX];
  char flavor[SOLAR_OS_FLASH_FLAVOR_MAX];
  char version[SOLAR_OS_FLASH_VERSION_MAX];
  bool valid;
} flash_app_node_key_t;

typedef struct {
  flash_app_event_type_t type;
  solar_os_flash_progress_t progress;
  esp_err_t result;
} flash_app_event_t;

typedef struct {
  solar_os_context_t *ctx;
  solar_os_shell_io_t fallback_io;
  solar_os_tui_t tui;
  solar_os_flash_catalog_t *catalog;
  flash_app_node_t *nodes;
  size_t node_count;
  solar_os_flash_artifact_t artifact;
  solar_os_flash_program_options_t program;
  char port[SOLAR_OS_BUS_NAME_MAX];
  char message[128];
  size_t cursor;
  size_t top;
  size_t settings_cursor;
  flash_app_operation_t operation;
  flash_app_tab_t tab;
  flash_app_modal_t modal;
  QueueHandle_t events;
  TaskHandle_t task;
  volatile bool task_done;
  bool running;
  bool command_mode;
  bool command_exit_requested;
  int command_exit_code;
  bool result_received;
  bool tui_active;
  char input[4];
  size_t input_len;
  solar_os_flash_progress_t progress;
  bool progress_valid;
  volatile solar_os_flash_progress_stage_t worker_stage;
  volatile bool worker_stage_valid;
  solar_os_flash_progress_stage_t last_stage;
  bool last_stage_valid;
} flash_app_state_t;

/* The only idle mutable storage is this pointer. The state itself is transient.
 */
static flash_app_state_t *flash_app;

static const char *flash_app_operation_name(flash_app_operation_t operation) {
  switch (operation) {
  case FLASH_APP_OPERATION_REFRESH:
    return "catalog refresh";
  case FLASH_APP_OPERATION_DOWNLOAD:
    return "artifact download";
  case FLASH_APP_OPERATION_PROGRAM:
    return "program";
  case FLASH_APP_OPERATION_NONE:
  default:
    return "operation";
  }
}

static solar_os_shell_io_t *flash_io(flash_app_state_t *state) {
  solar_os_shell_io_t *io = solar_os_context_shell_io(state->ctx);
  if (io == NULL || solar_os_shell_io_kind(io) == SOLAR_OS_SHELL_IO_KIND_NONE) {
    solar_os_shell_io_init_terminal(&state->fallback_io,
                                    solar_os_context_terminal(state->ctx));
    solar_os_context_set_shell_io(state->ctx, &state->fallback_io);
    io = &state->fallback_io;
  }
  return io;
}

static bool flash_app_artifact_first_board(
    const solar_os_flash_catalog_t *catalog, size_t index) {
  for (size_t i = 0U; i < index; i++) {
    if (strcmp(catalog->artifacts[i].board_id,
               catalog->artifacts[index].board_id) == 0) {
      return false;
    }
  }
  return true;
}

static bool flash_app_artifact_first_flavor(
    const solar_os_flash_catalog_t *catalog, size_t index) {
  for (size_t i = 0U; i < index; i++) {
    if (strcmp(catalog->artifacts[i].board_id,
               catalog->artifacts[index].board_id) == 0 &&
        strcmp(catalog->artifacts[i].flavor,
               catalog->artifacts[index].flavor) == 0) {
      return false;
    }
  }
  return true;
}

static void flash_app_node_key(const solar_os_flash_catalog_t *catalog,
                               const flash_app_node_t *node,
                               flash_app_node_key_t *key) {
  if (key == NULL)
    return;
  memset(key, 0, sizeof(*key));
  if (catalog == NULL || node == NULL ||
      node->artifact_index >= catalog->count) {
    return;
  }
  const solar_os_flash_artifact_t *artifact =
      &catalog->artifacts[node->artifact_index];
  key->kind = node->kind;
  strlcpy(key->board_id, artifact->board_id, sizeof(key->board_id));
  if (node->kind >= FLASH_APP_NODE_FLAVOR)
    strlcpy(key->flavor, artifact->flavor, sizeof(key->flavor));
  if (node->kind == FLASH_APP_NODE_ARTIFACT)
    strlcpy(key->version, artifact->version, sizeof(key->version));
  key->valid = true;
}

static bool flash_app_node_matches_key(
    const solar_os_flash_catalog_t *catalog, const flash_app_node_t *node,
    const flash_app_node_key_t *key) {
  if (catalog == NULL || node == NULL || key == NULL || !key->valid ||
      node->kind != key->kind || node->artifact_index >= catalog->count) {
    return false;
  }
  const solar_os_flash_artifact_t *artifact =
      &catalog->artifacts[node->artifact_index];
  return strcmp(artifact->board_id, key->board_id) == 0 &&
         (node->kind == FLASH_APP_NODE_BOARD ||
          strcmp(artifact->flavor, key->flavor) == 0) &&
         (node->kind != FLASH_APP_NODE_ARTIFACT ||
          strcmp(artifact->version, key->version) == 0);
}

static bool flash_app_find_node(const solar_os_flash_catalog_t *catalog,
                                const flash_app_node_t *nodes,
                                size_t node_count,
                                const flash_app_node_key_t *key,
                                size_t *node_index) {
  for (size_t i = 0U; i < node_count; i++) {
    if (!flash_app_node_matches_key(catalog, &nodes[i], key))
      continue;
    if (node_index != NULL)
      *node_index = i;
    return true;
  }
  return false;
}

static bool flash_app_previous_expanded(
    const flash_app_state_t *previous, flash_app_node_kind_t kind,
    const solar_os_flash_artifact_t *artifact) {
  if (previous == NULL || previous->catalog == NULL || artifact == NULL)
    return false;
  flash_app_node_key_t key = {
      .kind = kind,
      .valid = true,
  };
  strlcpy(key.board_id, artifact->board_id, sizeof(key.board_id));
  if (kind == FLASH_APP_NODE_FLAVOR)
    strlcpy(key.flavor, artifact->flavor, sizeof(key.flavor));
  size_t node_index = 0U;
  return flash_app_find_node(previous->catalog, previous->nodes,
                             previous->node_count, &key, &node_index) &&
         previous->nodes[node_index].expanded;
}

static void flash_app_free_tree(flash_app_state_t *state) {
  if (state == NULL)
    return;
  solar_os_memory_free(state->nodes);
  state->nodes = NULL;
  state->node_count = 0U;
  state->cursor = 0U;
  state->top = 0U;
}

static flash_app_node_t *
flash_app_build_tree(const solar_os_flash_catalog_t *catalog,
                     const flash_app_state_t *previous, size_t *node_count) {
  if (node_count == NULL)
    return NULL;
  *node_count = 0U;
  if (catalog == NULL || catalog->count == 0U)
    return NULL;
  const size_t capacity = catalog->count * 3U;
  flash_app_node_t *nodes = solar_os_memory_calloc(
      capacity, sizeof(nodes[0]), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
      "flash.tree");
  if (nodes == NULL)
    return NULL;

  for (size_t board = 0U; board < catalog->count; board++) {
    if (!flash_app_artifact_first_board(catalog, board))
      continue;
    const solar_os_flash_artifact_t *board_artifact =
        &catalog->artifacts[board];
    const size_t board_node = (*node_count)++;
    nodes[board_node] = (flash_app_node_t){
        .kind = FLASH_APP_NODE_BOARD,
        .artifact_index = board,
        .parent = SIZE_MAX,
        .depth = 0U,
        .expanded = flash_app_previous_expanded(
            previous, FLASH_APP_NODE_BOARD, board_artifact),
    };
    for (size_t flavor = 0U; flavor < catalog->count; flavor++) {
      const solar_os_flash_artifact_t *flavor_artifact =
          &catalog->artifacts[flavor];
      if (strcmp(flavor_artifact->board_id,
                 board_artifact->board_id) != 0 ||
          !flash_app_artifact_first_flavor(catalog, flavor)) {
        continue;
      }
      const size_t flavor_node = (*node_count)++;
      nodes[flavor_node] = (flash_app_node_t){
          .kind = FLASH_APP_NODE_FLAVOR,
          .artifact_index = flavor,
          .parent = board_node,
          .depth = 1U,
          .expanded = flash_app_previous_expanded(
              previous, FLASH_APP_NODE_FLAVOR, flavor_artifact),
      };
      for (size_t artifact = 0U; artifact < catalog->count; artifact++) {
        const solar_os_flash_artifact_t *candidate =
            &catalog->artifacts[artifact];
        if (strcmp(candidate->board_id, flavor_artifact->board_id) == 0 &&
            strcmp(candidate->flavor, flavor_artifact->flavor) == 0) {
          nodes[(*node_count)++] = (flash_app_node_t){
              .kind = FLASH_APP_NODE_ARTIFACT,
              .artifact_index = artifact,
              .parent = flavor_node,
              .depth = 2U,
          };
        }
      }
    }
  }
  return nodes;
}

static bool flash_app_node_visible(const flash_app_state_t *state,
                                   size_t node_index) {
  if (state == NULL || node_index >= state->node_count)
    return false;
  size_t parent = state->nodes[node_index].parent;
  while (parent != SIZE_MAX) {
    if (parent >= state->node_count || !state->nodes[parent].expanded)
      return false;
    parent = state->nodes[parent].parent;
  }
  return true;
}

static size_t flash_app_visible_count(const flash_app_state_t *state) {
  size_t count = 0U;
  for (size_t i = 0U; state != NULL && i < state->node_count; i++) {
    if (flash_app_node_visible(state, i))
      count++;
  }
  return count;
}

static bool flash_app_visible_node_at(const flash_app_state_t *state,
                                      size_t visible_index,
                                      size_t *node_index) {
  for (size_t i = 0U; state != NULL && i < state->node_count; i++) {
    if (!flash_app_node_visible(state, i))
      continue;
    if (visible_index == 0U) {
      if (node_index != NULL)
        *node_index = i;
      return true;
    }
    visible_index--;
  }
  return false;
}

static size_t flash_app_visible_index_of(const flash_app_state_t *state,
                                         size_t wanted_node) {
  size_t visible = 0U;
  for (size_t i = 0U; state != NULL && i < state->node_count; i++) {
    if (!flash_app_node_visible(state, i))
      continue;
    if (i == wanted_node)
      return visible;
    visible++;
  }
  return 0U;
}

static void flash_app_ensure_visible(flash_app_state_t *state,
                                     size_t visible_rows) {
  const size_t count = flash_app_visible_count(state);
  if (count == 0U || visible_rows == 0U) {
    state->cursor = 0U;
    state->top = 0U;
    return;
  }
  if (state->cursor >= count)
    state->cursor = count - 1U;
  if (state->cursor < state->top)
    state->top = state->cursor;
  else if (state->cursor >= state->top + visible_rows)
    state->top = state->cursor - visible_rows + 1U;
}

static const solar_os_flash_artifact_t *
flash_app_selected_artifact(const flash_app_state_t *state,
                            size_t *node_index) {
  size_t node = 0U;
  if (!flash_app_visible_node_at(state, state->cursor, &node) ||
      state->nodes[node].kind != FLASH_APP_NODE_ARTIFACT ||
      state->catalog == NULL ||
      state->nodes[node].artifact_index >= state->catalog->count) {
    return NULL;
  }
  if (node_index != NULL)
    *node_index = node;
  return &state->catalog->artifacts[state->nodes[node].artifact_index];
}

static void flash_app_render_status(flash_app_state_t *state) {
  char status[192];
  if (state->running) {
    const char *stage = state->progress_valid
                            ? solar_os_flash_progress_stage_name(
                                  state->progress.stage)
                            : flash_app_operation_name(state->operation);
    if (state->progress_valid && state->progress.total_known) {
      const unsigned percent =
          state->progress.bytes_total > 0U
              ? (unsigned)(((uint64_t)state->progress.bytes_done * 100U) /
                           state->progress.bytes_total)
              : 100U;
      snprintf(status, sizeof(status), "Flash | %s | %s %u%%",
               flash_app_operation_name(state->operation), stage, percent);
    } else {
      snprintf(status, sizeof(status), "Flash | %s | %s",
               flash_app_operation_name(state->operation), stage);
    }
  } else if (state->message[0] != '\0') {
    snprintf(status, sizeof(status), "Flash | %s", state->message);
  } else if (state->catalog != NULL) {
    size_t cached = 0U;
    for (size_t i = 0U; i < state->catalog->count; i++)
      cached += state->catalog->artifacts[i].cached ? 1U : 0U;
    snprintf(status, sizeof(status), "Flash | ready | %u/%u cached | %s",
             (unsigned)cached, (unsigned)state->catalog->count, state->port);
  } else {
    snprintf(status, sizeof(status), "Flash | no verified catalog | %s",
             state->port);
  }
  solar_os_tui_draw_title(&state->tui, status, NULL);
}

static void flash_app_render_tabs(flash_app_state_t *state, size_t cols) {
  (void)solar_os_tui_fill(&state->tui, 1U, 0U, 1U, cols, ' ',
                          SOLAR_OS_TUI_ATTR_NORMAL);
  solar_os_tui_draw_tab(&state->tui, 1U, 1U,
                        cols > 1U ? cols - 1U : 0U, " Catalog ",
                        state->tab == FLASH_APP_TAB_CATALOG);
  if (cols > 13U) {
    solar_os_tui_draw_tab(&state->tui, 1U, 12U, cols - 12U,
                          " Settings ",
                          state->tab == FLASH_APP_TAB_SETTINGS);
  }
}

static void flash_app_render_catalog(flash_app_state_t *state, size_t rows,
                                     size_t cols) {
  const size_t visible_rows = rows > 3U ? rows - 3U : 0U;
  flash_app_ensure_visible(state, visible_rows);
  if (!solar_os_storage_sd_is_mounted()) {
    solar_os_tui_write_cell(&state->tui, 3U, 2U, cols > 4U ? cols - 4U : 0U,
                          "SD card is not mounted.",
                          SOLAR_OS_TUI_ATTR_BOLD);
    return;
  }
  if (state->catalog == NULL) {
    solar_os_tui_write_cell(&state->tui, 3U, 2U, cols > 4U ? cols - 4U : 0U,
                          "No verified catalog. Press r to refresh.",
                          SOLAR_OS_TUI_ATTR_BOLD);
    return;
  }
  for (size_t row = 0U; row < visible_rows; row++) {
    size_t node_index = 0U;
    if (!flash_app_visible_node_at(state, state->top + row, &node_index))
      continue;
    const flash_app_node_t *node = &state->nodes[node_index];
    const solar_os_flash_artifact_t *artifact =
        &state->catalog->artifacts[node->artifact_index];
    char line[192];
    if (node->kind == FLASH_APP_NODE_BOARD) {
      snprintf(line, sizeof(line), "[%c] %s",
               node->expanded ? '-' : '+', artifact->board_name);
    } else if (node->kind == FLASH_APP_NODE_FLAVOR) {
      snprintf(line, sizeof(line), "  |-- [%c] %s",
               node->expanded ? '-' : '+', artifact->flavor);
    } else if (cols >= 54U) {
      snprintf(line, sizeof(line), "      |-- %c %-12s %-10s %s",
               artifact->cached ? '*' : ' ', artifact->version,
               artifact->chip, artifact->cached ? "cached on SD" : "remote");
    } else {
      snprintf(line, sizeof(line), "      |-- %c %s",
               artifact->cached ? '*' : ' ', artifact->version);
    }
    const bool selected = state->top + row == state->cursor;
    const uint8_t attr =
        selected ? SOLAR_OS_TUI_ATTR_INVERSE | SOLAR_OS_TUI_ATTR_BOLD
                 : (node->kind == FLASH_APP_NODE_ARTIFACT
                        ? SOLAR_OS_TUI_ATTR_NORMAL
                        : SOLAR_OS_TUI_ATTR_BOLD);
    (void)solar_os_tui_fill(&state->tui, row + 2U, 0U, 1U, cols, ' ', attr);
    solar_os_tui_write_cell(&state->tui, row + 2U, 0U, cols, line, attr);
  }
}

static const char *flash_app_pin_text(int pin, char *buffer,
                                      size_t buffer_len) {
  if (pin < 0)
    return "not set";
  snprintf(buffer, buffer_len, "gpio%d", pin);
  return buffer;
}

static void flash_app_render_settings(flash_app_state_t *state, size_t rows,
                                      size_t cols) {
  static const char *const labels[] = {"Port", "BOOT pin", "RESET pin"};
  if (state->settings_cursor >= 3U)
    state->settings_cursor = 2U;
  for (size_t row = 0U; row < 3U && row + 3U < rows; row++) {
    char pin[16];
    const char *value = row == 0U
                            ? state->port
                            : flash_app_pin_text(
                                  row == 1U ? state->program.boot_pin
                                            : state->program.reset_pin,
                                  pin, sizeof(pin));
    char line[96];
    snprintf(line, sizeof(line), "%-12s %s", labels[row], value);
    const uint8_t attr = row == state->settings_cursor
                             ? SOLAR_OS_TUI_ATTR_INVERSE |
                                   SOLAR_OS_TUI_ATTR_BOLD
                             : SOLAR_OS_TUI_ATTR_NORMAL;
    (void)solar_os_tui_fill(&state->tui, row + 3U, 1U, 1U,
                            cols > 2U ? cols - 2U : 0U, ' ', attr);
    solar_os_tui_write_cell(&state->tui, row + 3U, 1U,
                          cols > 2U ? cols - 2U : 0U, line, attr);
  }
  if (rows > 8U) {
    solar_os_tui_write_cell(
        &state->tui, 7U, 2U, cols > 4U ? cols - 4U : 0U,
        "Control pins are optional and refer to GPIOs on this SolarOS device.",
        SOLAR_OS_TUI_ATTR_NORMAL);
  }
}

static void flash_app_footer(const flash_app_state_t *state, size_t cols,
                             char *footer, size_t footer_len) {
  const char *full = NULL;
  const char *compact = NULL;
  if (footer == NULL || footer_len == 0U)
    return;
  if (state->running) {
    full = "Operation active  Ctrl+] waits for completion";
    compact = "Working  Ctrl+]:wait";
  } else {
    switch (state->modal) {
    case FLASH_APP_MODAL_EDIT_BOOT:
    case FLASH_APP_MODAL_EDIT_RESET:
      full = "Type GPIO number  Enter save  Esc cancel  Del clear";
      compact = "GPIO  Enter:save  Esc:cancel  Del:clear";
      break;
    case FLASH_APP_MODAL_DOWNLOAD:
    case FLASH_APP_MODAL_PROGRAM:
    case FLASH_APP_MODAL_DELETE:
      full = compact = "Enter:continue  Esc:cancel";
      break;
    case FLASH_APP_MODAL_RESULT:
      full = compact = "Enter:close  Ctrl+]:exit";
      break;
    case FLASH_APP_MODAL_NONE:
    default:
      if (state->tab == FLASH_APP_TAB_CATALOG) {
        full = "Tab settings  Up/Down select  Left/Right tree  Enter action  Del delete  r refresh  q exit";
        compact = "Arrows  Enter:action  Del:delete  r:refresh  q:exit";
      } else {
        full = "Tab catalog  Up/Down select  Left/Right port  Enter edit  Del clear  q exit";
        compact = "Arrows  Enter:edit  Del:clear  Tab:catalog  q:exit";
      }
      break;
    }
  }

  const char *text = strlen(full) <= cols ? full : compact;
  size_t length = strlen(text);
  size_t limit = cols < footer_len - 1U ? cols : footer_len - 1U;
  if (length <= limit) {
    memcpy(footer, text, length + 1U);
    return;
  }
  if (limit <= 3U) {
    memset(footer, '.', limit);
    footer[limit] = '\0';
    return;
  }
  memcpy(footer, text, limit - 3U);
  memcpy(footer + limit - 3U, "...", 4U);
}

static void flash_app_render_footer(flash_app_state_t *state, size_t rows,
                                    size_t cols) {
  if (rows == 0U)
    return;
  char footer[SOLAR_OS_TERMINAL_MAX_COLS + 1U];
  flash_app_footer(state, cols, footer, sizeof(footer));
  solar_os_tui_draw_help(&state->tui, footer);
}

static solar_os_tui_rect_t flash_app_popup_bounds(size_t rows, size_t cols) {
  return (solar_os_tui_rect_t){
      .row = rows > 2U ? 1U : 0U,
      .col = 0U,
      .height = rows > 2U ? rows - 2U : rows,
      .width = cols,
  };
}

static void flash_app_render_modal(flash_app_state_t *state, size_t rows,
                                   size_t cols) {
  const solar_os_tui_rect_t bounds = flash_app_popup_bounds(rows, cols);
  solar_os_tui_rect_t popup = {0};
  char text[256];
  const solar_os_flash_artifact_t *artifact = &state->artifact;
  if (state->running) {
    const char *stage = state->progress_valid
                            ? solar_os_flash_progress_stage_name(
                                  state->progress.stage)
                            : flash_app_operation_name(state->operation);
    snprintf(text, sizeof(text), "%s is in progress. Please wait.\n ",
             flash_app_operation_name(state->operation));
    (void)solar_os_tui_text_popup(&state->tui, &bounds, "Working", text,
                                  &popup);
    if (popup.height > 2U && popup.width > 4U) {
      (void)solar_os_tui_progress_bar(
          &state->tui, popup.row + popup.height - 2U, popup.col + 2U,
          popup.width - 4U, stage,
          state->progress_valid ? state->progress.bytes_done : 0U,
          state->progress_valid ? state->progress.bytes_total : 0U,
          state->progress_valid && state->progress.total_known);
    }
    return;
  }

  switch (state->modal) {
  case FLASH_APP_MODAL_DOWNLOAD:
    snprintf(text, sizeof(text),
             "Download and verify %s / %s / %s to the SD card?",
             artifact->board_id, artifact->flavor, artifact->version);
    (void)solar_os_tui_text_popup(&state->tui, &bounds, "Download artifact",
                                  text, NULL);
    break;
  case FLASH_APP_MODAL_PROGRAM:
    if (state->program.boot_pin >= 0 && state->program.reset_pin >= 0) {
      snprintf(text, sizeof(text),
               "SolarOS will put the target in ROM download mode using BOOT gpio%d and RESET gpio%d. Connect crossed TX/RX and common GND.",
               state->program.boot_pin, state->program.reset_pin);
    } else if (state->program.boot_pin < 0 && state->program.reset_pin < 0) {
      snprintf(text, sizeof(text),
               "Put the target in ROM download mode now: hold BOOT, tap RESET, then release BOOT. Connect crossed TX/RX and common GND.");
    } else if (state->program.boot_pin >= 0) {
      snprintf(text, sizeof(text),
               "SolarOS will hold BOOT gpio%d low. After continuing, reset the target manually while the connection is attempted.",
               state->program.boot_pin);
    } else {
      snprintf(text, sizeof(text),
               "Hold the target BOOT signal active before continuing. SolarOS will toggle RESET gpio%d.",
               state->program.reset_pin);
    }
    (void)solar_os_tui_text_popup(&state->tui, &bounds, "Ready to flash",
                                  text, NULL);
    break;
  case FLASH_APP_MODAL_DELETE:
    snprintf(text, sizeof(text),
             "Delete %s / %s / %s from the SD card? The catalog entry remains available for download.",
             artifact->board_id, artifact->flavor, artifact->version);
    (void)solar_os_tui_text_popup(&state->tui, &bounds, "Delete artifact",
                                  text, NULL);
    break;
  case FLASH_APP_MODAL_RESULT:
    (void)solar_os_tui_text_popup(
        &state->tui, &bounds,
        strstr(state->message, "succeeded") != NULL ? "Success" : "Failed",
        state->message, NULL);
    break;
  case FLASH_APP_MODAL_EDIT_BOOT:
  case FLASH_APP_MODAL_EDIT_RESET:
    snprintf(text, sizeof(text),
             "Enter a GPIO number from 0 to 63. Leave it empty to disable automatic control.\nValue: %s_",
             state->input);
    (void)solar_os_tui_text_popup(
        &state->tui, &bounds,
        state->modal == FLASH_APP_MODAL_EDIT_BOOT ? "Set BOOT pin"
                                                  : "Set RESET pin",
        text, NULL);
    break;
  case FLASH_APP_MODAL_NONE:
  default:
    break;
  }
}

static void flash_app_render(flash_app_state_t *state) {
  if (state == NULL || !state->tui_active)
    return;
  const size_t rows = solar_os_tui_rows(&state->tui);
  const size_t cols = solar_os_tui_cols(&state->tui);
  (void)solar_os_tui_set_cursor_visible(&state->tui, false);
  solar_os_tui_clear(&state->tui);
  if (rows == 0U || cols == 0U) {
    solar_os_tui_refresh(&state->tui);
    return;
  }
  flash_app_render_status(state);
  if (rows < 4U || cols < 20U) {
    solar_os_tui_draw_too_small(&state->tui, "flash");
  } else {
    flash_app_render_tabs(state, cols);
    if (state->tab == FLASH_APP_TAB_CATALOG)
      flash_app_render_catalog(state, rows, cols);
    else
      flash_app_render_settings(state, rows, cols);
  }
  flash_app_render_footer(state, rows, cols);
  if (state->running || state->modal != FLASH_APP_MODAL_NONE)
    flash_app_render_modal(state, rows, cols);
  solar_os_tui_refresh(&state->tui);
}

static void flash_app_progress(const solar_os_flash_progress_t *progress,
                               void *user) {
  flash_app_state_t *state = (flash_app_state_t *)user;
  if (state == NULL || state->events == NULL || progress == NULL) {
    return;
  }
  state->worker_stage = progress->stage;
  state->worker_stage_valid = true;
  const flash_app_event_t event = {
      .type = FLASH_APP_EVENT_PROGRESS,
      .progress = *progress,
  };
  (void)xQueueSend(state->events, &event, 0);
}

static void flash_app_worker(void *parameter) {
  flash_app_state_t *state = (flash_app_state_t *)parameter;
  esp_err_t result = ESP_ERR_INVALID_STATE;
  switch (state->operation) {
  case FLASH_APP_OPERATION_REFRESH:
    result = solar_os_flash_catalog_refresh(flash_app_progress, state);
    break;
  case FLASH_APP_OPERATION_DOWNLOAD:
    result = solar_os_flash_artifact_download(state->catalog, &state->artifact,
                                              flash_app_progress, state);
    break;
  case FLASH_APP_OPERATION_PROGRAM:
    result = solar_os_flash_artifact_program(&state->artifact, &state->program,
                                             flash_app_progress, state);
    break;
  case FLASH_APP_OPERATION_NONE:
  default:
    break;
  }
  const flash_app_event_t event = {
      .type = FLASH_APP_EVENT_DONE,
      .result = result,
  };
  (void)xQueueSend(state->events, &event, pdMS_TO_TICKS(100));
  state->task_done = true;
  solar_os_task_delete_external(NULL);
}

static bool flash_app_start_worker(flash_app_state_t *state,
                                   flash_app_operation_t operation) {
  if (state->running) {
    return false;
  }
  if (state->events == NULL) {
    state->events = solar_os_queue_create(FLASH_APP_EVENT_QUEUE_LEN,
                                          sizeof(flash_app_event_t));
    if (state->events == NULL)
      return false;
  } else {
    xQueueReset(state->events);
  }
  state->operation = operation;
  state->task_done = false;
  state->running = true;
  state->last_stage_valid = false;
  state->worker_stage_valid = false;
  state->result_received = false;
  state->progress_valid = false;
  state->modal = FLASH_APP_MODAL_NONE;
  state->message[0] = '\0';
  SOLAR_OS_LOGI(TAG, "%s started", flash_app_operation_name(operation));
  const BaseType_t created = solar_os_task_create_pinned_external(
      flash_app_worker, "solar_os_flash", FLASH_APP_TASK_STACK, state,
      FLASH_APP_TASK_PRIORITY, &state->task, tskNO_AFFINITY,
      SOLAR_OS_TASK_ROLE_FOREGROUND);
  if (created != pdPASS) {
    state->running = false;
    state->task = NULL;
    return false;
  }
  return true;
}

static void
flash_app_print_progress(flash_app_state_t *state,
                         const solar_os_flash_progress_t *progress) {
  solar_os_shell_io_t *io = flash_io(state);
  if (!state->last_stage_valid || state->last_stage != progress->stage) {
    solar_os_shell_io_printf(
        io, "flash: %s", solar_os_flash_progress_stage_name(progress->stage));
    if (!progress->total_known)
      solar_os_shell_io_put_char(io, '\n');
    state->last_stage = progress->stage;
    state->last_stage_valid = true;
  }
  if (progress->total_known) {
    const unsigned percent =
        progress->bytes_total > 0U
            ? (unsigned)(((uint64_t)progress->bytes_done * 100U) /
                         progress->bytes_total)
            : 100U;
    solar_os_shell_io_printf(io, " %u%% (%u/%u)\n", percent,
                             (unsigned)progress->bytes_done,
                             (unsigned)progress->bytes_total);
  }
  solar_os_shell_io_flush(io);
}

static void flash_app_reload_catalog(flash_app_state_t *state) {
  flash_app_node_key_t selected_key = {0};
  flash_app_node_key_t top_key = {0};
  size_t node_index = 0U;
  if (flash_app_visible_node_at(state, state->cursor, &node_index))
    flash_app_node_key(state->catalog, &state->nodes[node_index],
                       &selected_key);
  if (flash_app_visible_node_at(state, state->top, &node_index))
    flash_app_node_key(state->catalog, &state->nodes[node_index], &top_key);

  solar_os_flash_catalog_t *catalog = NULL;
  const esp_err_t err = solar_os_flash_catalog_load(&catalog);
  if (err != ESP_OK) {
    solar_os_flash_catalog_free(catalog);
    return;
  }
  size_t node_count = 0U;
  flash_app_node_t *nodes = flash_app_build_tree(catalog, state, &node_count);
  if (catalog->count > 0U && nodes == NULL) {
    solar_os_flash_catalog_free(catalog);
    return;
  }

  solar_os_flash_catalog_free(state->catalog);
  solar_os_memory_free(state->nodes);
  state->catalog = catalog;
  state->nodes = nodes;
  state->node_count = node_count;
  state->cursor = 0U;
  state->top = 0U;

  if (flash_app_find_node(state->catalog, state->nodes, state->node_count,
                          &selected_key, &node_index) &&
      flash_app_node_visible(state, node_index)) {
    state->cursor = flash_app_visible_index_of(state, node_index);
  }
  if (flash_app_find_node(state->catalog, state->nodes, state->node_count,
                          &top_key, &node_index) &&
      flash_app_node_visible(state, node_index)) {
    state->top = flash_app_visible_index_of(state, node_index);
  }
}

static void flash_app_drain_events(flash_app_state_t *state) {
  if (state->events == NULL)
    return;
  bool redraw = false;
  flash_app_event_t event;
  while (xQueueReceive(state->events, &event, 0) == pdPASS) {
    if (event.type == FLASH_APP_EVENT_PROGRESS) {
      if (state->command_mode) {
        flash_app_print_progress(state, &event.progress);
      } else {
        state->progress = event.progress;
        state->progress_valid = true;
        redraw = true;
      }
      continue;
    }
    state->result_received = true;
    solar_os_shell_io_t *io = flash_io(state);
    const char *operation = flash_app_operation_name(state->operation);
    const char *stage =
        state->worker_stage_valid
            ? solar_os_flash_progress_stage_name(state->worker_stage)
            : "startup";
    if (event.result == ESP_OK) {
      snprintf(state->message, sizeof(state->message), "%s succeeded",
               operation);
      SOLAR_OS_LOGI(TAG, "%s succeeded", operation);
      if (state->command_mode)
        solar_os_shell_io_writeln(io, "flash: success");
      if (state->operation == FLASH_APP_OPERATION_REFRESH ||
          state->operation == FLASH_APP_OPERATION_DOWNLOAD) {
        flash_app_reload_catalog(state);
      }
    } else {
      state->command_exit_code = 1;
      snprintf(state->message, sizeof(state->message),
               "%s failed at %s: %s (0x%x)", operation, stage,
               esp_err_to_name(event.result), (unsigned)event.result);
      SOLAR_OS_LOGE(TAG, "%s failed stage=%s error=%s (0x%x)", operation, stage,
                    esp_err_to_name(event.result), (unsigned)event.result);
      if (state->command_mode)
        solar_os_shell_io_printf(io, "flash: %s\n", state->message);
    }
    if (state->command_mode)
      solar_os_shell_io_flush(io);
    redraw = true;
  }
  if (state->task_done && state->running) {
    state->running = false;
    if (!state->result_received) {
      state->command_exit_code = 1;
      strlcpy(state->message, "operation ended without a result event",
              sizeof(state->message));
      SOLAR_OS_LOGE(TAG, "%s", state->message);
      if (state->command_mode) {
        solar_os_shell_io_writeln(
            flash_io(state), "flash: operation ended without a result event");
      }
    }
    if (state->command_mode) {
      state->command_exit_requested = true;
    } else {
      state->modal = FLASH_APP_MODAL_RESULT;
      redraw = true;
    }
  }
  if (redraw && !state->command_mode)
    flash_app_render(state);
}

static bool flash_parse_pin(const char *text, int *pin) {
  if (text == NULL || pin == NULL || text[0] == '\0')
    return false;
  char *end = NULL;
  errno = 0;
  const long value = strtol(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0' || value < 0 || value > 63) {
    return false;
  }
  *pin = (int)value;
  return true;
}

static bool flash_parse_baud(const char *text, uint32_t *baud_rate) {
  if (text == NULL || baud_rate == NULL || text[0] == '\0')
    return false;
  char *end = NULL;
  errno = 0;
  const unsigned long value = strtoul(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0' || value < 9600U ||
      value > 2000000U) {
    return false;
  }
  *baud_rate = (uint32_t)value;
  return true;
}

static bool flash_app_select(flash_app_state_t *state, const char *board,
                             const char *flavor, const char *version) {
  const solar_os_flash_artifact_t *artifact =
      solar_os_flash_catalog_find(state->catalog, board, flavor, version);
  if (artifact == NULL)
    return false;
  state->artifact = *artifact;
  return true;
}

static void flash_app_move_catalog(flash_app_state_t *state, int delta) {
  const size_t count = flash_app_visible_count(state);
  if (count == 0U)
    return;
  if (delta < 0 && state->cursor > 0U)
    state->cursor--;
  else if (delta > 0 && state->cursor + 1U < count)
    state->cursor++;
}

static void flash_app_move_catalog_page(flash_app_state_t *state, bool down) {
  const size_t rows = solar_os_tui_rows(&state->tui);
  const size_t visible = rows > 3U ? rows - 3U : 1U;
  const size_t step = visible > 1U ? visible - 1U : 1U;
  const size_t count = flash_app_visible_count(state);
  if (count == 0U)
    return;
  if (down)
    state->cursor = state->cursor + step < count ? state->cursor + step
                                                 : count - 1U;
  else
    state->cursor = state->cursor > step ? state->cursor - step : 0U;
}

static void flash_app_tree_left(flash_app_state_t *state) {
  size_t node_index = 0U;
  if (!flash_app_visible_node_at(state, state->cursor, &node_index))
    return;
  flash_app_node_t *node = &state->nodes[node_index];
  if (node->kind != FLASH_APP_NODE_ARTIFACT && node->expanded) {
    node->expanded = false;
    return;
  }
  if (node->parent != SIZE_MAX)
    state->cursor = flash_app_visible_index_of(state, node->parent);
}

static void flash_app_tree_right(flash_app_state_t *state) {
  size_t node_index = 0U;
  if (!flash_app_visible_node_at(state, state->cursor, &node_index))
    return;
  flash_app_node_t *node = &state->nodes[node_index];
  if (node->kind != FLASH_APP_NODE_ARTIFACT)
    node->expanded = true;
}

static void flash_app_open_selected(flash_app_state_t *state,
                                    bool force_download,
                                    bool force_program) {
  const solar_os_flash_artifact_t *artifact =
      flash_app_selected_artifact(state, NULL);
  if (artifact == NULL) {
    size_t node_index = 0U;
    if (flash_app_visible_node_at(state, state->cursor, &node_index) &&
        state->nodes[node_index].kind != FLASH_APP_NODE_ARTIFACT) {
      state->nodes[node_index].expanded = !state->nodes[node_index].expanded;
    }
    return;
  }
  state->artifact = *artifact;
  if (force_program && !artifact->cached) {
    strlcpy(state->message, "artifact is not cached; download it first",
            sizeof(state->message));
    state->modal = FLASH_APP_MODAL_RESULT;
  } else if (force_download || !artifact->cached) {
    state->modal = FLASH_APP_MODAL_DOWNLOAD;
  } else {
    state->modal = FLASH_APP_MODAL_PROGRAM;
  }
}

static void flash_app_delete_selected(flash_app_state_t *state) {
  const solar_os_flash_artifact_t *artifact =
      flash_app_selected_artifact(state, NULL);
  if (artifact == NULL)
    return;
  if (!artifact->cached) {
    strlcpy(state->message, "artifact is not cached",
            sizeof(state->message));
    state->modal = FLASH_APP_MODAL_RESULT;
    return;
  }
  state->artifact = *artifact;
  state->modal = FLASH_APP_MODAL_DELETE;
}

static void flash_app_cycle_port(flash_app_state_t *state, int delta) {
  const size_t count =
      solar_os_bus_count_protocol(SOLAR_OS_BUS_PROTOCOL_UART);
  if (count == 0U)
    return;
  size_t current = 0U;
  bool found = false;
  solar_os_bus_info_t info;
  for (size_t i = 0U; i < count; i++) {
    if (solar_os_bus_get_protocol(SOLAR_OS_BUS_PROTOCOL_UART, i, &info) &&
        strcmp(info.name, state->port) == 0) {
      current = i;
      found = true;
      break;
    }
  }
  if (!found)
    current = delta < 0 ? count - 1U : 0U;
  else if (delta < 0)
    current = current > 0U ? current - 1U : count - 1U;
  else
    current = current + 1U < count ? current + 1U : 0U;
  if (solar_os_bus_get_protocol(SOLAR_OS_BUS_PROTOCOL_UART, current, &info)) {
    strlcpy(state->port, info.name, sizeof(state->port));
    state->program.port = state->port;
  }
}

static void flash_app_begin_pin_edit(flash_app_state_t *state, bool boot) {
  const int pin = boot ? state->program.boot_pin : state->program.reset_pin;
  state->input[0] = '\0';
  state->input_len = 0U;
  if (pin >= 0) {
    snprintf(state->input, sizeof(state->input), "%d", pin);
    state->input_len = strlen(state->input);
  }
  state->modal =
      boot ? FLASH_APP_MODAL_EDIT_BOOT : FLASH_APP_MODAL_EDIT_RESET;
}

static bool flash_app_handle_pin_edit(flash_app_state_t *state, uint8_t ch) {
  if (ch == SOLAR_OS_KEY_ESCAPE) {
    state->modal = FLASH_APP_MODAL_NONE;
    return true;
  }
  if (ch == '\r' || ch == '\n') {
    int pin = -1;
    if (state->input_len > 0U && !flash_parse_pin(state->input, &pin)) {
      strlcpy(state->message, "pin must be a GPIO number from 0 to 63",
              sizeof(state->message));
      return true;
    }
    if (state->modal == FLASH_APP_MODAL_EDIT_BOOT)
      state->program.boot_pin = pin;
    else
      state->program.reset_pin = pin;
    state->modal = FLASH_APP_MODAL_NONE;
    state->message[0] = '\0';
    return true;
  }
  if (ch == '\b' || ch == 0x7fU || ch == SOLAR_OS_KEY_DELETE) {
    if (state->input_len > 0U)
      state->input[--state->input_len] = '\0';
    return true;
  }
  if (ch >= '0' && ch <= '9' &&
      state->input_len + 1U < sizeof(state->input)) {
    state->input[state->input_len++] = (char)ch;
    state->input[state->input_len] = '\0';
  }
  return true;
}

static bool flash_app_handle_modal(flash_app_state_t *state, uint8_t ch) {
  if (state->modal == FLASH_APP_MODAL_EDIT_BOOT ||
      state->modal == FLASH_APP_MODAL_EDIT_RESET) {
    return flash_app_handle_pin_edit(state, ch);
  }
  if (ch == SOLAR_OS_KEY_ESCAPE) {
    state->modal = FLASH_APP_MODAL_NONE;
    return true;
  }
  if (state->modal == FLASH_APP_MODAL_RESULT) {
    if (ch == '\r' || ch == '\n' || ch == ' ')
      state->modal = FLASH_APP_MODAL_NONE;
    return true;
  }
  if (ch != '\r' && ch != '\n')
    return true;
  if (state->modal == FLASH_APP_MODAL_DELETE) {
    const esp_err_t err = solar_os_flash_artifact_delete(&state->artifact);
    if (err == ESP_OK) {
      strlcpy(state->message, "artifact delete succeeded",
              sizeof(state->message));
      flash_app_reload_catalog(state);
    } else {
      snprintf(state->message, sizeof(state->message),
               "artifact delete failed: %s (0x%x)", esp_err_to_name(err),
               (unsigned)err);
    }
    state->modal = FLASH_APP_MODAL_RESULT;
    return true;
  }
  const flash_app_operation_t operation =
      state->modal == FLASH_APP_MODAL_DOWNLOAD ? FLASH_APP_OPERATION_DOWNLOAD
                                                : FLASH_APP_OPERATION_PROGRAM;
  if (!flash_app_start_worker(state, operation)) {
    strlcpy(state->message, "worker could not start", sizeof(state->message));
    state->modal = FLASH_APP_MODAL_RESULT;
  }
  return true;
}

static bool flash_app_handle_command(flash_app_state_t *state) {
  solar_os_context_t *ctx = state->ctx;
  solar_os_shell_io_t *io = flash_io(state);
  const int argc = solar_os_context_argc(ctx);
  if (argc <= 1)
    return false;
  state->command_mode = true;

  const char *command = solar_os_context_argv(ctx, 1);
  if (strcmp(command, "refresh") == 0 && argc == 2) {
    solar_os_shell_io_writeln(io, "flash: refreshing signed catalog");
    if (!flash_app_start_worker(state, FLASH_APP_OPERATION_REFRESH)) {
      solar_os_shell_io_writeln(io, "flash: worker could not start");
      state->command_exit_code = 1;
      state->command_exit_requested = true;
    }
    return true;
  }
  if (strcmp(command, "list") == 0 && argc == 2) {
    if (state->catalog == NULL) {
      solar_os_shell_io_writeln(
          io, "flash: no verified catalog; run flash refresh");
      state->command_exit_code = 1;
    } else {
      for (size_t i = 0U; i < state->catalog->count; i++) {
        const solar_os_flash_artifact_t *artifact =
            &state->catalog->artifacts[i];
        solar_os_shell_io_printf(io, "%c %s %s %s %s\n",
                                 artifact->cached ? '*' : ' ',
                                 artifact->board_id, artifact->flavor,
                                 artifact->version, artifact->chip);
      }
    }
    state->command_exit_requested = true;
    return true;
  }
  if (strcmp(command, "download") == 0) {
    if (argc != 4 && argc != 5) {
      solar_os_shell_io_writeln(io,
                                "usage: flash download BOARD FLAVOR [VERSION]");
      state->command_exit_code = 2;
      state->command_exit_requested = true;
    } else if (state->catalog == NULL ||
        !flash_app_select(state, solar_os_context_argv(ctx, 2),
                          solar_os_context_argv(ctx, 3),
                          argc == 5 ? solar_os_context_argv(ctx, 4) : NULL)) {
      solar_os_shell_io_writeln(
          io, "flash: artifact not found in the verified catalog");
      state->command_exit_code = 1;
      state->command_exit_requested = true;
    } else if (!flash_app_start_worker(state, FLASH_APP_OPERATION_DOWNLOAD)) {
      solar_os_shell_io_writeln(io, "flash: worker could not start");
      state->command_exit_code = 1;
      state->command_exit_requested = true;
    }
    return true;
  }

  if (argc < 3) {
    solar_os_shell_io_writeln(
        io, "usage: flash BOARD FLAVOR [version=VERSION] [port=uart0] "
            "[boot=PIN] [reset=PIN] [baud=RATE]");
    state->command_exit_code = 2;
    state->command_exit_requested = true;
    return true;
  }
  if (state->catalog == NULL) {
    solar_os_shell_io_writeln(
        io, "flash: no verified catalog; run flash refresh");
    state->command_exit_code = 1;
    state->command_exit_requested = true;
    return true;
  }
  const char *version = NULL;
  strlcpy(state->port, SOLAR_OS_UART_PORT_NAME, sizeof(state->port));
  state->program.boot_pin = -1;
  state->program.reset_pin = -1;
  state->program.baud_rate = 460800U;
  bool valid = true;
  for (int i = 3; i < argc && valid; i++) {
    const char *arg = solar_os_context_argv(ctx, i);
    if (strncmp(arg, "version=", 8U) == 0)
      version = arg + 8U;
    else if (strncmp(arg, "port=", 5U) == 0) {
      valid = strlcpy(state->port, arg + 5U, sizeof(state->port)) <
                  sizeof(state->port) &&
              state->port[0] != '\0';
    } else if (strncmp(arg, "boot=", 5U) == 0)
      valid = flash_parse_pin(arg + 5U, &state->program.boot_pin);
    else if (strncmp(arg, "reset=", 6U) == 0)
      valid = flash_parse_pin(arg + 6U, &state->program.reset_pin);
    else if (strncmp(arg, "baud=", 5U) == 0)
      valid = flash_parse_baud(arg + 5U, &state->program.baud_rate);
    else
      valid = false;
  }
  state->program.port = state->port;
  if (!valid || !flash_app_select(state, solar_os_context_argv(ctx, 1),
                                  solar_os_context_argv(ctx, 2), version)) {
    solar_os_shell_io_writeln(io,
                              "flash: invalid options or artifact not found");
    state->command_exit_code = 2;
    state->command_exit_requested = true;
  } else if (!state->artifact.cached) {
    solar_os_shell_io_writeln(
        io, "flash: selected artifact is not cached; use flash download first");
    state->command_exit_code = 1;
    state->command_exit_requested = true;
  } else {
    solar_os_shell_io_printf(io, "flash: %s/%s/%s via %s\n",
                             state->artifact.board_id, state->artifact.flavor,
                             state->artifact.version, state->port);
    if (state->program.boot_pin < 0 || state->program.reset_pin < 0) {
      solar_os_shell_io_writeln(io,
                                "flash: put the target in ROM download mode "
                                "now; connect TX to RX, RX to TX, and GND");
    }
    if (!flash_app_start_worker(state, FLASH_APP_OPERATION_PROGRAM)) {
      solar_os_shell_io_writeln(io, "flash: worker could not start");
      state->command_exit_code = 1;
      state->command_exit_requested = true;
    }
  }
  return true;
}

static void flash_app_cleanup(void) {
  if (flash_app == NULL)
    return;
  if (flash_app->tui_active) {
    (void)solar_os_tui_set_cursor_visible(&flash_app->tui, true);
    solar_os_tui_refresh(&flash_app->tui);
    solar_os_tui_end(&flash_app->tui);
    flash_app->tui_active = false;
  }
  if (flash_app->events != NULL)
    solar_os_queue_delete(flash_app->events);
  flash_app_free_tree(flash_app);
  solar_os_flash_catalog_free(flash_app->catalog);
  solar_os_memory_free(flash_app);
  flash_app = NULL;
}

static esp_err_t flash_app_start(solar_os_context_t *ctx) {
  if (solar_os_context_argc(ctx) > 1) {
    solar_os_context_set_app_class(ctx, SOLAR_OS_APP_CLASS_COMMAND);
  }
  if (flash_app != NULL) {
    if (!flash_app->task_done && flash_app->task != NULL) {
      return ESP_ERR_INVALID_STATE;
    }
    flash_app_cleanup();
  }
  flash_app = solar_os_memory_calloc(1U, sizeof(*flash_app),
                                     SOLAR_OS_MEMORY_TRANSIENT, "flash.app");
  if (flash_app == NULL)
    return ESP_ERR_NO_MEM;
  flash_app->ctx = ctx;
  strlcpy(flash_app->port, SOLAR_OS_UART_PORT_NAME, sizeof(flash_app->port));
  flash_app->program = (solar_os_flash_program_options_t){
      .port = flash_app->port,
      .boot_pin = -1,
      .reset_pin = -1,
      .baud_rate = 460800U,
  };
  flash_app_reload_catalog(flash_app);
  if (!flash_app_handle_command(flash_app)) {
    const esp_err_t err = solar_os_tui_screen_begin(&flash_app->tui, ctx);
    if (err != ESP_OK) {
      flash_app_cleanup();
      return err;
    }
    flash_app->tui_active = true;
    flash_app_render(flash_app);
  } else if (flash_app->command_exit_requested) {
    solar_os_context_finish(ctx, flash_app->command_exit_code, NULL);
  }
  solar_os_shell_io_flush(flash_io(flash_app));
  return ESP_OK;
}

static void flash_app_stop(solar_os_context_t *ctx) {
  (void)ctx;
  if (flash_app == NULL)
    return;
  if (flash_app->running &&
      !solar_os_task_wait_done(flash_app->task, &flash_app->task_done,
                               SOLAR_OS_TASK_STOP_WAIT_MS)) {
    return;
  }
  flash_app_cleanup();
}

static bool flash_app_event(solar_os_context_t *ctx,
                            const solar_os_event_t *event) {
  if (flash_app == NULL || event == NULL)
    return false;
  if (event->type == SOLAR_OS_EVENT_TICK) {
    flash_app_drain_events(flash_app);
    if (flash_app->command_exit_requested) {
      solar_os_context_finish(ctx, flash_app->command_exit_code, NULL);
    }
    return true;
  }
  if (event->type != SOLAR_OS_EVENT_CHAR)
    return false;
  const uint8_t ch = (uint8_t)event->data.ch;
  if (ch == SOLAR_OS_KEY_APP_EXIT) {
    if (flash_app->running) {
      strlcpy(flash_app->message,
              "operation is active; wait for its result",
              sizeof(flash_app->message));
      flash_app_render(flash_app);
    } else {
      solar_os_context_finish(ctx, 0, NULL);
    }
    return true;
  }
  if (flash_app->command_mode)
    return true;
  if (flash_app->modal != FLASH_APP_MODAL_NONE) {
    (void)flash_app_handle_modal(flash_app, ch);
    flash_app_render(flash_app);
    return true;
  }
  if (flash_app->running)
    return true;

  if (ch == SOLAR_OS_KEY_ESCAPE || ch == 'q' || ch == 'Q') {
    solar_os_context_finish(ctx, 0, NULL);
    return true;
  }
  if (ch == '\t') {
    flash_app->tab = flash_app->tab == FLASH_APP_TAB_CATALOG
                         ? FLASH_APP_TAB_SETTINGS
                         : FLASH_APP_TAB_CATALOG;
    flash_app_render(flash_app);
    return true;
  }

  if (flash_app->tab == FLASH_APP_TAB_CATALOG) {
    switch (ch) {
    case SOLAR_OS_KEY_UP:
    case 'k':
      flash_app_move_catalog(flash_app, -1);
      break;
    case SOLAR_OS_KEY_DOWN:
    case 'j':
      flash_app_move_catalog(flash_app, 1);
      break;
    case SOLAR_OS_KEY_PAGE_UP:
      flash_app_move_catalog_page(flash_app, false);
      break;
    case SOLAR_OS_KEY_PAGE_DOWN:
      flash_app_move_catalog_page(flash_app, true);
      break;
    case SOLAR_OS_KEY_HOME:
      flash_app->cursor = 0U;
      break;
    case SOLAR_OS_KEY_END:
      if (flash_app_visible_count(flash_app) > 0U)
        flash_app->cursor = flash_app_visible_count(flash_app) - 1U;
      break;
    case SOLAR_OS_KEY_LEFT:
      flash_app_tree_left(flash_app);
      break;
    case SOLAR_OS_KEY_RIGHT:
      flash_app_tree_right(flash_app);
      break;
    case '\r':
    case '\n':
    case ' ':
      flash_app_open_selected(flash_app, false, false);
      break;
    case 'd':
    case 'D':
      flash_app_open_selected(flash_app, true, false);
      break;
    case 'f':
    case 'F':
      flash_app_open_selected(flash_app, false, true);
      break;
    case SOLAR_OS_KEY_DELETE:
    case 0x7fU:
    case 'x':
    case 'X':
      flash_app_delete_selected(flash_app);
      break;
    case 'r':
    case 'R':
      if (!flash_app_start_worker(flash_app, FLASH_APP_OPERATION_REFRESH)) {
        strlcpy(flash_app->message, "worker could not start",
                sizeof(flash_app->message));
        flash_app->modal = FLASH_APP_MODAL_RESULT;
      }
      break;
    default:
      return true;
    }
  } else {
    switch (ch) {
    case SOLAR_OS_KEY_UP:
    case 'k':
      if (flash_app->settings_cursor > 0U)
        flash_app->settings_cursor--;
      break;
    case SOLAR_OS_KEY_DOWN:
    case 'j':
      if (flash_app->settings_cursor < 2U)
        flash_app->settings_cursor++;
      break;
    case SOLAR_OS_KEY_LEFT:
      if (flash_app->settings_cursor == 0U)
        flash_app_cycle_port(flash_app, -1);
      break;
    case SOLAR_OS_KEY_RIGHT:
      if (flash_app->settings_cursor == 0U)
        flash_app_cycle_port(flash_app, 1);
      break;
    case '\r':
    case '\n':
    case ' ':
      if (flash_app->settings_cursor == 0U)
        flash_app_cycle_port(flash_app, 1);
      else
        flash_app_begin_pin_edit(flash_app,
                                 flash_app->settings_cursor == 1U);
      break;
    case SOLAR_OS_KEY_DELETE:
    case 0x7fU:
      if (flash_app->settings_cursor == 1U)
        flash_app->program.boot_pin = -1;
      else if (flash_app->settings_cursor == 2U)
        flash_app->program.reset_pin = -1;
      break;
    default:
      return true;
    }
  }
  flash_app_render(flash_app);
  return true;
}

const solar_os_app_t solar_os_flash_app = {
    .name = "flash",
    .summary = "download and flash SolarOS onto another ESP board",
    .app_class = SOLAR_OS_APP_CLASS_TUI,
    .start = flash_app_start,
    .stop = flash_app_stop,
    .event = flash_app_event,
    .worker_stack_bytes = FLASH_APP_TASK_STACK,
    .worker_stack_external = true,
};

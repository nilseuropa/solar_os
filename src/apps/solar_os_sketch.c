#include "solar_os_sketch.h"

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "solar_os_gfx.h"
#include "solar_os_keys.h"
#include "solar_os_memory.h"
#include "solar_os_sketch_canvas.h"
#include "solar_os_stb_image.h"
#include "solar_os_storage.h"
#include "solar_os_storage_browser.h"

#define SKETCH_SIDE_BAR 64
#define SKETCH_BOTTOM_BAR 30
#define SKETCH_BROWSER_ROW_HEIGHT 16
#define SKETCH_IMPORT_MAX_BYTES (4U * 1024U * 1024U)
#define SKETCH_IMPORT_MAX_PIXELS (2U * 1024U * 1024U)
#define SKETCH_TICK_MS 40U

typedef enum {
    SKETCH_TOOL_FREEHAND,
    SKETCH_TOOL_LINE,
    SKETCH_TOOL_RECTANGLE,
    SKETCH_TOOL_ELLIPSE,
    SKETCH_TOOL_FILL,
    SKETCH_TOOL_ERASER,
    SKETCH_TOOL_COUNT,
} sketch_tool_t;

typedef enum {
    SKETCH_MODAL_NONE,
    SKETCH_MODAL_NO_POINTER,
    SKETCH_MODAL_OPEN,
    SKETCH_MODAL_IMPORT,
    SKETCH_MODAL_INFO,
} sketch_modal_t;

typedef struct {
    int screen_width;
    int screen_height;
    int canvas_x;
    int canvas_y;
    int canvas_width;
    int canvas_height;
} sketch_layout_t;

typedef struct {
    solar_os_sketch_canvas_t canvas;
    uint8_t *canvas_bitmap;
    size_t canvas_bitmap_size;
    uint8_t *preview_pixels;
    solar_os_storage_browser_t *browser;
    sketch_tool_t tool;
    sketch_modal_t modal;
    uint8_t color;
    uint8_t pattern;
    uint8_t weight;
    int pointer_x;
    int pointer_y;
    int stroke_x;
    int stroke_y;
    int preview_x;
    int preview_y;
    bool drawing;
    bool dirty;
    bool suspended;
    bool render_pending;
    bool pointer_relative;
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    char message[128];
} sketch_state_t;

static void *sketch_state_storage;
#define sketch (*(sketch_state_t *)sketch_state_storage)

static const solar_os_gfx_color_t sketch_colors[4] = {
    SOLAR_OS_GFX_COLOR_RGB_FLAG | 0xffffffU,
    SOLAR_OS_GFX_COLOR_RGB_FLAG | 0xe53935U,
    SOLAR_OS_GFX_COLOR_RGB_FLAG | 0x1e63d5U,
    SOLAR_OS_GFX_COLOR_RGB_FLAG | 0x000000U,
};

static bool sketch_is_shape_tool(void);
static void sketch_draw_shape(solar_os_sketch_canvas_t *canvas, int x, int y);

static sketch_layout_t sketch_layout(solar_os_gfx_t *gfx)
{
    sketch_layout_t layout = {
        .screen_width = (int)solar_os_gfx_width(gfx),
        .screen_height = (int)solar_os_gfx_height(gfx),
    };
    const int side = layout.screen_width >= 200 ? SKETCH_SIDE_BAR : 42;
    const int bottom = layout.screen_height >= 120 ? SKETCH_BOTTOM_BAR : 22;
    const int button_height = (layout.screen_height - bottom - 4) / 9;
    layout.canvas_x = side;
    layout.canvas_y = button_height + 4;
    layout.canvas_width = layout.screen_width - side;
    layout.canvas_height = button_height * 8;
    return layout;
}

static bool sketch_suffix(const char *name, const char *suffix)
{
    if (name == NULL || suffix == NULL) {
        return false;
    }
    const size_t name_len = strlen(name);
    const size_t suffix_len = strlen(suffix);
    if (name_len < suffix_len) {
        return false;
    }
    name += name_len - suffix_len;
    for (size_t index = 0; index < suffix_len; index++) {
        if (tolower((unsigned char)name[index]) !=
            tolower((unsigned char)suffix[index])) {
            return false;
        }
    }
    return true;
}

static bool sketch_browser_filter(const char *name, void *user)
{
    const sketch_modal_t modal = (sketch_modal_t)(uintptr_t)user;
    if (modal == SKETCH_MODAL_OPEN) {
        return sketch_suffix(name, ".png");
    }
    return sketch_suffix(name, ".png") || sketch_suffix(name, ".jpg") ||
        sketch_suffix(name, ".jpeg") || sketch_suffix(name, ".gif");
}

static bool sketch_has_pointer(void)
{
    const size_t count = solar_os_input_source_count();
    for (size_t index = 0; index < count; index++) {
        solar_os_input_source_info_t info = {0};
        if (solar_os_input_source_get(index, &info) && info.ready &&
            (info.capabilities & (SOLAR_OS_INPUT_CAP_POINTER_ABSOLUTE |
                                  SOLAR_OS_INPUT_CAP_POINTER_RELATIVE)) != 0U) {
            return true;
        }
    }
    return false;
}

static const char *sketch_basename(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        return "untitled.png";
    }
    const char *slash = path != NULL ? strrchr(path, '/') : NULL;
    return slash != NULL ? slash + 1 : (path != NULL ? path : "untitled.png");
}

static void sketch_set_info(const char *message)
{
    strlcpy(sketch.message, message != NULL ? message : "", sizeof(sketch.message));
    sketch.modal = SKETCH_MODAL_INFO;
    sketch.render_pending = true;
}

static uint8_t sketch_nearest_color(uint8_t red, uint8_t green, uint8_t blue)
{
    uint8_t nearest = 0U;
    uint32_t nearest_distance = UINT32_MAX;
    for (uint8_t index = 0; index < 4U; index++) {
        const uint32_t rgb = solar_os_sketch_palette_rgb888[index];
        const int delta_red = (int)red - (int)((rgb >> 16U) & 0xffU);
        const int delta_green = (int)green - (int)((rgb >> 8U) & 0xffU);
        const int delta_blue = (int)blue - (int)(rgb & 0xffU);
        const uint32_t distance = (uint32_t)(delta_red * delta_red +
            delta_green * delta_green + delta_blue * delta_blue);
        if (distance < nearest_distance) {
            nearest = index;
            nearest_distance = distance;
        }
    }
    return nearest;
}

static void sketch_canvas_from_rgb(const uint8_t *rgb,
                                   uint32_t width,
                                   uint32_t height)
{
    int draw_width = sketch.canvas.width;
    int draw_height = sketch.canvas.height;
    const uint64_t height_for_width =
        (uint64_t)draw_width * height / width;
    if (height_for_width <= sketch.canvas.height) {
        draw_height = height_for_width > 0U ? (int)height_for_width : 1;
    } else {
        draw_height = sketch.canvas.height;
        const uint64_t width_for_height =
            (uint64_t)draw_height * width / height;
        draw_width = width_for_height > 0U ? (int)width_for_height : 1;
    }
    const int origin_x = ((int)sketch.canvas.width - draw_width) / 2;
    const int origin_y = ((int)sketch.canvas.height - draw_height) / 2;
    solar_os_sketch_canvas_clear(&sketch.canvas, 0U);
    for (int y = 0; y < draw_height; y++) {
        const uint32_t source_y =
            (uint32_t)(((uint64_t)y * height) / (uint32_t)draw_height);
        for (int x = 0; x < draw_width; x++) {
            const uint32_t source_x =
                (uint32_t)(((uint64_t)x * width) / (uint32_t)draw_width);
            const size_t source = ((size_t)source_y * width + source_x) * 3U;
            const uint8_t color = sketch_nearest_color(
                rgb[source], rgb[source + 1U], rgb[source + 2U]);
            solar_os_sketch_canvas_set(&sketch.canvas, origin_x + x,
                                       origin_y + y, color, 0U);
        }
    }
}

static esp_err_t sketch_read_file(const char *path,
                                  uint8_t **bytes,
                                  size_t *length)
{
    *bytes = NULL;
    *length = 0U;
    struct stat info;
    if (stat(path, &info) != 0 || !S_ISREG(info.st_mode)) {
        return ESP_ERR_NOT_FOUND;
    }
    if (info.st_size <= 0 || (uint64_t)info.st_size > SKETCH_IMPORT_MAX_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return ESP_FAIL;
    }
    uint8_t *data = solar_os_memory_alloc((size_t)info.st_size,
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "sketch.import");
    if (data == NULL) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }
    bool ok = fread(data, 1, (size_t)info.st_size, file) ==
        (size_t)info.st_size;
    if (fclose(file) != 0) {
        ok = false;
    }
    if (!ok) {
        solar_os_memory_free(data);
        return ESP_FAIL;
    }
    *bytes = data;
    *length = (size_t)info.st_size;
    return ESP_OK;
}

static esp_err_t sketch_load_image(const char *path, bool opened)
{
    uint8_t *bytes = NULL;
    size_t length = 0U;
    esp_err_t err = sketch_read_file(path, &bytes, &length);
    if (err != ESP_OK) {
        return err;
    }
    uint8_t *rgb = NULL;
    uint32_t width = 0U;
    uint32_t height = 0U;
    err = solar_os_stb_decode_rgb(bytes, length, SKETCH_IMPORT_MAX_PIXELS,
                                  &rgb, &width, &height);
    solar_os_memory_free(bytes);
    if (err != ESP_OK || rgb == NULL || width == 0U || height == 0U) {
        solar_os_stb_image_free(rgb);
        if (err == ESP_OK) {
            err = ESP_ERR_INVALID_SIZE;
        }
        return err;
    }
    sketch_canvas_from_rgb(rgb, width, height);
    solar_os_stb_image_free(rgb);
    if (opened) {
        strlcpy(sketch.path, path, sizeof(sketch.path));
        sketch.dirty = false;
    } else {
        sketch.path[0] = '\0';
        sketch.dirty = true;
    }
    return ESP_OK;
}

static esp_err_t sketch_default_save_path(char *path, size_t path_len)
{
    char directory[SOLAR_OS_STORAGE_PATH_MAX];
    esp_err_t err = solar_os_storage_default_path("Sketches", directory,
                                                   sizeof(directory));
    if (err != ESP_OK) {
        return err;
    }
    struct stat info;
    if (stat(directory, &info) != 0) {
        err = solar_os_storage_mkdir(directory);
        if (err != ESP_OK) {
            return err;
        }
    } else if (!S_ISDIR(info.st_mode)) {
        return ESP_ERR_INVALID_STATE;
    }
    for (unsigned number = 1U; number <= 999U; number++) {
        const int written = snprintf(path, path_len, "%s/sketch%03u.png",
                                     directory, number);
        if (written < 0 || (size_t)written >= path_len) {
            return ESP_ERR_INVALID_SIZE;
        }
        if (stat(path, &info) != 0) {
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t sketch_save(void)
{
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    if (sketch.path[0] != '\0') {
        strlcpy(path, sketch.path, sizeof(path));
    } else {
        esp_err_t err = sketch_default_save_path(path, sizeof(path));
        if (err != ESP_OK) {
            return err;
        }
    }
    char temporary[SOLAR_OS_STORAGE_PATH_MAX];
    char backup[SOLAR_OS_STORAGE_PATH_MAX];
    esp_err_t err = solar_os_storage_sibling_path(path, ".tmp", temporary,
                                                   sizeof(temporary));
    if (err == ESP_OK) {
        err = solar_os_storage_sibling_path(path, ".bak", backup,
                                            sizeof(backup));
    }
    if (err != ESP_OK) {
        return err;
    }
    FILE *file = fopen(temporary, "wb");
    if (file == NULL) {
        return ESP_FAIL;
    }
    err = solar_os_sketch_canvas_write_png(file, &sketch.canvas);
    if (err == ESP_OK) {
        err = solar_os_storage_sync_file(file);
    }
    if (fclose(file) != 0 && err == ESP_OK) {
        err = ESP_FAIL;
    }
    if (err == ESP_OK) {
        err = solar_os_storage_replace_file(temporary, path, backup);
    } else {
        (void)solar_os_storage_remove(temporary);
    }
    if (err == ESP_OK) {
        strlcpy(sketch.path, path, sizeof(sketch.path));
        sketch.dirty = false;
    }
    return err;
}

static void sketch_browser_close(void)
{
    solar_os_storage_browser_destroy(sketch.browser);
    sketch.browser = NULL;
    sketch.modal = SKETCH_MODAL_NONE;
    sketch.render_pending = true;
}

static void sketch_path_directory(const char *path,
                                  char *directory,
                                  size_t directory_len)
{
    strlcpy(directory, path != NULL && path[0] != '\0' ? path : "/",
            directory_len);
    char *slash = strrchr(directory, '/');
    if (slash == NULL || slash == directory) {
        strlcpy(directory, "/", directory_len);
    } else {
        *slash = '\0';
    }
}

static void sketch_browser_open(sketch_modal_t modal)
{
    solar_os_storage_browser_destroy(sketch.browser);
    sketch.browser = NULL;
    esp_err_t err = solar_os_storage_browser_create(
        sketch_browser_filter, (void *)(uintptr_t)modal, &sketch.browser);
    char directory[SOLAR_OS_STORAGE_PATH_MAX];
    if (sketch.path[0] != '\0') {
        sketch_path_directory(sketch.path, directory, sizeof(directory));
    } else {
        strlcpy(directory, solar_os_storage_mount_point(), sizeof(directory));
        if (directory[0] == '\0') {
            strlcpy(directory, "/", sizeof(directory));
        }
    }
    if (err == ESP_OK) {
        err = solar_os_storage_browser_open(sketch.browser, directory);
    }
    if (err != ESP_OK) {
        solar_os_storage_browser_destroy(sketch.browser);
        sketch.browser = NULL;
        sketch_set_info("Cannot open storage browser");
        return;
    }
    sketch.modal = modal;
    sketch.render_pending = true;
}

static void sketch_browser_activate(void)
{
    if (sketch.browser == NULL) {
        return;
    }
    char selected[SOLAR_OS_STORAGE_PATH_MAX];
    bool file_selected = false;
    const sketch_modal_t modal = sketch.modal;
    esp_err_t err = solar_os_storage_browser_activate(
        sketch.browser, selected, sizeof(selected), &file_selected);
    if (err != ESP_OK) {
        solar_os_storage_browser_destroy(sketch.browser);
        sketch.browser = NULL;
        sketch_set_info("Cannot open selection");
        return;
    }
    if (!file_selected) {
        sketch.render_pending = true;
        return;
    }
    err = sketch_load_image(selected, modal == SKETCH_MODAL_OPEN);
    solar_os_storage_browser_destroy(sketch.browser);
    sketch.browser = NULL;
    if (err != ESP_OK) {
        sketch_set_info("Image load failed");
    } else {
        sketch.modal = SKETCH_MODAL_NONE;
        sketch.render_pending = true;
    }
}

static void sketch_draw_button(solar_os_gfx_t *gfx,
                               int x,
                               int y,
                               int width,
                               int height,
                               const char *label,
                               bool active)
{
    solar_os_gfx_set_color(gfx, active ? SOLAR_OS_GFX_COLOR_DARK :
                                      SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_fill_rect(gfx, x, y, width, height);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_rect(gfx, x, y, width, height);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, x + 4, y + height - 5, label);
}

typedef struct {
    int x;
    int y;
    int size;
} sketch_button_symbol_layout_t;

static sketch_button_symbol_layout_t sketch_draw_button_symbol_frame(
    solar_os_gfx_t *gfx,
    int x,
    int y,
    int width,
    int height,
    const char *label,
    bool active)
{
    solar_os_gfx_set_color(gfx, active ? SOLAR_OS_GFX_COLOR_DARK :
                                      SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_fill_rect(gfx, x, y, width, height);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_rect(gfx, x, y, width, height);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);

    const int symbol_size = height >= 18 ? 16 : 8;
    const int gap = 3;
    const int label_width = label != NULL ?
        (int)solar_os_gfx_text_width(gfx, label) : 0;
    const int content_width = symbol_size +
        (label_width > 0 ? gap + label_width : 0);
    if (content_width <= width - 4) {
        const int symbol_x = x + (width - content_width) / 2;
        if (label_width > 0) {
            solar_os_gfx_text(gfx, symbol_x + symbol_size + gap,
                              y + height - 5, label);
        }
        return (sketch_button_symbol_layout_t){
            .x = symbol_x,
            .y = y + (height - symbol_size) / 2,
            .size = symbol_size,
        };
    }
    return (sketch_button_symbol_layout_t){
        .x = x + (width - symbol_size) / 2,
        .y = y + (height - symbol_size) / 2,
        .size = symbol_size,
    };
}

static void sketch_draw_icon_button(solar_os_gfx_t *gfx,
                                    int x,
                                    int y,
                                    int width,
                                    int height,
                                    const char *label,
                                    solar_os_gfx_icon_t icon,
                                    bool active)
{
    const sketch_button_symbol_layout_t symbol =
        sketch_draw_button_symbol_frame(
        gfx, x, y, width, height, label, active);
    solar_os_gfx_icon(gfx, symbol.x, symbol.y, icon,
                      symbol.size == 16 ? SOLAR_OS_GFX_ICON_SIZE_16 :
                                          SOLAR_OS_GFX_ICON_SIZE_8);
}

typedef enum {
    SKETCH_BUTTON_SYMBOL_LINE,
    SKETCH_BUTTON_SYMBOL_RECTANGLE,
    SKETCH_BUTTON_SYMBOL_ELLIPSE,
} sketch_button_symbol_t;

static void sketch_draw_shape_button(solar_os_gfx_t *gfx,
                                     int x,
                                     int y,
                                     int width,
                                     int height,
                                     const char *label,
                                     sketch_button_symbol_t symbol,
                                     bool active)
{
    const sketch_button_symbol_layout_t layout =
        sketch_draw_button_symbol_frame(
            gfx, x, y, width, height, label, active);
    const int left = layout.x + 1;
    const int right = layout.x + layout.size - 2;
    const int top = layout.y + 1;
    const int bottom = layout.y + layout.size - 2;
    switch (symbol) {
    case SKETCH_BUTTON_SYMBOL_LINE:
        solar_os_gfx_line(gfx, left, bottom, right, top);
        break;
    case SKETCH_BUTTON_SYMBOL_RECTANGLE:
        solar_os_gfx_rect(gfx, left, top + 1,
                          right - left + 1, bottom - top - 1);
        break;
    case SKETCH_BUTTON_SYMBOL_ELLIPSE:
        solar_os_gfx_line(gfx, left + 2, top, right - 2, top);
        solar_os_gfx_line(gfx, left + 2, bottom, right - 2, bottom);
        solar_os_gfx_line(gfx, left, top + 2, left, bottom - 2);
        solar_os_gfx_line(gfx, right, top + 2, right, bottom - 2);
        solar_os_gfx_line(gfx, left, top + 2, left + 2, top);
        solar_os_gfx_line(gfx, right - 2, top, right, top + 2);
        solar_os_gfx_line(gfx, left, bottom - 2, left + 2, bottom);
        solar_os_gfx_line(gfx, right - 2, bottom, right, bottom - 2);
        break;
    }
}

static void sketch_render_canvas(solar_os_gfx_t *gfx,
                                 const sketch_layout_t *layout)
{
    solar_os_sketch_canvas_t preview = sketch.canvas;
    const solar_os_sketch_canvas_t *render_canvas = &sketch.canvas;
    if (sketch.drawing && sketch_is_shape_tool() &&
        sketch.preview_pixels != NULL) {
        memcpy(sketch.preview_pixels, sketch.canvas.pixels,
               solar_os_sketch_canvas_bytes(sketch.canvas.width,
                                            sketch.canvas.height));
        preview.pixels = sketch.preview_pixels;
        sketch_draw_shape(&preview, sketch.preview_x, sketch.preview_y);
        render_canvas = &preview;
    }
    if (solar_os_gfx_format(gfx) == SOLAR_OS_DISPLAY_FORMAT_INDEX8) {
        solar_os_gfx_bitmap_2bpp(gfx, layout->canvas_x, layout->canvas_y,
                                 layout->canvas_width, layout->canvas_height,
                                 render_canvas->pixels,
                                 solar_os_sketch_canvas_bytes(
                                     render_canvas->width,
                                     render_canvas->height),
                                 sketch_colors);
    } else {
        if (!solar_os_sketch_canvas_render_xbm(
                render_canvas, sketch.canvas_bitmap,
                sketch.canvas_bitmap_size)) {
            return;
        }
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        solar_os_gfx_bitmap(gfx, layout->canvas_x, layout->canvas_y,
                            layout->canvas_width, layout->canvas_height,
                            sketch.canvas_bitmap);
    }
}

static void sketch_render_browser(solar_os_gfx_t *gfx,
                                  const sketch_layout_t *layout)
{
    const int x = 10;
    const int y = 12;
    const int width = layout->screen_width - 20;
    const int height = layout->screen_height - 24;
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_fill_rect(gfx, x, y, width, height);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_rect(gfx, x, y, width, height);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, x + 5, y + 12,
                      sketch.modal == SKETCH_MODAL_OPEN ? "Open PNG" :
                                                         "Import image");
    const char *path = solar_os_storage_browser_path(sketch.browser);
    solar_os_gfx_text(gfx, x + 70, y + 12, path);

    const int list_top = y + 18;
    const int rows = (height - 42) / SKETCH_BROWSER_ROW_HEIGHT;
    const size_t count = solar_os_storage_browser_count(sketch.browser);
    const size_t cursor = solar_os_storage_browser_cursor(sketch.browser);
    size_t first = cursor >= (size_t)rows ? cursor - (size_t)rows + 1U : 0U;
    if (first + (size_t)rows > count && count > (size_t)rows) {
        first = count - (size_t)rows;
    }
    for (int row = 0; row < rows && first + (size_t)row < count; row++) {
        solar_os_storage_browser_entry_t entry = {0};
        if (!solar_os_storage_browser_entry(sketch.browser,
                                             first + (size_t)row, &entry)) {
            continue;
        }
        const int row_y = list_top + row * SKETCH_BROWSER_ROW_HEIGHT;
        if (first + (size_t)row == cursor) {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_LIGHT);
            solar_os_gfx_fill_rect(gfx, x + 2, row_y, width - 4,
                                   SKETCH_BROWSER_ROW_HEIGHT);
        }
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        char label[SOLAR_OS_STORAGE_BROWSER_NAME_MAX + 4U];
        snprintf(label, sizeof(label), "%s%s", entry.is_directory ? "> " : "  ",
                 entry.name);
        solar_os_gfx_text(gfx, x + 5, row_y + 11, label);
    }
    sketch_draw_button(gfx, x + width - 52, y + height - 20,
                       46, 16, "Cancel", false);
}

static void sketch_render_popup(solar_os_gfx_t *gfx,
                                const sketch_layout_t *layout)
{
    const int width = layout->screen_width > 300 ? 250 :
        layout->screen_width - 24;
    const int height = 78;
    const int x = (layout->screen_width - width) / 2;
    const int y = (layout->screen_height - height) / 2;
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_fill_rect(gfx, x, y, width, height);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_rect(gfx, x, y, width, height);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_12);
    solar_os_gfx_text(gfx, x + 7, y + 16,
                      sketch.modal == SKETCH_MODAL_NO_POINTER ?
                          "No pointer attached" : "Sketch");
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    if (sketch.modal == SKETCH_MODAL_NO_POINTER) {
        solar_os_gfx_text(gfx, x + 7, y + 34,
                          "Attach a pointer at any time.");
        solar_os_gfx_text(gfx, x + 7, y + 48,
                          "Enter or Esc dismisses this message.");
    } else {
        solar_os_gfx_text(gfx, x + 7, y + 38, sketch.message);
    }
    sketch_draw_button(gfx, x + width - 40, y + height - 20,
                       34, 15, "OK", false);
}

static void sketch_render(solar_os_context_t *ctx)
{
    if (sketch.suspended) {
        return;
    }
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) {
        return;
    }
    const sketch_layout_t layout = sketch_layout(gfx);
    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    sketch_render_canvas(gfx, &layout);

    const int menu_width = layout.canvas_x - 4;
    const int menu_step = menu_width + 2;
    static const char *const menu_labels[] = {"Save", "Open", "Import"};
    static const solar_os_gfx_icon_t menu_icons[] = {
        SOLAR_OS_GFX_ICON_HARD_DRIVE,
        SOLAR_OS_GFX_ICON_FOLDER,
        SOLAR_OS_GFX_ICON_DATA_TRANSFER_DOWNLOAD,
    };
    for (int item = 0; item < 3; item++) {
        sketch_draw_icon_button(gfx, 2 + item * menu_step, 2,
                                menu_width, layout.canvas_y - 4,
                                menu_labels[item], menu_icons[item], false);
    }
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    if (layout.screen_width > 220) {
        char title[64];
        snprintf(title, sizeof(title), "%s%s",
                 sketch.dirty ? "*" : "", sketch_basename(sketch.path));
        solar_os_gfx_text(gfx, 2 + 3 * menu_step + 4,
                          layout.canvas_y - 7, title);
    }

    const int tool_height = layout.canvas_height / 8;
    char weight[8];
    snprintf(weight, sizeof(weight), "W:%u", sketch.weight);
    sketch_draw_button(gfx, 2, layout.canvas_y, layout.canvas_x - 4,
                       tool_height, weight, false);
    for (int tool = 0; tool < SKETCH_TOOL_COUNT; tool++) {
        const int y = layout.canvas_y + (tool + 1) * tool_height;
        const bool active = sketch.tool == (sketch_tool_t)tool;
        switch ((sketch_tool_t)tool) {
        case SKETCH_TOOL_FREEHAND:
            sketch_draw_icon_button(gfx, 2, y, layout.canvas_x - 4,
                                    tool_height, "Pen",
                                    SOLAR_OS_GFX_ICON_PENCIL, active);
            break;
        case SKETCH_TOOL_LINE:
            sketch_draw_shape_button(gfx, 2, y, layout.canvas_x - 4,
                                     tool_height, "Line",
                                     SKETCH_BUTTON_SYMBOL_LINE, active);
            break;
        case SKETCH_TOOL_RECTANGLE:
            sketch_draw_shape_button(gfx, 2, y, layout.canvas_x - 4,
                                     tool_height, "Rect",
                                     SKETCH_BUTTON_SYMBOL_RECTANGLE, active);
            break;
        case SKETCH_TOOL_ELLIPSE:
            sketch_draw_shape_button(gfx, 2, y, layout.canvas_x - 4,
                                     tool_height, "Oval",
                                     SKETCH_BUTTON_SYMBOL_ELLIPSE, active);
            break;
        case SKETCH_TOOL_FILL:
            sketch_draw_icon_button(gfx, 2, y, layout.canvas_x - 4,
                                    tool_height, "Fill",
                                    SOLAR_OS_GFX_ICON_DROPLET, active);
            break;
        case SKETCH_TOOL_ERASER:
            sketch_draw_icon_button(gfx, 2, y, layout.canvas_x - 4,
                                    tool_height, "Erase",
                                    SOLAR_OS_GFX_ICON_DELETE, active);
            break;
        case SKETCH_TOOL_COUNT:
            break;
        }
    }
    sketch_draw_icon_button(gfx, 2, layout.canvas_y + 7 * tool_height,
                            layout.canvas_x - 4,
                            layout.canvas_height - 7 * tool_height,
                            "Clear", SOLAR_OS_GFX_ICON_TRASH, false);

    const int swatch_y = layout.canvas_y + layout.canvas_height + 3;
    const int swatch = (layout.screen_width - 8) / 8;
    for (int color = 0; color < 4; color++) {
        const int x = 3 + color * swatch;
        solar_os_gfx_set_color(gfx, sketch_colors[color]);
        solar_os_gfx_fill_rect(gfx, x, swatch_y, swatch - 3,
                               layout.screen_height - swatch_y - 2);
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        solar_os_gfx_rect(gfx, x, swatch_y, swatch - 3,
                          layout.screen_height - swatch_y - 2);
        if (sketch.color == (uint8_t)color) {
            solar_os_gfx_rect(gfx, x + 2, swatch_y + 2, swatch - 7,
                              layout.screen_height - swatch_y - 6);
        }
    }
    for (int pattern = 0; pattern < 4; pattern++) {
        const int x = 3 + (pattern + 4) * swatch;
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
        solar_os_gfx_fill_rect(gfx, x, swatch_y, swatch - 3,
                               layout.screen_height - swatch_y - 2);
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        solar_os_gfx_rect(gfx, x, swatch_y, swatch - 3,
                          layout.screen_height - swatch_y - 2);
        for (int py = swatch_y + 2; py < layout.screen_height - 3; py++) {
            for (int px = x + 2; px < x + swatch - 4; px++) {
                const bool mark = pattern == 0 ||
                    (pattern == 1 && ((px + py) & 1) == 0) ||
                    (pattern == 2 && (px & 3) == 0 && (py & 3) == 0) ||
                    (pattern == 3 && ((px + py) & 3) == 0);
                if (mark) {
                    solar_os_gfx_pixel(gfx, px, py);
                }
            }
        }
        if (sketch.pattern == (uint8_t)pattern) {
            solar_os_gfx_rect(gfx, x + 2, swatch_y + 2, swatch - 7,
                              layout.screen_height - swatch_y - 6);
        }
    }

    if (sketch.modal == SKETCH_MODAL_OPEN ||
        sketch.modal == SKETCH_MODAL_IMPORT) {
        sketch_render_browser(gfx, &layout);
    } else if (sketch.modal != SKETCH_MODAL_NONE) {
        sketch_render_popup(gfx, &layout);
    }
    if (sketch.pointer_relative && sketch_has_pointer()) {
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
        solar_os_gfx_fill_circle(gfx, sketch.pointer_x, sketch.pointer_y, 3);
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        solar_os_gfx_circle(gfx, sketch.pointer_x, sketch.pointer_y, 4);
    }
    solar_os_gfx_present(gfx);
    sketch.render_pending = false;
}

static bool sketch_is_shape_tool(void)
{
    return sketch.tool == SKETCH_TOOL_LINE ||
        sketch.tool == SKETCH_TOOL_RECTANGLE ||
        sketch.tool == SKETCH_TOOL_ELLIPSE;
}

static void sketch_draw_shape(solar_os_sketch_canvas_t *canvas, int x, int y)
{
    switch (sketch.tool) {
    case SKETCH_TOOL_LINE:
        solar_os_sketch_canvas_line(canvas, sketch.stroke_x,
                                    sketch.stroke_y, x, y, sketch.color,
                                    sketch.pattern, sketch.weight);
        break;
    case SKETCH_TOOL_RECTANGLE:
        solar_os_sketch_canvas_rect(canvas, sketch.stroke_x,
                                    sketch.stroke_y, x, y, sketch.color,
                                    sketch.pattern, sketch.weight);
        break;
    case SKETCH_TOOL_ELLIPSE:
        solar_os_sketch_canvas_ellipse(canvas, sketch.stroke_x,
                                       sketch.stroke_y, x, y, sketch.color,
                                       sketch.pattern, sketch.weight);
        break;
    case SKETCH_TOOL_FILL:
    case SKETCH_TOOL_ERASER:
    case SKETCH_TOOL_FREEHAND:
    case SKETCH_TOOL_COUNT:
        break;
    }
}

static bool sketch_is_brush_tool(void)
{
    return sketch.tool == SKETCH_TOOL_FREEHAND ||
        sketch.tool == SKETCH_TOOL_ERASER;
}

static uint8_t sketch_brush_color(void)
{
    return sketch.tool == SKETCH_TOOL_ERASER ? 0U : sketch.color;
}

static uint8_t sketch_brush_pattern(void)
{
    return sketch.tool == SKETCH_TOOL_ERASER ? 0U : sketch.pattern;
}

static void sketch_bucket_fill(int x, int y)
{
    const size_t workspace_size =
        solar_os_sketch_canvas_fill_workspace_bytes(&sketch.canvas);
    uint8_t *workspace = solar_os_memory_alloc(
        workspace_size, SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "sketch.fill");
    if (workspace == NULL) {
        sketch_set_info("Not enough memory for fill");
        return;
    }
    const bool changed = solar_os_sketch_canvas_flood_fill(
        &sketch.canvas, x, y, sketch.color, sketch.pattern,
        workspace, workspace_size);
    solar_os_memory_free(workspace);
    if (changed) {
        sketch.dirty = true;
        sketch.render_pending = true;
    }
}

static void sketch_canvas_pointer(const sketch_layout_t *layout,
                                  const solar_os_input_pointer_event_t *pointer)
{
    const int x = sketch.pointer_x - layout->canvas_x;
    const int y = sketch.pointer_y - layout->canvas_y;
    const bool inside = x >= 0 && y >= 0 && x < layout->canvas_width &&
        y < layout->canvas_height;
    if (pointer->action == SOLAR_OS_INPUT_POINTER_PRESS && inside) {
        if (sketch.tool == SKETCH_TOOL_FILL) {
            sketch_bucket_fill(x, y);
            return;
        }
        sketch.drawing = true;
        sketch.stroke_x = x;
        sketch.stroke_y = y;
        sketch.preview_x = x;
        sketch.preview_y = y;
        if (sketch_is_brush_tool()) {
            solar_os_sketch_canvas_line(&sketch.canvas, x, y, x, y,
                                        sketch_brush_color(),
                                        sketch_brush_pattern(),
                                        sketch.weight);
            sketch.dirty = true;
        }
        sketch.render_pending = true;
    } else if (pointer->action == SOLAR_OS_INPUT_POINTER_MOVE &&
               sketch.drawing && sketch_is_brush_tool() && inside) {
        solar_os_sketch_canvas_line(&sketch.canvas, sketch.stroke_x,
                                    sketch.stroke_y, x, y,
                                    sketch_brush_color(),
                                    sketch_brush_pattern(), sketch.weight);
        sketch.stroke_x = x;
        sketch.stroke_y = y;
        sketch.dirty = true;
        sketch.render_pending = true;
    } else if (pointer->action == SOLAR_OS_INPUT_POINTER_MOVE &&
               sketch.drawing && sketch_is_shape_tool()) {
        sketch.preview_x = x < 0 ? 0 :
            (x >= layout->canvas_width ? layout->canvas_width - 1 : x);
        sketch.preview_y = y < 0 ? 0 :
            (y >= layout->canvas_height ? layout->canvas_height - 1 : y);
        sketch.render_pending = true;
    } else if (pointer->action == SOLAR_OS_INPUT_POINTER_RELEASE &&
               sketch.drawing) {
        if (sketch_is_shape_tool()) {
            const int end_x = x < 0 ? 0 :
                (x >= layout->canvas_width ? layout->canvas_width - 1 : x);
            const int end_y = y < 0 ? 0 :
                (y >= layout->canvas_height ? layout->canvas_height - 1 : y);
            sketch_draw_shape(&sketch.canvas, end_x, end_y);
            sketch.dirty = true;
        }
        sketch.drawing = false;
        sketch.render_pending = true;
    }
}

static void sketch_toolbar_pointer(const sketch_layout_t *layout)
{
    const int x = sketch.pointer_x;
    const int y = sketch.pointer_y;
    if (y < layout->canvas_y) {
        const int menu_width = layout->canvas_x - 4;
        const int menu_step = menu_width + 2;
        const int relative_x = x - 2;
        const int item = relative_x >= 0 ? relative_x / menu_step : -1;
        const bool inside_button = item >= 0 && item < 3 &&
            relative_x % menu_step < menu_width && y >= 2 &&
            y < layout->canvas_y - 2;
        if (inside_button && item == 0) {
            const esp_err_t err = sketch_save();
            sketch_set_info(err == ESP_OK ? sketch.path : "Save failed");
        } else if (inside_button && item == 1) {
            sketch_browser_open(SKETCH_MODAL_OPEN);
        } else if (inside_button && item == 2) {
            sketch_browser_open(SKETCH_MODAL_IMPORT);
        }
        return;
    }
    if (x < layout->canvas_x && y < layout->canvas_y + layout->canvas_height) {
        const int tool_height = layout->canvas_height / 8;
        const int row = tool_height > 0 ? (y - layout->canvas_y) / tool_height : 0;
        if (row == 0) {
            sketch.weight = sketch.weight == 1U ? 2U :
                (sketch.weight == 2U ? 4U :
                 (sketch.weight == 4U ? 8U : 1U));
        } else if (row >= 1 && row <= SKETCH_TOOL_COUNT) {
            sketch.tool = (sketch_tool_t)(row - 1);
        } else if (row >= 7) {
            solar_os_sketch_canvas_clear(&sketch.canvas, 0U);
            sketch.dirty = true;
        }
        sketch.render_pending = true;
        return;
    }
    if (y >= layout->canvas_y + layout->canvas_height) {
        int swatch = (layout->screen_width - 8) / 8;
        int index = swatch > 0 ? (x - 3) / swatch : 0;
        if (index >= 0 && index < 4) {
            sketch.color = (uint8_t)index;
        } else if (index >= 4 && index < 8) {
            sketch.pattern = (uint8_t)(index - 4);
        }
        sketch.render_pending = true;
    }
}

static void sketch_browser_pointer(const sketch_layout_t *layout)
{
    const int box_x = 10;
    const int box_y = 12;
    const int box_width = layout->screen_width - 20;
    const int box_height = layout->screen_height - 24;
    if (sketch.pointer_y >= box_y + box_height - 22 &&
        sketch.pointer_x >= box_x + box_width - 58) {
        sketch_browser_close();
        return;
    }
    const int list_top = box_y + 18;
    const int rows = (box_height - 42) / SKETCH_BROWSER_ROW_HEIGHT;
    if (sketch.pointer_y < list_top ||
        sketch.pointer_y >= list_top + rows * SKETCH_BROWSER_ROW_HEIGHT) {
        return;
    }
    const size_t count = solar_os_storage_browser_count(sketch.browser);
    const size_t cursor = solar_os_storage_browser_cursor(sketch.browser);
    size_t first = cursor >= (size_t)rows ? cursor - (size_t)rows + 1U : 0U;
    if (first + (size_t)rows > count && count > (size_t)rows) {
        first = count - (size_t)rows;
    }
    const size_t selected = first +
        (size_t)((sketch.pointer_y - list_top) / SKETCH_BROWSER_ROW_HEIGHT);
    if (selected < count) {
        solar_os_storage_browser_move(sketch.browser,
                                      (int)selected - (int)cursor);
        sketch_browser_activate();
    }
}

static bool sketch_pointer_event(solar_os_context_t *ctx,
                                 const solar_os_input_pointer_event_t *pointer)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) {
        return true;
    }
    const sketch_layout_t layout = sketch_layout(gfx);
    if (pointer->mode == SOLAR_OS_INPUT_POINTER_ABSOLUTE) {
        sketch.pointer_relative = false;
        sketch.pointer_x = pointer->x;
        sketch.pointer_y = pointer->y;
    } else {
        sketch.pointer_relative = true;
        sketch.pointer_x += pointer->delta_x;
        sketch.pointer_y += pointer->delta_y;
    }
    if (sketch.pointer_x < 0) sketch.pointer_x = 0;
    if (sketch.pointer_y < 0) sketch.pointer_y = 0;
    if (sketch.pointer_x >= layout.screen_width)
        sketch.pointer_x = layout.screen_width - 1;
    if (sketch.pointer_y >= layout.screen_height)
        sketch.pointer_y = layout.screen_height - 1;
    if (pointer->action == SOLAR_OS_INPUT_POINTER_MOVE &&
        pointer->mode == SOLAR_OS_INPUT_POINTER_RELATIVE) {
        sketch.render_pending = true;
    }

    if (sketch.modal == SKETCH_MODAL_NO_POINTER ||
        sketch.modal == SKETCH_MODAL_INFO) {
        if (pointer->action == SOLAR_OS_INPUT_POINTER_PRESS) {
            sketch.modal = SKETCH_MODAL_NONE;
            sketch.render_pending = true;
        }
        return true;
    }
    if (sketch.modal == SKETCH_MODAL_OPEN ||
        sketch.modal == SKETCH_MODAL_IMPORT) {
        if (pointer->action == SOLAR_OS_INPUT_POINTER_PRESS) {
            sketch_browser_pointer(&layout);
        }
        return true;
    }
    if (pointer->action == SOLAR_OS_INPUT_POINTER_PRESS &&
        (sketch.pointer_y < layout.canvas_y ||
         sketch.pointer_x < layout.canvas_x ||
         sketch.pointer_y >= layout.canvas_y + layout.canvas_height)) {
        sketch_toolbar_pointer(&layout);
    } else {
        sketch_canvas_pointer(&layout, pointer);
    }
    return true;
}

static bool sketch_char_event(solar_os_context_t *ctx, uint8_t ch)
{
    if (sketch.modal == SKETCH_MODAL_NO_POINTER ||
        sketch.modal == SKETCH_MODAL_INFO) {
        if (ch == '\r' || ch == '\n' || ch == SOLAR_OS_KEY_ESCAPE ||
            ch == SOLAR_OS_KEY_APP_EXIT) {
            sketch.modal = SKETCH_MODAL_NONE;
            sketch.render_pending = true;
        }
        return true;
    }
    if (sketch.modal == SKETCH_MODAL_OPEN ||
        sketch.modal == SKETCH_MODAL_IMPORT) {
        if (ch == SOLAR_OS_KEY_ESCAPE || ch == SOLAR_OS_KEY_APP_EXIT) {
            sketch_browser_close();
        } else if (ch == SOLAR_OS_KEY_UP) {
            solar_os_storage_browser_move(sketch.browser, -1);
            sketch.render_pending = true;
        } else if (ch == SOLAR_OS_KEY_DOWN) {
            solar_os_storage_browser_move(sketch.browser, 1);
            sketch.render_pending = true;
        } else if (ch == '\r' || ch == '\n') {
            sketch_browser_activate();
        }
        return true;
    }
    if (ch == SOLAR_OS_KEY_APP_EXIT || ch == SOLAR_OS_KEY_ESCAPE ||
        ch == 'q' || ch == 'Q') {
        solar_os_context_finish(ctx, 0, NULL);
    } else if (ch == 's' || ch == 'S') {
        const esp_err_t err = sketch_save();
        sketch_set_info(err == ESP_OK ? sketch.path : "Save failed");
    } else if (ch == 'o' || ch == 'O') {
        sketch_browser_open(SKETCH_MODAL_OPEN);
    } else if (ch == 'i' || ch == 'I') {
        sketch_browser_open(SKETCH_MODAL_IMPORT);
    } else if (ch == 'p' || ch == 'P' || ch == 'f' || ch == 'F') {
        sketch.tool = SKETCH_TOOL_FREEHAND;
        sketch.render_pending = true;
    } else if (ch == 'l' || ch == 'L') {
        sketch.tool = SKETCH_TOOL_LINE;
        sketch.render_pending = true;
    } else if (ch == 'r' || ch == 'R') {
        sketch.tool = SKETCH_TOOL_RECTANGLE;
        sketch.render_pending = true;
    } else if (ch == 'e' || ch == 'E') {
        sketch.tool = SKETCH_TOOL_ELLIPSE;
        sketch.render_pending = true;
    } else if (ch == 'b' || ch == 'B') {
        sketch.tool = SKETCH_TOOL_FILL;
        sketch.render_pending = true;
    } else if (ch == 'x' || ch == 'X') {
        sketch.tool = SKETCH_TOOL_ERASER;
        sketch.render_pending = true;
    } else if (ch == 'c' || ch == 'C') {
        solar_os_sketch_canvas_clear(&sketch.canvas, 0U);
        sketch.dirty = true;
        sketch.render_pending = true;
    }
    return true;
}

static esp_err_t sketch_start(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(&sketch, 0, sizeof(sketch));
    const sketch_layout_t layout = sketch_layout(gfx);
    if (layout.canvas_width <= 0 || layout.canvas_height <= 0 ||
        layout.canvas_width > UINT16_MAX || layout.canvas_height > UINT16_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    sketch.canvas.width = (uint16_t)layout.canvas_width;
    sketch.canvas.height = (uint16_t)layout.canvas_height;
    sketch.canvas.pixels = solar_os_memory_alloc(
        solar_os_sketch_canvas_bytes(sketch.canvas.width, sketch.canvas.height),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "sketch.canvas");
    if (sketch.canvas.pixels == NULL) {
        return ESP_ERR_NO_MEM;
    }
    sketch.canvas_bitmap_size = solar_os_sketch_canvas_xbm_bytes(
        sketch.canvas.width, sketch.canvas.height);
    sketch.canvas_bitmap = solar_os_memory_alloc(
        sketch.canvas_bitmap_size, SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
        "sketch.bitmap");
    if (sketch.canvas_bitmap == NULL) {
        solar_os_memory_free(sketch.canvas.pixels);
        sketch.canvas.pixels = NULL;
        return ESP_ERR_NO_MEM;
    }
    sketch.preview_pixels = solar_os_memory_alloc(
        solar_os_sketch_canvas_bytes(sketch.canvas.width,
                                     sketch.canvas.height),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "sketch.preview");
    if (sketch.preview_pixels == NULL) {
        solar_os_memory_free(sketch.canvas_bitmap);
        sketch.canvas_bitmap = NULL;
        sketch.canvas_bitmap_size = 0U;
        solar_os_memory_free(sketch.canvas.pixels);
        sketch.canvas.pixels = NULL;
        return ESP_ERR_NO_MEM;
    }
    solar_os_sketch_canvas_clear(&sketch.canvas, 0U);
    sketch.tool = SKETCH_TOOL_FREEHAND;
    sketch.color = 3U;
    sketch.weight = 2U;
    sketch.pointer_x = layout.canvas_x + layout.canvas_width / 2;
    sketch.pointer_y = layout.canvas_y + layout.canvas_height / 2;
    sketch.modal = sketch_has_pointer() ? SKETCH_MODAL_NONE :
                                           SKETCH_MODAL_NO_POINTER;
    sketch.render_pending = true;
    solar_os_context_set_graphics_active(ctx, true);

    const int argc = solar_os_context_argc(ctx);
    if (argc > 2) {
        solar_os_context_finish(ctx, 2, "usage: sketch [file.png]");
        return ESP_OK;
    } else if (argc == 2) {
        char path[SOLAR_OS_STORAGE_PATH_MAX];
        esp_err_t err = solar_os_storage_resolve_path(
            solar_os_context_argv(ctx, 1), path, sizeof(path));
        if (err == ESP_OK) {
            err = sketch_load_image(path, true);
        }
        if (err != ESP_OK) {
            char message[SOLAR_OS_CONTEXT_STATUS_MESSAGE_MAX];
            snprintf(message,
                     sizeof(message),
                     "sketch: PNG open failed: %s",
                     esp_err_to_name(err));
            solar_os_context_finish(ctx, 1, message);
            return ESP_OK;
        }
    }
    sketch_render(ctx);
    return ESP_OK;
}

static void sketch_cleanup(void)
{
    if (sketch_state_storage == NULL) {
        return;
    }
    solar_os_storage_browser_destroy(sketch.browser);
    sketch.browser = NULL;
    solar_os_memory_free(sketch.canvas.pixels);
    sketch.canvas.pixels = NULL;
    solar_os_memory_free(sketch.canvas_bitmap);
    sketch.canvas_bitmap = NULL;
    sketch.canvas_bitmap_size = 0U;
    solar_os_memory_free(sketch.preview_pixels);
    sketch.preview_pixels = NULL;
}

static void sketch_stop(solar_os_context_t *ctx)
{
    sketch_cleanup();
    solar_os_context_set_graphics_active(ctx, false);
}

static void sketch_suspend(solar_os_context_t *ctx)
{
    sketch.suspended = true;
    solar_os_context_set_graphics_active(ctx, false);
}

static void sketch_resume(solar_os_context_t *ctx)
{
    sketch.suspended = false;
    sketch.render_pending = true;
    solar_os_context_set_graphics_active(ctx, true);
    sketch_render(ctx);
}

static void sketch_title(solar_os_context_t *ctx,
                         char *buffer,
                         size_t buffer_len)
{
    (void)ctx;
    if (buffer != NULL && buffer_len != 0U) {
        snprintf(buffer, buffer_len, "Sketch%s%s",
                 sketch_state_storage != NULL && sketch.dirty ? " * - " : " - ",
                 sketch_state_storage != NULL ? sketch_basename(sketch.path) :
                                                "untitled.png");
    }
}

static bool sketch_event(solar_os_context_t *ctx,
                         const solar_os_event_t *event)
{
    if (event == NULL) {
        return false;
    }
    if (event->type == SOLAR_OS_EVENT_POINTER) {
        return sketch_pointer_event(ctx, &event->data.pointer);
    }
    if (event->type == SOLAR_OS_EVENT_CHAR) {
        return sketch_char_event(ctx, (uint8_t)event->data.ch);
    }
    if (event->type == SOLAR_OS_EVENT_TICK) {
        if (sketch.render_pending) {
            sketch_render(ctx);
        }
        return true;
    }
    if (event->type == SOLAR_OS_EVENT_RESUME) {
        sketch_resume(ctx);
        return true;
    }
    return false;
}

const solar_os_app_t solar_os_sketch_app = {
    .name = "sketch",
    .summary = "pointer-driven paint application",
    .app_class = SOLAR_OS_APP_CLASS_GUI,
    .flags = SOLAR_OS_APP_FLAG_RESUMABLE | SOLAR_OS_APP_FLAG_POINTER_EVENTS,
    .start = sketch_start,
    .suspend = sketch_suspend,
    .resume = sketch_resume,
    .stop = sketch_stop,
    .event = sketch_event,
    .title = sketch_title,
    .state_slot = &sketch_state_storage,
    .state_size = sizeof(sketch_state_t),
    .state_storage = SOLAR_OS_APP_STATE_EXTERNAL_PREFERRED,
    .state_release_cleanup = sketch_cleanup,
    .tick_interval_ms = SKETCH_TICK_MS,
    .tick_deadline_ms = 30U,
};

#pragma once
#include "solar_os_native_service_abi.h"

#define SOLAR_OS_NATIVE_FONT_PACKS_SERVICE "display.fonts"
#define SOLAR_OS_NATIVE_FONT_PACKS_ABI 1U
#define SOLAR_OS_FONT_PACK_NAME_MAX 16
#define SOLAR_OS_FONT_PACK_MAX 4

typedef struct {
    char name[SOLAR_OS_FONT_PACK_NAME_MAX];
    size_t bytes;
    uint8_t faces_loaded, faces_total;
    bool reduced;
} solar_os_font_pack_info_t;

typedef void (*solar_os_font_pack_visit_fn)(const char *name, void *user);

typedef struct {
    uint32_t abi_version, struct_size;
    /* Loaded packs supply missing glyphs to both terminal and graphics text. */
    size_t (*count)(void);
    int (*info)(size_t index, solar_os_font_pack_info_t *info);
    uint32_t (*generation)(void);
    bool (*name_valid)(const char *name);
    /* Load or reload an .sfp file by name from the default volume's fonts
     * directory. Only save_selection changes the persisted boot selection. */
    int (*load)(const char *name);
    int (*unload)(const char *name);
    int (*save_selection)(void);
    int (*saved_selection)(char *names, size_t capacity);
    int (*directory)(char *path, size_t capacity);
    /* Names and the visitor are borrowed only during this blocking call. */
    int (*visit_files)(solar_os_font_pack_visit_fn visit, void *user);
} solar_os_native_font_packs_api_v1_t;

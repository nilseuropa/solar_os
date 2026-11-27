#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "doc_test_support.h"
#include "solar_os.h"
#include "solar_os_memory.h"
#include "solar_os_native_document.h"
#include "solar_os_native_document_abi.h"
#include "solar_os_native_handles.h"
#include "solar_os_stb_image.h"
#include "solar_os_webp_decoder.h"

static const solar_os_app_t owner = {0};

const solar_os_app_t *solar_os_application_descriptor(void)
{
    return &owner;
}

static const uint8_t asset_bytes[] = {0x89, 0x50, 0x4e, 0x47};

esp_err_t solar_os_stb_decode_gray(const uint8_t *data, size_t bytes, uint32_t max_pixels,
                                   uint8_t **gray, uint32_t *width, uint32_t *height)
{
    assert(data == asset_bytes && bytes == sizeof(asset_bytes) && max_pixels >= 6);
    *gray = solar_os_memory_calloc(1, 6, SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "test.document.image");
    if (!*gray) return ESP_ERR_NO_MEM;
    *width = 2;
    *height = 3;
    return ESP_OK;
}

esp_err_t solar_os_webp_decode_gray(const uint8_t *data, size_t bytes, uint32_t max_pixels,
                                    uint8_t **gray, uint32_t *width, uint32_t *height)
{
    return solar_os_stb_decode_gray(data, bytes, max_pixels, gray, width, height);
}

typedef struct {
    const char *path;
    unsigned reads, releases;
    bool bad_size;
} asset_source_t;

static int lend_asset(void *user, const char *path, const char *target,
                      const uint8_t **data, size_t *bytes)
{
    asset_source_t *source = user;
    assert(strcmp(source->path, path) == 0 && strcmp(target, "cover.png") == 0);
    source->reads++;
    *data = asset_bytes;
    *bytes = source->bad_size ? 0 : sizeof(asset_bytes);
    return SOLAR_OS_NATIVE_OK;
}

static void release_asset(void *user, const uint8_t *data)
{
    asset_source_t *source = user;
    assert(data == asset_bytes && source->reads == source->releases + 1);
    source->releases++;
}

static void test_assets(const solar_os_native_document_api_v1_t *api, solar_os_native_handle_t doc)
{
    const char image[] = "![Cover](cover.png)\n";
    asset_source_t source = {.path = "/notes.md"};
    const solar_os_native_document_assets_v1_t assets = {
        .user = &source, .read = lend_asset, .release = release_asset,
    };
    solar_os_native_document_block_v1_t block;
    assert(!api->parse_with_assets(doc, SOLAR_OS_DOCUMENT_MARKDOWN, image, sizeof(image) - 1, source.path, &assets));
    assert(source.reads == 1 && source.releases == 1);
    assert(!api->block(doc, 0, &block) && block.type == SOLAR_OS_DOC_BLOCK_IMAGE);
    assert(block.image_attempted && !block.image_error && block.image_width == 2 && block.image_height == 3);

    source.bad_size = true;
    assert(!api->parse_with_assets(doc, SOLAR_OS_DOCUMENT_MARKDOWN, image, sizeof(image) - 1, source.path, &assets));
    assert(source.reads == 2 && source.releases == 2);
    assert(!api->block(doc, 0, &block) && block.image_error == SOLAR_OS_NATIVE_ERROR_INVALID_SIZE);

    char file[] = "/tmp/solaros-document-assets-XXXXXX";
    int fd = mkstemp(file);
    assert(fd >= 0 && write(fd, image, sizeof(image) - 1) == sizeof(image) - 1 && close(fd) == 0);
    source.path = file;
    source.bad_size = false;
    assert(!api->load_with_assets(doc, SOLAR_OS_DOCUMENT_MARKDOWN, file, &assets));
    assert(source.reads == 3 && source.releases == 3);
    assert(!api->block(doc, 0, &block) && !block.image_error && block.image_width == 2);
    assert(!api->load(doc, SOLAR_OS_DOCUMENT_MARKDOWN, file));
    assert(source.reads == 3 && source.releases == 3);
    assert(!api->block(doc, 0, &block) && block.image_error == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    assert(unlink(file) == 0);
}

int main(void)
{
    const solar_os_native_document_api_v1_t *api = solar_os_native_document_get_service(
        SOLAR_OS_NATIVE_DOCUMENT_SERVICE, SOLAR_OS_NATIVE_DOCUMENT_ABI, sizeof(*api));
    assert(api);
    solar_os_native_handle_t doc;
    assert(!api->create(&doc) && doc);
    const char text[] = "# Title\n\nText with **bold**.\n";
    assert(!api->parse(doc, SOLAR_OS_DOCUMENT_MARKDOWN, text, sizeof(text) - 1, "/notes.md"));
    solar_os_native_document_info_v1_t info;
    assert(!api->info(doc, &info));
    assert(!strcmp(info.path, "/notes.md") && info.source_bytes == sizeof(text) - 1);
    assert(info.block_count >= 2 && info.run_count > 1);
    solar_os_native_document_block_v1_t block;
    assert(!api->block(doc, 0, &block) && block.type == SOLAR_OS_DOC_BLOCK_HEADING && block.level == 1);
    char buffer[128];
    size_t read;
    assert(!api->read_block(doc, 0, 1, buffer, sizeof(buffer), &read));
    assert(read == 4 && !memcmp(buffer, "itle", 4));
    bool bold = false;
    for (size_t i = 0; i < info.run_count; ++i) {
        solar_os_doc_run_t run;
        assert(!api->run(doc, i, &run));
        bold |= (run.style & SOLAR_OS_DOC_RUN_BOLD) != 0;
    }
    assert(bold);
    assert(api->block(doc, info.block_count, &block) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    assert(api->read_source(doc, SIZE_MAX, buffer, sizeof(buffer), &read) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(!read);

    const size_t memory = doc_test_memory_used();
    const char replacement[] = "# Replacement\n\nPlain text.\n";
    doc_test_memory_fail_after(2);
    assert(api->parse(doc, SOLAR_OS_DOCUMENT_MARKDOWN, replacement, sizeof(replacement) - 1, NULL) ==
           SOLAR_OS_NATIVE_ERROR_NO_MEMORY);
    doc_test_memory_fail_after(-1);
    assert(doc_test_memory_used() == memory);
    assert(!api->read_source(doc, 0, buffer, sizeof(buffer), &read));
    assert(read == sizeof(text) - 1 && !memcmp(buffer, text, read));
    assert(api->parse(doc, SOLAR_OS_DOCUMENT_MARKDOWN, text, SIZE_MAX, NULL) == SOLAR_OS_NATIVE_ERROR_INVALID_SIZE);

    char export_path[] = "/tmp/solaros-native-document-XXXXXX";
    int fd = mkstemp(export_path);
    assert(fd >= 0 && close(fd) == 0);
    assert(!api->write_markdown(doc, export_path));
    assert(!api->load(doc, SOLAR_OS_DOCUMENT_TEXT, export_path));
    assert(!api->read_source(doc, 0, buffer, sizeof(buffer), &read));
    assert(read > 8 && !memcmp(buffer, "# Title\n", 8));
    assert(unlink(export_path) == 0);
    assert(api->load(doc, SOLAR_OS_DOCUMENT_TEXT, export_path) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);

    const char html[] = "<h1>HTML</h1><p>Body</p>";
    char *markdown;
    size_t written;
    assert(!api->html_to_markdown(html, sizeof(html) - 1, NULL, &markdown, &written));
    assert(markdown && written && strstr(markdown, "# HTML"));
    solar_os_memory_free(markdown);
    assert(!api->parse(doc, SOLAR_OS_DOCUMENT_HTML, html, sizeof(html) - 1, NULL));
    assert(!api->block(doc, 0, &block) && block.type == SOLAR_OS_DOC_BLOCK_HEADING);
    assert(!api->read_block(doc, 0, 0, buffer, sizeof(buffer), &read));
    assert(read == 4 && !memcmp(buffer, "HTML", 4));

    test_assets(api, doc);
    assert(!solar_os_native_handle_close_owner(&owner));
    assert(api->close(doc) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    assert(api->info(doc, &info) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND && !info.source_bytes);
    assert(!solar_os_native_handle_count() && !doc_test_memory_used());
    puts("native documents: copied records, transactional parsing, file round trip, scoped assets and owned cleanup passed");
    return 0;
}

#include "solar_os_epub.h"

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "solar_os_html.h"
#include "solar_os_log.h"
#include "solar_os_memory.h"
#include "solar_os_storage.h"
#include "solar_os_xml.h"
#include "solar_os_zip.h"

#define EPUB_ENTRY_MAX 256
#define EPUB_ID_MAX 64
#define EPUB_MEDIA_MAX 96
#define EPUB_TITLE_MAX 128
#define EPUB_MANIFEST_MAX 128
#define EPUB_SPINE_MAX 96
#define EPUB_XML_MAX (512U * 1024U)
#define EPUB_XHTML_MAX (1024U * 1024U)
#define EPUB_ASSET_MAX (1024U * 1024U)

static const char *TAG = "solar_os_epub";

typedef struct {
    char id[EPUB_ID_MAX];
    char href[EPUB_ENTRY_MAX];
    char media_type[EPUB_MEDIA_MAX];
    char properties[EPUB_MEDIA_MAX];
} epub_manifest_item_t;

typedef struct {
    char opf_path[EPUB_ENTRY_MAX];
    char opf_dir[EPUB_ENTRY_MAX];
    char title[EPUB_TITLE_MAX];
    char creator[EPUB_TITLE_MAX];
    epub_manifest_item_t manifest[EPUB_MANIFEST_MAX];
    size_t manifest_count;
    char spine[EPUB_SPINE_MAX][EPUB_ID_MAX];
    size_t spine_count;
    bool in_metadata;
    char metadata_text[EPUB_ID_MAX];
} epub_package_t;

typedef struct {
    char rootfile[EPUB_ENTRY_MAX];
} epub_container_t;

struct solar_os_epub_book {
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    epub_package_t package;
};

static void *epub_calloc(size_t count, size_t size)
{
    return solar_os_memory_calloc(count,
                                  size,
                                  SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                                  "epub");
}

static void epub_free(void *ptr)
{
    solar_os_memory_free(ptr);
}

static bool epub_path_has_scheme(const char *path)
{
    if (path == NULL) {
        return false;
    }
    for (const char *p = path; *p != '\0'; p++) {
        if (*p == ':') {
            return true;
        }
        if (*p == '/' || *p == '?' || *p == '#') {
            return false;
        }
    }
    return false;
}

static esp_err_t epub_clean_ref(const char *target, char *out, size_t out_len)
{
    if (target == NULL || out == NULL || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t len = strcspn(target, "?#");
    if (len == 0 || len + 1U > out_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(out, target, len);
    out[len] = '\0';
    return ESP_OK;
}

static void epub_pop_dir(char *dir)
{
    if (dir == NULL || dir[0] == '\0') {
        return;
    }
    size_t len = strlen(dir);
    if (len > 0 && dir[len - 1U] == '/') {
        dir[--len] = '\0';
    }
    char *slash = strrchr(dir, '/');
    if (slash == NULL) {
        dir[0] = '\0';
    } else {
        slash[1] = '\0';
    }
}

static esp_err_t epub_join_entry(const char *base_dir,
                                 const char *href,
                                 char *out,
                                 size_t out_len)
{
    if (href == NULL || out == NULL || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    char clean[EPUB_ENTRY_MAX];
    esp_err_t ret = epub_clean_ref(href, clean, sizeof(clean));
    if (ret != ESP_OK) {
        return ret;
    }
    if (epub_path_has_scheme(clean)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (clean[0] == '/') {
        return strlcpy(out, clean + 1, out_len) < out_len ? ESP_OK : ESP_ERR_INVALID_SIZE;
    }

    char dir[EPUB_ENTRY_MAX] = "";
    if (base_dir != NULL && base_dir[0] != '\0' &&
        strlcpy(dir, base_dir, sizeof(dir)) >= sizeof(dir)) {
        return ESP_ERR_INVALID_SIZE;
    }

    const char *rel = clean;
    while (strncmp(rel, "./", 2) == 0) {
        rel += 2;
    }
    while (strncmp(rel, "../", 3) == 0) {
        epub_pop_dir(dir);
        rel += 3;
        while (strncmp(rel, "./", 2) == 0) {
            rel += 2;
        }
    }

    const int written = snprintf(out, out_len, "%s%s", dir, rel);
    return written >= 0 && (size_t)written < out_len ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static void epub_dirname(const char *path, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (path == NULL) {
        return;
    }
    const char *slash = strrchr(path, '/');
    if (slash == NULL) {
        return;
    }
    const size_t len = (size_t)(slash - path + 1);
    if (len >= out_len) {
        return;
    }
    memcpy(out, path, len);
    out[len] = '\0';
}

static esp_err_t epub_container_event(const solar_os_xml_event_t *event, void *user)
{
    epub_container_t *container = (epub_container_t *)user;
    if (event == NULL || container == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (event->type != SOLAR_OS_XML_EVENT_START || !solar_os_xml_name_eq(event->name, "rootfile")) {
        return ESP_OK;
    }

    const char *full_path = solar_os_xml_attr(event, "full-path");
    if (full_path != NULL && container->rootfile[0] == '\0') {
        strlcpy(container->rootfile, full_path, sizeof(container->rootfile));
    }
    return ESP_OK;
}

static esp_err_t epub_parse_container(const char *archive_path, epub_container_t *container)
{
    uint8_t *data = NULL;
    size_t len = 0;
    esp_err_t ret = solar_os_zip_read_file(archive_path,
                                           "META-INF/container.xml",
                                           EPUB_XML_MAX,
                                           &data,
                                           &len);
    if (ret != ESP_OK) {
        return ret;
    }

    memset(container, 0, sizeof(*container));
    ret = solar_os_xml_parse((const char *)data, len, epub_container_event, container);
    solar_os_zip_free(data);
    if (ret != ESP_OK) {
        return ret;
    }
    return container->rootfile[0] != '\0' ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static epub_manifest_item_t *epub_find_manifest(epub_package_t *package, const char *id)
{
    if (package == NULL || id == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < package->manifest_count; i++) {
        if (strcmp(package->manifest[i].id, id) == 0) {
            return &package->manifest[i];
        }
    }
    return NULL;
}

static esp_err_t epub_package_event(const solar_os_xml_event_t *event, void *user)
{
    epub_package_t *package = (epub_package_t *)user;
    if (event == NULL || package == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (event->type == SOLAR_OS_XML_EVENT_START) {
        if (solar_os_xml_name_eq(event->name, "metadata")) {
            package->in_metadata = true;
            return ESP_OK;
        }
        if (solar_os_xml_name_eq(event->name, "item")) {
            if (package->manifest_count >= EPUB_MANIFEST_MAX) {
                return ESP_ERR_NO_MEM;
            }
            const char *id = solar_os_xml_attr(event, "id");
            const char *href = solar_os_xml_attr(event, "href");
            if (id == NULL || href == NULL) {
                return ESP_OK;
            }
            epub_manifest_item_t *item = &package->manifest[package->manifest_count++];
            memset(item, 0, sizeof(*item));
            strlcpy(item->id, id, sizeof(item->id));
            strlcpy(item->href, href, sizeof(item->href));
            const char *media = solar_os_xml_attr(event, "media-type");
            const char *properties = solar_os_xml_attr(event, "properties");
            if (media != NULL) {
                strlcpy(item->media_type, media, sizeof(item->media_type));
            }
            if (properties != NULL) {
                strlcpy(item->properties, properties, sizeof(item->properties));
            }
            return ESP_OK;
        }
        if (solar_os_xml_name_eq(event->name, "itemref")) {
            if (package->spine_count >= EPUB_SPINE_MAX) {
                return ESP_ERR_NO_MEM;
            }
            const char *idref = solar_os_xml_attr(event, "idref");
            if (idref != NULL) {
                strlcpy(package->spine[package->spine_count++],
                        idref,
                        sizeof(package->spine[0]));
            }
            return ESP_OK;
        }
        if (package->in_metadata &&
            (solar_os_xml_name_eq(event->name, "title") ||
             solar_os_xml_name_eq(event->name, "creator"))) {
            strlcpy(package->metadata_text,
                    solar_os_xml_local_name(event->name),
                    sizeof(package->metadata_text));
        }
        return ESP_OK;
    }

    if (event->type == SOLAR_OS_XML_EVENT_TEXT && package->metadata_text[0] != '\0') {
        if (strcmp(package->metadata_text, "title") == 0 && package->title[0] == '\0') {
            strlcpy(package->title, event->text, sizeof(package->title));
        } else if (strcmp(package->metadata_text, "creator") == 0 && package->creator[0] == '\0') {
            strlcpy(package->creator, event->text, sizeof(package->creator));
        }
        return ESP_OK;
    }

    if (event->type == SOLAR_OS_XML_EVENT_END) {
        if (solar_os_xml_name_eq(event->name, "metadata")) {
            package->in_metadata = false;
        }
        if (package->metadata_text[0] != '\0' &&
            (solar_os_xml_name_eq(event->name, "title") ||
             solar_os_xml_name_eq(event->name, "creator"))) {
            package->metadata_text[0] = '\0';
        }
    }
    return ESP_OK;
}

static esp_err_t epub_parse_package(const char *archive_path,
                                    const char *opf_path,
                                    epub_package_t *package)
{
    uint8_t *data = NULL;
    size_t len = 0;
    esp_err_t ret = solar_os_zip_read_file(archive_path,
                                           opf_path,
                                           EPUB_XML_MAX,
                                           &data,
                                           &len);
    if (ret != ESP_OK) {
        return ret;
    }

    memset(package, 0, sizeof(*package));
    strlcpy(package->opf_path, opf_path, sizeof(package->opf_path));
    epub_dirname(opf_path, package->opf_dir, sizeof(package->opf_dir));

    ret = solar_os_xml_parse((const char *)data, len, epub_package_event, package);
    solar_os_zip_free(data);
    if (ret != ESP_OK) {
        return ret;
    }
    return package->manifest_count > 0 && package->spine_count > 0 ?
        ESP_OK :
        ESP_ERR_NOT_FOUND;
}

static bool epub_manifest_item_is_xhtml(const epub_manifest_item_t *item)
{
    if (item == NULL) {
        return false;
    }
    return strcasecmp(item->media_type, "application/xhtml+xml") == 0 ||
        strcasecmp(item->media_type, "text/html") == 0 ||
        strcasecmp(item->media_type, "application/xml") == 0 ||
        strcasecmp(item->media_type, "image/svg+xml") == 0 ||
        strcasecmp(item->media_type, "") == 0;
}

static bool epub_manifest_item_is_image(const epub_manifest_item_t *item)
{
    return item != NULL &&
        strncasecmp(item->media_type, "image/", 6) == 0 &&
        strcasecmp(item->media_type, "image/svg+xml") != 0;
}

static esp_err_t epub_spine_entry(const epub_package_t *package,
                                  const epub_manifest_item_t *item,
                                  char *entry,
                                  size_t entry_len)
{
    if (package == NULL || item == NULL || entry == NULL || entry_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!epub_manifest_item_is_xhtml(item)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return epub_join_entry(package->opf_dir, item->href, entry, entry_len);
}

static esp_err_t epub_load_image_spine_item(const char *archive_path,
                                            const epub_package_t *package,
                                            const epub_manifest_item_t *item,
                                            solar_os_doc_t *doc)
{
    char entry[EPUB_ENTRY_MAX];
    esp_err_t ret = epub_join_entry(package->opf_dir, item->href, entry, sizeof(entry));
    if (ret != ESP_OK) {
        return ret;
    }

    const char *leaf = strrchr(entry, '/');
    leaf = leaf != NULL ? leaf + 1 : entry;

    char markdown[EPUB_ENTRY_MAX + 8];
    const int markdown_len = snprintf(markdown, sizeof(markdown), "![](%s)\n", leaf);
    if (markdown_len < 0 || (size_t)markdown_len >= sizeof(markdown)) {
        return ESP_ERR_INVALID_SIZE;
    }

    char doc_path[SOLAR_OS_STORAGE_PATH_MAX];
    const int written = snprintf(doc_path, sizeof(doc_path), "%s|%s", archive_path, entry);
    if (written < 0 || (size_t)written >= sizeof(doc_path)) {
        return ESP_ERR_INVALID_SIZE;
    }

    return solar_os_doc_parse_markdown(doc, markdown, (size_t)markdown_len, doc_path);
}

static esp_err_t epub_load_spine_item(const char *archive_path,
                                      const epub_package_t *package,
                                      const epub_manifest_item_t *item,
                                      solar_os_doc_t *doc)
{
    if (archive_path == NULL || package == NULL || item == NULL || doc == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (epub_manifest_item_is_image(item)) {
        return epub_load_image_spine_item(archive_path, package, item, doc);
    }

    char entry[EPUB_ENTRY_MAX];
    esp_err_t ret = epub_spine_entry(package, item, entry, sizeof(entry));
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t *data = NULL;
    size_t len = 0;
    ret = solar_os_zip_read_file(archive_path, entry, EPUB_XHTML_MAX, &data, &len);
    if (ret != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "spine read failed %s: %s", entry, esp_err_to_name(ret));
        return ret;
    }

    char *markdown = NULL;
    size_t markdown_len = 0;
    ret = solar_os_html_to_markdown((const char *)data, len, NULL, &markdown, &markdown_len);
    solar_os_zip_free(data);
    if (ret != ESP_OK) {
        solar_os_html_free(markdown);
        SOLAR_OS_LOGW(TAG, "spine parse failed %s: %s", entry, esp_err_to_name(ret));
        return ret;
    }

    char doc_path[SOLAR_OS_STORAGE_PATH_MAX];
    const int written = snprintf(doc_path, sizeof(doc_path), "%s|%s", archive_path, entry);
    if (written < 0 || (size_t)written >= sizeof(doc_path)) {
        ret = ESP_ERR_INVALID_SIZE;
    } else {
        ret = solar_os_doc_parse_markdown(doc, markdown != NULL ? markdown : "", markdown_len, doc_path);
    }
    solar_os_html_free(markdown);
    return ret;
}

esp_err_t solar_os_epub_open(const char *path, solar_os_epub_book_t **out_book)
{
    if (path == NULL || path[0] == '\0' || out_book == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_book = NULL;

    epub_container_t *container = epub_calloc(1, sizeof(*container));
    solar_os_epub_book_t *book = epub_calloc(1, sizeof(*book));
    if (container == NULL || book == NULL) {
        epub_free(container);
        epub_free(book);
        return ESP_ERR_NO_MEM;
    }
    strlcpy(book->path, path, sizeof(book->path));

    esp_err_t ret = epub_parse_container(path, container);
    if (ret != ESP_OK) {
        epub_free(container);
        epub_free(book);
        return ret;
    }

    ret = epub_parse_package(path, container->rootfile, &book->package);
    epub_free(container);
    if (ret != ESP_OK) {
        epub_free(book);
        return ret;
    }

    *out_book = book;
    return ESP_OK;
}

void solar_os_epub_close(solar_os_epub_book_t *book)
{
    epub_free(book);
}

size_t solar_os_epub_spine_count(const solar_os_epub_book_t *book)
{
    return book != NULL ? book->package.spine_count : 0;
}

const char *solar_os_epub_title(const solar_os_epub_book_t *book)
{
    return book != NULL && book->package.title[0] != '\0' ? book->package.title : "";
}

esp_err_t solar_os_epub_load_spine_doc(solar_os_epub_book_t *book,
                                       size_t spine_index,
                                       solar_os_doc_t *doc)
{
    if (book == NULL || doc == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (spine_index >= book->package.spine_count) {
        return ESP_ERR_INVALID_ARG;
    }

    epub_manifest_item_t *item = epub_find_manifest(&book->package, book->package.spine[spine_index]);
    if (item == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    return epub_load_spine_item(book->path, &book->package, item, doc);
}

esp_err_t solar_os_epub_load_doc(solar_os_doc_t *doc, const char *path)
{
    solar_os_epub_book_t *book = NULL;
    esp_err_t ret = solar_os_epub_open(path, &book);
    if (ret == ESP_OK) {
        ret = solar_os_epub_load_spine_doc(book, 0, doc);
    }
    solar_os_epub_close(book);
    return ret;
}

esp_err_t solar_os_epub_asset_read(void *user,
                                   const char *document_path,
                                   const char *target,
                                   uint8_t **out_data,
                                   size_t *out_len)
{
    (void)user;
    if (document_path == NULL || target == NULL || out_data == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_data = NULL;
    *out_len = 0;

    char archive[SOLAR_OS_STORAGE_PATH_MAX];
    char base_entry[EPUB_ENTRY_MAX] = "";
    const char *pipe = strchr(document_path, '|');
    if (pipe != NULL) {
        const size_t archive_len = (size_t)(pipe - document_path);
        if (archive_len == 0 || archive_len >= sizeof(archive)) {
            return ESP_ERR_INVALID_SIZE;
        }
        memcpy(archive, document_path, archive_len);
        archive[archive_len] = '\0';
        strlcpy(base_entry, pipe + 1, sizeof(base_entry));
    } else {
        strlcpy(archive, document_path, sizeof(archive));
    }

    char base_dir[EPUB_ENTRY_MAX] = "";
    epub_dirname(base_entry, base_dir, sizeof(base_dir));

    char entry[EPUB_ENTRY_MAX];
    esp_err_t ret = epub_join_entry(base_dir, target, entry, sizeof(entry));
    if (ret != ESP_OK) {
        return ret;
    }
    ret = solar_os_zip_read_file(archive, entry, EPUB_ASSET_MAX, out_data, out_len);
    if (ret == ESP_ERR_NOT_FOUND && base_dir[0] != '\0') {
        char root_entry[EPUB_ENTRY_MAX];
        esp_err_t root_ret = epub_join_entry("", target, root_entry, sizeof(root_entry));
        if (root_ret == ESP_OK && strcmp(root_entry, entry) != 0) {
            ret = solar_os_zip_read_file(archive, root_entry, EPUB_ASSET_MAX, out_data, out_len);
        }
    }
    return ret;
}

void solar_os_epub_asset_release(void *user, uint8_t *data)
{
    (void)user;
    solar_os_zip_free(data);
}

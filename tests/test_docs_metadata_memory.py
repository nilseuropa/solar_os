from pathlib import Path
import subprocess
import tempfile
import unittest
from test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class DocsMetadataMemoryTest(unittest.TestCase):
    def test_catalog_validation_reads_each_id_once_and_rejects_duplicates(self):
        source = (ROOT / 'src/services/solar_os_docs.c').read_text()
        functions = production_functions(source, ('docs_compare_page_ids', 'docs_validate_catalog_pages'))
        code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_err.h"
#define SOLAR_OS_DOCS_PAGE_COUNT_MAX 1024U
#define SOLAR_OS_CRYPTO_SHA256_HEX_LEN 65U
#define SOLAR_OS_MEMORY_EXTERNAL_REQUIRED 1
typedef struct { char id[64]; bool invalid, invalid_packages; } solar_os_json_value_t;
typedef struct { const char *id; } solar_os_manual_page_t;
static solar_os_json_value_t pages[1024];
static size_t reads, allocations;
static bool fail_alloc;
static void *solar_os_memory_calloc(size_t n, size_t s, unsigned cls, const char *tag)
{ assert(cls == 1 && !strcmp(tag, "docs.validation")); if (fail_alloc) return NULL; allocations++; return calloc(n, s); }
static void solar_os_memory_free(void *p) { if (p) { assert(allocations); allocations--; free(p); } }
static const solar_os_json_value_t *solar_os_json_array_get(const solar_os_json_value_t *p, size_t i)
{ assert(i < 1024); reads++; return p + i; }
static esp_err_t docs_page_metadata(const solar_os_json_value_t *p, char *id, size_t id_len,
    char *relative, size_t relative_len, char *sha, uint32_t *size)
{ (void)relative; (void)relative_len; (void)sha; (void)size;
  if (p->invalid) return ESP_ERR_INVALID_RESPONSE; snprintf(id, id_len, "%s", p->id); return ESP_OK; }
static esp_err_t docs_catalog_page_applies(const solar_os_json_value_t *p, bool *applies)
{ *applies = true; return p->invalid_packages ? ESP_ERR_INVALID_RESPONSE : ESP_OK; }
static size_t solar_os_manual_embedded_count(void) { return 1; }
static const solar_os_manual_page_t *solar_os_manual_embedded_get(size_t i)
{ assert(i == 0); static const solar_os_manual_page_t p = {"topic-0000"}; return &p; }
''' + functions + r'''
int main(void) {
    for (unsigned i = 0; i < 1024; i++) snprintf(pages[i].id, sizeof(pages[i].id), "topic-%04u", 1023-i);
    assert(docs_validate_catalog_pages(pages, 1024, true) == ESP_OK);
    assert(reads == 1024 && allocations == 0);
    strcpy(pages[1023].id, pages[0].id);
    assert(docs_validate_catalog_pages(pages, 1024, false) == ESP_ERR_INVALID_RESPONSE && allocations == 0);
    strcpy(pages[1023].id, "different");
    assert(docs_validate_catalog_pages(pages, 1024, true) == ESP_ERR_INVALID_RESPONSE && allocations == 0);
    assert(docs_validate_catalog_pages(pages, 1024, false) == ESP_OK);
    pages[511].invalid = true;
    assert(docs_validate_catalog_pages(pages, 1024, false) == ESP_ERR_INVALID_RESPONSE && allocations == 0);
    pages[511].invalid = false; pages[511].invalid_packages = true;
    assert(docs_validate_catalog_pages(pages, 1024, false) == ESP_ERR_INVALID_RESPONSE && allocations == 0);
    fail_alloc = true;
    assert(docs_validate_catalog_pages(pages, 1024, false) == ESP_ERR_NO_MEM && allocations == 0);
    assert(docs_validate_catalog_pages(pages, 0, false) == ESP_ERR_INVALID_RESPONSE);
    assert(docs_validate_catalog_pages(pages, 1025, false) == ESP_ERR_INVALID_RESPONSE);
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp, 'catalog')
            subprocess.run(['cc', '-x', 'c', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                            '-I', str(ROOT / 'tests/host'), '-o', str(binary), '-'],
                           input=code, text=True, check=True)
            subprocess.run([str(binary)], check=True)

    def test_pack_rebases_metadata_and_keeps_embedded_text_and_failure_state(self):
        source = (ROOT / 'src/services/solar_os_docs.c').read_text()
        function = production_functions(source, ('docs_manual_index_free', 'docs_manual_index_pack'))
        code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
typedef struct {
    const char *id, *title, *section, *section_title, *summary, *aliases;
    const char *keywords, *body, *contract, *markdown;
} solar_os_manual_page_t;
typedef struct { size_t count, blob_used; char *blob; bool packed; solar_os_manual_page_t pages[]; } docs_manual_index_t;
#define SOLAR_OS_MEMORY_EXTERNAL_RESIDENT 6
static bool fail;
static size_t allocated;
static void *solar_os_memory_alloc(size_t n, unsigned cls, const char *tag)
{ assert(cls == 6 && !strcmp(tag, "docs.metadata")); allocated = n; return fail ? NULL : malloc(n); }
static void solar_os_memory_free(void *p) { free(p); }
''' + function + r'''
int main(void) {
    docs_manual_index_t *index = calloc(1, sizeof(*index) + 2 * sizeof(index->pages[0]));
    index->count = 2; index->blob = malloc(512 * 1024); index->blob_used = 32;
    memcpy(index->blob, "abc\0def\0ghi\0jkl\0mno\0pqr\0stu\0vwx\0", 32);
    const char *body = "embedded body", *markdown = "embedded markdown";
    for (unsigned i = 0; i < 2; i++) {
        solar_os_manual_page_t *p = &index->pages[i];
        *p = (solar_os_manual_page_t){ index->blob, index->blob+4, index->blob+8,
            index->blob+12, index->blob+16, index->blob+20, index->blob+24,
            body, index->blob+28, markdown };
    }
    index->pages[1].keywords = NULL;
    char *original = index->blob; fail = true;
    docs_manual_index_pack(&index);
    assert(index->blob == original && index->pages[0].id == original);
    fail = false; docs_manual_index_pack(&index);
    assert(index->blob != original && allocated == sizeof(*index) + 2 * sizeof(index->pages[0]) + 32 && index->packed);
    for (unsigned i = 0; i < 2; i++) {
        solar_os_manual_page_t *p = &index->pages[i];
        assert(!strcmp(p->id,"abc") && !strcmp(p->title,"def"));
        assert(!strcmp(p->section,"ghi") && !strcmp(p->section_title,"jkl"));
        assert(!strcmp(p->summary,"mno") && !strcmp(p->aliases,"pqr"));
        assert(!strcmp(p->contract,"vwx") && p->body == body && p->markdown == markdown);
        assert(p->keywords == (i ? NULL : index->blob + 24));
    }
    docs_manual_index_free(index);
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp, 'metadata')
            subprocess.run(['cc', '-x', 'c', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                            '-o', str(binary), '-'], input=code, text=True, check=True)
            subprocess.run([str(binary)], check=True)

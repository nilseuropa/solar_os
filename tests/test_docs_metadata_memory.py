from pathlib import Path
import subprocess
import tempfile
import unittest
from test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class DocsMetadataMemoryTest(unittest.TestCase):
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

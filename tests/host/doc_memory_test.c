#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "solar_os_doc.h"
#include "doc_test_support.h"

int main(void)
{
    const char paragraph[] =
        "The reader follows a long story through a small window. "
        "Each paragraph keeps its words and its place in the book.\n\n";
    const size_t repeats = 3000, length = repeats * (sizeof(paragraph) - 1U);
    char *source = malloc(length + 1U);
    assert(source);
    for (size_t i = 0; i < repeats; i++)
        memcpy(source + i * (sizeof(paragraph) - 1U), paragraph, sizeof(paragraph) - 1U);
    source[length] = 0;

    solar_os_doc_t doc;
    solar_os_doc_layout_t layout;
    solar_os_doc_init(&doc);
    solar_os_doc_layout_init(&layout);
    doc_test_memory_budget(4608U * 1024U);
    assert(solar_os_doc_parse_markdown(&doc, source, length, "book.md") == ESP_OK);
    free(source);
    const size_t parsed_bytes = doc_test_memory_used();
    /* Enough total memory, but no contiguous allocation larger than a page. */
    doc_test_memory_largest(8192);
    for (int zoom = 0; zoom <= 4; zoom++) {
        assert(solar_os_doc_layout_build(&layout, &doc, 384, zoom) == ESP_OK);
        assert(layout.line_count > 8000 && layout.run_count > 8000);
        assert(layout.height > INT16_MAX && solar_os_doc_layout_line(&layout, layout.line_count - 1)->y > INT16_MAX);
        int x, y, height;
        size_t offset;
        assert(solar_os_doc_layout_source_to_xy(&layout, 0, &x, &y, &height));
        assert(solar_os_doc_layout_hit_test(&layout, x, y, &offset) && offset == 0);
        const size_t end = doc.blocks[doc.block_count - 1].source_end;
        assert(solar_os_doc_layout_source_to_xy(&layout, end, &x, &y, &height));
        assert(y > 0 && height > 0);
    }
    assert(solar_os_doc_layout_build(&layout, &doc, INT16_MAX + 1, 1) == ESP_ERR_INVALID_ARG);
    const size_t used = doc_test_memory_used();
    doc_test_memory_budget(used);
    assert(solar_os_doc_layout_build(&layout, &doc, 384, 4) == ESP_OK);
    assert(doc_test_memory_used() == used);
    doc_test_memory_budget(4608U * 1024U);

    for (int failure = 0; failure < 2; failure++) {
        doc_test_memory_fail_after(failure);
        assert(solar_os_doc_layout_build(&layout, &doc, 96, 4) == ESP_ERR_NO_MEM);
        assert(!layout.lines.pages && !layout.runs.pages && !layout.line_count && !layout.run_count);
        assert(doc_test_memory_used() == parsed_bytes);
        doc_test_memory_fail_after(-1);
        assert(solar_os_doc_layout_build(&layout, &doc, 384, 1) == ESP_OK);
    }
    printf("book layout: %zu source bytes, %zu peak heap bytes\n", length, doc_test_memory_peak());
    solar_os_doc_layout_free(&layout);
    solar_os_doc_free(&doc);
    assert(doc_test_memory_used() == 0);
    return 0;
}

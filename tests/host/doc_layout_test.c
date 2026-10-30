#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "solar_os_doc.h"

static size_t find_block(const solar_os_doc_t *doc, solar_os_doc_block_type_t type, size_t after)
{
    for (size_t i = after; i < doc->block_count; i++) {
        if (doc->blocks[i].type == type) {
            return i;
        }
    }
    assert(!"block not found");
    return 0;
}

static void test_raw_ranges_and_reader_wrapper(void)
{
    static const char markdown[] =
        "# Heading **bold**\n"
        "\n"
        "- item\n"
        "> quote\n"
        "```c\n"
        "code\n"
        "```\n"
        "| A | B |\n"
        "|---|---|\n"
        "| 1 | 2 |\n"
        "---\n"
        "![alt](image.png)\n"
        "malformed ** marker\n";

    solar_os_doc_t doc;
    solar_os_doc_layout_t layout;
    solar_os_doc_init(&doc);
    solar_os_doc_layout_init(&layout);
    assert(solar_os_doc_parse_markdown(&doc,
                                       markdown,
                                       sizeof(markdown) - 1U,
                                       "test.md") == ESP_OK);

    const size_t heading = find_block(&doc, SOLAR_OS_DOC_BLOCK_HEADING, 0);
    assert(doc.blocks[heading].raw_source_start == 0);
    assert(doc.blocks[heading].source_start == 2);
    assert(doc.blocks[heading].raw_source_end == strlen("# Heading **bold**\n"));

    const size_t pre = find_block(&doc, SOLAR_OS_DOC_BLOCK_PRE, 0);
    assert(strncmp(&markdown[doc.blocks[pre].raw_source_start], "```c", 4) == 0);
    assert(doc.blocks[pre].raw_source_end > doc.blocks[pre].source_end - 1U);

    const size_t table = find_block(&doc, SOLAR_OS_DOC_BLOCK_TABLE_ROW, 0);
    const size_t raw_table_len = doc.blocks[table].raw_source_end -
        doc.blocks[table].raw_source_start;
    assert(memmem(&markdown[doc.blocks[table].raw_source_start],
                  raw_table_len,
                  "|---|---|",
                  9) != NULL);

    assert(solar_os_doc_layout_build(&layout, &doc, 180, 1) == ESP_OK);
    for (size_t i = 0; i < layout.run_count; i++) {
        assert(!solar_os_doc_layout_run(&layout, i)->raw_source);
    }

    solar_os_doc_layout_free(&layout);
    solar_os_doc_free(&doc);
}

static void test_reveal_round_trip(void)
{
    static const char markdown[] =
        "# H **bold**\n"
        "\n"
        "paragraph wraps across the narrow layout\n"
        "1ä12ä123ä1234ä12345ä123456ä1234567ä\n"
        "- item\n"
        "> quote\n"
        "```c\n"
        "code\n"
        "```\n";
    solar_os_doc_t doc;
    solar_os_doc_layout_t layout;
    solar_os_doc_init(&doc);
    solar_os_doc_layout_init(&layout);
    assert(solar_os_doc_parse_markdown(&doc,
                                       markdown,
                                       sizeof(markdown) - 1U,
                                       "test.md") == ESP_OK);

    const solar_os_doc_reveal_range_t reveal = {
        .start = 0,
        .end = sizeof(markdown) - 1U,
    };
    assert(solar_os_doc_layout_build_ex(&layout, &doc, 56, 1, &reveal, 1) == ESP_OK);

    bool saw_heading_marker = false;
    bool saw_list_marker = false;
    bool saw_quote_marker = false;
    bool saw_fence_marker = false;
    int paragraph_first_y = -1;
    int paragraph_last_y = -1;
    const size_t list_offset = (size_t)(strstr(markdown, "- item") - markdown);
    const size_t quote_offset = (size_t)(strstr(markdown, "> quote") - markdown);
    const size_t fence_offset = (size_t)(strstr(markdown, "```c") - markdown);
    const size_t paragraph_offset = (size_t)(strstr(markdown, "paragraph") - markdown);
    const size_t paragraph_end = list_offset;
    for (size_t i = 0; i < layout.run_count; i++) {
        const solar_os_doc_layout_run_t *run = solar_os_doc_layout_run(&layout, i);
        if (!run->raw_source) {
            continue;
        }
        assert(run->text_offset + run->text_len <= sizeof(markdown) - 1U);
        if (run->text_offset + run->text_len < sizeof(markdown) - 1U) {
            const unsigned char next =
                (unsigned char)markdown[run->text_offset + run->text_len];
            assert((next & 0xc0U) != 0x80U);
        }
        if (run->source_start == 0 && run->text_len > 0) {
            saw_heading_marker = markdown[run->text_offset] == '#';
        }
        if (run->source_start == list_offset && run->text_len > 0) {
            saw_list_marker = markdown[run->text_offset] == '-';
        }
        if (run->source_start == quote_offset && run->text_len > 0) {
            saw_quote_marker = markdown[run->text_offset] == '>';
        }
        if (run->source_start == fence_offset && run->text_len > 0) {
            saw_fence_marker = markdown[run->text_offset] == '`';
        }
        if (run->source_start >= paragraph_offset && run->source_start < paragraph_end) {
            if (paragraph_first_y < 0) {
                paragraph_first_y = run->y;
            }
            paragraph_last_y = run->y;
        }
    }
    assert(saw_heading_marker);
    assert(saw_list_marker);
    assert(saw_quote_marker);
    assert(saw_fence_marker);
    assert(paragraph_last_y > paragraph_first_y);

    const size_t offsets[] = {
        0,
        2,
        (size_t)(strstr(markdown, "**bold**") - markdown),
        (size_t)(strstr(markdown, "bold") - markdown) + 2U,
        paragraph_offset - 1U,
        paragraph_offset,
        paragraph_offset + 20U,
        list_offset,
        quote_offset,
        fence_offset,
        (size_t)(strstr(markdown, "code\n") - markdown) + 2U,
        sizeof(markdown) - 2U,
    };
    for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        int x = 0;
        int y = 0;
        assert(solar_os_doc_layout_source_to_xy(&layout, offsets[i], &x, &y, NULL));
        size_t hit = SIZE_MAX;
        assert(solar_os_doc_layout_hit_test(&layout, x, y, &hit));
        if (hit != offsets[i]) {
            fprintf(stderr,
                    "round trip mismatch: source=%zu x=%d y=%d hit=%zu\n",
                    offsets[i],
                    x,
                    y,
                    hit);
        }
        assert(hit == offsets[i]);
    }

    solar_os_doc_layout_free(&layout);
    solar_os_doc_free(&doc);
}

static void test_adjacent_heading_navigation(void)
{
    static const char markdown[] =
        "# Heading\n"
        "## Followed by a smaller title\n";
    solar_os_doc_t doc;
    solar_os_doc_layout_t layout;
    solar_os_doc_init(&doc);
    solar_os_doc_layout_init(&layout);
    assert(solar_os_doc_parse_markdown(&doc,
                                       markdown,
                                       sizeof(markdown) - 1U,
                                       "test.md") == ESP_OK);

    const size_t first = find_block(&doc, SOLAR_OS_DOC_BLOCK_HEADING, 0);
    const size_t second = find_block(&doc, SOLAR_OS_DOC_BLOCK_HEADING, first + 1U);
    const solar_os_doc_reveal_range_t first_reveal = {
        .start = doc.blocks[first].source_start,
        .end = doc.blocks[first].source_start,
    };
    assert(solar_os_doc_layout_build_ex(&layout,
                                        &doc,
                                        180,
                                        1,
                                        &first_reveal,
                                        1) == ESP_OK);

    size_t target = SIZE_MAX;
    assert(solar_os_doc_layout_adjacent_source(&layout,
                                               first_reveal.start,
                                               true,
                                               &target));
    assert(target >= doc.blocks[second].raw_source_start);
    assert(target < doc.blocks[second].raw_source_end);

    const solar_os_doc_reveal_range_t second_reveal = {
        .start = target,
        .end = target,
    };
    assert(solar_os_doc_layout_build_ex(&layout,
                                        &doc,
                                        180,
                                        1,
                                        &second_reveal,
                                        1) == ESP_OK);
    size_t back = SIZE_MAX;
    assert(solar_os_doc_layout_adjacent_source(&layout, target, false, &back));
    assert(back >= doc.blocks[first].raw_source_start);
    assert(back < doc.blocks[first].raw_source_end);

    solar_os_doc_layout_free(&layout);
    solar_os_doc_free(&doc);
}

int main(void)
{
    test_raw_ranges_and_reader_wrapper();
    test_reveal_round_trip();
    test_adjacent_heading_navigation();
    puts("document reveal layout tests: ok");
    return 0;
}

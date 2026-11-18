#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "solar_os_doc.h"
#include "doc_test_support.h"

struct solar_os_gfx {
    char text[128][160];
    int x[128];
    int baseline[128];
    size_t text_count;
    size_t bullets;
};

void solar_os_gfx_text(solar_os_gfx_t *gfx, int x, int baseline, const char *text)
{
    assert(gfx->text_count < 128);
    const size_t i = gfx->text_count++;
    snprintf(gfx->text[i], sizeof(gfx->text[i]), "%s", text);
    gfx->x[i] = x;
    gfx->baseline[i] = baseline;
}

void solar_os_gfx_fill_circle(solar_os_gfx_t *gfx, int x, int y, int radius)
{
    (void)x; (void)y; (void)radius;
    gfx->bullets++;
}

void solar_os_gfx_set_color(solar_os_gfx_t *gfx, solar_os_gfx_color_t color)
{ (void)gfx; (void)color; }
void solar_os_gfx_set_font(solar_os_gfx_t *gfx, solar_os_gfx_font_t font)
{ (void)gfx; (void)font; }
void solar_os_gfx_rect(solar_os_gfx_t *gfx, int x, int y, int width, int height)
{ (void)gfx; (void)x; (void)y; (void)width; (void)height; }
void solar_os_gfx_fill_rect(solar_os_gfx_t *gfx, int x, int y, int width, int height)
{ (void)gfx; (void)x; (void)y; (void)width; (void)height; }
void solar_os_gfx_line(solar_os_gfx_t *gfx, int x0, int y0, int x1, int y1)
{ (void)gfx; (void)x0; (void)y0; (void)x1; (void)y1; }
esp_err_t solar_os_gfx_blit_raster(solar_os_gfx_t *gfx,
                                   const solar_os_gfx_raster_t *raster,
                                   int x, int y, int width, int height,
                                   const solar_os_gfx_clip_t *clip)
{
    (void)gfx; (void)raster; (void)x; (void)y; (void)width; (void)height; (void)clip;
    return ESP_OK;
}

static void render(solar_os_gfx_t *gfx, const solar_os_doc_t *doc,
                    const solar_os_doc_layout_t *layout)
{
    memset(gfx, 0, sizeof(*gfx));
    const solar_os_doc_view_t view = {
        .x = 7, .y = 11, .width = layout->width, .height = layout->height,
        .zoom = layout->zoom,
    };
    solar_os_doc_layout_render(gfx, doc, layout, &view);
}

static size_t text_index(const solar_os_gfx_t *gfx, const char *text)
{
    for (size_t i = 0; i < gfx->text_count; i++) {
        if (strcmp(gfx->text[i], text) == 0) {
            return i;
        }
    }
    fprintf(stderr, "missing rendered text: %s\n", text);
    assert(!"rendered text not found");
    return 0;
}

static void test_numbered_lists(void)
{
    /* Both hand-written Markdown and Writer's F1 => 7 use this syntax. */
    static const char markdown[] = "1. First\n1. Second\n";
    solar_os_doc_t doc;
    solar_os_doc_layout_t layout;
    solar_os_gfx_t gfx;
    solar_os_doc_init(&doc);
    solar_os_doc_layout_init(&layout);
    assert(solar_os_doc_parse_markdown(&doc, markdown, strlen(markdown), "list.md") == ESP_OK);
    for (int zoom = 0; zoom <= 4; zoom++) {
        assert(solar_os_doc_layout_build(&layout, &doc, 384, zoom) == ESP_OK);
        render(&gfx, &doc, &layout);
        assert(text_index(&gfx, "1.") < text_index(&gfx, "First"));
        assert(text_index(&gfx, "2.") < text_index(&gfx, "Second"));
        assert(gfx.bullets == 0);
    }

    /* Moving the cursor into one item must not renumber the preview of the next. */
    const solar_os_doc_reveal_range_t first = {.start = 0, .end = strlen("1. First")};
    assert(solar_os_doc_layout_build_ex(&layout, &doc, 384, 1, &first, 1) == ESP_OK);
    render(&gfx, &doc, &layout);
    text_index(&gfx, "1. First");
    text_index(&gfx, "2.");
    text_index(&gfx, "Second");
    assert(gfx.bullets == 0);

    /* Writer must still reveal the original repeated 1. markers for editing. */
    const solar_os_doc_reveal_range_t reveal = {.start = 0, .end = strlen(markdown)};
    assert(solar_os_doc_layout_build_ex(&layout, &doc, 384, 1, &reveal, 1) == ESP_OK);
    render(&gfx, &doc, &layout);
    text_index(&gfx, "1. First");
    text_index(&gfx, "1. Second");
    assert(gfx.bullets == 0);
    assert(strcmp(doc.source, markdown) == 0);

    char path[] = "/tmp/solar-os-doc-list-XXXXXX";
    const int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    assert(solar_os_doc_write_markdown(&doc, path) == ESP_OK);
    assert(solar_os_doc_load_path(&doc, path) == ESP_OK);
    assert(solar_os_doc_layout_build(&layout, &doc, 384, 1) == ESP_OK);
    render(&gfx, &doc, &layout);
    text_index(&gfx, "1.");
    text_index(&gfx, "2.");
    assert(gfx.bullets == 0);
    unlink(path);
    solar_os_doc_layout_free(&layout);
    solar_os_doc_free(&doc);
    assert(doc_test_memory_used() == 0);
}

static void test_list_boundaries_and_wrapping(void)
{
    static const char markdown[] =
        "9. A long item that wraps across several lines of the narrow view\n"
        "1. Next\n"
        "\n"
        "1. Still the same list\n"
        "- Bullet\n"
        "+ Bullet\n"
        "* Bullet\n"
        "4) Another\n"
        "99) Following\n"
        "Paragraph\n"
        "0. Zero\n"
        "1. One\n"
        "# Heading\n"
        "999999999. Last\n"
        "1. Beyond\n";
    static const char *const labels[] = {"9.", "10.", "11.", "4)", "5)", "0.", "1.",
                                         "999999999.", "1000000000."};
    solar_os_doc_t doc;
    solar_os_doc_layout_t layout;
    solar_os_gfx_t gfx;
    solar_os_doc_init(&doc);
    solar_os_doc_layout_init(&layout);
    assert(solar_os_doc_parse_markdown(&doc, markdown, strlen(markdown), "list.md") == ESP_OK);
    assert(solar_os_doc_layout_build(&layout, &doc, 180, 1) == ESP_OK);
    render(&gfx, &doc, &layout);
    for (size_t i = 0; i < sizeof(labels) / sizeof(labels[0]); i++) {
        const size_t index = text_index(&gfx, labels[i]);
        assert(gfx.x[index] == 7);
    }
    assert(gfx.bullets == 3);
    size_t first_item_lines = 0;
    int text_x = -1;
    for (size_t i = 0; i < layout.run_count; i++) {
        const solar_os_doc_layout_run_t *run = solar_os_doc_layout_run(&layout, i);
        if (run->block_index != 0 || run->text_offset == SIZE_MAX) {
            continue;
        }
        if (text_x < 0) text_x = run->x;
        assert(run->x == text_x);
        assert(run->x >= 3 * run->char_w);
        assert(run->x + run->width <= layout.width);
        size_t source;
        assert(solar_os_doc_layout_hit_test(&layout, run->x, run->y, &source));
        assert(source == run->source_start);
        first_item_lines++;
    }
    assert(first_item_lines > 1);
    /* Each marker is decoration on the first line only. */
    size_t first_label_count = 0;
    for (size_t i = 0; i < gfx.text_count; i++) {
        first_label_count += strcmp(gfx.text[i], "9.") == 0;
    }
    assert(first_label_count == 1);
    const size_t last_label = text_index(&gfx, "1000000000.");
    const size_t last_text = text_index(&gfx, "Beyond");
    const solar_os_doc_layout_run_t *last_run = solar_os_doc_layout_run(&layout, layout.run_count - 1U);
    assert(gfx.x[last_text] >= gfx.x[last_label] + 12 * last_run->char_w);
    assert(gfx.baseline[last_text] == gfx.baseline[last_label]);
    solar_os_doc_layout_free(&layout);
    solar_os_doc_free(&doc);
    assert(doc_test_memory_used() == 0);
}

int main(void)
{
    test_numbered_lists();
    test_list_boundaries_and_wrapping();
    puts("document list rendering tests: ok");
    return 0;
}

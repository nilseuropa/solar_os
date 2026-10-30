#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "solar_os_doc.h"
#include "solar_os_memory.h"
#include "doc_test_support.h"

typedef struct { bool graphics; } solar_os_context_t;
typedef struct {
    solar_os_doc_t doc;
    solar_os_doc_layout_t layout;
    solar_os_memory_reclaimer_t layout_reclaimer;
    bool layout_valid, suspended, suspended_anchor_valid;
    size_t suspended_anchor;
    int content_height;
    void *epub_book;
} reader_state_t;
static reader_state_t reader;
static void *reader_state = &reader;
static solar_os_memory_reclaimer_t *registered;
static unsigned waits, reader_renders, reader_restores, reader_saves, writer_renders;
static bool registration_fail, join_pending;
static size_t position = 32, disk_position;

bool solar_os_memory_register_reclaimer(solar_os_memory_reclaimer_t *entry)
{
    assert(!registered && entry->reclaim);
    if (registration_fail) return false;
    registered = entry;
    return true;
}
bool solar_os_memory_unregister_reclaimer(solar_os_memory_reclaimer_t *entry)
{
    if (join_pending) { join_pending = false; return false; }
    assert(!registered || registered == entry);
    registered = NULL;
    return true;
}
static void vTaskDelay(unsigned ticks) { assert(ticks == 1); waits++; }
static void solar_os_context_set_graphics_active(solar_os_context_t *ctx, bool active)
{ ctx->graphics = active; }
static size_t reader_anchor_offset_for_scroll(void) { return position; }
static void reader_save_position(solar_os_context_t *ctx)
{
    (void)ctx;
    disk_position = reader.suspended && !reader.layout_valid ? reader.suspended_anchor : position;
    reader_saves++;
}
static void reader_scroll_to_anchor(solar_os_context_t *ctx, uint64_t anchor)
{
    assert(ctx->graphics && !registered && anchor == position);
    if (!reader.layout_valid) {
        assert(solar_os_doc_layout_build(&reader.layout, &reader.doc, 280, 1) == ESP_OK);
        reader.layout_valid = true;
    }
    reader_restores++;
}
static void reader_render(solar_os_context_t *ctx)
{ assert(ctx->graphics && !registered && reader.layout_valid); reader_renders++; }
static void solar_os_epub_close(void *book) { assert(!book); }
#include "reader_cache.inc"

typedef struct {
    solar_os_doc_t doc;
    solar_os_doc_layout_t layout;
    solar_os_memory_reclaimer_t layout_reclaimer;
    bool suspended, layout_pending, render_pending, dirty, discard_on_stop;
    int content_height;
    size_t cursor, saved_scroll_anchor;
    char buffer[96];
} writer_state_t;
static writer_state_t writer;
static void *writer_state = &writer;
static unsigned recoveries;
static esp_err_t writer_write_recovery(void) { recoveries++; return ESP_OK; }
static void writer_save_metadata(void) { assert(writer.saved_scroll_anchor == SIZE_MAX || writer.saved_scroll_anchor == position); }
static size_t writer_scroll_anchor(void) { return position; }
static void writer_wake_cursor(void) {}
static esp_err_t writer_update_document(solar_os_context_t *ctx)
{
    assert(ctx->graphics && !registered && writer.layout_pending);
    assert(writer.saved_scroll_anchor == position && writer.cursor == 48 && writer.dirty);
    assert(!strcmp(writer.buffer, "Unsaved editor text survives cache eviction."));
    esp_err_t err = solar_os_doc_layout_build(&writer.layout, &writer.doc, 280, 1);
    writer.layout_pending = false;
    writer.saved_scroll_anchor = SIZE_MAX;
    return err;
}
static void writer_render(solar_os_context_t *ctx)
{ assert(ctx->graphics && !registered && writer.layout.line_count); writer_renders++; }
static void solar_os_writer_buffer_free(void *buffer) { (void)buffer; }
#include "writer_cache.inc"

static void load(solar_os_doc_t *doc, solar_os_doc_layout_t *layout)
{
    const char *text = "# Chapter\n\nA document keeps its original text and its reading position.\n";
    solar_os_doc_init(doc); solar_os_doc_layout_init(layout);
    assert(solar_os_doc_parse_markdown(doc, text, strlen(text), "test.md") == ESP_OK);
    assert(solar_os_doc_layout_build(layout, doc, 280, 1) == ESP_OK);
}
int main(void)
{
    solar_os_context_t ctx = {.graphics = true};
    load(&reader.doc, &reader.layout); reader.layout_valid = true;
    reader.layout_reclaimer = (solar_os_memory_reclaimer_t){
        .owner = "reader.layout", .reclaim = reader_reclaim_layout, .user = &reader};
    const char *source = reader.doc.source;
    for (unsigned i = 0; i < 2; ++i) {
        reader_suspend(&ctx);
        assert(registered && reader.suspended && !ctx.graphics);
        size_t before = doc_test_memory_used();
        size_t released = registered->reclaim(NULL, registered->user);
        assert(released && doc_test_memory_used() == before - released);
        assert(!reader.layout_valid && !reader.layout.line_count && reader.doc.source == source);
        assert(reader.suspended_anchor == position && reader.suspended_anchor_valid);
        join_pending = true;
        reader_resume(&ctx);
        assert(reader.layout_valid && !reader.suspended && !reader.suspended_anchor_valid);
    }
    assert(waits == 2 && reader_restores == 2 && reader_renders == 2);
    registration_fail = true;
    reader_suspend(&ctx);
    assert(!registered && !reader.layout_valid); /* Safe eager drop if registry is full. */
    reader_resume(&ctx);
    registration_fail = false;
    reader_suspend(&ctx); registered->reclaim(NULL, registered->user);
    reader_stop(&ctx);
    assert(!registered && disk_position == position && !doc_test_memory_used());

    load(&writer.doc, &writer.layout);
    strcpy(writer.buffer, "Unsaved editor text survives cache eviction.");
    writer.cursor = 48; writer.dirty = true; writer.saved_scroll_anchor = SIZE_MAX;
    writer.layout_reclaimer = (solar_os_memory_reclaimer_t){
        .owner = "writer.layout", .reclaim = writer_reclaim_layout, .user = &writer};
    writer_suspend(&ctx);
    size_t before = doc_test_memory_used();
    size_t released = registered->reclaim(NULL, registered->user);
    assert(released && doc_test_memory_used() == before - released);
    assert(writer.dirty && writer.cursor == 48 && writer.saved_scroll_anchor == position);
    join_pending = true;
    writer_resume(&ctx);
    assert(writer_renders == 1 && waits == 3 && recoveries == 1);
    writer_stop(&ctx);
    assert(!registered && !doc_test_memory_used());
    load(&writer.doc, &writer.layout);
    writer.dirty = true; writer.saved_scroll_anchor = SIZE_MAX;
    writer.layout_reclaimer = (solar_os_memory_reclaimer_t){
        .owner = "writer.layout", .reclaim = writer_reclaim_layout, .user = &writer};
    writer_suspend(&ctx); registered->reclaim(NULL, registered->user);
    join_pending = true;
    writer_stop(&ctx); /* Closing a reclaimed dirty editor must also join. */
    assert(!registered && !doc_test_memory_used() && waits == 4);
    puts("document caches: quiescent reclaim, join before resume/close, anchors and unsaved edits passed");
}

/* Read the actual legacy Inbox format without changing its file or live state. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
static int xPortInIsrContext(void) { return 0; }
#include "../../src/services/solar_os_inbox.c"

int main(void)
{
    char path[] = "/tmp/solaros-inbox-history-XXXXXX";
    int fd = mkstemp(path); assert(fd >= 0);
    FILE *file = fdopen(fd, "w+b"); assert(file);
    inbox_capacity = SOLAR_OS_INBOX_CAPACITY;
    inbox_ring = calloc(inbox_capacity, sizeof(*inbox_ring)); assert(inbox_ring);
    inbox_head = 1; inbox_count = 2; inbox_next_id = 3;
    inbox_ring[63].entry.id = 1; inbox_ring[0].entry.id = 2;
    strcpy(inbox_ring[63].entry.body, "oldest"); strcpy(inbox_ring[0].entry.body, "newest");
    inbox_store_header_t header;
    inbox_make_header(&header, 7);
    assert(fwrite(&header, sizeof(header), 1, file) == 1);
    assert(fwrite(&header, sizeof(header), 1, file) == 1);
    for (size_t i = 0; i < inbox_capacity; i++) {
        inbox_store_record_t record; inbox_make_record(&record, i);
        assert(fwrite(&record, sizeof(record), 1, file) == 1);
    }
    assert(fclose(file) == 0);
    solar_os_inbox_entry_t entries[2]; size_t count;
    assert(solar_os_inbox_saved_snapshot(path, entries, 2, &count) == ESP_OK && count == 2);
    assert(entries[0].id == 1 && !strcmp(entries[0].body, "oldest"));
    assert(entries[1].id == 2 && !strcmp(entries[1].body, "newest"));
    assert(solar_os_inbox_saved_snapshot(path, entries, 1, &count) == ESP_ERR_INVALID_SIZE);
    struct stat before, after; assert(stat(path, &before) == 0);
    file = fopen(path, "r+b"); assert(file);
    assert(fseek(file, INBOX_STORE_RECORDS_OFFSET, SEEK_SET) == 0 && fputc(0, file) != EOF);
    assert(fclose(file) == 0);
    assert(solar_os_inbox_saved_snapshot(path, entries, 2, &count) == ESP_FAIL);
    assert(stat(path, &after) == 0 && before.st_size == after.st_size);
    assert(inbox_count == 2 && inbox_head == 1 && inbox_ring[0].entry.id == 2);
    assert(unlink(path) == 0); free(inbox_ring);
    puts("saved Inbox history tests: ok");
    return 0;
}

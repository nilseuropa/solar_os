/*
 * Derived from esp_picorsrc.c in DiUS esp-picotts.
 * Copyright (C) 2024 DiUS Computing Pty Ltd.
 * Licensed under the Apache License, Version 2.0.
 */

#include "picotts_resource.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "picoapi.h"
#include "picoapid.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wunused-function"
/* Pico exposes no public zero-copy resource loader. */
#include "picorsrc.c"
#pragma GCC diagnostic pop

#define SVOX_MARKER_SIZE 13U
#define HEADER_LENGTH_OFFSET SVOX_MARKER_SIZE
#define RESOURCE_HEADER_OFFSET (HEADER_LENGTH_OFFSET + 2U)

static uint16_t load_u16_le(const uint8_t *raw)
{
    return (uint16_t)raw[0] | ((uint16_t)raw[1] << 8U);
}

static uint32_t load_u32_le(const uint8_t *raw)
{
    return (uint32_t)raw[0] |
           ((uint32_t)raw[1] << 8U) |
           ((uint32_t)raw[2] << 16U) |
           ((uint32_t)raw[3] << 24U);
}

static bool svox_marker_valid(const uint8_t *raw)
{
    static const char marker[] = " (C) SVOX AG ";
    for (size_t i = 0; i < sizeof(marker) - 1U; i++) {
        if (raw[i] != (uint8_t)(marker[i] - 0x20)) {
            return false;
        }
    }
    return true;
}

static pico_status_t resource_corrupt(picorsrc_ResourceManager manager)
{
    return picoos_emRaiseException(manager->common->em,
                                   PICO_EXC_FILE_CORRUPT,
                                   NULL,
                                   NULL);
}

pico_status_t solar_os_pico_load_resource(pico_System system,
                                           const void *raw_data,
                                           size_t raw_size,
                                           pico_Resource *out_resource)
{
    if (system == NULL || raw_data == NULL || out_resource == NULL) {
        return PICO_ERR_NULLPTR_ACCESS;
    }

    picorsrc_Resource *resource_handle =
        (picorsrc_Resource *)out_resource;
    picorsrc_ResourceManager manager = system->rm;
    const uint8_t *raw = raw_data;
    if (raw_size < RESOURCE_HEADER_OFFSET + 4U ||
        !svox_marker_valid(raw)) {
        return resource_corrupt(manager);
    }

    const size_t header_size = load_u16_le(raw + HEADER_LENGTH_OFFSET);
    if (header_size == 0U ||
        header_size > PICOOS_MAX_HEADER_STRING_LEN ||
        header_size > raw_size - RESOURCE_HEADER_OFFSET - 4U) {
        return resource_corrupt(manager);
    }
    const size_t payload_length_offset = RESOURCE_HEADER_OFFSET + header_size;
    const uint32_t payload_size = load_u32_le(raw + payload_length_offset);
    const size_t payload_offset = payload_length_offset + 4U;
    if ((size_t)payload_size > raw_size - payload_offset) {
        return resource_corrupt(manager);
    }

    picorsrc_Resource resource = picorsrc_newResource(manager->common->mm);
    if (resource == NULL) {
        return picoos_emRaiseException(manager->common->em,
                                       PICO_EXC_OUT_OF_MEM,
                                       NULL,
                                       NULL);
    }
    if (manager->numResources >= PICO_MAX_NUM_RESOURCES) {
        picoos_deallocate(manager->common->mm, (void *)&resource);
        return picoos_emRaiseException(manager->common->em,
                                       PICO_EXC_MAX_NUM_EXCEED,
                                       NULL,
                                       (picoos_char *)
                                           "no more than %i resources",
                                       PICO_MAX_NUM_RESOURCES);
    }

    picoos_char header_data[PICOOS_MAX_HEADER_STRING_LEN + 1U];
    memcpy(header_data, raw + RESOURCE_HEADER_OFFSET, header_size);
    header_data[header_size] = '\0';

    picoos_file_header_t header;
    pico_status_t status = picoos_hdrParseHeader(&header, header_data);

    if (status == PICO_OK &&
        isResourceLoaded(manager, header.field[PICOOS_HEADER_NAME].value)) {
        status = PICO_WARN_RESOURCE_DOUBLE_LOAD;
    }

    if (status == PICO_OK) {
        /* The PSRAM-backed file buffer stays alive until engine shutdown. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdiscarded-qualifiers"
        resource->raw_mem = raw + payload_offset;
#pragma GCC diagnostic pop
        resource->start = resource->raw_mem;

        const int copied = picoos_strlcpy(
            resource->name,
            header.field[PICOOS_HEADER_NAME].value,
            PICORSRC_MAX_RSRC_NAME_SIZ);
        if (copied > PICORSRC_MAX_RSRC_NAME_SIZ) {
            status = PICO_ERR_INDEX_OUT_OF_RANGE;
            picoos_emRaiseException(manager->common->em,
                                     PICO_ERR_INDEX_OUT_OF_RANGE,
                                     NULL,
                                     (picoos_char *)"resource %s",
                                     resource->name);
        }
    }

    if (status == PICO_OK) {
        const uint8_t *content_type =
            header.field[PICOOS_HEADER_CONTENT_TYPE].value;
        if (picoos_strcmp(content_type, PICORSRC_FIELD_VALUE_TEXTANA) == 0) {
            resource->type = PICORSRC_TYPE_TEXTANA;
        } else if (picoos_strcmp(content_type,
                                 PICORSRC_FIELD_VALUE_SIGGEN) == 0) {
            resource->type = PICORSRC_TYPE_SIGGEN;
        } else {
            resource->type = PICORSRC_TYPE_OTHER;
        }
        status = picorsrc_getKbList(manager,
                                    resource->start,
                                    payload_size,
                                    &resource->kbList);
    }

    if (status == PICO_OK) {
        resource->next = manager->resources;
        manager->resources = resource;
        manager->numResources++;
        *resource_handle = resource;
    } else {
        picorsrc_disposeResource(manager->common->mm, &resource);
    }
    return status;
}

pico_status_t solar_os_pico_unload_resource(pico_System system,
                                             pico_Resource *in_resource)
{
    if (system == NULL || in_resource == NULL) {
        return PICO_ERR_NULLPTR_ACCESS;
    }

    picorsrc_Resource *resource_handle =
        (picorsrc_Resource *)in_resource;
    picorsrc_ResourceManager manager = system->rm;
    picorsrc_Resource resource = *resource_handle;
    if (resource == NULL) {
        return PICO_ERR_NULLPTR_ACCESS;
    }
    if (resource->lockCount > 0) {
        return PICO_EXC_RESOURCE_BUSY;
    }

    picorsrc_Resource previous = NULL;
    picorsrc_Resource current = manager->resources;
    while (current != NULL && current != resource) {
        previous = current;
        current = current->next;
    }
    if (current == NULL) {
        return PICO_ERR_OTHER;
    }
    if (previous == NULL) {
        manager->resources = resource->next;
    } else {
        previous->next = resource->next;
    }

    if (resource->kbList != NULL) {
        picorsrc_releaseKbList(manager, &resource->kbList);
    }
    picoos_deallocate(manager->common->mm, (void **)resource_handle);
    manager->numResources--;
    return PICO_OK;
}

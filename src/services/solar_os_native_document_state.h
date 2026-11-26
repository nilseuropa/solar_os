#pragma once

#include "solar_os_doc.h"
#include "solar_os_native_handles.h"

typedef struct {
    solar_os_doc_t document;
    uint64_t generation;
} solar_os_native_document_state_t;

static inline solar_os_native_document_state_t *solar_os_native_document_acquire(solar_os_native_handle_t id)
{
    return solar_os_native_handle_acquire(id, SOLAR_OS_NATIVE_HANDLE_DOCUMENT);
}

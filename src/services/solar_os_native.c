#include "solar_os_native.h"
#include "solar_os_native_image.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#include <errno.h>
#include "esp_elf.h"
#include "solar_os_memory.h"
#include "solar_os_storage.h"

static atomic_bool native_symbols_registered;
static const struct esp_elfsym native_symbols[] = {
    ESP_ELFSYM_EXPORT(solar_os_native_host_v1),
    ESP_ELFSYM_EXPORT(solar_os_native_job_host_v1),
    ESP_ELFSYM_EXPORT(solar_os_native_driver_host_v1),
    ESP_ELFSYM_EXPORT(solar_os_native_library_host_v1),
    /* Compilers emit these standard calls even for struct assignment and
     * simple loops. Keep the native substrate independent of broad SDK exports. */
    ESP_ELFSYM_EXPORT(memcpy),
    ESP_ELFSYM_EXPORT(memset),
    ESP_ELFSYM_EXPORT(memmove),
    ESP_ELFSYM_EXPORT(memcmp),
    ESP_ELFSYM_EXPORT(strlen),
    ESP_ELFSYM_EXPORT(setjmp),
    ESP_ELFSYM_EXPORT(longjmp),
    ESP_ELFSYM_END,
};

static void native_set_detail(char *detail, size_t detail_len, const char *text)
{
    if (detail != NULL && detail_len > 0U) {
        snprintf(detail, detail_len, "%s", text != NULL ? text : "native module failed");
    }
}

static esp_err_t native_register_symbols(char *detail, size_t detail_len)
{
    if (atomic_load(&native_symbols_registered)) {
        return ESP_OK;
    }
    const int ret = esp_elf_register_symbol(native_symbols);
    if (ret != 0) {
        native_set_detail(detail, detail_len, "could not register the SolarOS native ABI");
        return ESP_FAIL;
    }
    atomic_store(&native_symbols_registered, true);
    return ESP_OK;
}

esp_err_t solar_os_native_image_load(const char *path, void **result,
    char *detail, size_t detail_len)
{
    if (!result) return ESP_ERR_INVALID_ARG;
    *result = NULL;
    solar_os_storage_metadata_t metadata;
    esp_err_t err = solar_os_storage_stat(path, &metadata);
    if (err != ESP_OK || metadata.type != SOLAR_OS_STORAGE_ENTRY_FILE) {
        native_set_detail(detail, detail_len, "ELF file was not found");
        return ESP_ERR_NOT_FOUND;
    }
    if (metadata.size_bytes < 52U ||
        metadata.size_bytes > SOLAR_OS_NATIVE_ELF_MAX_BYTES ||
        metadata.size_bytes > SIZE_MAX) {
        native_set_detail(detail, detail_len, "ELF file size is outside the supported range");
        return ESP_ERR_INVALID_SIZE;
    }

    esp_elf_t *elf = NULL;
    bool initialized = false;
    solar_os_native_elf_info_t info;
    const size_t file_size = (size_t)metadata.size_bytes;
    uint8_t *data = solar_os_memory_alloc(file_size,
                                          SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                                          "native.resident.elf");
    if (data == NULL) {
        native_set_detail(detail, detail_len, "not enough external memory for the ELF file");
        return ESP_ERR_NO_MEM;
    }
    size_t read_len = 0U;
    err = solar_os_storage_read_file(path, data, file_size, &read_len);
    if (err != ESP_OK || read_len != file_size) {
        native_set_detail(detail, detail_len, "could not read the complete ELF file");
        err = ESP_FAIL;
        goto done;
    }
    err = solar_os_native_elf_validate(data,
                                       file_size,
                                       SOLAR_OS_NATIVE_ELF_MACHINE_XTENSA,
                                       &info,
                                       detail,
                                       detail_len);
    if (err != ESP_OK) {
        goto done;
    }
    err = native_register_symbols(detail, detail_len);
    if (err != ESP_OK) {
        goto done;
    }
    elf = solar_os_memory_calloc(1, sizeof(*elf), SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "native.resident.image");
    if (!elf) {
        err = ESP_ERR_NO_MEM;
        goto done;
    }
    if (esp_elf_init(elf) != 0) {
        native_set_detail(detail, detail_len, "Espressif ELF loader initialization failed");
        err = ESP_FAIL;
        goto done;
    }
    initialized = true;
    const int relocation = esp_elf_relocate(elf, data);
    if (relocation != 0) {
        native_set_detail(detail, detail_len, relocation == -ENOMEM ?
            "not enough memory for ELF code and data" :
            "ELF relocation or symbol resolution failed");
        err = relocation == -ENOMEM ? ESP_ERR_NO_MEM : ESP_ERR_INVALID_RESPONSE;
        goto done;
    }

    *result = elf;
    elf = NULL;
    err = ESP_OK;

done:
    solar_os_memory_free(data);
    if (elf) {
        if (initialized) esp_elf_deinit(elf);
        solar_os_memory_free(elf);
    }
    return err;
}

int solar_os_native_image_request(void *image, int argc, char **argv)
{
    return esp_elf_request(image, 0, argc, argv);
}

void solar_os_native_image_close(void *image)
{
    if (!image) return;
    esp_elf_deinit(image);
    solar_os_memory_free(image);
}

esp_err_t solar_os_native_run(const char *path,
                              int argc,
                              char **argv,
                              const solar_os_native_run_options_t *options,
                              solar_os_native_run_result_t *result,
                              char *detail,
                              size_t detail_len)
{
    if (detail != NULL && detail_len > 0U) {
        detail[0] = '\0';
    }
    if (path == NULL || path[0] == '\0' || argc < 1 || argv == NULL ||
        options == NULL || options->write == NULL) {
        native_set_detail(detail, detail_len, "missing path, arguments, or output callback");
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t admission = solar_os_native_operation_begin();
    if (admission != ESP_OK) {
        native_set_detail(detail, detail_len, admission == ESP_ERR_NO_MEM ?
            "could not register module memory reclamation" : "another native module is already running");
        return admission;
    }

    esp_err_t err = ESP_OK;
    bool operation_locked = true;
    bool scope_linked = false;
    native_run_scope_t scope = {
        .options = *options,
    };
    uint8_t *data = NULL;
    esp_elf_t elf;
    bool elf_initialized = false;
    solar_os_storage_metadata_t metadata;
    solar_os_native_elf_info_t elf_info;

    err = solar_os_storage_stat(path, &metadata);
    if (err != ESP_OK || metadata.type != SOLAR_OS_STORAGE_ENTRY_FILE) {
        native_set_detail(detail, detail_len, "ELF file was not found");
        err = ESP_ERR_NOT_FOUND;
        goto done;
    }
    if (metadata.size_bytes < 52U ||
        metadata.size_bytes > SOLAR_OS_NATIVE_ELF_MAX_BYTES ||
        metadata.size_bytes > SIZE_MAX) {
        native_set_detail(detail, detail_len, "ELF file size is outside the supported range");
        err = ESP_ERR_INVALID_SIZE;
        goto done;
    }

    const size_t file_size = (size_t)metadata.size_bytes;
    data = solar_os_memory_alloc(file_size,
                                 SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                                 "native.elf");
    if (data == NULL) {
        native_set_detail(detail, detail_len, "not enough external memory for the ELF file");
        err = ESP_ERR_NO_MEM;
        goto done;
    }

    size_t read_len = 0U;
    err = solar_os_storage_read_file(path, data, file_size, &read_len);
    if (err != ESP_OK || read_len != file_size) {
        native_set_detail(detail, detail_len, "could not read the complete ELF file");
        err = ESP_FAIL;
        goto done;
    }

    err = solar_os_native_elf_validate(data,
                                       file_size,
                                       SOLAR_OS_NATIVE_ELF_MACHINE_XTENSA,
                                       &elf_info,
                                       detail,
                                       detail_len);
    if (err != ESP_OK) {
        goto done;
    }
    if ((elf_info.foreground && options->expected_manifest == NULL) ||
        (options->expected_manifest != NULL &&
         (!elf_info.foreground || memcmp(&elf_info.app, options->expected_manifest,
                                         sizeof(elf_info.app)) != 0))) {
        native_set_detail(detail, detail_len, "foreground app requires its admitted session manifest");
        err = ESP_ERR_INVALID_STATE;
        goto done;
    }
    err = native_register_symbols(detail, detail_len);
    if (err != ESP_OK) {
        goto done;
    }

    int loader_ret = esp_elf_init(&elf);
    if (loader_ret != 0) {
        native_set_detail(detail, detail_len, "Espressif ELF loader initialization failed");
        err = ESP_FAIL;
        goto done;
    }
    elf_initialized = true;

    loader_ret = esp_elf_relocate(&elf, data);
    if (loader_ret != 0) {
        native_set_detail(detail, detail_len, loader_ret == -ENOMEM ?
            "not enough memory for ELF code and data" :
            "ELF relocation or symbol resolution failed");
        err = loader_ret == -ENOMEM ? ESP_ERR_NO_MEM : ESP_ERR_INVALID_RESPONSE;
        goto done;
    }

    /* Relocation and symbol registration are serialized. Module execution
     * can remain resident while another task loads a different application. */
    solar_os_memory_free(data);
    data = NULL;
    solar_os_system_scope_enter(&scope);
    scope_linked = true;
    solar_os_native_operation_end();
    operation_locked = false;
    loader_ret = esp_elf_request(&elf, 0, argc, argv);
    if (loader_ret != 0) {
        native_set_detail(detail, detail_len, "ELF entry point failed to run");
        err = ESP_FAIL;
        goto done;
    }

    if (result != NULL) {
        *result = (solar_os_native_run_result_t){.elf = elf_info};
    }

done:
    if (options->cleanup != NULL) options->cleanup(options->service_user);
    if (scope_linked) solar_os_system_scope_leave(&scope);
    if (elf_initialized) {
        esp_elf_deinit(&elf);
    }
    solar_os_memory_free(data);
    if (operation_locked) {
        solar_os_native_operation_end();
    }
    return err;
}

#include "solar_os_espdl_backend.h"
#include "solar_os_espdl_validate.h"
extern "C" {
#include "solar_os_memory.h"
}
#include "dl_model_base.hpp"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <vector>

/* All backend operations, including destruction, use service admission. */
static size_t resident_backends;
struct solar_os_inference_backend {
    solar_os_inference_backend() { ++resident_backends; }
    void *allocation = nullptr;
    uint8_t *buffer = nullptr;
    std::unique_ptr<fbs::FbsModel> fbs;
    std::unique_ptr<dl::Model> model;
    size_t layers = 0;
    struct workspace_binding { dl::TensorBase *tensor; size_t offset; };
    std::vector<workspace_binding> workspace_bindings;
    size_t workspace_bytes = 0;
    void release_workspace()
    {
        if (!workspace_bytes) return;
        for (auto &binding : workspace_bindings) binding.tensor->data = nullptr;
        model->module_context()->root_free();
    }
    bool acquire_workspace()
    {
        if (!workspace_bytes) return true;
        auto *context = model->module_context();
        if (!context->root_alloc(0, workspace_bytes)) return false;
        auto *root = static_cast<uint8_t *>(context->get_psram_root());
        for (auto &binding : workspace_bindings) binding.tensor->data = root + binding.offset;
        return true;
    }
    void capture_workspace()
    {
        auto *context = model->module_context();
        dl::mem_info_t memory = {};
        context->get_variable_memory_size(memory);
        if (memory.internal || !memory.psram) return;
        const uintptr_t root = reinterpret_cast<uintptr_t>(context->get_psram_root());
        /* Parameters are persistent. If a future memory planner places them
         * in the root too, retain that root instead of discarding weights. */
        for (auto *tensor : context->m_parameters) {
            if (!tensor) continue;
            const uintptr_t data = reinterpret_cast<uintptr_t>(tensor->data);
            if (data >= root && data - root < memory.psram) return;
        }
        auto capture = [&](const std::vector<dl::TensorBase *> &tensors) {
            for (auto *tensor : tensors) {
                if (!tensor) continue;
                const uintptr_t data = reinterpret_cast<uintptr_t>(tensor->data);
                if (data >= root && data - root < memory.psram)
                    workspace_bindings.push_back({tensor, data - root});
            }
        };
        capture(context->m_variables);
        workspace_bytes = memory.psram;
        release_workspace();
    }
    dl::runtime_mode_t mode = dl::RUNTIME_MODE_SINGLE_CORE;
    ~solar_os_inference_backend()
    {
        model.reset(); fbs.reset(); solar_os_memory_free(allocation);
        if (--resident_backends == 0) dl::module::module_workers_release();
    }
};
static esp_err_t dtype(dl::dtype_t type, solar_os_tensor_dtype_t &out)
{
    switch (type) {
    case dl::DATA_TYPE_INT8: out = SOLAR_OS_TENSOR_INT8; break;
    case dl::DATA_TYPE_UINT8: out = SOLAR_OS_TENSOR_UINT8; break;
    case dl::DATA_TYPE_INT16: out = SOLAR_OS_TENSOR_INT16; break;
    case dl::DATA_TYPE_UINT16: out = SOLAR_OS_TENSOR_UINT16; break;
    case dl::DATA_TYPE_INT32: out = SOLAR_OS_TENSOR_INT32; break;
    case dl::DATA_TYPE_UINT32: out = SOLAR_OS_TENSOR_UINT32; break;
    case dl::DATA_TYPE_INT64: out = SOLAR_OS_TENSOR_INT64; break;
    case dl::DATA_TYPE_UINT64: out = SOLAR_OS_TENSOR_UINT64; break;
    case dl::DATA_TYPE_FLOAT: out = SOLAR_OS_TENSOR_FLOAT32; break;
    case dl::DATA_TYPE_DOUBLE: out = SOLAR_OS_TENSOR_FLOAT64; break;
    case dl::DATA_TYPE_FLOAT16: out = SOLAR_OS_TENSOR_FLOAT16; break;
    case dl::DATA_TYPE_BOOL: out = SOLAR_OS_TENSOR_BOOL; break;
    default: return ESP_ERR_NOT_SUPPORTED;
    }
    return ESP_OK;
}
static esp_err_t describe(const std::string &name, dl::TensorBase *tensor,
    solar_os_inference_tensor_t &out)
{
    if (!tensor || !tensor->get_element_ptr()) return ESP_ERR_NO_MEM;
    if (name.empty() || name.size() >= sizeof(out.name) ||
        name.find('\0') != std::string::npos) return ESP_ERR_INVALID_SIZE;
    auto shape = tensor->get_shape();
    if (shape.size() > SOLAR_OS_INFERENCE_RANK_MAX) return ESP_ERR_NOT_SUPPORTED;
    esp_err_t error = dtype(tensor->get_dtype(), out.dtype);
    if (error != ESP_OK) return error;
    out.bytes = solar_os_tensor_dtype_bytes(out.dtype);
    for (size_t i = 0; i < shape.size(); ++i) {
        if (shape[i] <= 0 || (size_t)shape[i] > SOLAR_OS_INFERENCE_TENSOR_MAX / out.bytes)
            return ESP_ERR_INVALID_SIZE;
        out.shape[i] = shape[i]; out.bytes *= shape[i];
    }
    out.rank = shape.size();
    std::memcpy(out.name, name.c_str(), name.size() + 1);
    const bool floating = out.dtype == SOLAR_OS_TENSOR_FLOAT32 ||
        out.dtype == SOLAR_OS_TENSOR_FLOAT64 || out.dtype == SOLAR_OS_TENSOR_FLOAT16 ||
        out.dtype == SOLAR_OS_TENSOR_BOOL;
    if (!floating) {
        if (!tensor->exponent.is_valid()) return ESP_ERR_NO_MEM;
        out.exponent_count = tensor->exponent.channel_size();
        if (out.exponent_count > SOLAR_OS_INFERENCE_TENSOR_MAX / sizeof(int32_t))
            return ESP_ERR_INVALID_SIZE;
        out.exponents = static_cast<int32_t *>(solar_os_memory_alloc(
            out.exponent_count * sizeof(int32_t), SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
            "inference.exponents"));
        if (!out.exponents) return ESP_ERR_NO_MEM;
        for (size_t i = 0; i < out.exponent_count; ++i) out.exponents[i] = tensor->exponent.get(i);
    }
    return ESP_OK;
}
static esp_err_t load_buffer(const uint8_t *data, size_t size, void *allocation,
    solar_os_inference_cancel_fn cancel, void *user,
    solar_os_inference_backend_t **out, solar_os_inference_model_info_t *info)
{
    std::unique_ptr<void, decltype(&solar_os_memory_free)> owned(allocation, solar_os_memory_free);
    *out = nullptr;
    try {
        const size_t internal_before = solar_os_memory_dl_free_bytes(false);
        const size_t external_before = solar_os_memory_dl_free_bytes(true);
        esp_err_t error;
        if (!data || size < 16 || size > SOLAR_OS_INFERENCE_FILE_MAX)
            return ESP_ERR_INVALID_SIZE;
        std::unique_ptr<solar_os_inference_backend> b(new solar_os_inference_backend);
        b->allocation = allocation ? owned.release() : solar_os_memory_alloc(size + 31,
            SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "inference.file");
        if (!b->allocation) return ESP_ERR_NO_MEM;
        b->buffer = reinterpret_cast<uint8_t *>((reinterpret_cast<uintptr_t>(b->allocation) + 15) & ~uintptr_t(15));
        if (!allocation) std::memcpy(b->buffer, data, size);
        size_t offset, payload;
        error = solar_os_espdl_validate(b->buffer, size, &offset, &payload, &b->layers);
        if (error != ESP_OK) return error;
        if (cancel(user)) return ESP_ERR_TIMEOUT;
        /* Move EDL1's 12-byte header to an aligned payload too. We own the bytes
         * for the entire model lifetime, so weights need not be copied again. */
        if (offset != 16) { std::memmove(b->buffer + 16, b->buffer + offset, payload); }
        b->fbs.reset(new fbs::FbsModel(b->buffer + 16, payload,
            fbs::MODEL_LOCATION_IN_SDCARD, false, false, false, false));
        b->model.reset(new dl::Model());
        /* Use an already-owned model before load/build. Constructor exceptions
         * cannot otherwise destroy ESP-DL's raw context/partial execution plan. */
        error = b->model->load(b->fbs.get());
        if (error != ESP_OK) return ESP_ERR_NOT_SUPPORTED;
        b->model->build(0, dl::MEMORY_MANAGER_GREEDY); /* Workspace in PSRAM. */
        auto &inputs = b->model->get_inputs(); auto &outputs = b->model->get_outputs();
        if (inputs.empty() || outputs.empty()) return ESP_ERR_NOT_SUPPORTED;
        if (inputs.size() > SOLAR_OS_INFERENCE_PORTS_MAX || outputs.size() > SOLAR_OS_INFERENCE_PORTS_MAX)
            return ESP_ERR_NOT_SUPPORTED;
        for (auto &entry : inputs) {
            error = describe(entry.first, entry.second, info->inputs[info->input_count]);
            if (error != ESP_OK) return error;
            ++info->input_count;
        }
        for (auto &entry : outputs) {
            error = describe(entry.first, entry.second, info->outputs[info->output_count]);
            if (error != ESP_OK) return error;
            ++info->output_count;
        }
        if (cancel(user)) return ESP_ERR_TIMEOUT;
        /* Graph variables are activation scratch. Keep weights and tensor
         * descriptors resident, and acquire scratch only while executing. */
        b->capture_workspace();
        const size_t internal_after = solar_os_memory_dl_free_bytes(false);
        const size_t external_after = solar_os_memory_dl_free_bytes(true);
        info->model_bytes = size;
        info->internal_bytes = internal_before > internal_after ? internal_before - internal_after : 0;
        info->external_bytes = external_before > external_after ? external_before - external_after : 0;
        /* A transferred source predates this snapshot but stays resident as
         * model weights. Include its requested allocation in the estimate. */
        if (allocation) info->external_bytes += size + 31;
        *out = b.release(); return ESP_OK;
    } catch (const std::bad_alloc &) { return ESP_ERR_NO_MEM; }
    catch (...) { return ESP_FAIL; }
}
extern "C" esp_err_t solar_os_espdl_backend_load_buffer(const uint8_t *data, size_t size,
    solar_os_inference_cancel_fn cancel, void *user,
    solar_os_inference_backend_t **out, solar_os_inference_model_info_t *info)
{ return load_buffer(data, size, nullptr, cancel, user, out, info); }
extern "C" esp_err_t solar_os_espdl_backend_load_owned_buffer(void *allocation, uint8_t *data, size_t size,
    solar_os_inference_cancel_fn cancel, void *user,
    solar_os_inference_backend_t **out, solar_os_inference_model_info_t *info)
{
    if (!allocation || !data || (uintptr_t)data % 16 ||
        (uintptr_t)data < (uintptr_t)allocation || (uintptr_t)data - (uintptr_t)allocation > 15) {
        solar_os_memory_free(allocation); *out = nullptr; return ESP_ERR_INVALID_ARG;
    }
    return load_buffer(data, size, allocation, cancel, user, out, info);
}
extern "C" esp_err_t solar_os_espdl_backend_run(solar_os_inference_backend_t *b,
    const solar_os_inference_input_t *inputs, size_t count,
    solar_os_inference_cancel_fn cancel, void *user, solar_os_inference_result_t *r)
{
    try {
        const int64_t start = esp_timer_get_time();
        auto &ports = b->model->get_inputs();
        if (count != ports.size()) return ESP_ERR_INVALID_ARG;
        /* The firmware facade normally checks these before calling the engine.
         * A standalone provider must check sizes before copying into tensors. */
        for (size_t i = 0; i < count; ++i) {
            if (!inputs[i].name || !inputs[i].data) return ESP_ERR_INVALID_ARG;
            auto found = ports.find(inputs[i].name);
            if (found == ports.end() || !found->second) return ESP_ERR_INVALID_ARG;
            if (inputs[i].bytes != found->second->get_bytes()) return ESP_ERR_INVALID_SIZE;
            for (size_t j = 0; j < i; ++j)
                if (!std::strcmp(inputs[i].name, inputs[j].name)) return ESP_ERR_INVALID_ARG;
            if (inputs[i].tensor) {
                const auto &t = *inputs[i].tensor;
                auto shape = found->second->get_shape();
                solar_os_tensor_dtype_t type;
                if (dtype(found->second->get_dtype(), type) || t.dtype != type ||
                    t.rank != shape.size() || t.bytes != inputs[i].bytes) return ESP_ERR_INVALID_ARG;
                for (size_t j = 0; j < shape.size(); ++j)
                    if (t.shape[j] != shape[j]) return ESP_ERR_INVALID_ARG;
                const size_t exponents = type == SOLAR_OS_TENSOR_FLOAT32 || type == SOLAR_OS_TENSOR_FLOAT64 ||
                    type == SOLAR_OS_TENSOR_FLOAT16 || type == SOLAR_OS_TENSOR_BOOL ? 0 : found->second->exponent.channel_size();
                if (t.exponent_count != exponents || (exponents && !t.exponents)) return ESP_ERR_INVALID_ARG;
                for (size_t j = 0; j < exponents; ++j)
                    if (t.exponents[j] != found->second->exponent.get(j)) return ESP_ERR_INVALID_ARG;
            }
        }
        struct WorkspaceLease {
            solar_os_inference_backend *backend;
            ~WorkspaceLease() { backend->release_workspace(); }
        } workspace{b};
        if (!b->acquire_workspace()) return ESP_ERR_NO_MEM;
        for (size_t i = 0; i < count; ++i) {
            auto found = ports.find(inputs[i].name);
            if (found == ports.end() || !found->second) return ESP_ERR_INVALID_ARG;
            std::memcpy(found->second->get_element_ptr(), inputs[i].data, inputs[i].bytes);
        }
        r->input_us = esp_timer_get_time() - start;
        const int64_t run_start = esp_timer_get_time();
        for (size_t i = 0; i < b->layers; ++i) {
            if (cancel(user)) return ESP_ERR_TIMEOUT;
            b->model->run(b->layers, i, b->mode);
            vTaskDelay(1);
        }
        r->inference_us = esp_timer_get_time() - run_start;
        if (cancel(user)) return ESP_ERR_TIMEOUT;
        const int64_t output_start = esp_timer_get_time();
        for (auto &entry : b->model->get_outputs()) {
            auto &output = r->outputs[r->count];
            esp_err_t error = describe(entry.first, entry.second, output.tensor);
            if (error != ESP_OK) return error;
            output.data = solar_os_memory_alloc(output.tensor.bytes,
                SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "inference.output");
            if (!output.data) return ESP_ERR_NO_MEM;
            std::memcpy(output.data, entry.second->get_element_ptr(), output.tensor.bytes);
            ++r->count;
        }
        r->output_us = esp_timer_get_time() - output_start;
        r->elapsed_us = esp_timer_get_time() - start;
        return ESP_OK;
    } catch (const std::bad_alloc &) { return ESP_ERR_NO_MEM; }
    catch (...) { return ESP_FAIL; }
}
extern "C" void solar_os_espdl_backend_reset(solar_os_inference_backend_t *b)
{
    if (b) b->model->reset();
}
extern "C" void solar_os_espdl_backend_set_mode(solar_os_inference_backend_t *b,
    solar_os_inference_mode_t mode)
{
    switch (mode) {
    case SOLAR_OS_INFERENCE_AUTO: b->mode = dl::RUNTIME_MODE_AUTO; break;
    case SOLAR_OS_INFERENCE_DUAL: b->mode = dl::RUNTIME_MODE_MULTI_CORE; break;
    default: b->mode = dl::RUNTIME_MODE_SINGLE_CORE; break;
    }
}
extern "C" void solar_os_espdl_backend_close(solar_os_inference_backend_t *b)
{
    delete b;
}
extern "C" bool solar_os_espdl_backend_idle(void) { return resident_backends == 0; }

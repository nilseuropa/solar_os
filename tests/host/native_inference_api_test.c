#define main original_inference_suite
#include "inference_test.c"
#undef main
#include "solar_os_native_media.h"
#include "solar_os_native_inference_abi.h"

int main(void)
{
    const solar_os_native_inference_api_v1_t *api = solar_os_native_media_get_service("inference", 1, sizeof(*api));
    const solar_os_native_tensor_image_api_v1_t *prepare = solar_os_native_media_get_service("tensor.image", 1, sizeof(*prepare));
    assert(api && prepare && !solar_os_native_media_get_service("image", 1, 0));
    solar_os_inference_t *client = NULL; uint32_t model, found;
    atomic_int cancellation = 0;
    assert(!api->create(cancelled, &cancellation, &client));
    assert(!api->load(client, "/model.espdl", 1000, &model));
    assert(!api->find("/model.espdl", &found) && found == model);
    const solar_os_inference_model_info_t *info;
    assert(!api->info(client, model, &info) && info->input_count == 2);
    assert(!api->retain(model));
    assert(api->unload(client, model) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    assert(!api->release(model));
    assert(!api->set_mode(client, model, SOLAR_OS_INFERENCE_DUAL));
    solar_os_inference_result_t *result = NULL;
    assert(!api->run(client, model, inputs, 2, 1000, &result));
    api->destroy(client); client = NULL;
    assert(!api->find("/model.espdl", &found) && found == model);
    assert(!api->create(cancelled, &cancellation, &client));
    assert(!api->reset(client, model));
    assert(!api->unload(client, model));
    const int8_t *sum = result->outputs[0].data;
    for (unsigned i = 0; i < 8; ++i) assert(sum[i] == a[i]+b[i]);
    api->result_free(result);
    assert(api->info(client, model, &info) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    uint8_t rgb[12] = {0}; uint8_t tensor[4]; int32_t exponent = 0;
    solar_os_raster_image_pixels_t view = {.data = rgb, .length = 12, .stride = 6, .width = 2, .height = 2};
    solar_os_inference_tensor_t port = {.dtype = SOLAR_OS_TENSOR_INT8, .rank = 4,
        .shape = {1,2,2,1}, .bytes = 4, .exponent_count = 1, .exponents = &exponent};
    solar_os_tensor_image_options_t o; prepare->defaults(&o); o.color = SOLAR_OS_TENSOR_GRAY;
    solar_os_tensor_image_transform_t transform;
    assert(!prepare->inspect(&view, &port, &o, &transform) && transform.bytes == 4);
    cancellation = 1;
    assert(prepare->prepare(&view, &port, &o, tensor, sizeof(tensor), &transform, cancelled, &cancellation) == SOLAR_OS_NATIVE_ERROR_TIMEOUT);
    cancellation = 0;
    assert(!prepare->prepare(&view, &port, &o, tensor, sizeof(tensor), &transform, NULL, NULL));
    api->destroy(client); assert(!allocations && !workers);
    puts("native inference/tensor tables: residency, retained model, independent output, cancellation passed");
    return 0;
}

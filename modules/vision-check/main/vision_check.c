/* Standalone ELF acceptance client. Its sole SolarOS import is the host entry;
 * it uses no firmware service symbols, ESP-IDF types, or interpreter APIs. */
#include "solar_os_native_abi.h"
#include "solar_os_native_image_abi.h"
#include "solar_os_native_media_abi.h"
#include "solar_os_native_vision_abi.h"
#include "solar_os_native_inference_abi.h"

static const solar_os_native_host_api_v1_t *host;
static const solar_os_native_image_api_v1_t *image;
static const solar_os_native_imlib_api_v1_t *imlib;
static const solar_os_native_qr_api_v1_t *qr;
static const solar_os_native_inference_api_v1_t *inference;
static const solar_os_native_tensor_image_api_v1_t *tensor_image;
static const solar_os_native_media_api_v1_t *media;
static const solar_os_native_memory_api_v1_t *memory;

static size_t length(const char *s) { size_t n = 0; while (s[n]) ++n; return n; }
static bool equal(const char *a, const char *b)
{ while (*a && *a == *b) { ++a; ++b; } return *a == *b; }
static void text(const char *s) { (void)host->write_utf8(s, length(s)); }
static void number(uint32_t value)
{
    char digits[10]; unsigned n = 0;
    do { digits[n++] = '0'+value%10; value /= 10; } while (value);
    while (n) (void)host->write_utf8(&digits[--n], 1);
}
static void metric(const char *name, uint32_t value)
{ text(name); text(" "); number(value); text("\n"); }
static bool abort_request(void *user) { return ++*(unsigned *)user >= 2; }

static int image_check(const char *path, const char *qr_path)
{
    int error = 0; solar_os_raster_image_t *source = NULL, *mask = NULL;
    solar_os_imlib_result_t *result = NULL;
    solar_os_vision_qr_results_t *codes = NULL;
    solar_os_imlib_options_t options; imlib->defaults(&options);
    if ((error = image->open(path, &source))) goto done;
    if ((error = imlib->run(source, NULL, SOLAR_OS_IMLIB_STATISTICS, &options, NULL, NULL, &result))) goto done;
    metric("NATIVE_GRAY_MEAN", result->statistics[0].mean); imlib->result_free(result); result = NULL;
    options.threshold_count = 1;
    options.thresholds[0] = (solar_os_imlib_threshold_t){128,255,0,0,0,0};
    if ((error = imlib->run(source, NULL, SOLAR_OS_IMLIB_BINARY, &options, NULL, NULL, &result))) goto done;
    mask = result->image; image->retain(mask); imlib->result_free(result); result = NULL;
    if ((error = imlib->run(mask, NULL, SOLAR_OS_IMLIB_BLOBS, &options, NULL, NULL, &result))) goto done;
    metric("NATIVE_BLOBS", result->count);
    if (result->count != 1 || result->blobs[0].pixels != 48) { error = -100; goto done; }
    imlib->result_free(result); result = NULL;
    unsigned checks = 0;
    error = imlib->run(source, NULL, SOLAR_OS_IMLIB_GAUSSIAN, NULL, abort_request, &checks, &result);
    if (error != SOLAR_OS_NATIVE_ERROR_TIMEOUT || result) { error = -101; goto done; }
    error = 0; text("NATIVE_IMLIB_CANCEL_OK\n");
    image->release(source); source = NULL;
    if ((error = image->open(qr_path, &source))) goto done;
    if ((error = qr->qrcodes(source, NULL, NULL, NULL, &codes))) goto done;
    image->release(source); source = NULL;
    metric("NATIVE_QR", codes->count);
    if (codes->count != 2) error = -102;
done:
    qr->result_free(codes); imlib->result_free(result); image->release(mask); image->release(source);
    return error;
}

static int prepare_check(void)
{
    uint8_t rgb[12] = {0,0,0, 20,20,20, 40,40,40, 60,60,60};
    solar_os_raster_image_t *source = NULL;
    int error = image->from_pixels(rgb, sizeof(rgb), 2, 2, SOLAR_OS_RASTER_IMAGE_RGB888, 0, &source);
    if (error) return error;
    uint8_t *bytes = memory->alloc(4);
    if (!bytes) { image->release(source); return SOLAR_OS_NATIVE_ERROR_NO_MEMORY; }
    solar_os_raster_image_pixels_t view;
    error = image->pixels(source, &view);
    int32_t exponent = 0;
    solar_os_inference_tensor_t port = {.dtype = SOLAR_OS_TENSOR_INT8, .rank = 4,
        .shape = {1,2,2,1}, .bytes = 4, .exponent_count = 1, .exponents = &exponent};
    solar_os_tensor_image_options_t options; tensor_image->defaults(&options);
    options.color = SOLAR_OS_TENSOR_GRAY;
    solar_os_tensor_image_transform_t transform;
    if (!error) error = tensor_image->inspect(&view, &port, &options, &transform);
    if (!error && transform.bytes != 4) error = -103;
    if (!error) error = tensor_image->prepare(&view, &port, &options, bytes, 4, &transform, NULL, NULL);
    image->release(source);
    for (unsigned i = 0; !error && i < 4; ++i) if (bytes[i] != i*20) error = -104;
    memory->free(bytes);
    if (!error) text("NATIVE_TENSOR_IMAGE_OK\n");
    return error;
}

static int arithmetic_result(const solar_os_inference_result_t *result)
{
    if (result->count != 2) return -105;
    for (unsigned i = 0; i < 2; ++i) {
        const solar_os_inference_tensor_t *port = &result->outputs[i].tensor;
        if (port->dtype != SOLAR_OS_TENSOR_INT8 || port->bytes != 8) return -106;
        bool sum = equal(port->name, "sum");
        if (!sum && !equal(port->name, "difference")) return -107;
        const int8_t *data = result->outputs[i].data;
        for (unsigned j = 0; j < 8; ++j) if (data[j] != (sum ? (int)(j+1)*2 : 0)) return -108;
    }
    return 0;
}
static int model_check(const char *path, const char *bundle_path)
{
    solar_os_inference_t *client = NULL;
    solar_os_inference_result_t *result = NULL;
    uint32_t model = 0, bundle = 0; bool retained = false;
    int error = inference->create(NULL, NULL, &client);
    if (error) return error;
    if (inference->find(path, &model)) error = inference->load(client, path, 10000, &model);
    if (error) goto done;
    metric("NATIVE_MODEL_HANDLE", model);
    const solar_os_inference_model_info_t *info;
    if ((error = inference->info(client, model, &info))) goto done;
    if (info->input_count != 2 || info->inputs[0].bytes != 8 || info->inputs[1].bytes != 8) { error = -109; goto done; }
    if ((error = inference->retain(model))) goto done;
    retained = true;
    if (inference->unload(client, model) != SOLAR_OS_NATIVE_ERROR_INVALID_STATE) { error = -110; goto done; }
    const int8_t values[8] = {1,2,3,4,5,6,7,8};
    solar_os_inference_input_t inputs[2] = {{"a",values,8,NULL}, {"b",values,8,NULL}};
    if ((error = inference->set_mode(client, model, SOLAR_OS_INFERENCE_AUTO))) goto done;
    if ((error = inference->run(client, model, inputs, 2, 10000, &result))) goto done;
    if ((error = arithmetic_result(result))) goto done;
    metric("NATIVE_INFERENCE_US", result->inference_us);
    inference->release(model); retained = false;
    inference->destroy(client); client = NULL;
    if ((error = arithmetic_result(result))) goto done;
    inference->result_free(result); result = NULL;
    if ((error = inference->create(NULL, NULL, &client))) goto done;
    if ((error = inference->reset(client, model))) goto done;
    if (inference->find(bundle_path, &bundle)) error = inference->load_bundle(client, bundle_path, 60000, &bundle);
    if (error) goto done;
    metric("NATIVE_BUNDLE_HANDLE", bundle);
    solar_os_inference_value_t values_in[2] = {{.tensor = inputs[0]}, {.tensor = inputs[1]}};
    if ((error = inference->run_bundle(client, bundle, values_in, 2, 10000, &result))) goto done;
    if ((error = arithmetic_result(result))) goto done;
    if (!result->result_json) { error = -111; goto done; }
    text("NATIVE_BUNDLE_OK\n");
done:
    if (retained) inference->release(model);
    inference->result_free(result); inference->destroy(client);
    return error; /* Both models remain OS-owned after ELF unload. */
}

static int video_check(const char *source)
{
    solar_os_native_media_t *session = NULL;
    solar_os_raster_image_t *decoded = NULL;
    uint32_t stream = 0, frame = 0;
    bool network = source[0]=='r' && source[1]=='t' && source[2]=='s' && source[3]=='p' && source[4]==':';
    int error = media->create("native-vision", NULL, NULL, &session);
    if (error) return error;
    solar_os_native_video_options_v1_t options = {.width = 320, .height = 240, .jpeg_quality = 12, .timeout_ms = 5000};
    error = network ? media->rtsp_open(session, source, &stream) : media->open(session, source, &options, &stream);
    if (error) goto done;
    for (unsigned i = 0; i < 3; ++i) {
        error = network ? media->rtsp_read(session, stream, 5000, &frame) : media->acquire(session, stream, &frame);
        if (error || !frame) { if (!error) error = -112; goto done; }
        solar_os_native_video_frame_v1_t view;
        if ((error = media->frame(session, frame, &view))) goto done;
        if (view.network != network || !view.timestamp_us) { error = -113; goto done; }
        if ((error = image->decode(view.data, view.length, &decoded))) goto done;
        if ((error = media->release_frame(session, frame))) goto done;
        frame = 0;
        solar_os_raster_image_pixels_t pixels;
        if ((error = image->pixels(decoded, &pixels))) goto done;
        metric("NATIVE_VIDEO_WIDTH", pixels.width);
        image->release(decoded); decoded = NULL;
    }
    text("NATIVE_VIDEO_OK\n");
done:
    image->release(decoded);
    int close_error = media->destroy(session);
    return error ? error : close_error;
}

int main(int argc, char **argv)
{
    host = solar_os_native_host_v1();
    if (!host || host->abi_version != 1 || host->struct_size < sizeof(*host) || !host->get_service || !host->write_utf8) return 1;
    if (argc < 5 || argc > 6) { text("vision-check <scene.png> <qr.png> <model.espdl> <bundle.json> [camera0|rtsp://...]\n"); return 2; }
    image = host->get_service(SOLAR_OS_NATIVE_IMAGE_SERVICE, 1, sizeof(*image));
    imlib = host->get_service(SOLAR_OS_NATIVE_IMLIB_SERVICE, 1, sizeof(*imlib));
    qr = host->get_service(SOLAR_OS_NATIVE_QR_SERVICE, 1, sizeof(*qr));
    inference = host->get_service(SOLAR_OS_NATIVE_INFERENCE_SERVICE, 1, sizeof(*inference));
    tensor_image = host->get_service(SOLAR_OS_NATIVE_TENSOR_IMAGE_SERVICE, 1, sizeof(*tensor_image));
    media = host->get_service(SOLAR_OS_NATIVE_MEDIA_SERVICE, 1, sizeof(*media));
    memory = host->get_service(SOLAR_OS_NATIVE_MEMORY_SERVICE, 1, sizeof(*memory));
    if (!image || !imlib || !qr || !inference || !tensor_image || !memory || (argc > 5 && !media)) { text("NATIVE_SERVICE_MISSING\n"); return 3; }
    if (host->get_service("image", 2, 0) || host->get_service("image", 1, sizeof(*image)+1) || host->get_service("unknown", 1, 0)) return 4;
    int error = image_check(argv[1], argv[2]);
    if (!error) error = prepare_check();
    if (!error) error = model_check(argv[3], argv[4]);
    if (!error && argc > 5) error = video_check(argv[5]);
    if (error) { metric("NATIVE_CHECK_ERROR", (uint32_t)-error); return 5; }
    text("NATIVE_VISION_CHECK_OK\n"); return 0;
}

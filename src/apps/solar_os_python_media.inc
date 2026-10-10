/* Interpreter conversion only; leases/workers belong to service.script-media. */
#if SOLAR_OS_PACKAGE_SERVICE_VISION
#include "solar_os_vision.h"
#endif
static solar_os_script_media_t *python_media_session;

static solar_os_script_media_t *python_media_get(void)
{
    if (!python_media_session) {
        solar_os_script_media_t *created = NULL;
        python_check_esp(solar_os_script_media_create(
            python_runner_control ? "python.runner" : "python.app",
            python_should_cancel, NULL, &created));
        __atomic_store_n(&python_media_session, created, __ATOMIC_RELEASE);
    }
    return python_media_session;
}

static void python_media_destroy(void)
{
    solar_os_script_media_destroy(python_media_session);
    __atomic_store_n(&python_media_session, NULL, __ATOMIC_RELEASE);
}

static mp_obj_t python_stream_info(const solar_os_stream_info_t *info)
{
    mp_obj_t d = mp_obj_new_dict(0);
    python_dict_store_cstr(d, "id", info->id);
    python_dict_store_cstr(d, "provider", info->provider);
    python_dict_store_cstr(d, "device", info->device);
    python_dict_store_cstr(d, "type", solar_os_stream_type_name(info->type));
    python_dict_store_cstr(d, "direction", solar_os_stream_direction_name(info->direction));
    python_dict_store_cstr(d, "sharing", solar_os_stream_sharing_name(info->sharing));
    python_dict_store_cstr(d, "unit", info->unit);
    python_dict_store_cstr(d, "format", info->format);
    python_dict_store_cstr(d, "summary", info->summary);
    python_dict_store_cstr(d, "owner", info->owner);
    python_dict_store_uint(d, "active_handles", info->active_handles);
    python_dict_store_u64(d, "read_units", info->read_units);
    python_dict_store_u64(d, "written_units", info->written_units);
    python_dict_store_uint(d, "overruns", info->overruns);
    python_dict_store_uint(d, "underruns", info->underruns);
    if (info->type == SOLAR_OS_STREAM_TYPE_VIDEO) {
        python_dict_store_cstr(d, "codec", "jpeg");
        python_dict_store_uint(d, "width", info->video.width);
        python_dict_store_uint(d, "height", info->video.height);
        python_dict_store_uint(d, "jpeg_quality", info->video.jpeg_quality);
    } else if (info->type == SOLAR_OS_STREAM_TYPE_AUDIO) {
        python_dict_store_cstr(d, "sample_format", solar_os_stream_audio_sample_format_name(info->audio.sample_format));
        python_dict_store_uint(d, "sample_rate", info->audio.sample_rate);
        python_dict_store_uint(d, "channels", info->audio.channels);
        python_dict_store_uint(d, "bits_per_sample", info->audio.bits_per_sample);
        python_dict_store_uint(d, "frames_per_block", info->audio.frames_per_block);
    }
    return d;
}

static mp_obj_t solaros_streams_list(void)
{
    mp_obj_t list = mp_obj_new_list(0, NULL);
    const size_t count = solar_os_stream_count();
    for (size_t i = 0; i < count; ++i) {
        solar_os_stream_info_t info;
        if (solar_os_stream_get(i, &info)) mp_obj_list_append(list, python_stream_info(&info));
    }
    return list;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_streams_list_obj, solaros_streams_list);

static mp_obj_t solaros_streams_info(mp_obj_t id)
{
    solar_os_stream_info_t info;
    python_check_esp(solar_os_stream_get_info(mp_obj_str_get_str(id), &info));
    return python_stream_info(&info);
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_streams_info_obj, solaros_streams_info);

static mp_obj_t solaros_streams_status(mp_obj_t handle)
{
    solar_os_stream_info_t info;
    python_check_esp(solar_os_script_media_stream_info(python_media_session, python_u32_from_obj(handle), &info));
    return python_stream_info(&info);
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_streams_status_obj, solaros_streams_status);

static uint32_t python_media_option(mp_obj_t config, const char *key, uint32_t fallback, uint32_t max)
{
    mp_obj_t obj = config == mp_const_none ? MP_OBJ_NULL : python_get_dict_obj(config, key, false);
    uint32_t value = obj == MP_OBJ_NULL ? fallback : python_u32_from_obj(obj);
    if (value > max) mp_raise_ValueError(MP_ERROR_TEXT("media option out of range"));
    return value;
}

static mp_obj_t solaros_streams_open(size_t n, const mp_obj_t *args)
{
    const char *id = mp_obj_str_get_str(args[0]);
    const mp_obj_t config = n > 1 ? args[1] : mp_const_none;
    solar_os_stream_info_t info;
    python_check_esp(solar_os_stream_get_info(id, &info));
    if (config != mp_const_none) {
        if (!mp_obj_is_type(config, &mp_type_dict)) mp_raise_TypeError(MP_ERROR_TEXT("expected dict"));
        mp_obj_dict_t *dict = MP_OBJ_TO_PTR(config);
        for (size_t i = 0; i < dict->map.alloc; ++i) {
            if (!mp_map_slot_is_filled(&dict->map, i)) continue;
            const char *key = mp_obj_str_get_str(dict->map.table[i].key);
            if (!solar_os_script_media_option_valid(info.type, key))
                mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT("unsupported stream option %s"), key);
        }
    }
    solar_os_stream_open_options_t options = {
        .direction = info.direction == SOLAR_OS_STREAM_DIRECTION_SINK ?
            SOLAR_OS_STREAM_DIRECTION_SINK : SOLAR_OS_STREAM_DIRECTION_SOURCE,
        .timeout_ms = python_media_option(config, "timeout_ms", 1000, SOLAR_OS_SCRIPT_MEDIA_TIMEOUT_MAX),
    };
    mp_obj_t direction = config == mp_const_none ? MP_OBJ_NULL : python_get_dict_obj(config, "direction", false);
    if (direction != MP_OBJ_NULL) {
        const char *name = mp_obj_str_get_str(direction);
        if (!strcmp(name, "source")) options.direction = SOLAR_OS_STREAM_DIRECTION_SOURCE;
        else if (!strcmp(name, "sink")) options.direction = SOLAR_OS_STREAM_DIRECTION_SINK;
        else if (!strcmp(name, "duplex")) options.direction = SOLAR_OS_STREAM_DIRECTION_DUPLEX;
        else mp_raise_ValueError(MP_ERROR_TEXT("expected source, sink or duplex"));
    }
    if (info.type == SOLAR_OS_STREAM_TYPE_VIDEO) {
        options.requested_video = (solar_os_stream_video_format_t){
            .codec = SOLAR_OS_STREAM_VIDEO_JPEG,
            .width = python_media_option(config, "width", 0, UINT16_MAX),
            .height = python_media_option(config, "height", 0, UINT16_MAX),
            .jpeg_quality = python_media_option(config, "jpeg_quality", 0, 63),
        };
    } else if (info.type == SOLAR_OS_STREAM_TYPE_AUDIO) {
        options.requested_audio = info.audio;
        options.requested_audio.sample_rate = python_media_option(config, "sample_rate", info.audio.sample_rate, 192000);
        options.requested_audio.channels = python_media_option(config, "channels", info.audio.channels, 8);
        options.requested_audio.frames_per_block = python_media_option(config, "frames_per_block", info.audio.frames_per_block, 4096);
    }
    uint32_t handle;
    python_check_esp(solar_os_script_media_open(python_media_get(), id, &options, &handle));
    return mp_obj_new_int_from_uint(handle);
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_streams_open_obj, 1, 2, solaros_streams_open);

static mp_obj_t solaros_streams_close(mp_obj_t handle)
{
    python_check_esp(solar_os_script_media_close(python_media_session, python_u32_from_obj(handle)));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_streams_close_obj, solaros_streams_close);

static mp_obj_t solaros_streams_close_all(void)
{
    python_check_esp(solar_os_script_media_close_all(python_media_session));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_streams_close_all_obj, solaros_streams_close_all);

static mp_obj_t solaros_streams_read(size_t n, const mp_obj_t *args)
{
    const uint32_t handle = python_u32_from_obj(args[0]);
    const size_t size = python_size_from_obj(args[1]);
    const uint32_t timeout = n > 2 ? python_u32_from_obj(args[2]) : 1000;
    if (!size || size > SOLAR_OS_SCRIPT_MEDIA_TRANSFER_MAX) mp_raise_ValueError(MP_ERROR_TEXT("read size must be 1..16384"));
    vstr_t buffer;
    vstr_init_len(&buffer, size);
    size_t length = 0;
    esp_err_t err = solar_os_script_media_read(python_media_session, handle, buffer.buf, size, timeout, &length);
    if (err != ESP_OK) { vstr_clear(&buffer); python_raise_esp(err); }
    buffer.len = length;
    return mp_obj_new_bytes_from_vstr(&buffer);
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_streams_read_obj, 2, 3, solaros_streams_read);

static mp_obj_t solaros_streams_write(size_t n, const mp_obj_t *args)
{
    mp_buffer_info_t buffer;
    mp_get_buffer_raise(args[1], &buffer, MP_BUFFER_READ);
    size_t length;
    python_check_esp(solar_os_script_media_write(python_media_session, python_u32_from_obj(args[0]),
        buffer.buf, buffer.len, n > 2 ? python_u32_from_obj(args[2]) : 1000, &length));
    return mp_obj_new_int_from_uint(length);
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_streams_write_obj, 2, 3, solaros_streams_write);

static mp_obj_t solaros_streams_read_scalar(size_t n, const mp_obj_t *args)
{
    const solar_os_stream_read_options_t options = {
        .window_ms = n > 1 ? python_u32_from_obj(args[1]) : 0,
        .timeout_ms = n > 2 ? python_u32_from_obj(args[2]) : 1000,
    };
    float value;
    python_check_esp(solar_os_script_media_scalar(python_media_session, python_u32_from_obj(args[0]), &options, &value));
    return mp_obj_new_float(value);
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_streams_read_scalar_obj, 1, 3, solaros_streams_read_scalar);

static mp_obj_t solaros_streams_acquire_frame(mp_obj_t handle)
{
    uint32_t frame;
    python_check_esp(solar_os_script_media_acquire(python_media_session, python_u32_from_obj(handle), &frame));
    return mp_obj_new_int_from_uint(frame);
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_streams_acquire_frame_obj, solaros_streams_acquire_frame);

static mp_obj_t solaros_streams_frame_info(mp_obj_t handle)
{
    solar_os_script_media_frame_t frame;
    python_check_esp(solar_os_script_media_frame(python_media_session, python_u32_from_obj(handle), &frame));
    mp_obj_t d = mp_obj_new_dict(0);
    python_dict_store_cstr(d, "codec", "jpeg");
    python_dict_store_uint(d, "width", frame.jpeg.width);
    python_dict_store_uint(d, "height", frame.jpeg.height);
    python_dict_store_uint(d, "length", frame.jpeg.length);
    python_dict_store_u64(d, "timestamp_us", frame.jpeg.timestamp_us);
    python_dict_store_bool(d, "network", frame.network);
    if (frame.network) {
        python_dict_store_u64(d, "rtp_timestamp", frame.rtp_timestamp);
        python_dict_store_u64(d, "arrived_us", frame.arrived_us);
    }
    return d;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_streams_frame_info_obj, solaros_streams_frame_info);

static mp_obj_t solaros_streams_frame_data(mp_obj_t handle)
{
    solar_os_script_media_frame_t frame;
    python_check_esp(solar_os_script_media_frame(python_media_session, python_u32_from_obj(handle), &frame));
    return mp_obj_new_bytes(frame.jpeg.data, frame.jpeg.length);
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_streams_frame_data_obj, solaros_streams_frame_data);

static void python_media_check_save(esp_err_t error, int file_errno, const char *path)
{
    if (error != ESP_OK && file_errno) {
        vstr_t message;
        vstr_init(&message, 32);
        vstr_printf(&message, "save '%s': %s", path, strerror(file_errno));
        const mp_obj_t args[] = {mp_obj_new_int(file_errno), mp_obj_new_str_from_vstr(&message)};
        nlr_raise(mp_obj_new_exception_args(&mp_type_OSError, 2, args));
    }
    python_check_esp(error);
}

static mp_obj_t solaros_streams_frame_save(mp_obj_t handle, mp_obj_t path_obj)
{
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    python_resolve_path_obj(path_obj, path, sizeof(path));
    int file_errno;
    esp_err_t error = solar_os_script_media_save(python_media_session, python_u32_from_obj(handle), path, &file_errno);
    python_media_check_save(error, file_errno, path);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(solaros_streams_frame_save_obj, solaros_streams_frame_save);

static mp_obj_t solaros_streams_release_frame(mp_obj_t handle)
{
    python_check_esp(solar_os_script_media_release(python_media_session, python_u32_from_obj(handle)));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_streams_release_frame_obj, solaros_streams_release_frame);

#if SOLAR_OS_PACKAGE_SERVICE_CAMERA
static mp_obj_t solaros_camera_status(void)
{
    solar_os_camera_status_t status;
    python_check_esp(solar_os_camera_get_status(&status));
    mp_obj_t d = mp_obj_new_dict(0);
    python_dict_store_bool(d, "backend_registered", status.backend_registered);
    python_dict_store_bool(d, "initialized", status.initialized);
    python_dict_store_bool(d, "owner_leased", status.owner_leased);
    python_dict_store_bool(d, "frame_leased", status.frame_leased);
    python_dict_store_cstr(d, "owner", status.owner);
    python_dict_store_cstr(d, "driver", status.driver);
    python_dict_store_cstr(d, "sensor", status.sensor.name);
    python_dict_store_cstr(d, "size", solar_os_camera_frame_size_name(status.config.frame_size));
    python_dict_store_uint(d, "jpeg_quality", status.config.jpeg_quality);
    python_dict_store_uint(d, "capture_count", status.capture_count);
    python_dict_store_cstr(d, "last_error_name", esp_err_to_name(status.last_error));
    return d;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_camera_status_obj, solaros_camera_status);

static uint32_t python_camera_snapshot(size_t n, const mp_obj_t *args)
{
    const char *size = n > 0 ? mp_obj_str_get_str(args[0]) : "qvga";
    bool vga = !strcmp(size, "vga");
    if (!vga && strcmp(size, "qvga")) mp_raise_ValueError(MP_ERROR_TEXT("expected qvga or vga"));
    uint32_t quality = n > 1 ? python_u32_from_obj(args[1]) : 12;
    if (quality > 63) mp_raise_ValueError(MP_ERROR_TEXT("jpeg_quality must be 0..63"));
    const char *source = n > 2 ? mp_obj_str_get_str(args[2]) : "camera0";
    uint32_t frame;
    python_check_esp(solar_os_script_media_snapshot(python_media_get(), source,
        vga ? 640 : 320, vga ? 480 : 240, quality, &frame));
    return frame;
}

static mp_obj_t solaros_camera_snapshot(size_t n, const mp_obj_t *args)
{
    return mp_obj_new_int_from_uint(python_camera_snapshot(n, args));
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_camera_snapshot_obj, 0, 3, solaros_camera_snapshot);

static mp_obj_t solaros_camera_capture(size_t n, const mp_obj_t *args)
{
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    python_resolve_path_obj(args[0], path, sizeof(path));
    uint32_t frame = python_camera_snapshot(n - 1, args + 1);
    int file_errno;
    esp_err_t err = solar_os_script_media_save(python_media_session, frame, path, &file_errno);
    esp_err_t release = solar_os_script_media_release(python_media_session, frame);
    python_media_check_save(err, file_errno, path);
    python_check_esp(release);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_camera_capture_obj, 1, 4, solaros_camera_capture);
#endif

#if SOLAR_OS_PACKAGE_SERVICE_RTSP_CLIENT
static mp_obj_t solaros_rtsp_open(size_t n, const mp_obj_t *args)
{
    uint32_t handle;
    python_check_esp(solar_os_script_media_rtsp_open(python_media_get(), mp_obj_str_get_str(args[0]),
        n > 1 ? mp_obj_is_true(args[1]) : true, n > 2 ? mp_obj_is_true(args[2]) : true, &handle));
    return mp_obj_new_int_from_uint(handle);
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_rtsp_open_obj, 1, 3, solaros_rtsp_open);

static mp_obj_t solaros_rtsp_status(mp_obj_t handle)
{
    solar_os_rtsp_client_status_t status;
    bool ended;
    python_check_esp(solar_os_script_media_rtsp_status(python_media_session, python_u32_from_obj(handle), &status, &ended));
    mp_obj_t d = mp_obj_new_dict(0);
    python_dict_store_bool(d, "ended", ended);
    python_dict_store_bool(d, "negotiated", status.negotiated);
    python_dict_store_bool(d, "playing", status.playing);
    python_dict_store_bool(d, "audio_playing", status.audio_playing);
    python_dict_store_bool(d, "video", status.video);
    python_dict_store_bool(d, "audio", status.audio);
    python_dict_store_uint(d, "sample_rate", status.sample_rate);
    python_dict_store_uint(d, "channels", status.channels);
    python_dict_store_uint(d, "video_frames", status.video_frames);
    python_dict_store_uint(d, "video_dropped", status.video_dropped);
    python_dict_store_uint(d, "audio_dropped", status.audio_dropped);
    python_dict_store_int(d, "error", status.error);
    python_dict_store_cstr(d, "error_name", esp_err_to_name(status.error));
    python_dict_store_cstr(d, "error_detail", status.error_detail);
    return d;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_rtsp_status_obj, solaros_rtsp_status);

static mp_obj_t solaros_rtsp_read_frame(size_t n, const mp_obj_t *args)
{
    uint32_t frame;
    python_check_esp(solar_os_script_media_rtsp_read(python_media_session, python_u32_from_obj(args[0]),
        n > 1 ? python_u32_from_obj(args[1]) : 0, &frame));
    return frame ? mp_obj_new_int_from_uint(frame) : mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_rtsp_read_frame_obj, 1, 2, solaros_rtsp_read_frame);

static mp_obj_t solaros_rtsp_close(mp_obj_t handle)
{
    solar_os_rtsp_client_status_t status;
    bool ended;
    python_check_esp(solar_os_script_media_rtsp_status(python_media_session, python_u32_from_obj(handle), &status, &ended));
    return solaros_streams_close(handle);
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_rtsp_close_obj, solaros_rtsp_close);

static mp_obj_t solaros_rtsp_lateness(mp_obj_t handle, mp_obj_t frame)
{
    int64_t lateness;
    python_check_esp(solar_os_script_media_rtsp_lateness(python_media_session,
        python_u32_from_obj(handle), python_u32_from_obj(frame), &lateness));
    return python_i64_to_obj(lateness);
}
MP_DEFINE_CONST_FUN_OBJ_2(solaros_rtsp_lateness_obj, solaros_rtsp_lateness);
#endif

#if SOLAR_OS_PACKAGE_SERVICE_IMAGE
static size_t python_image_free_slot(void)
{
    for (size_t i = 0; i < PYTHON_RASTER_IMAGE_MAX; ++i) if (!python_app.images[i]) return i;
    mp_raise_ValueError(MP_ERROR_TEXT("too many open images"));
}

static mp_obj_t solaros_image_decode(mp_obj_t data)
{
    const size_t slot = python_image_free_slot();
    mp_buffer_info_t buffer;
    mp_get_buffer_raise(data, &buffer, MP_BUFFER_READ);
    python_check_esp(solar_os_raster_image_decode(buffer.buf, buffer.len, &python_app.images[slot]));
    return mp_obj_new_int_from_uint(slot + 1);
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_image_decode_obj, solaros_image_decode);

static mp_obj_t solaros_image_from_frame(mp_obj_t handle)
{
    const size_t slot = python_image_free_slot();
    solar_os_script_media_frame_t frame;
    python_check_esp(solar_os_script_media_frame(python_media_session, python_u32_from_obj(handle), &frame));
    python_check_esp(solar_os_raster_image_decode(frame.jpeg.data, frame.jpeg.length, &python_app.images[slot]));
    return mp_obj_new_int_from_uint(slot + 1);
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_image_from_frame_obj, solaros_image_from_frame);

static mp_obj_t solaros_image_present(size_t n, const mp_obj_t *args)
{
    if (n == 4) mp_raise_ValueError(MP_ERROR_TEXT("width and height must be supplied together"));
    solar_os_raster_image_t *image = python_app.images[python_image_slot_from_obj(args[0])];
    const python_event_t event = {
        .type = PYTHON_EVENT_IMAGE_DRAW, .attr = 1,
        .x0 = python_i32_from_obj(args[1]), .y0 = python_i32_from_obj(args[2]),
        .width = n == 5 ? python_u16_from_size(python_size_from_obj(args[3])) : 0,
        .height = n == 5 ? python_u16_from_size(python_size_from_obj(args[4])) : 0,
        .object = (uintptr_t)image,
    };
    if (event.x0 < 0 || event.y0 < 0 || (n == 5 && (!event.width || !event.height)))
        mp_raise_ValueError(MP_ERROR_TEXT("invalid presentation rectangle"));
    solar_os_raster_image_retain(image);
    if (!python_send_event(&event)) {
        solar_os_raster_image_release(image);
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("ui event queue stopped"));
    }
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_image_present_obj, 3, 5, solaros_image_present);
#endif

#if SOLAR_OS_PACKAGE_SERVICE_VISION
static mp_obj_t python_vision_result(const solar_os_vision_qr_results_t *result)
{
    mp_obj_t d = mp_obj_new_dict(0);
    python_dict_store_uint(d, "width", result->width);
    python_dict_store_uint(d, "height", result->height);
    python_dict_store_uint(d, "processed_width", result->processed_width);
    python_dict_store_uint(d, "processed_height", result->processed_height);
    python_dict_store_uint(d, "candidates", result->candidates);
    python_dict_store_uint(d, "decode_failures", result->decode_failures);
    python_dict_store_bool(d, "truncated", result->truncated);
    python_dict_store_u64(d, "preprocess_us", result->preprocess_us);
    python_dict_store_u64(d, "detect_us", result->detect_us);
    python_dict_store_u64(d, "decode_us", result->decode_us);
    python_dict_store_u64(d, "elapsed_us", result->elapsed_us);
    mp_obj_t codes = mp_obj_new_list(0, NULL);
    mp_obj_dict_store(d, mp_obj_new_str("codes", 5), codes);
    for (size_t i = 0; i < result->count; ++i) {
        const solar_os_vision_qr_code_t *code = &result->codes[i];
        mp_obj_t entry = mp_obj_new_dict(0);
        mp_obj_dict_store(entry, mp_obj_new_str("payload", 7),
            mp_obj_new_bytes(code->payload ? code->payload : (const uint8_t *)"", code->length));
        python_dict_store_uint(entry, "eci", code->eci);
        python_dict_store_uint(entry, "version", code->version);
        python_dict_store_uint(entry, "ecc_level", code->ecc_level);
        python_dict_store_uint(entry, "data_type", code->data_type);
        mp_obj_t points[4];
        for (size_t j = 0; j < 4; ++j) {
            mp_obj_t xy[] = {mp_obj_new_int(code->corners[j].x), mp_obj_new_int(code->corners[j].y)};
            points[j] = mp_obj_new_tuple(2, xy);
        }
        mp_obj_dict_store(entry, mp_obj_new_str("corners", 7), mp_obj_new_tuple(4, points));
        mp_obj_list_append(codes, entry);
    }
    return d;
}

static mp_obj_t solaros_vision_qrcodes(size_t n, const mp_obj_t *args)
{
    solar_os_raster_image_t *image = python_app.images[python_image_slot_from_obj(args[0])];
    const mp_obj_t config = n > 1 ? args[1] : mp_const_none;
    if (config != mp_const_none) {
        if (!mp_obj_is_type(config, &mp_type_dict)) mp_raise_TypeError(MP_ERROR_TEXT("expected dict"));
        mp_obj_dict_t *dict = MP_OBJ_TO_PTR(config);
        for (size_t i = 0; i < dict->map.alloc; ++i) {
            if (!mp_map_slot_is_filled(&dict->map, i)) continue;
            const char *key = mp_obj_str_get_str(dict->map.table[i].key);
            if (strcmp(key, "x") && strcmp(key, "y") && strcmp(key, "width") &&
                strcmp(key, "height") && strcmp(key, "output_width") && strcmp(key, "output_height"))
                mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT("unsupported vision option %s"), key);
        }
    }
    const solar_os_raster_image_convert_options_t options = {
        .x = python_media_option(config, "x", 0, UINT32_MAX),
        .y = python_media_option(config, "y", 0, UINT32_MAX),
        .width = python_media_option(config, "width", 0, UINT32_MAX),
        .height = python_media_option(config, "height", 0, UINT32_MAX),
        .output_width = python_media_option(config, "output_width", 0, SOLAR_OS_VISION_MAX_WIDTH),
        .output_height = python_media_option(config, "output_height", 0, SOLAR_OS_VISION_MAX_HEIGHT),
    };
    solar_os_vision_qr_results_t *result = NULL;
    python_check_esp(solar_os_vision_qrcodes(image, &options, python_should_cancel, NULL, &result));
    /* Interpreter allocation can throw; release native results in either path. */
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_obj_t value = python_vision_result(result);
        nlr_pop();
        solar_os_vision_qr_results_free(result);
        return value;
    }
    solar_os_vision_qr_results_free(result);
    nlr_jump(nlr.ret_val);
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_vision_qrcodes_obj, 1, 2, solaros_vision_qrcodes);
#endif

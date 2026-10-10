#if SOLAR_OS_PACKAGE_SERVICE_IMLIB
#include "solar_os_imlib.h"
static bool python_imlib_inflight;
static bool python_imlib_active(void)
{ return __atomic_load_n(&python_imlib_inflight, __ATOMIC_ACQUIRE); }
static mp_obj_t python_imlib_option(mp_obj_t options, const char *name)
{ return options == mp_const_none ? MP_OBJ_NULL : python_get_dict_obj(options, name, false); }
static uint32_t python_imlib_uint(mp_obj_t options, const char *name, uint32_t fallback)
{
    mp_obj_t value = python_imlib_option(options, name);
    return value == MP_OBJ_NULL ? fallback : python_u32_from_obj(value);
}
static bool python_imlib_bool(mp_obj_t options, const char *name, bool fallback)
{
    mp_obj_t value = python_imlib_option(options, name);
    if (value == MP_OBJ_NULL) return fallback;
    if (value != mp_const_true && value != mp_const_false)
        mp_raise_TypeError(MP_ERROR_TEXT("vision boolean option required"));
    return value == mp_const_true;
}
static solar_os_imlib_options_t python_imlib_options(mp_obj_t options,
    solar_os_imlib_operation_t op)
{
    solar_os_imlib_options_t o = solar_os_imlib_default_options();
    if (options == mp_const_none) return o;
    if (!mp_obj_is_type(options, &mp_type_dict)) mp_raise_TypeError(MP_ERROR_TEXT("expected vision options dict"));
    mp_obj_dict_t *dict = MP_OBJ_TO_PTR(options);
    for (size_t i = 0; i < dict->map.alloc; ++i) {
        if (!mp_map_slot_is_filled(&dict->map, i)) continue;
        size_t len; const char *key = mp_obj_str_get_data(dict->map.table[i].key, &len);
        if (memchr(key, 0, len) || !solar_os_imlib_option_allowed(op, key))
            mp_raise_ValueError(MP_ERROR_TEXT("unsupported vision option"));
    }
    mp_obj_t format = python_imlib_option(options, "format");
    if (format != MP_OBJ_NULL) {
        size_t len; const char *text = mp_obj_str_get_data(format, &len);
        if (len == 4 && !memcmp(text, "gray", 4)) o.image.format = SOLAR_OS_RASTER_IMAGE_GRAY8;
        else if (len == 6 && !memcmp(text, "rgb565", 6)) o.image.format = SOLAR_OS_RASTER_IMAGE_RGB565_LE;
        else mp_raise_ValueError(MP_ERROR_TEXT("format must be gray or rgb565"));
    }
    o.image.x = python_imlib_uint(options, "x", o.image.x);
    o.image.y = python_imlib_uint(options, "y", o.image.y);
    o.image.width = python_imlib_uint(options, "width", o.image.width);
    o.image.height = python_imlib_uint(options, "height", o.image.height);
    o.image.output_width = python_imlib_uint(options, "output_width", o.image.output_width);
    o.image.output_height = python_imlib_uint(options, "output_height", o.image.output_height);
    o.timeout_ms = python_imlib_uint(options, "timeout_ms", o.timeout_ms);
    o.ksize = python_imlib_uint(options, "ksize", o.ksize);
    o.bins = python_imlib_uint(options, "bins", o.bins);
    o.x_stride = python_imlib_uint(options, "x_stride", o.x_stride);
    o.y_stride = python_imlib_uint(options, "y_stride", o.y_stride);
    o.area_threshold = python_imlib_uint(options, "area_threshold", o.area_threshold);
    o.pixels_threshold = python_imlib_uint(options, "pixels_threshold", o.pixels_threshold);
    o.margin = python_imlib_uint(options, "margin", o.margin);
    o.max_blobs = python_imlib_uint(options, "max_blobs", o.max_blobs);
    o.invert = python_imlib_bool(options, "invert", o.invert);
    o.merge = python_imlib_bool(options, "merge", o.merge);
    mp_obj_t thresholds = python_imlib_option(options, "thresholds");
    if (thresholds != MP_OBJ_NULL) {
        size_t count; mp_obj_t *items; mp_obj_get_array(thresholds, &count, &items);
        if (count > SOLAR_OS_IMLIB_THRESHOLDS_MAX) mp_raise_ValueError(MP_ERROR_TEXT("too many thresholds"));
        o.threshold_count = count;
        for (size_t i = 0; i < count; ++i) {
            size_t n; mp_obj_t *values; mp_obj_get_array(items[i], &n, &values);
            if (n != (o.image.format == SOLAR_OS_RASTER_IMAGE_GRAY8 ? 2U : 6U))
                mp_raise_ValueError(MP_ERROR_TEXT("expected gray pair or LAB six limits"));
            int16_t v[6] = {0};
            for (size_t j = 0; j < n; ++j) {
                mp_int_t x = mp_obj_get_int(values[j]);
                if (x < -128 || x > 255) mp_raise_ValueError(MP_ERROR_TEXT("threshold out of range"));
                v[j] = x;
            }
            o.thresholds[i] = (solar_os_imlib_threshold_t){v[0],v[1],v[2],v[3],v[4],v[5]};
        }
    }
    return o;
}
static mp_obj_t python_imlib_result(const solar_os_imlib_result_t *r)
{
    mp_obj_t d = mp_obj_new_dict(0);
    python_dict_store_cstr(d, "operation", solar_os_imlib_operation_name(r->operation));
    python_dict_store_cstr(d, "format", r->image_options.format == SOLAR_OS_RASTER_IMAGE_GRAY8 ? "gray" : "rgb565");
    python_dict_store_uint(d, "processed_width", r->image_options.output_width);
    python_dict_store_uint(d, "processed_height", r->image_options.output_height);
    python_dict_store_u64(d, "preprocess_us", r->preprocess_us);
    python_dict_store_u64(d, "process_us", r->process_us);
    python_dict_store_u64(d, "output_us", r->output_us);
    python_dict_store_u64(d, "elapsed_us", r->elapsed_us);
    python_dict_store_uint(d, "workspace_peak_bytes", r->workspace_peak_bytes);
    mp_obj_t roi[] = {mp_obj_new_int_from_uint(r->image_options.x), mp_obj_new_int_from_uint(r->image_options.y),
        mp_obj_new_int_from_uint(r->image_options.width), mp_obj_new_int_from_uint(r->image_options.height)};
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(qstr_from_str("roi")), mp_obj_new_tuple(4, roi));
    if (r->operation <= SOLAR_OS_IMLIB_STATISTICS) {
        mp_obj_t channels = mp_obj_new_dict(0);
        for (unsigned i = 0; i < r->channels; ++i) {
            const char *name = r->channels == 1 ? "gray" : i == 0 ? "l" : i == 1 ? "a" : "b";
            mp_obj_t values;
            if (r->operation == SOLAR_OS_IMLIB_HISTOGRAM) {
                values = mp_obj_new_list(0, NULL);
                for (unsigned j = 0; j < r->bins[i]; ++j)
                    mp_obj_list_append(values, mp_obj_new_float(r->histogram[i][j]));
            } else {
                values = mp_obj_new_dict(0);
                const solar_os_imlib_statistics_t *s = &r->statistics[i];
                python_dict_store_int(values, "mean", s->mean); python_dict_store_int(values, "median", s->median);
                python_dict_store_int(values, "mode", s->mode); python_dict_store_int(values, "stdev", s->stdev);
                python_dict_store_int(values, "min", s->min); python_dict_store_int(values, "max", s->max);
                python_dict_store_int(values, "lower_quartile", s->lower_quartile);
                python_dict_store_int(values, "upper_quartile", s->upper_quartile);
            }
            mp_obj_dict_store(channels, MP_OBJ_NEW_QSTR(qstr_from_str(name)), values);
        }
        mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(qstr_from_str("channels")), channels);
    }
    if (r->operation == SOLAR_OS_IMLIB_BLOBS) {
        mp_obj_t blobs = mp_obj_new_list(0, NULL);
        for (size_t i = 0; i < r->count; ++i) {
            const solar_os_imlib_blob_t *b = &r->blobs[i]; mp_obj_t blob = mp_obj_new_dict(0);
            python_dict_store_uint(blob, "x", b->x); python_dict_store_uint(blob, "y", b->y);
            python_dict_store_uint(blob, "width", b->width); python_dict_store_uint(blob, "height", b->height);
            python_dict_store_uint(blob, "pixels", b->pixels); python_dict_store_uint(blob, "code", b->code);
            python_dict_store_uint(blob, "count", b->count);
            mp_obj_dict_store(blob, MP_OBJ_NEW_QSTR(qstr_from_str("cx")), mp_obj_new_float(b->cx));
            mp_obj_dict_store(blob, MP_OBJ_NEW_QSTR(qstr_from_str("cy")), mp_obj_new_float(b->cy));
            mp_obj_dict_store(blob, MP_OBJ_NEW_QSTR(qstr_from_str("rotation")), mp_obj_new_float(b->rotation));
            mp_obj_list_append(blobs, blob);
        }
        mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(qstr_from_str("blobs")), blobs);
        mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(qstr_from_str("truncated")), mp_obj_new_bool(r->truncated));
        python_dict_store_cstr(d, "coordinates", "source_pixels");
    }
    return d;
}
static mp_obj_t python_imlib_call(mp_obj_t source, mp_obj_t reference, mp_obj_t options,
    solar_os_imlib_operation_t op, bool record)
{
    solar_os_raster_image_t *image = python_app.images[python_image_slot_from_obj(source)];
    solar_os_raster_image_t *other = reference == mp_const_none ? NULL :
        python_app.images[python_image_slot_from_obj(reference)];
    solar_os_imlib_options_t o = python_imlib_options(options, op);
    size_t slot = PYTHON_RASTER_IMAGE_MAX;
    if (solar_os_imlib_is_transform(op)) {
        for (size_t i = 0; i < PYTHON_RASTER_IMAGE_MAX; ++i) if (!python_app.images[i]) { slot = i; break; }
        if (slot == PYTHON_RASTER_IMAGE_MAX) mp_raise_ValueError(MP_ERROR_TEXT("too many open images"));
    }
    solar_os_imlib_result_t *r = NULL;
    __atomic_store_n(&python_imlib_inflight, true, __ATOMIC_RELEASE);
    esp_err_t error = solar_os_imlib_run(image, other, op, &o, python_should_cancel, NULL, &r);
    if (error) {
        __atomic_store_n(&python_imlib_inflight, false, __ATOMIC_RELEASE);
        python_check_esp(error);
    }
    nlr_buf_t guard;
    if (!nlr_push(&guard)) {
        mp_obj_t result = record || !r->image ? python_imlib_result(r) : mp_const_none;
        if (r->image) {
            mp_obj_t handle = mp_obj_new_int_from_uint(slot+1);
            if (record) mp_obj_dict_store(result, MP_OBJ_NEW_QSTR(qstr_from_str("image")), handle);
            else result = handle;
            python_app.images[slot] = r->image; r->image = NULL;
        }
        nlr_pop(); solar_os_imlib_result_free(r);
        __atomic_store_n(&python_imlib_inflight, false, __ATOMIC_RELEASE);
        return result;
    }
    solar_os_imlib_result_free(r);
    __atomic_store_n(&python_imlib_inflight, false, __ATOMIC_RELEASE);
    nlr_jump(guard.ret_val);
}
static mp_obj_t solaros_vision_process(size_t n, const mp_obj_t *args)
{
    size_t length; const char *name = mp_obj_str_get_data(args[1], &length);
    if (memchr(name, 0, length)) mp_raise_ValueError(MP_ERROR_TEXT("invalid operation"));
    for (unsigned i = 0; i < SOLAR_OS_IMLIB_OPERATIONS_COUNT; ++i)
        if (!strcmp(name, solar_os_imlib_operation_name(i))) return python_imlib_call(args[0],
            n > 3 ? args[3] : mp_const_none, n > 2 ? args[2] : mp_const_none, i, true);
    mp_raise_ValueError(MP_ERROR_TEXT("unknown vision operation"));
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_vision_process_obj, 2, 4, solaros_vision_process);
#define PYTHON_IMLIB_FUNCTION(name, op) \
    static mp_obj_t solaros_vision_##name(size_t n, const mp_obj_t *args) \
    { return python_imlib_call(args[0], mp_const_none, n > 1 ? args[1] : mp_const_none, op, false); } \
    MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_vision_##name##_obj, 1, 2, solaros_vision_##name)
PYTHON_IMLIB_FUNCTION(histogram, SOLAR_OS_IMLIB_HISTOGRAM);
PYTHON_IMLIB_FUNCTION(statistics, SOLAR_OS_IMLIB_STATISTICS);
PYTHON_IMLIB_FUNCTION(binary, SOLAR_OS_IMLIB_BINARY);
PYTHON_IMLIB_FUNCTION(invert, SOLAR_OS_IMLIB_INVERT);
PYTHON_IMLIB_FUNCTION(mean, SOLAR_OS_IMLIB_MEAN);
PYTHON_IMLIB_FUNCTION(gaussian, SOLAR_OS_IMLIB_GAUSSIAN);
PYTHON_IMLIB_FUNCTION(median, SOLAR_OS_IMLIB_MEDIAN);
PYTHON_IMLIB_FUNCTION(erode, SOLAR_OS_IMLIB_ERODE);
PYTHON_IMLIB_FUNCTION(dilate, SOLAR_OS_IMLIB_DILATE);
PYTHON_IMLIB_FUNCTION(opening, SOLAR_OS_IMLIB_OPENING);
PYTHON_IMLIB_FUNCTION(closing, SOLAR_OS_IMLIB_CLOSING);
PYTHON_IMLIB_FUNCTION(blobs, SOLAR_OS_IMLIB_BLOBS);
#undef PYTHON_IMLIB_FUNCTION
static mp_obj_t solaros_vision_difference(size_t n, const mp_obj_t *args)
{ return python_imlib_call(args[0], args[1], n > 2 ? args[2] : mp_const_none, SOLAR_OS_IMLIB_DIFFERENCE, false); }
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_vision_difference_obj, 2, 3, solaros_vision_difference);
#else
static bool python_imlib_active(void) { return false; }
#endif

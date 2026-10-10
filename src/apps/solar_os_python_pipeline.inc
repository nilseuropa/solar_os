#if SOLAR_OS_PACKAGE_SERVICE_PIPELINE
#include "solar_os_pipeline.h"
static uint32_t python_pipeline_uint(mp_obj_t value)
{
    mp_int_t n = mp_obj_get_int(value);
    if (n < 0) mp_raise_ValueError(MP_ERROR_TEXT("pipeline integer must be nonnegative"));
    return (uint32_t)n;
}
static mp_obj_t solaros_pipeline_start(size_t n, const mp_obj_t *args)
{
    size_t length; const char *source = mp_obj_str_get_data(args[0], &length);
    if (memchr(source, 0, length)) mp_raise_ValueError(MP_ERROR_TEXT("embedded NUL in source"));
    const char *kind = n > 1 ? mp_obj_str_get_data(args[1], &length) : "qr";
    if (n > 1 && memchr(kind, 0, length)) mp_raise_ValueError(MP_ERROR_TEXT("embedded NUL in processor"));
    solar_os_pipeline_config_t c = {.source = source, .processor = kind, .timeout_ms = 10000};
    if (n > 2) c.model = python_pipeline_uint(args[2]);
    if (n > 3) c.limit = python_pipeline_uint(args[3]);
    if (n > 4) c.interval_ms = python_pipeline_uint(args[4]);
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    if (source[0] == '/' || source[0] == '.') {
        python_resolve_path_obj(args[0], path, sizeof(path)); c.source = path;
    }
    uint32_t id; python_check_esp(solar_os_pipeline_start(&c, &id));
    return mp_obj_new_int_from_uint(id);
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_pipeline_start_obj, 1, 5, solaros_pipeline_start);
static mp_obj_t solaros_pipeline_list(void)
{
    uint32_t ids[SOLAR_OS_PIPELINES_MAX]; size_t count;
    python_check_esp(solar_os_pipeline_list(ids, SOLAR_OS_PIPELINES_MAX, &count));
    mp_obj_t result = mp_obj_new_list(0, NULL);
    for (size_t i = 0; i < count; ++i) mp_obj_list_append(result, mp_obj_new_int_from_uint(ids[i]));
    return result;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_pipeline_list_obj, solaros_pipeline_list);
static mp_obj_t solaros_pipeline_status(mp_obj_t id)
{
    solar_os_pipeline_status_t s;
    python_check_esp(solar_os_pipeline_status(python_pipeline_uint(id), &s));
    mp_obj_t d = mp_obj_new_dict(0);
    python_dict_store_uint(d, "id", s.id); python_dict_store_uint(d, "model", s.model);
    python_dict_store_cstr(d, "job", s.job); python_dict_store_cstr(d, "state", s.state);
    python_dict_store_cstr(d, "source", s.source); python_dict_store_cstr(d, "processor", s.processor);
    python_dict_store_u64(d, "sequence", s.sequence); python_dict_store_u64(d, "frames", s.frames);
    python_dict_store_u64(d, "busy_frames", s.busy_frames);
    python_dict_store_u64(d, "source_timestamp_us", s.source_timestamp_us);
    python_dict_store_u64(d, "decode_us", s.decode_us); python_dict_store_u64(d, "process_us", s.process_us);
    python_dict_store_u64(d, "frame_us", s.frame_us); python_dict_store_int(d, "last_error", s.last_error);
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(qstr_from_str("done")), mp_obj_new_bool(s.done));
    return d;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_pipeline_status_obj, solaros_pipeline_status);
static mp_obj_t solaros_pipeline_result(size_t n, const mp_obj_t *args)
{
    char *json = NULL;
    python_check_esp(solar_os_pipeline_result(python_pipeline_uint(args[0]),
        n > 1 ? python_pipeline_uint(args[1]) : 0, &json));
    if (!json) return mp_const_none;
    nlr_buf_t guard;
    if (nlr_push(&guard) == 0) {
        mp_obj_t module = mp_import_name(qstr_from_str("json"), mp_const_none, MP_OBJ_NEW_SMALL_INT(0));
        mp_obj_t result = mp_call_function_1(mp_load_attr(module, qstr_from_str("loads")), mp_obj_new_str(json, strlen(json)));
        nlr_pop(); solar_os_memory_free(json); return result;
    }
    solar_os_memory_free(json); nlr_jump(guard.ret_val);
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_pipeline_result_obj, 1, 2, solaros_pipeline_result);
static mp_obj_t solaros_pipeline_stop(mp_obj_t id)
{ python_check_esp(solar_os_pipeline_stop(python_pipeline_uint(id))); return mp_const_none; }
MP_DEFINE_CONST_FUN_OBJ_1(solaros_pipeline_stop_obj, solaros_pipeline_stop);
static mp_obj_t solaros_pipeline_destroy(mp_obj_t id)
{ python_check_esp(solar_os_pipeline_destroy(python_pipeline_uint(id))); return mp_const_none; }
MP_DEFINE_CONST_FUN_OBJ_1(solaros_pipeline_destroy_obj, solaros_pipeline_destroy);
#endif

#if SOLAR_OS_PACKAGE_SERVICE_IMAGE
#include "solar_os_tensor_image.h"
static void solua_tensor_image_vector(lua_State *L, const char *key, double *values, unsigned channels)
{
    solua_inference_field(L,4,key);
    if (lua_isnil(L,-1)) { lua_pop(L,1); return; }
    luaL_checktype(L,-1,LUA_TTABLE); size_t count = lua_rawlen(L,-1);
    if (count != 1 && count != channels) luaL_error(L,"expected one value or one per channel");
    for (unsigned c = 0; c < channels; ++c) {
        lua_rawgeti(L,-1,count == 1 ? 1 : c+1); luaL_checktype(L,-1,LUA_TNUMBER);
        values[c] = lua_tonumber(L,-1); lua_pop(L,1);
    }
    lua_pop(L,1);
}
static int solua_inference_prepare_image(lua_State *L)
{
    if (lua_gettop(L) != 3 && lua_gettop(L) != 4) return luaL_error(L,"expected model, input name, image [, options]");
    const solar_os_inference_model_info_t *info = solua_inference_info(L);
    const char *name = solua_inference_name(L,2); size_t index = 0;
    while (index < info->input_count && strcmp(name,info->inputs[index].name)) ++index;
    if (index == info->input_count) return luaL_error(L,"unknown input name");
    solar_os_raster_image_pixels_t source;
    (void)solua_check_esp(L,solar_os_raster_image_pixels(solua.images[solua_image_slot(L,3)],&source));
    solar_os_tensor_image_options_t options; solar_os_tensor_image_defaults(&options);
    if (!lua_isnoneornil(L,4)) {
        luaL_checktype(L,4,LUA_TTABLE); lua_pushnil(L);
        while (lua_next(L,4)) {
            if (lua_type(L,-2) != LUA_TSTRING) return luaL_error(L,"expected image option name");
            const char *key = solua_inference_name(L,-2);
            if (!strcmp(key,"layout") || !strcmp(key,"color") || !strcmp(key,"resize"))
                (void)solua_check_esp(L,solar_os_tensor_image_option(&options,key,solua_inference_name(L,-1)));
            else if (!strcmp(key,"x")) options.x = solua_inference_uint(L,-1);
            else if (!strcmp(key,"y")) options.y = solua_inference_uint(L,-1);
            else if (!strcmp(key,"width")) options.width = solua_inference_uint(L,-1);
            else if (!strcmp(key,"height")) options.height = solua_inference_uint(L,-1);
            else if (strcmp(key,"mean") && strcmp(key,"std") && strcmp(key,"pad"))
                return luaL_error(L,"unknown image option");
            lua_pop(L,1);
        }
        unsigned channels = options.color == SOLAR_OS_TENSOR_GRAY ? 1 : 3;
        solua_tensor_image_vector(L,"mean",options.mean,channels);
        solua_tensor_image_vector(L,"std",options.std,channels);
        double pad[3] = {0}; solua_tensor_image_vector(L,"pad",pad,channels);
        for (unsigned c = 0; c < channels; ++c) {
            if (!isfinite(pad[c]) || pad[c] < 0 || pad[c] > 255 || floor(pad[c]) != pad[c])
                return luaL_error(L,"pad values must be integers 0..255");
            options.pad[c] = pad[c];
        }
    }
    solar_os_tensor_image_transform_t transform;
    (void)solua_check_esp(L,solar_os_tensor_image_inspect(&source,&info->inputs[index],&options,&transform));
    luaL_Buffer buffer; char *data = luaL_buffinitsize(L,&buffer,transform.bytes);
    (void)solua_check_esp(L,solar_os_tensor_image_prepare(&source,&info->inputs[index],&options,
        (uint8_t *)data,transform.bytes,&transform,solua_should_cancel,NULL));
    luaL_pushresultsize(&buffer,transform.bytes); lua_newtable(L); lua_pushvalue(L,-2); lua_setfield(L,-2,"data");
    lua_newtable(L);
#define SOLUA_TENSOR_IMAGE_FIELD(field) solua_set_int(L,-1,#field,transform.field)
    SOLUA_TENSOR_IMAGE_FIELD(source_width); SOLUA_TENSOR_IMAGE_FIELD(source_height);
    SOLUA_TENSOR_IMAGE_FIELD(input_width); SOLUA_TENSOR_IMAGE_FIELD(input_height);
    SOLUA_TENSOR_IMAGE_FIELD(crop_x); SOLUA_TENSOR_IMAGE_FIELD(crop_y);
    SOLUA_TENSOR_IMAGE_FIELD(crop_width); SOLUA_TENSOR_IMAGE_FIELD(crop_height);
    SOLUA_TENSOR_IMAGE_FIELD(resized_width); SOLUA_TENSOR_IMAGE_FIELD(resized_height);
    SOLUA_TENSOR_IMAGE_FIELD(pad_left); SOLUA_TENSOR_IMAGE_FIELD(pad_top);
#undef SOLUA_TENSOR_IMAGE_FIELD
    lua_setfield(L,-2,"transform"); solua_set_int(L,-1,"preprocess_us",transform.preprocess_us);
    return 1;
}
#endif

#include "lua_runtime.h"

#include <math.h>
#include <string.h>
#include "solar_os_jet_abi.h"

#define JET_SCENE_TYPE "solaros.jet.scene"
#define FIELD_COUNT(table) (sizeof(table) / sizeof(*(table)))

static uint32_t jet_id(lua_State *L, int index, bool optional)
{
    return (uint32_t)solar_os_lua_integer(L, index, optional ? 0 : 1, UINT32_MAX, 0);
}

static solar_os_native_handle_t *jet_scene(lua_State *L, bool require_live)
{
    solar_os_native_handle_t *handle = luaL_checkudata(L, 1, JET_SCENE_TYPE);
    if (require_live && (!*handle || *handle != lua_runtime.scene)) luaL_error(L, "Jet scene is closed");
    return handle;
}

static int jet_close(lua_State *L)
{
    solar_os_native_handle_t *handle = jet_scene(L, false);
    if (!*handle || *handle != lua_runtime.scene) return 0;
    const int result = lua_runtime.jet->close(*handle);
    if (result) return solar_os_lua_check(L, result);

    *handle = 0;
    lua_runtime.scene = 0;
    return solar_os_lua_check(L, lua_runtime.foreground->console_gfx_release());
}

static int jet_open(lua_State *L)
{
    if (lua_runtime.surface || lua_runtime.scene) return luaL_error(L, "display is already in use");
    const solar_os_jet_options_t options = {
        .display = luaL_optstring(L, 1, NULL),
        .fps = (uint16_t)solar_os_lua_integer(L, 2, 0, 30, 0),
    };
    solar_os_native_handle_t *handle = lua_newuserdatauv(L, sizeof(*handle), 0);
    *handle = 0;
    luaL_setmetatable(L, JET_SCENE_TYPE);

    solar_os_gfx_t *surface = NULL;
    solar_os_lua_check(L, lua_runtime.foreground->console_gfx_acquire(&surface));
    char detail[128] = {0};
    const int result = lua_runtime.jet->open(&options, 0, &lua_runtime.scene, detail, sizeof(detail));
    if (result) {
        if (!lua_runtime.scene) (void)lua_runtime.foreground->console_gfx_release();
        if (*detail) return luaL_error(L, "%s", detail);
        return solar_os_lua_check(L, result);
    }
    *handle = lua_runtime.scene;
    return 1;
}

static int jet_background(lua_State *L)
{
    const solar_os_native_handle_t handle = *jet_scene(L, true);
    const uint32_t color = solar_os_lua_integer(L, 2, 0, 0xffffff, 0xffffff);
    return solar_os_lua_check(L, lua_runtime.jet->background(handle, color));
}

static solar_os_jet_pose_t jet_pose(lua_State *L, int index, int default_z)
{
    return (solar_os_jet_pose_t){
        solar_os_lua_integer(L, index, -4096, 4096, 0),
        solar_os_lua_integer(L, index + 1, -4096, 4096, 0),
        solar_os_lua_integer(L, index + 2, -4096, 4096, default_z),
        solar_os_lua_integer(L, index + 3, INT32_MIN, INT32_MAX, 0),
        solar_os_lua_integer(L, index + 4, INT32_MIN, INT32_MAX, 0),
        solar_os_lua_integer(L, index + 5, INT32_MIN, INT32_MAX, 0),
    };
}

static int jet_camera(lua_State *L)
{
    const solar_os_native_handle_t handle = *jet_scene(L, true);
    const solar_os_jet_pose_t pose = jet_pose(L, 2, -500);
    const uint16_t fov = solar_os_lua_integer(L, 8, 10, 150, 60);
    return solar_os_lua_check(L, lua_runtime.jet->camera(handle, &pose, fov));
}

static int jet_light(lua_State *L)
{
    const solar_os_native_handle_t handle = *jet_scene(L, true);
    const uint32_t ambient = solar_os_lua_integer(L, 2, 0, 0xffffff, 0x5a5a5a);
    const uint32_t color = solar_os_lua_integer(L, 3, 0, 0xffffff, 0xfff5e6);
    const uint16_t intensity = solar_os_lua_integer(L, 4, 0, 255, 220);
    const int32_t azimuth = solar_os_lua_integer(L, 5, INT32_MIN, INT32_MAX, 35);
    const int32_t elevation = solar_os_lua_integer(L, 6, INT32_MIN, INT32_MAX, 40);
    return solar_os_lua_check(L, lua_runtime.jet->light(handle, ambient, color, intensity, azimuth, elevation));
}

static int jet_transform(lua_State *L)
{
    const solar_os_native_handle_t handle = *jet_scene(L, true);
    const uint32_t object = jet_id(L, 2, false);
    const solar_os_jet_pose_t pose = jet_pose(L, 3, 0);
    return solar_os_lua_check(L, lua_runtime.jet->transform(handle, object, &pose));
}

static int jet_remove(lua_State *L)
{
    const solar_os_native_handle_t handle = *jet_scene(L, true);
    return solar_os_lua_check(L, lua_runtime.jet->remove(handle, jet_id(L, 2, false)));
}

static bool jet_cancelled(void *user)
{
    (void)user;
    return solar_os_lua_stopping();
}

static int jet_render(lua_State *L)
{
    return solar_os_lua_check(L, lua_runtime.jet->render(*jet_scene(L, true), jet_cancelled, NULL));
}

static int jet_status(lua_State *L)
{
    solar_os_jet_status_t status;
    solar_os_lua_check(L, lua_runtime.jet->status(*jet_scene(L, true), &status));
    lua_createtable(L, 0, 5);
    solar_os_lua_set_number(L, "width", status.width);
    solar_os_lua_set_number(L, "height", status.height);
    solar_os_lua_set_number(L, "frames", status.frames);
    solar_os_lua_set_number(L, "presented", status.presented);
    solar_os_lua_set_number(L, "dropped", status.dropped);
    return 1;
}

static int jet_capabilities(lua_State *L)
{
    const solar_os_jet_command_t query = {.opcode = SOLAR_OS_JET_INFO};
    solar_os_jet_reply_t reply = {0};
    const int result = lua_runtime.jet->execute(*jet_scene(L, true), &query, &reply);
    if (result != SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED) solar_os_lua_check(L, result);
    lua_createtable(L, 0, 4);
    lua_pushboolean(L, reply.handle & SOLAR_OS_JET_FEATURE_GEOMETRY);
    lua_setfield(L, -2, "geometry");
    lua_pushboolean(L, reply.handle & SOLAR_OS_JET_FEATURE_SCENE);
    lua_setfield(L, -2, "scene");
    lua_pushboolean(L, reply.handle & SOLAR_OS_JET_FEATURE_MATERIALS);
    lua_setfield(L, -2, "materials");
    lua_pushboolean(L, reply.handle & SOLAR_OS_JET_FEATURE_BACKGROUND_CACHE);
    lua_setfield(L, -2, "background_cache");
    return 1;
}

static void jet_options(lua_State *L, int index, solar_os_jet_command_t *query,
                        const solar_os_jet_field_t *fields, size_t count)
{
    for (size_t i = 0; i < count; ++i) query->values[i] = fields[i].initial;
    if (lua_isnoneornil(L, index)) return;
    luaL_checktype(L, index, LUA_TTABLE);
    lua_pushnil(L);
    while (lua_next(L, index)) {
        luaL_checktype(L, -2, LUA_TSTRING);
        const char *name = luaL_checkstring(L, -2);
        size_t i = 0;
        while (i < count && strcmp(fields[i].name, name)) ++i;
        if (i == count) luaL_error(L, "unknown Jet option '%s'", name);
        const solar_os_jet_field_t *field = &fields[i];
        query->fields |= 1U << i;
        switch (field->kind) {
        case JET_NAME: {
            size_t length;
            const char *value = luaL_checklstring(L, -1, &length);
            if (length >= sizeof(query->name) || memchr(value, 0, length))
                luaL_error(L, "invalid Jet name");
            memcpy(query->name, value, length);
            break;
        }
        case JET_ENUM: {
            size_t length;
            const char *value = luaL_checklstring(L, -1, &length);
            query->values[i] = solar_os_jet_choice(field->choices, value, length);
            break;
        }
        case JET_BOOL:
            luaL_checktype(L, -1, LUA_TBOOLEAN);
            query->values[i] = lua_toboolean(L, -1);
            break;
        case JET_FLOAT:
            query->values[i] = luaL_checknumber(L, -1);
            break;
        default:
            query->values[i] = luaL_checkinteger(L, -1);
            break;
        }
        double value = query->values[i];
        if (!isfinite(value) || value < field->minimum || value > field->maximum)
            luaL_error(L, "Jet option '%s' is out of range", name);
        lua_pop(L, 1);
    }
}

static int jet_options_command(lua_State *L, uint32_t opcode, bool update,
                                const solar_os_jet_field_t *fields, size_t count)
{
    solar_os_jet_command_t query = {.opcode = opcode};
    if (update) query.handle = jet_id(L, 2, false);
    jet_options(L, update ? 3 : 2, &query, fields, count);
    solar_os_jet_reply_t reply;
    solar_os_lua_check(L, lua_runtime.jet->execute(*jet_scene(L, true), &query, &reply));
    if (update || opcode == SOLAR_OS_JET_SCENE_SET) return 0;
    lua_pushinteger(L, reply.handle);
    return 1;
}

static int jet_material(lua_State *L)
{
    return jet_options_command(L, SOLAR_OS_JET_MATERIAL, false, jet_material_fields, JET_MAT_COUNT);
}

static int jet_material_set(lua_State *L)
{
    return jet_options_command(L, SOLAR_OS_JET_MATERIAL_SET, true, jet_material_fields, JET_MAT_COUNT);
}

static int jet_primitive(lua_State *L)
{
    return jet_options_command(L, SOLAR_OS_JET_PRIMITIVE, false, jet_primitive_fields, JET_PRIM_COUNT);
}

static int jet_object_set(lua_State *L)
{
    return jet_options_command(L, SOLAR_OS_JET_OBJECT_SET, true, jet_object_fields, JET_OBJ_COUNT);
}

static int jet_settings(lua_State *L)
{
    return jet_options_command(L, SOLAR_OS_JET_SCENE_SET, false, jet_scene_fields, JET_SCENE_COUNT);
}

static int jet_texture(lua_State *L)
{
    const solar_os_native_handle_t handle = *jet_scene(L, true);
    solar_os_jet_command_t query = {.opcode = SOLAR_OS_JET_TEXTURE};
    query.values[16] = solar_os_lua_integer(L, 2, 1, 1024, 0);
    query.values[17] = solar_os_lua_integer(L, 3, 1, 1024, 0);
    query.data = luaL_checklstring(L, 4, &query.bytes);
    uint32_t palette[256];
    if (!lua_isnoneornil(L, 5)) {
        luaL_checktype(L, 5, LUA_TTABLE);
        lua_getfield(L, 5, "palette");
        if (!lua_isnil(L, -1)) {
            luaL_checktype(L, -1, LUA_TTABLE);
            size_t count = lua_rawlen(L, -1);
            if (!count || count > FIELD_COUNT(palette)) return luaL_error(L, "invalid Jet palette");
            for (size_t i = 0; i < count; ++i) {
                lua_rawgeti(L, -1, i + 1);
                palette[i] = solar_os_lua_integer(L, -1, 0, 0xffffff, 0);
                lua_pop(L, 1);
            }
            query.extra = palette;
            query.extra_bytes = count * sizeof(*palette);
        }
        lua_pop(L, 1);
        /* Parse scalar options using the shared schema. The palette is
         * already copied above and is passed through the command payload. */
        lua_createtable(L, 0, JET_TEX_COUNT);
        int scalar = lua_gettop(L);
        lua_pushnil(L);
        while (lua_next(L, 5)) {
            luaL_checktype(L, -2, LUA_TSTRING);
            const char *name = luaL_checkstring(L, -2);
            if (strcmp(name, "palette")) {
                if (!strcmp(name, "pixels")) return luaL_error(L, "duplicate Jet pixels");
                lua_pushvalue(L, -1);
                lua_setfield(L, scalar, name);
            }
            lua_pop(L, 1);
        }
        jet_options(L, scalar, &query, jet_texture_fields, JET_TEX_COUNT);
        if (query.extra) query.fields |= 1U << JET_TEX_palette;
    } else {
        jet_options(L, 5, &query, jet_texture_fields, JET_TEX_COUNT);
    }
    solar_os_jet_reply_t reply;
    solar_os_lua_check(L, lua_runtime.jet->execute(handle, &query, &reply));
    lua_pushinteger(L, reply.handle);
    return 1;
}

static int jet_mesh(lua_State *L)
{
    const solar_os_native_handle_t handle = *jet_scene(L, true);
    luaL_checktype(L, 2, LUA_TTABLE);
    luaL_checktype(L, 3, LUA_TTABLE);
    const size_t vertices_count = lua_rawlen(L, 2), faces_count = lua_rawlen(L, 3);
    if (!vertices_count || vertices_count > SOLAR_OS_JET_MAX_VERTICES ||
        !faces_count || faces_count > SOLAR_OS_JET_MAX_TRIANGLES)
        return luaL_error(L, "invalid Jet mesh size");
    const uint32_t material = jet_id(L, 4, true);
    solar_os_jet_mesh_vertex_t *vertices = lua_newuserdatauv(L, vertices_count * sizeof(*vertices), 0);
    solar_os_jet_mesh_face_t *faces = lua_newuserdatauv(L, faces_count * sizeof(*faces), 0);

    size_t stride = 0;
    for (size_t i = 0; i < vertices_count; ++i) {
        lua_rawgeti(L, 2, i + 1);
        luaL_checktype(L, -1, LUA_TTABLE);
        const size_t width = lua_rawlen(L, -1);
        if ((width != 3 && width != 5 && width != 8) || (stride && stride != width))
            return luaL_error(L, "expected consistent xyz, xyzuv, or xyzuv-normal vertices");
        stride = width;
        int32_t values[8] = {0};
        for (size_t k = 0; k < width; ++k) {
            lua_rawgeti(L, -1, k + 1);
            if (k < 3) {
                values[k] = solar_os_lua_integer(L, -1, -4096, 4096, 0);
            } else {
                const double value = luaL_checknumber(L, -1);
                const double limit = k < 5 ? 32.0 : 1.0;
                if (!isfinite(value) || value < -limit || value > limit)
                    return luaL_error(L, "Jet vector out of range");
                values[k] = (int32_t)(value * 1024);
            }
            lua_pop(L, 1);
        }
        vertices[i] = (solar_os_jet_mesh_vertex_t){
            values[0], values[1], values[2], values[3], values[4], values[5], values[6], values[7],
        };
        lua_pop(L, 1);
    }
    for (size_t i = 0; i < faces_count; ++i) {
        lua_rawgeti(L, 3, i + 1);
        luaL_checktype(L, -1, LUA_TTABLE);
        const size_t width = lua_rawlen(L, -1);
        if (width != 3 && width != 4) return luaL_error(L, "invalid Jet mesh face");
        uint16_t indices[3];
        for (unsigned k = 0; k < 3; ++k) {
            lua_rawgeti(L, -1, k + 1);
            indices[k] = (uint16_t)(solar_os_lua_integer(L, -1, 1, vertices_count, 0) - 1);
            lua_pop(L, 1);
        }
        lua_rawgeti(L, -1, 4);
        const uint32_t face_material = lua_isnil(L, -1) ? material : jet_id(L, -1, true);
        lua_pop(L, 2);
        faces[i] = (solar_os_jet_mesh_face_t){indices[0], indices[1], indices[2], 0, face_material};
    }
    const solar_os_jet_command_t query = {
        .opcode = SOLAR_OS_JET_MESH_DATA,
        .fields = stride == 8 ? SOLAR_OS_JET_MESH_NORMALS : 0,
        .data = vertices, .bytes = vertices_count * sizeof(*vertices),
        .extra = faces, .extra_bytes = faces_count * sizeof(*faces),
    };
    solar_os_jet_reply_t reply;
    solar_os_lua_check(L, lua_runtime.jet->execute(handle, &query, &reply));
    lua_pushinteger(L, reply.handle);
    return 1;
}

void solar_os_lua_jet(lua_State *L)
{
    if (!lua_runtime.jet) return;
    static const luaL_Reg methods[] = {
        {"close", jet_close}, {"background", jet_background}, {"camera", jet_camera}, {"light", jet_light},
        {"capabilities", jet_capabilities}, {"status", jet_status}, {"transform", jet_transform},
        {"remove", jet_remove}, {"render", jet_render}, {"material", jet_material},
        {"material_set", jet_material_set}, {"texture", jet_texture}, {"mesh_data", jet_mesh},
        {"primitive", jet_primitive}, {"object_set", jet_object_set}, {"settings", jet_settings}, {NULL, NULL},
    };
    luaL_newmetatable(L, JET_SCENE_TYPE);
    luaL_setfuncs(L, methods, 0);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);

    static const luaL_Reg jet[] = {{"open", jet_open}, {NULL, NULL}};
    luaL_newlib(L, jet);
    lua_setfield(L, -2, "jet");
}

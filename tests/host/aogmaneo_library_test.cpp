#include "solar_os_aogmaneo_abi.h"
#include "solar_os_native_library_abi.h"
#include "vendor/aogmaneo/hierarchy.h"
#include "vendor/aogmaneo/image_encoder.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

extern "C" const solar_os_aogmaneo_api_v1_t *solar_os_aogmaneo_engine_api_v1(void);
extern "C" bool solar_os_aogmaneo_engine_idle(void);
extern "C" int app_main(int, char **);
static const auto *api = solar_os_aogmaneo_engine_api_v1();
static long requests, fail_at = -1, live;
static bool reenter;

void *operator new(size_t size)
{
    if (reenter) {
        reenter = false;
        assert(api->destroy(0) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    }
    if (requests++ == fail_at) throw std::bad_alloc();
    void *p = std::malloc(size ? size : 1);
    if (!p) throw std::bad_alloc();
    ++live;
    return p;
}
void operator delete(void *p) noexcept { if (p) { --live; std::free(p); } }
void operator delete(void *p, size_t) noexcept { operator delete(p); }
void *operator new[](size_t n) { return operator new(n); }
void operator delete[](void *p) noexcept { operator delete(p); }
void operator delete[](void *p, size_t) noexcept { operator delete(p); }

static solar_os_native_library_descriptor_v1_t descriptor;
static int registrations, cleanups, binds;
static int register_library(const solar_os_native_library_descriptor_v1_t *value)
{
    ++registrations;
    descriptor = *value;
    return 0;
}
static const void *get_service(const char *, uint32_t, uint32_t) { return nullptr; }
static solar_os_native_library_host_api_v1_t host = {1, sizeof(host), "esp32s3", "4.18.1",
    register_library, get_service};
extern "C" const solar_os_native_library_host_api_v1_t *solar_os_native_library_host_v1(void) { return &host; }
extern "C" int solar_os_espdl_port_bind(const solar_os_native_library_host_api_v1_t *) { ++binds; return 0; }
extern "C" void solar_os_espdl_port_cleanup(void) { ++cleanups; }
extern "C" {
unsigned char __module_eh_start[4] = {};
void (*__module_ctor_start[1])() = {}, (*__module_ctor_end[1])() = {};
void (*__module_dtor_start[1])() = {}, (*__module_dtor_end[1])() = {};
}

static solar_os_aogmaneo_io_v1_t ios[] = {
    {{2, 1, 4}, SOLAR_OS_AOGMANEO_PREDICTION, 2, 1, 1, 8, 2, 4},
    {{1, 1, 3}, SOLAR_OS_AOGMANEO_INPUT, 2, 1, 1, 8, 2, 4},
    {{1, 1, 4}, SOLAR_OS_AOGMANEO_ACTION, 2, 1, 1, 8, 2, 4},
};
static solar_os_aogmaneo_layer_v1_t layers[] = {
    {{2, 2, 8}, 2, 1, 0, 1}, {{1, 2, 8}, 2, 1, -1, 1},
};
static solar_os_aogmaneo_visible_v1_t visible[] = {{{4, 4, 1}, 1}, {{2, 2, 3}, 1}};
static int create(uint32_t *out) { return api->create(ios, 3, layers, 2, 12345, out); }
static int image_create(uint32_t *out) { return api->image_create({2, 2, 8}, visible, 2, 12345, out); }

template<class T, class U> static void equal(const aon::Array<T> &expected, const U *actual, size_t count)
{
    assert(count == size_t(expected.size()));
    for (size_t i = 0; i < count; ++i) assert(expected[int(i)] == actual[i]);
}

static void hierarchy_parity()
{
    uint32_t model = 0, other = 0;
    reenter = true;
    assert(create(&model) == 0 && model);
    assert(create(&other) == 0 && other != model);
    assert(descriptor.shutdown() == SOLAR_OS_NATIVE_ERROR_INVALID_STATE && !cleanups);
    aon::Array<aon::Hierarchy::IO_Desc> io_descs(3);
    aon::Array<aon::Hierarchy::Layer_Desc> layer_descs(2);
    aon::Array<aon::Int_Buffer> codes(3);
    aon::Array<aon::Int_Buffer_View> views(3);
    for (int i = 0; i < 3; ++i) {
        const auto &d = ios[i];
        io_descs[i] = {{d.size.x, d.size.y, d.size.z}, aon::IO_Type(d.type),
            d.dendrites, d.up_radius, d.down_radius, d.value_size, d.value_dendrites, d.history_capacity};
        codes[i].resize(d.size.x * d.size.y);
        views[i] = codes[i];
    }
    for (int i = 0; i < 2; ++i) {
        const auto &d = layers[i];
        layer_descs[i] = {{d.size.x, d.size.y, d.size.z}, d.dendrites,
            d.up_radius, d.recurrent_radius, d.down_radius};
    }
    aon::global_state = aon::rand_get_state(12345);
    aon::Hierarchy reference(io_descs, layer_descs);
    uint64_t state = aon::global_state;
    int32_t inputs[3][2] = {};
    solar_os_aogmaneo_codes_v1_t in[] = {{inputs[0], 2}, {inputs[1], 1}, {inputs[2], 1}};
    int32_t prediction[2], other_prediction[2], hidden[4];
    float activations[8], values[1];
    assert(api->prediction(model, 0, prediction, 2) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    for (int tick = 0; tick < 80; ++tick) {
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < codes[i].size(); ++j) inputs[i][j] = codes[i][j] = (tick+j) % ios[i].size.z;
        bool learn = tick % 7 != 0;
        aon::global_state = state;
        reference.step(views, learn, 0.5f, 0.2f);
        state = aon::global_state;
        assert(api->step(model, in, 3, learn, 0.5f, 0.2f) == 0);
        // A second model must not perturb this model's random stream.
        assert(api->step(other, in, 3, learn, 0.5f, 0.2f) == 0);
        for (int i : {0, 2}) {
            size_t count = size_t(ios[i].size.x) * ios[i].size.y;
            assert(api->prediction(model, i, prediction, count) == 0);
            assert(api->prediction(other, i, other_prediction, count) == 0);
            equal(reference.get_prediction_cis(i), prediction, count);
            assert(!std::memcmp(prediction, other_prediction, count * sizeof(int32_t)));
            assert(api->activations(model, i, activations, count * ios[i].size.z) == 0);
            equal(reference.get_prediction_acts(i), activations, count * ios[i].size.z);
        }
        assert(api->values(model, 2, values, 1) == 0 && std::isfinite(values[0]));
        equal(reference.get_prediction_values(2), values, 1);
        assert(api->hidden(model, 0, hidden, 4) == 0);
        equal(reference.get_encoder(0).get_hidden_cis(), hidden, 4);
    }
    assert(api->prediction(model, 1, prediction, 1) == SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED);
    assert(api->values(model, 0, values, 1) == SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED);
    inputs[0][0] = 4;
    assert(api->step(model, in, 3, true, 0, 0) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    inputs[0][0] = 0;
    assert(api->step(model, in, 2, true, 0, 0) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(api->step(model, in, 3, true, NAN, 0) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(api->step(model, in, 3, true, 0, 2) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(api->prediction(model, 3, prediction, 2) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(api->prediction(model, 0, prediction, 1) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(api->hidden(model, UINT32_MAX, hidden, 4) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(api->clear(model) == 0);
    assert(api->prediction(model, 0, prediction, 2) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    assert(api->step(model, in, 3, true, 0, 0) == 0);
    assert(api->destroy(model) == 0 && api->destroy(model) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    assert(api->clear(model) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    assert(api->destroy(other) == 0);
}

static void image_parity()
{
    uint32_t model = 0;
    assert(image_create(&model) == 0);
    aon::Array<aon::Image_Encoder::Visible_Layer_Desc> descriptors(2);
    aon::Array<aon::Byte_Buffer> pixels(2);
    aon::Array<aon::Byte_Buffer_View> views(2);
    for (int i = 0; i < 2; ++i) {
        auto size = visible[i].size;
        descriptors[i].size = {size.x, size.y, size.z};
        descriptors[i].radius = visible[i].radius;
        pixels[i].resize(size.x * size.y * size.z);
        views[i] = pixels[i];
    }
    aon::global_state = aon::rand_get_state(12345);
    aon::Image_Encoder reference;
    reference.init_random({2, 2, 8}, descriptors);
    uint64_t state = aon::global_state;
    uint8_t data[2][16] = {};
    solar_os_aogmaneo_bytes_v1_t in[] = {{data[0], 16}, {data[1], 12}};
    int32_t codes[4] = {};
    uint8_t reconstruction[16];
    for (int tick = 0; tick < 20; ++tick) {
        for (int i = 0; i < 2; ++i)
            for (int j = 0; j < pixels[i].size(); ++j) data[i][j] = pixels[i][j] = (j*17+tick) % 256;
        aon::global_state = state;
        reference.step(views, true, true);
        state = aon::global_state;
        assert(api->image_step(model, in, 2, true, true, codes, 4) == 0);
        equal(reference.get_hidden_cis(), codes, 4);
        reference.reconstruct(reference.get_hidden_cis());
        for (int i = 0; i < 2; ++i) {
            assert(api->image_reconstruct(model, codes, 4, i, reconstruction, in[i].count) == 0);
            equal(reference.get_reconstruction(i), reconstruction, in[i].count);
        }
    }
    codes[0] = -1;
    assert(api->image_reconstruct(model, codes, 4, 0, reconstruction, 16) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(api->image_step(model, in, 1, true, true, codes, 4) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(api->image_step(model, in, 2, true, true, codes, 3) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(api->clear(model) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(api->destroy(model) == 0);
}

static void allocation_failures()
{
    for (auto create_model : {create, image_create}) {
        bool completed = false;
        for (long failure = 0; failure < 1000; ++failure) {
            long baseline = live;
            requests = 0; fail_at = failure;
            uint32_t model = UINT32_MAX;
            int result = create_model(&model);
            fail_at = -1;
            if (result == 0) {
                assert(model && api->destroy(model) == 0);
                completed = true;
            } else assert(result == SOLAR_OS_NATIVE_ERROR_NO_MEMORY && !model);
            assert(solar_os_aogmaneo_engine_idle() && live == baseline);
            if (completed) break;
        }
        assert(completed);
    }
    bool completed = false;
    for (long failure = 0; failure < 100; ++failure) {
        long baseline = live;
        uint32_t model = 0;
        assert(create(&model) == 0);
        int32_t codes[2] = {};
        solar_os_aogmaneo_codes_v1_t in[] = {{codes, 2}, {codes, 1}, {codes, 1}};
        requests = 0; fail_at = failure;
        int result = api->step(model, in, 3, true, 0, 0);
        fail_at = -1;
        if (result == 0) completed = true;
        else {
            assert(result == SOLAR_OS_NATIVE_ERROR_NO_MEMORY);
            assert(api->clear(model) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
        }
        assert(api->destroy(model) == 0 && live == baseline);
        if (completed) break;
    }
    assert(completed);
}

static void validation()
{
    uint32_t model = UINT32_MAX;
    assert(api->create(nullptr, 3, layers, 2, 0, &model) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT && !model);
    assert(api->create(ios, 0, layers, 2, 0, &model) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(api->create(ios, 3, layers, 0, 0, &model) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    auto io = ios[0];
    io.size.x = INT32_MAX;
    assert(api->create(&io, 1, layers, 2, 0, &model) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    io = ios[0]; io.dendrites = 3;
    assert(api->create(&io, 1, layers, 2, 0, &model) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    io = ios[2]; io.history_capacity = 0;
    assert(api->create(&io, 1, layers, 2, 0, &model) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    io = ios[0]; io.size = {64, 4, 256};
    auto layer = layers[0]; layer.size = {64, 4, 256}; layer.recurrent_radius = 8;
    assert(api->create(&io, 1, &layer, 1, 0, &model) == SOLAR_OS_NATIVE_ERROR_INVALID_SIZE);
    assert(api->image_create({1, 1, 1}, visible, 2, 0, &model) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    uint32_t handles[SOLAR_OS_AOGMANEO_MAX_MODELS];
    for (auto &handle : handles) assert(image_create(&handle) == 0);
    assert(image_create(&model) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE && !model);
    for (auto handle : handles) assert(api->destroy(handle) == 0);
}

int main()
{
    assert(api->abi_version == 1 && api->struct_size == sizeof(*api));
    host.abi_version = 2;
    assert(app_main(0, nullptr) == SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED && !registrations && !binds);
    host.abi_version = 1;
    assert(app_main(0, nullptr) == 0 && registrations == 1 && binds == 1);
    assert(!std::strcmp(descriptor.id, "aogmaneo") && descriptor.api == (const void *)api);
    long baseline = live;
    hierarchy_parity();
    image_parity();
    allocation_failures();
    validation();
    assert(live == baseline && solar_os_aogmaneo_engine_idle());
    assert(descriptor.shutdown() == 0 && cleanups == 1);
    puts("AOgmaNeo: hierarchy/actor/image parity, validation, reentrancy, OOM cleanup and lifecycle passed");
}

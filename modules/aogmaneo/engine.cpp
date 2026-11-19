#include "solar_os_aogmaneo_abi.h"
#include "vendor/aogmaneo/hierarchy.h"
#include "vendor/aogmaneo/image_encoder.h"
#include <cmath>
#include <cstring>
#include <memory>
#include <new>

#ifdef SOLAR_OS_AOGMANEO_EXTERNAL
extern "C" void solar_os_aogmaneo_initialize(void);
extern "C" void solar_os_espdl_port_leave_task(void);
#endif

namespace {
using Size = solar_os_aogmaneo_size_v1_t;
struct Model {
    uint32_t id = 0;
    uint64_t random = 0;
    bool fault = false, ready = false;
    std::unique_ptr<aon::Hierarchy> hierarchy;
    std::unique_ptr<aon::Image_Encoder> image;
    Size sizes[SOLAR_OS_AOGMANEO_MAX_INPUTS] = {};
    aon::Array<aon::Int_Buffer> codes;
    aon::Array<aon::Int_Buffer_View> code_views;
    aon::Array<aon::Byte_Buffer> pixels;
    aon::Array<aon::Byte_Buffer_View> pixel_views;
    aon::Int_Buffer reconstruction;
};
Model *models[SOLAR_OS_AOGMANEO_MAX_MODELS] = {};
uint32_t next_id;
unsigned busy;

template<class F> int call(F function)
{
    if (__atomic_exchange_n(&busy, 1, __ATOMIC_ACQUIRE))
        return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    int result;
    try {
#ifdef SOLAR_OS_AOGMANEO_EXTERNAL
        solar_os_aogmaneo_initialize();
#endif
        result = function();
    } catch (const std::bad_alloc &) {
        result = SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    } catch (...) {
        result = SOLAR_OS_NATIVE_ERROR_FAILED;
    }
#ifdef SOLAR_OS_AOGMANEO_EXTERNAL
    solar_os_espdl_port_leave_task();
#endif
    __atomic_store_n(&busy, 0, __ATOMIC_RELEASE);
    return result;
}

Model *find(uint32_t id)
{
    for (auto *model : models)
        if (model && model->id == id) return model;
    return nullptr;
}

template<class F> int use(uint32_t id, bool image, F function)
{
    return call([&]() -> int {
        auto *model = find(id);
        if (!model) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
        if (bool(model->image) != image) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
        if (model->fault) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
        aon::global_state = model->random;
        try {
            int result = function(*model);
            model->random = aon::global_state;
            return result;
        } catch (...) {
            model->fault = true;
            throw;
        }
    });
}

bool shape(Size size, bool image = false)
{
    return size.x >= 1 && size.x <= 64 && size.y >= 1 && size.y <= 64 &&
           size.z >= (image ? 1 : 2) && size.z <= 256 &&
           uint64_t(size.x) * size.y * size.z <= 65536;
}
size_t columns(Size size) { return size_t(size.x) * size.y; }
size_t cells(Size size) { return columns(size) * size.z; }
aon::Int3 vector(Size size) { return {size.x, size.y, size.z}; }
bool radius(int r) { return r >= 0 && r <= 8; }
bool dendrites(int n) { return n >= 2 && n <= 16 && n % 2 == 0; }
bool weights(Size hidden, int n, int r, Size visible)
{
    return uint64_t(cells(hidden)) * n * (2*r+1) * (2*r+1) * visible.z <= 4194304;
}
bool codes_valid(const int32_t *codes, size_t count, int depth)
{
    if (!codes) return false;
    for (size_t i = 0; i < count; ++i)
        if (codes[i] < 0 || codes[i] >= depth) return false;
    return true;
}
template<class T, class U> int copy(const aon::Array<T> &source, U *output, size_t count)
{
    if (!output || count != size_t(source.size())) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    for (size_t i = 0; i < count; ++i) output[i] = static_cast<U>(source[int(i)]);
    return SOLAR_OS_NATIVE_OK;
}
int slot()
{
    if (next_id == UINT32_MAX) return -1;
    for (unsigned i = 0; i < SOLAR_OS_AOGMANEO_MAX_MODELS; ++i)
        if (!models[i]) return int(i);
    return -1;
}
int publish(std::unique_ptr<Model> model, int index, uint32_t *output)
{
    model->id = ++next_id;
    model->random = aon::global_state;
    *output = model->id;
    models[index] = model.release();
    return SOLAR_OS_NATIVE_OK;
}

int create(const solar_os_aogmaneo_io_v1_t *inputs, size_t input_count,
           const solar_os_aogmaneo_layer_v1_t *layers, size_t layer_count,
           uint64_t seed, uint32_t *output)
{
    if (!output) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *output = 0;
    return call([&]() -> int {
        if (!inputs || !layers || !input_count || input_count > SOLAR_OS_AOGMANEO_MAX_INPUTS ||
            !layer_count || layer_count > SOLAR_OS_AOGMANEO_MAX_LAYERS)
            return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
        for (size_t i = 0; i < layer_count; ++i) {
            const auto &l = layers[i];
            if (!shape(l.size) || !dendrites(l.dendrites) || !radius(l.up_radius) ||
                !radius(l.down_radius) || (l.recurrent_radius != -1 && !radius(l.recurrent_radius)))
                return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
            if ((l.recurrent_radius >= 0 && !weights(l.size, 1, l.recurrent_radius, l.size)) ||
                (i && (!weights(l.size, 1, l.up_radius, layers[i-1].size) ||
                       !weights(layers[i-1].size, l.dendrites, l.down_radius, l.size))))
                return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
        }
        for (size_t i = 0; i < input_count; ++i) {
            const auto &io = inputs[i];
            if (!shape(io.size) || io.type < 0 || io.type > 2 || !dendrites(io.dendrites) ||
                !radius(io.up_radius) || !radius(io.down_radius))
                return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
            if (!weights(layers[0].size, 1, io.up_radius, io.size) ||
                (io.type && !weights(io.size, io.dendrites, io.down_radius, layers[0].size)))
                return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
            if (io.type == SOLAR_OS_AOGMANEO_ACTION) {
                if (io.value_size < 2 || io.value_size > 256 || !dendrites(io.value_dendrites) ||
                    io.history_capacity < 2 || io.history_capacity > 512)
                    return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
                Size values = {io.size.x, io.size.y, io.value_size};
                if (!weights(values, io.value_dendrites, io.down_radius, layers[0].size))
                    return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
            }
        }
        int index = slot();
        if (index < 0) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
        auto model = std::make_unique<Model>();
        model->hierarchy = std::make_unique<aon::Hierarchy>();
        aon::Array<aon::Hierarchy::IO_Desc> ios{int(input_count)};
        aon::Array<aon::Hierarchy::Layer_Desc> descriptors{int(layer_count)};
        model->codes.resize(int(input_count));
        model->code_views.resize(int(input_count));
        for (size_t i = 0; i < input_count; ++i) {
            const auto &io = inputs[i];
            ios[int(i)] = aon::Hierarchy::IO_Desc(vector(io.size), aon::IO_Type(io.type),
                io.dendrites, io.up_radius, io.down_radius, io.value_size,
                io.value_dendrites, io.history_capacity);
            model->sizes[i] = io.size;
            model->codes[int(i)].resize(int(columns(io.size)));
            model->code_views[int(i)] = model->codes[int(i)];
        }
        for (size_t i = 0; i < layer_count; ++i) {
            const auto &l = layers[i];
            descriptors[int(i)] = aon::Hierarchy::Layer_Desc(vector(l.size), l.dendrites,
                l.up_radius, l.recurrent_radius, l.down_radius);
        }
        aon::global_state = aon::rand_get_state(seed);
        model->hierarchy->init_random(ios, descriptors);
        return publish(std::move(model), index, output);
    });
}

int destroy(uint32_t id)
{
    return call([&]() -> int {
        for (auto &model : models) if (model && model->id == id) {
            delete model;
            model = nullptr;
            return SOLAR_OS_NATIVE_OK;
        }
        return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    });
}
int step(uint32_t id, const solar_os_aogmaneo_codes_v1_t *inputs,
         size_t count, bool learn, float reward, float mimic)
{
    return use(id, false, [&](Model &m) -> int {
        if (!inputs || count != size_t(m.codes.size()) || !std::isfinite(reward) ||
            std::abs(reward) > 1000000 || !std::isfinite(mimic) || mimic < 0 || mimic > 1)
            return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
        for (size_t i = 0; i < count; ++i)
            if (inputs[i].count != columns(m.sizes[i]) ||
                !codes_valid(inputs[i].data, inputs[i].count, m.sizes[i].z))
                return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
        for (size_t i = 0; i < count; ++i)
            for (size_t j = 0; j < inputs[i].count; ++j) m.codes[int(i)][int(j)] = inputs[i].data[j];
        m.hierarchy->step(m.code_views, learn, reward, mimic);
        m.ready = true;
        return SOLAR_OS_NATIVE_OK;
    });
}
template<class F> int output(uint32_t id, uint32_t input, F function)
{
    return use(id, false, [&](Model &m) -> int {
        if (input >= unsigned(m.hierarchy->get_num_io())) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
        if (m.hierarchy->get_io_type(int(input)) == aon::none) return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
        if (!m.ready) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
        return function(*m.hierarchy);
    });
}
int prediction(uint32_t id, uint32_t input, int32_t *codes, size_t count)
{
    return output(id, input, [&](aon::Hierarchy &h) { return copy(h.get_prediction_cis(int(input)), codes, count); });
}
int activations(uint32_t id, uint32_t input, float *values, size_t count)
{
    return output(id, input, [&](aon::Hierarchy &h) { return copy(h.get_prediction_acts(int(input)), values, count); });
}
int values(uint32_t id, uint32_t input, float *values, size_t count)
{
    return output(id, input, [&](aon::Hierarchy &h) {
        if (h.get_io_type(int(input)) != aon::action) return int(SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED);
        return copy(h.get_prediction_values(int(input)), values, count);
    });
}
int hidden(uint32_t id, uint32_t layer, int32_t *codes, size_t count)
{
    return use(id, false, [&](Model &m) -> int {
        if (layer >= unsigned(m.hierarchy->get_num_layers())) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
        if (!m.ready) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
        return copy(m.hierarchy->get_encoder(int(layer)).get_hidden_cis(), codes, count);
    });
}
int clear(uint32_t id)
{
    return use(id, false, [&](Model &m) -> int {
        m.hierarchy->clear_state();
        m.ready = false;
        return SOLAR_OS_NATIVE_OK;
    });
}
int image_create(Size hidden, const solar_os_aogmaneo_visible_v1_t *visible,
                 size_t count, uint64_t seed, uint32_t *output)
{
    if (!output) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *output = 0;
    return call([&]() -> int {
        if (!shape(hidden) || !visible || !count || count > SOLAR_OS_AOGMANEO_MAX_INPUTS)
            return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
        for (size_t i = 0; i < count; ++i) {
            if (!shape(visible[i].size, true) || !radius(visible[i].radius))
                return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
            if (!weights(hidden, 1, visible[i].radius, visible[i].size))
                return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
        }
        int index = slot();
        if (index < 0) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
        auto model = std::make_unique<Model>();
        model->image = std::make_unique<aon::Image_Encoder>();
        aon::Array<aon::Image_Encoder::Visible_Layer_Desc> descriptors{int(count)};
        model->pixels.resize(int(count));
        model->pixel_views.resize(int(count));
        model->reconstruction.resize(int(columns(hidden)));
        for (size_t i = 0; i < count; ++i) {
            descriptors[int(i)].size = vector(visible[i].size);
            descriptors[int(i)].radius = visible[i].radius;
            model->sizes[i] = visible[i].size;
            model->pixels[int(i)].resize(int(cells(visible[i].size)));
            model->pixel_views[int(i)] = model->pixels[int(i)];
        }
        aon::global_state = aon::rand_get_state(seed);
        model->image->init_random(vector(hidden), descriptors);
        return publish(std::move(model), index, output);
    });
}
int image_step(uint32_t id, const solar_os_aogmaneo_bytes_v1_t *inputs, size_t count,
               bool learn, bool reconstruct, int32_t *codes, size_t code_count)
{
    return use(id, true, [&](Model &m) -> int {
        if (!inputs || count != size_t(m.pixels.size()) || !codes ||
            code_count != size_t(m.reconstruction.size())) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
        for (size_t i = 0; i < count; ++i)
            if (!inputs[i].data || inputs[i].count != cells(m.sizes[i]))
                return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
        for (size_t i = 0; i < count; ++i)
            for (size_t j = 0; j < inputs[i].count; ++j) m.pixels[int(i)][int(j)] = inputs[i].data[j];
        m.image->step(m.pixel_views, learn, reconstruct);
        m.ready = true;
        return copy(m.image->get_hidden_cis(), codes, code_count);
    });
}
int image_reconstruct(uint32_t id, const int32_t *codes, size_t count,
                      uint32_t visible, uint8_t *bytes, size_t byte_count)
{
    return use(id, true, [&](Model &m) -> int {
        if (visible >= unsigned(m.pixels.size()) || !bytes || byte_count != cells(m.sizes[visible]) ||
            count != size_t(m.reconstruction.size()) || !codes_valid(codes, count, m.image->get_hidden_size().z))
            return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
        for (size_t i = 0; i < count; ++i) m.reconstruction[int(i)] = codes[i];
        m.image->reconstruct(m.reconstruction);
        return copy(m.image->get_reconstruction(int(visible)), bytes, byte_count);
    });
}
const solar_os_aogmaneo_api_v1_t api = {
    SOLAR_OS_AOGMANEO_ABI, sizeof(api), create, destroy, step, prediction,
    activations, values, hidden, clear, image_create, image_step, image_reconstruct,
};
} // namespace

extern "C" const solar_os_aogmaneo_api_v1_t *solar_os_aogmaneo_engine_api_v1(void)
{
    return &api;
}
extern "C" bool solar_os_aogmaneo_engine_idle(void)
{
    if (__atomic_load_n(&busy, __ATOMIC_ACQUIRE)) return false;
    for (auto *model : models) if (model) return false;
    return true;
}

#include "solar_os_espdl_validate.h"
#include "espdl_generated.h"
#include "solar_os_inference.h"
#include <cstring>
#include <set>
#include <string>

static uint32_t word(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static bool name_ok(const flatbuffers::String *s)
{
    return s && s->size() && s->size() < SOLAR_OS_INFERENCE_NAME_MAX &&
        !std::memchr(s->c_str(), 0, s->size());
}
static bool ports_ok(const flatbuffers::Vector<flatbuffers::Offset<espdl::ValueInfo>> *ports,
    size_t max = SOLAR_OS_INFERENCE_PORTS_MAX, bool optional_placeholders = false)
{
    if (!ports || !ports->size() || ports->size() > max) return false;
    std::set<std::string> names;
    for (auto p : *ports) {
        /* Exported value_info may contain the unnamed placeholder used for
         * omitted optional operator inputs. It is not a public tensor port. */
        if (optional_placeholders && p && p->name() && !p->name()->size()) continue;
        if (!p || !name_ok(p->name()) || !names.emplace(p->name()->str()).second ||
            !p->value_info_type()) return false;
        auto t = p->value_info_type()->value_as_tensor_type();
        if (!t || !t->shape() || !t->shape()->dim() ||
            t->shape()->dim()->size() > SOLAR_OS_INFERENCE_RANK_MAX) return false;
        size_t count = 1;
        for (auto d : *t->shape()->dim()) {
            if (!d || !d->value() || d->value()->dim_type() != espdl::DimensionValueType_VALUE ||
                d->value()->dim_value() <= 0 ||
                (uint64_t)d->value()->dim_value() > SOLAR_OS_INFERENCE_TENSOR_MAX / count) return false;
            count *= d->value()->dim_value();
        }
    }
    return true;
}
extern "C" esp_err_t solar_os_espdl_validate(const uint8_t *data, size_t size,
    size_t *offset, size_t *payload_size, size_t *layers)
{
    if (!data || !offset || !payload_size || !layers) return ESP_ERR_INVALID_ARG;
    *offset = *payload_size = *layers = 0;
    if (size < 16 || size > SOLAR_OS_INFERENCE_FILE_MAX) return ESP_ERR_INVALID_SIZE;
    size_t header;
    if (!std::memcmp(data, "EDL2", 4)) header = 16;
    else if (!std::memcmp(data, "EDL1", 4)) header = 12;
    else return ESP_ERR_NOT_SUPPORTED; /* Packed/encrypted containers are separate capabilities. */
    if (word(data + 4)) return ESP_ERR_NOT_SUPPORTED;
    size_t length = word(data + 8);
    if (length < 8 || length > size - header || size - header - length > 15)
        return ESP_ERR_INVALID_SIZE;
    flatbuffers::Verifier verifier(data + header, length);
    if (!espdl::VerifyModelBuffer(verifier)) return ESP_ERR_INVALID_RESPONSE;
    auto model = espdl::GetModel(data + header);
    auto graph = model->graph();
    if (!graph || !ports_ok(graph->input()) || !ports_ok(graph->output()) ||
        !graph->initializer() || !ports_ok(graph->value_info(), 65536, true) ||
        !graph->test_inputs_value() || !graph->test_outputs_value() ||
        !graph->node() || !graph->node()->size() || graph->node()->size() > 4096)
        return ESP_ERR_INVALID_RESPONSE;
    std::set<std::string> node_names;
    std::set<std::string> values;
    for (auto value : *graph->value_info()) values.emplace(value->name()->str());
    for (auto port : *graph->input()) if (!values.count(port->name()->str())) return ESP_ERR_INVALID_RESPONSE;
    for (auto port : *graph->output()) if (!values.count(port->name()->str())) return ESP_ERR_INVALID_RESPONSE;
    for (auto n : *graph->node()) {
        if (!n || !name_ok(n->name()) || !name_ok(n->op_type()) ||
            !node_names.emplace(n->name()->str()).second || !n->input() || !n->output() || !n->attribute() ||
            !n->output()->size()) return ESP_ERR_INVALID_RESPONSE;
        for (auto output : *n->output())
            if (!name_ok(output) || !values.count(output->str())) return ESP_ERR_INVALID_RESPONSE;
        for (auto attribute : *n->attribute()) {
            if (!attribute || !name_ok(attribute->name())) return ESP_ERR_INVALID_RESPONSE;
            if (attribute->name()->str() == "quant_type") {
                if (attribute->attr_type() != espdl::AttributeType_STRING || !attribute->s())
                    return ESP_ERR_INVALID_RESPONSE;
                std::string quant(reinterpret_cast<const char *>(attribute->s()->data()), attribute->s()->size());
                if (quant != "S8" && quant != "S16" && quant != "F32" && quant != "W8A16")
                    return ESP_ERR_NOT_SUPPORTED;
            }
        }
    }
    if (graph->initializer()) for (auto tensor : *graph->initializer()) {
        if (!tensor || !name_ok(tensor->name()) || !tensor->dims() ||
            tensor->dims()->size() > SOLAR_OS_INFERENCE_RANK_MAX) return ESP_ERR_INVALID_RESPONSE;
        size_t count = 1;
        for (auto dim : *tensor->dims()) {
            if (dim <= 0 || (uint64_t)dim > SOLAR_OS_INFERENCE_TENSOR_MAX / count)
                return ESP_ERR_INVALID_SIZE;
            count *= dim;
        }
    }
    *offset = header; *payload_size = length; *layers = graph->node()->size();
    return ESP_OK;
}

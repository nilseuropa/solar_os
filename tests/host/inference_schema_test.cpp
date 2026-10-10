#include "solar_os_espdl_validate.h"
#include "espdl_generated.h"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>
static esp_err_t validate(const std::vector<uint8_t> &bytes)
{
    size_t offset, payload, layers;
    esp_err_t error = solar_os_espdl_validate(bytes.data(), bytes.size(), &offset, &payload, &layers);
    if (!error) assert((offset == 12 || offset == 16) && payload > 8 && layers == 2);
    else assert(!offset && !payload && !layers);
    return error;
}
int main()
{
    for (const char *name : {"arithmetic_int8", "arithmetic_float32", "arithmetic_simd_int8", "arithmetic_optional", "arithmetic_oversized"}) {
        std::ifstream file(std::string("../fixtures/inference/") + name + ".espdl", std::ios::binary);
        assert(file);
        std::vector<uint8_t> original((std::istreambuf_iterator<char>(file)), {});
        assert(validate(original) == ESP_OK);
        /* An unnamed internal placeholder is legal, but public ports must
         * always have a nonempty exported name. */
        auto unnamed = original;
        auto input_name = espdl::GetModel(unnamed.data() + 16)->graph()->input()->Get(0)->name();
        flatbuffers::WriteScalar<flatbuffers::uoffset_t>(
            const_cast<uint8_t *>(reinterpret_cast<const uint8_t *>(input_name)), 0);
        assert(validate(unnamed) == ESP_ERR_INVALID_RESPONSE);
        for (size_t size = 0; size < original.size(); ++size) {
            auto bytes = original; bytes.resize(size);
            assert(validate(bytes) != ESP_OK);
        }
        auto bytes = original; bytes[0] = 'X'; assert(validate(bytes) == ESP_ERR_NOT_SUPPORTED);
        bytes = original; bytes[4] = 1; assert(validate(bytes) == ESP_ERR_NOT_SUPPORTED);
        bytes = original; bytes[8] = 255; assert(validate(bytes) != ESP_OK);
        bytes = original; bytes[16] = 255; assert(validate(bytes) == ESP_ERR_INVALID_RESPONSE);
        bytes = original; bytes[3] = '1'; bytes.erase(bytes.begin() + 12, bytes.begin() + 16);
        assert(validate(bytes) == ESP_OK);
        bytes = original; bytes.resize(bytes.size() + 16); assert(validate(bytes) == ESP_ERR_INVALID_SIZE);
        /* The native parser defaults to float for a wrongly typed quant_type.
         * This must never select float kernels for smaller int8 buffers. */
        bytes = original;
        auto attribute = espdl::GetModel(bytes.data() + 16)->graph()->node()->Get(0)->attribute()->Get(0);
        assert(attribute->name()->str() == "quant_type");
        auto attribute_table = const_cast<uint8_t *>(reinterpret_cast<const uint8_t *>(attribute));
        auto attribute_vtable = attribute_table - flatbuffers::ReadScalar<flatbuffers::soffset_t>(attribute_table);
        auto type_field = flatbuffers::ReadScalar<flatbuffers::voffset_t>(
            attribute_vtable + espdl::Attribute::VT_ATTR_TYPE);
        assert(type_field);
        flatbuffers::WriteScalar<int32_t>(attribute_table + type_field, espdl::AttributeType_INT);
        assert(validate(bytes) == ESP_ERR_INVALID_RESPONSE);
        bytes = original;
        attribute = espdl::GetModel(bytes.data() + 16)->graph()->node()->Get(0)->attribute()->Get(0);
        const_cast<uint8_t *>(attribute->s()->data())[0] = 'X';
        assert(validate(bytes) == ESP_ERR_NOT_SUPPORTED);
        /* FlatBuffers considers absent vectors legal; the pinned native loader
         * unconditionally iterates these collections. Reject before construction. */
        for (auto slot : {espdl::Graph::VT_INITIALIZER, espdl::Graph::VT_VALUE_INFO,
                espdl::Graph::VT_TEST_INPUTS_VALUE, espdl::Graph::VT_TEST_OUTPUTS_VALUE}) {
            bytes = original;
            auto graph = const_cast<uint8_t *>(reinterpret_cast<const uint8_t *>(
                espdl::GetModel(bytes.data() + 16)->graph()));
            auto vtable = graph - flatbuffers::ReadScalar<flatbuffers::soffset_t>(graph);
            flatbuffers::WriteScalar<flatbuffers::voffset_t>(vtable + slot, 0);
            flatbuffers::Verifier verifier(bytes.data() + 16, bytes.size() - 16);
            assert(espdl::VerifyModelBuffer(verifier));
            assert(validate(bytes) == ESP_ERR_INVALID_RESPONSE);
        }
        bytes = original;
        auto shape = espdl::GetModel(bytes.data() + 16)->graph()->input()->Get(0)->value_info_type()
            ->value_as_tensor_type()->shape()->dim()->Get(0)->value();
        /* Alter the static dimension to zero without corrupting FlatBuffer bounds. */
        auto address = reinterpret_cast<const uint8_t *>(shape);
        auto table = const_cast<uint8_t *>(address);
        auto vtable = table - flatbuffers::ReadScalar<flatbuffers::soffset_t>(table);
        auto field = flatbuffers::ReadScalar<flatbuffers::voffset_t>(vtable + espdl::DimensionValue::VT_DIM_VALUE);
        assert(field);
        flatbuffers::WriteScalar<int64_t>(table + field, 0);
        assert(validate(bytes) == ESP_ERR_INVALID_RESPONSE);
    }
    puts("ESP-DL fixtures, truncation, EDL1/2, unsupported envelopes and invalid shapes passed");
}

// ----------------------------------------------------------------------------
//  AOgmaNeo
//  Copyright(c) 2020-2025 Ogma Intelligent Systems Corp. All rights reserved.
//
//  This copy of AOgmaNeo is licensed to you under the terms described
//  in the AOGMANEO_LICENSE.md file included in this distribution.
// ----------------------------------------------------------------------------

#include "image_encoder.h"

using namespace aon;

void Image_Encoder::forward(
    const Int2 &column_pos,
    const Array<Byte_Buffer_View> &inputs,
    bool learn_enabled
) {
    int hidden_column_index = address2(column_pos, Int2(hidden_size.x, hidden_size.y));

    int hidden_cells_start = hidden_column_index * hidden_size.z;

    const float byte_inv = 1.0f / 255.0f;

    float center = 0.0f;
    int count = 0;

    for (int vli = 0; vli < visible_layers.size(); vli++) {
        Visible_Layer &vl = visible_layers[vli];
        const Visible_Layer_Desc &vld = visible_layer_descs[vli];

        int diam = vld.radius * 2 + 1;

        // projection
        Float2 h_to_v = Float2(static_cast<float>(vld.size.x) / static_cast<float>(hidden_size.x),
            static_cast<float>(vld.size.y) / static_cast<float>(hidden_size.y));

        Int2 visible_center = project(column_pos, h_to_v);

        // lower corner
        Int2 field_lower_bound(visible_center.x - vld.radius, visible_center.y - vld.radius);

        // bounds of receptive field, clamped to input size
        Int2 iter_lower_bound(max(0, field_lower_bound.x), max(0, field_lower_bound.y));
        Int2 iter_upper_bound(min(vld.size.x - 1, visible_center.x + vld.radius), min(vld.size.y - 1, visible_center.y + vld.radius));

        count += (iter_upper_bound.x - iter_lower_bound.x + 1) * (iter_upper_bound.y - iter_lower_bound.y + 1) * vld.size.z;

        Byte_Buffer_View vl_inputs = inputs[vli];

        for (int ix = iter_lower_bound.x; ix <= iter_upper_bound.x; ix++)
            for (int iy = iter_lower_bound.y; iy <= iter_upper_bound.y; iy++) {
                int visible_column_index = address2(Int2(ix, iy), Int2(vld.size.x, vld.size.y));

                Int2 offset(ix - field_lower_bound.x, iy - field_lower_bound.y);

                int visible_cells_start = vld.size.z * (iy + ix * vld.size.y);

                for (int vc = 0; vc < vld.size.z; vc++) {
                    float input = vl_inputs[vc + visible_cells_start] * byte_inv;

                    center += input;
                }
            }
    }

    center /= count;

    for (int hc = 0; hc < hidden_size.z; hc++) {
        int hidden_cell_index = hc + hidden_cells_start;

        hidden_acts[hidden_cell_index] = 0.0f;
        hidden_totals[hidden_cell_index] = 0.0f;
    }

    for (int vli = 0; vli < visible_layers.size(); vli++) {
        Visible_Layer &vl = visible_layers[vli];
        const Visible_Layer_Desc &vld = visible_layer_descs[vli];

        int diam = vld.radius * 2 + 1;

        // projection
        Float2 h_to_v = Float2(static_cast<float>(vld.size.x) / static_cast<float>(hidden_size.x),
            static_cast<float>(vld.size.y) / static_cast<float>(hidden_size.y));

        Int2 visible_center = project(column_pos, h_to_v);

        // lower corner
        Int2 field_lower_bound(visible_center.x - vld.radius, visible_center.y - vld.radius);

        // bounds of receptive field, clamped to input size
        Int2 iter_lower_bound(max(0, field_lower_bound.x), max(0, field_lower_bound.y));
        Int2 iter_upper_bound(min(vld.size.x - 1, visible_center.x + vld.radius), min(vld.size.y - 1, visible_center.y + vld.radius));

        Byte_Buffer_View vl_inputs = inputs[vli];

        for (int ix = iter_lower_bound.x; ix <= iter_upper_bound.x; ix++)
            for (int iy = iter_lower_bound.y; iy <= iter_upper_bound.y; iy++) {
                int visible_column_index = address2(Int2(ix, iy), Int2(vld.size.x, vld.size.y));

                Int2 offset(ix - field_lower_bound.x, iy - field_lower_bound.y);

                int wi_start_partial = vld.size.z * (offset.y + diam * (offset.x + diam * hidden_column_index));

                int visible_cells_start = vld.size.z * (iy + ix * vld.size.y);

                for (int vc = 0; vc < vld.size.z; vc++) {
                    int wi_start = hidden_size.z * (vc + wi_start_partial);

                    float input_centered = (vl_inputs[vc + visible_cells_start] * byte_inv - center) * 2.0f;

                    for (int hc = 0; hc < hidden_size.z; hc++) {
                        int hidden_cell_index = hc + hidden_cells_start;

                        int wi = hc + wi_start;

                        float w = vl.weights[wi] * byte_inv * 2.0f - 1.0f;

                        hidden_acts[hidden_cell_index] += input_centered * w;
                        hidden_totals[hidden_cell_index] += w * w;
                    }
                }
            }
    }

    int max_index = 0;
    float max_activation = limit_min;

    for (int hc = 0; hc < hidden_size.z; hc++) {
        int hidden_cell_index = hc + hidden_cells_start;

        hidden_acts[hidden_cell_index] /= max(limit_small, sqrtf(hidden_totals[hidden_cell_index]));

        if (hidden_acts[hidden_cell_index] > max_activation) {
            max_activation = hidden_acts[hidden_cell_index];
            max_index = hc;
        }
    }

    hidden_cis[hidden_column_index] = max_index;

    if (learn_enabled) {
        for (int dhc = -params.n_radius; dhc <= params.n_radius; dhc++) {
            int hc = max_index + dhc;

            if (hc < 0 || hc >= hidden_size.z)
                continue;

            int hidden_cell_index = hc + hidden_cells_start;

            float rate = hidden_resources[hidden_cell_index] * powf(params.falloff, abs(dhc));

            for (int vli = 0; vli < visible_layers.size(); vli++) {
                Visible_Layer &vl = visible_layers[vli];
                const Visible_Layer_Desc &vld = visible_layer_descs[vli];

                int diam = vld.radius * 2 + 1;

                // projection
                Float2 h_to_v = Float2(static_cast<float>(vld.size.x) / static_cast<float>(hidden_size.x),
                    static_cast<float>(vld.size.y) / static_cast<float>(hidden_size.y));

                Int2 visible_center = project(column_pos, h_to_v);

                // lower corner
                Int2 field_lower_bound(visible_center.x - vld.radius, visible_center.y - vld.radius);

                // bounds of receptive field, clamped to input size
                Int2 iter_lower_bound(max(0, field_lower_bound.x), max(0, field_lower_bound.y));
                Int2 iter_upper_bound(min(vld.size.x - 1, visible_center.x + vld.radius), min(vld.size.y - 1, visible_center.y + vld.radius));

                Byte_Buffer_View vl_inputs = inputs[vli];

                for (int ix = iter_lower_bound.x; ix <= iter_upper_bound.x; ix++)
                    for (int iy = iter_lower_bound.y; iy <= iter_upper_bound.y; iy++) {
                        int visible_column_index = address2(Int2(ix, iy), Int2(vld.size.x, vld.size.y));

                        int visible_cells_start = visible_column_index * vld.size.z;

                        Int2 offset(ix - field_lower_bound.x, iy - field_lower_bound.y);

                        int wi_start_partial = vld.size.z * (offset.y + diam * (offset.x + diam * hidden_column_index));

                        for (int vc = 0; vc < vld.size.z; vc++) {
                            int wi = hc + hidden_size.z * (vc + wi_start_partial);

                            float input_centered = vl_inputs[vc + visible_cells_start] * byte_inv - center;

                            float w = vl.weights[wi] * byte_inv * 2.0f - 1.0f;

                            vl.weights[wi] = min(255, max(0, vl.weights[wi] + roundf2i(rate * 255.0f * (input_centered - w))));
                        }
                    }
            }

            hidden_resources[hidden_cell_index] -= params.lr * rate;
        }
    }
}

void Image_Encoder::learn_reconstruction(
    const Int2 &column_pos,
    Byte_Buffer_View inputs,
    int vli,
    uint64_t* state
) {
    Visible_Layer &vl = visible_layers[vli];
    Visible_Layer_Desc &vld = visible_layer_descs[vli];

    int diam = vld.radius * 2 + 1;

    int visible_column_index = address2(column_pos, Int2(vld.size.x, vld.size.y));

    int visible_cells_start = visible_column_index * vld.size.z;

    // projection
    Float2 v_to_h = Float2(static_cast<float>(hidden_size.x) / static_cast<float>(vld.size.x),
        static_cast<float>(hidden_size.y) / static_cast<float>(vld.size.y));

    Float2 h_to_v = Float2(static_cast<float>(vld.size.x) / static_cast<float>(hidden_size.x),
        static_cast<float>(vld.size.y) / static_cast<float>(hidden_size.y));
                
    Int2 reverse_radii(ceilf(v_to_h.x * (vld.radius * 2 + 1) * 0.5f), ceilf(v_to_h.y * (vld.radius * 2 + 1) * 0.5f));

    Int2 hidden_center = project(column_pos, v_to_h);

    // lower corner
    Int2 field_lower_bound(hidden_center.x - reverse_radii.x, hidden_center.y - reverse_radii.y);

    // bounds of receptive field, clamped to input size
    Int2 iter_lower_bound(max(0, field_lower_bound.x), max(0, field_lower_bound.y));
    Int2 iter_upper_bound(min(hidden_size.x - 1, hidden_center.x + reverse_radii.x), min(hidden_size.y - 1, hidden_center.y + reverse_radii.y));
    
    const float byte_inv = 1.0f / 255.0f;

    for (int vc = 0; vc < vld.size.z; vc++) {
        int visible_cell_index = vc + visible_cells_start;

        float sum = 0.0f;
        int count = 0;

        for (int ix = iter_lower_bound.x; ix <= iter_upper_bound.x; ix++)
            for (int iy = iter_lower_bound.y; iy <= iter_upper_bound.y; iy++) {
                Int2 hidden_pos = Int2(ix, iy);

                int hidden_column_index = address2(hidden_pos, Int2(hidden_size.x, hidden_size.y));

                Int2 visible_center = project(hidden_pos, h_to_v);

                if (in_bounds(column_pos, Int2(visible_center.x - vld.radius, visible_center.y - vld.radius), Int2(visible_center.x + vld.radius + 1, visible_center.y + vld.radius + 1))) {
                    int hidden_cell_index = hidden_cis[hidden_column_index] + hidden_column_index * hidden_size.z;

                    Int2 offset(column_pos.x - visible_center.x + vld.radius, column_pos.y - visible_center.y + vld.radius);

                    int wi = vc + vld.size.z * (offset.y + diam * (offset.x + diam * hidden_cell_index));

                    sum += vl.recon_weights[wi];
                    count++;
                }
            }

        sum /= max(1, count * 255);

        float target = inputs[visible_cell_index] * byte_inv;

        int delta = rand_roundf(params.rr * (target - min(1.0f, max(0.0f, (sum - 0.5f) * 2.0f * params.scale + 0.5f))) * 255.0f, state);

        for (int ix = iter_lower_bound.x; ix <= iter_upper_bound.x; ix++)
            for (int iy = iter_lower_bound.y; iy <= iter_upper_bound.y; iy++) {
                Int2 hidden_pos = Int2(ix, iy);

                int hidden_column_index = address2(hidden_pos, Int2(hidden_size.x, hidden_size.y));

                Int2 visible_center = project(hidden_pos, h_to_v);

                if (in_bounds(column_pos, Int2(visible_center.x - vld.radius, visible_center.y - vld.radius), Int2(visible_center.x + vld.radius + 1, visible_center.y + vld.radius + 1))) {
                    int hidden_cell_index = hidden_cis[hidden_column_index] + hidden_column_index * hidden_size.z;

                    Int2 offset(column_pos.x - visible_center.x + vld.radius, column_pos.y - visible_center.y + vld.radius);

                    int wi = vc + vld.size.z * (offset.y + diam * (offset.x + diam * hidden_cell_index));

                    vl.recon_weights[wi] = min(255, max(0, vl.recon_weights[wi] + delta));
                }
            }
    }
}

void Image_Encoder::reconstruct(
    const Int2 &column_pos,
    Int_Buffer_View recon_cis,
    int vli
) {
    Visible_Layer &vl = visible_layers[vli];
    Visible_Layer_Desc &vld = visible_layer_descs[vli];

    int diam = vld.radius * 2 + 1;

    int visible_column_index = address2(column_pos, Int2(vld.size.x, vld.size.y));

    int visible_cells_start = visible_column_index * vld.size.z;

    // projection
    Float2 v_to_h = Float2(static_cast<float>(hidden_size.x) / static_cast<float>(vld.size.x),
        static_cast<float>(hidden_size.y) / static_cast<float>(vld.size.y));

    Float2 h_to_v = Float2(static_cast<float>(vld.size.x) / static_cast<float>(hidden_size.x),
        static_cast<float>(vld.size.y) / static_cast<float>(hidden_size.y));
                
    Int2 reverse_radii(ceilf(v_to_h.x * (vld.radius * 2 + 1) * 0.5f), ceilf(v_to_h.y * (vld.radius * 2 + 1) * 0.5f));

    Int2 hidden_center = project(column_pos, v_to_h);

    // lower corner
    Int2 field_lower_bound(hidden_center.x - reverse_radii.x, hidden_center.y - reverse_radii.y);

    // bounds of receptive field, clamped to input size
    Int2 iter_lower_bound(max(0, field_lower_bound.x), max(0, field_lower_bound.y));
    Int2 iter_upper_bound(min(hidden_size.x - 1, hidden_center.x + reverse_radii.x), min(hidden_size.y - 1, hidden_center.y + reverse_radii.y));
    
    // find current max
    for (int vc = 0; vc < vld.size.z; vc++) {
        int visible_cell_index = vc + visible_cells_start;

        float sum = 0.0f;
        int count = 0;

        for (int ix = iter_lower_bound.x; ix <= iter_upper_bound.x; ix++)
            for (int iy = iter_lower_bound.y; iy <= iter_upper_bound.y; iy++) {
                Int2 hidden_pos = Int2(ix, iy);

                int hidden_column_index = address2(hidden_pos, Int2(hidden_size.x, hidden_size.y));

                Int2 visible_center = project(hidden_pos, h_to_v);

                if (in_bounds(column_pos, Int2(visible_center.x - vld.radius, visible_center.y - vld.radius), Int2(visible_center.x + vld.radius + 1, visible_center.y + vld.radius + 1))) {
                    int hidden_cell_index = recon_cis[hidden_column_index] + hidden_column_index * hidden_size.z;

                    Int2 offset(column_pos.x - visible_center.x + vld.radius, column_pos.y - visible_center.y + vld.radius);

                    int wi = vc + vld.size.z * (offset.y + diam * (offset.x + diam * hidden_cell_index));

                    sum += vl.recon_weights[wi];
                    count++;
                }
            }

        sum /= max(1, count * 255);

        vl.reconstruction[visible_cell_index] = roundf2b(min(1.0f, max(0.0f, (sum - 0.5f) * 2.0f * params.scale + 0.5f)) * 255.0f);
    }
}

void Image_Encoder::init_random(
    const Int3 &hidden_size,
    const Array<Visible_Layer_Desc> &visible_layer_descs
) {
    this->visible_layer_descs = visible_layer_descs;

    this->hidden_size = hidden_size;

    visible_layers.resize(visible_layer_descs.size());

    // pre-compute dimensions
    int num_hidden_columns = hidden_size.x * hidden_size.y;
    int num_hidden_cells = num_hidden_columns * hidden_size.z;

    // create layers
    for (int vli = 0; vli < visible_layers.size(); vli++) {
        Visible_Layer &vl = visible_layers[vli];
        const Visible_Layer_Desc &vld = this->visible_layer_descs[vli];

        int num_visible_columns = vld.size.x * vld.size.y;
        int num_visible_cells = num_visible_columns * vld.size.z;

        int diam = vld.radius * 2 + 1;
        int area = diam * diam;

        vl.weights.resize(num_hidden_cells * area * vld.size.z);
        vl.recon_weights.resize(vl.weights.size());

        // initialize to random values
        for (int i = 0; i < vl.weights.size(); i++) {
            vl.weights[i] = rand() % 256;
            vl.recon_weights[i] = 128;
        }

        vl.reconstruction = Byte_Buffer(num_visible_cells, 0);
    }

    hidden_cis = Int_Buffer(num_hidden_columns, 0);

    hidden_acts.resize(num_hidden_cells);
    hidden_totals.resize(num_hidden_cells);

    hidden_resources = Float_Buffer(num_hidden_cells, 0.5f);
}

void Image_Encoder::step(
    const Array<Byte_Buffer_View> &inputs,
    bool learn_enabled,
    bool learn_recon
) {
    int num_hidden_columns = hidden_size.x * hidden_size.y;
    
    PARALLEL_FOR
    for (int i = 0; i < num_hidden_columns; i++)
        forward(Int2(i / hidden_size.y, i % hidden_size.y), inputs, learn_enabled);

    if (learn_enabled && learn_recon) {
        for (int vli = 0; vli < visible_layers.size(); vli++) {
            const Visible_Layer_Desc &vld = visible_layer_descs[vli];

            int num_visible_columns = vld.size.x * vld.size.y;

            unsigned int base_state = rand();

            PARALLEL_FOR
            for (int i = 0; i < num_visible_columns; i++) {
                uint64_t state = rand_get_state(base_state + i * rand_subseed_offset);

                learn_reconstruction(Int2(i / vld.size.y, i % vld.size.y), inputs[vli], vli, &state);
            }
        }
    }
}

void Image_Encoder::reconstruct(
    Int_Buffer_View recon_cis
) {
    for (int vli = 0; vli < visible_layers.size(); vli++) {
        const Visible_Layer_Desc &vld = visible_layer_descs[vli];

        int num_visible_columns = vld.size.x * vld.size.y;

        PARALLEL_FOR
        for (int i = 0; i < num_visible_columns; i++)
            reconstruct(Int2(i / vld.size.y, i % vld.size.y), recon_cis, vli);
    }
}

long Image_Encoder::size() const {
    long size = sizeof(Int3) + sizeof(Params) + hidden_cis.size() * sizeof(int) + hidden_resources.size() * sizeof(float) + sizeof(int);

    for (int vli = 0; vli < visible_layers.size(); vli++) {
        const Visible_Layer &vl = visible_layers[vli];
        const Visible_Layer_Desc &vld = visible_layer_descs[vli];

        size += sizeof(Visible_Layer_Desc) + 2 * vl.weights.size() * sizeof(Byte);
    }

    return size;
}

long Image_Encoder::state_size() const {
    return hidden_cis.size() * sizeof(int);
}

long Image_Encoder::weights_size() const {
    long size = 0;

    for (int vli = 0; vli < visible_layers.size(); vli++) {
        const Visible_Layer &vl = visible_layers[vli];

        size += 2 * vl.weights.size() * sizeof(Byte);
    }

    return size;
}

void Image_Encoder::write(
    Stream_Writer &writer
) const {
    writer.write(&hidden_size, sizeof(Int3));

    writer.write(&params, sizeof(Params));
    
    writer.write(&hidden_cis[0], hidden_cis.size() * sizeof(int));
    
    writer.write(&hidden_resources[0], hidden_resources.size() * sizeof(float));

    int num_visible_layers = visible_layers.size();

    writer.write(&num_visible_layers, sizeof(int));
    
    for (int vli = 0; vli < visible_layers.size(); vli++) {
        const Visible_Layer &vl = visible_layers[vli];
        const Visible_Layer_Desc &vld = visible_layer_descs[vli];

        writer.write(&vld, sizeof(Visible_Layer_Desc));

        writer.write(&vl.weights[0], vl.weights.size() * sizeof(Byte));
        writer.write(&vl.recon_weights[0], vl.recon_weights.size() * sizeof(Byte));
    }
}

void Image_Encoder::read(
    Stream_Reader &reader
) {
    reader.read(&hidden_size, sizeof(Int3));

    int num_hidden_columns = hidden_size.x * hidden_size.y;
    int num_hidden_cells = num_hidden_columns * hidden_size.z;

    reader.read(&params, sizeof(Params));

    hidden_cis.resize(num_hidden_columns);

    reader.read(&hidden_cis[0], hidden_cis.size() * sizeof(int));

    hidden_acts.resize(num_hidden_cells);
    hidden_totals.resize(num_hidden_cells);

    hidden_resources.resize(num_hidden_cells);

    reader.read(&hidden_resources[0], hidden_resources.size() * sizeof(float));

    int num_visible_layers;

    reader.read(&num_visible_layers, sizeof(int));

    visible_layers.resize(num_visible_layers);
    visible_layer_descs.resize(num_visible_layers);
    
    for (int vli = 0; vli < visible_layers.size(); vli++) {
        Visible_Layer &vl = visible_layers[vli];
        Visible_Layer_Desc &vld = visible_layer_descs[vli];

        reader.read(&vld, sizeof(Visible_Layer_Desc));

        int num_visible_columns = vld.size.x * vld.size.y;
        int num_visible_cells = num_visible_columns * vld.size.z;

        int diam = vld.radius * 2 + 1;
        int area = diam * diam;

        vl.weights.resize(num_hidden_cells * area * vld.size.z);
        vl.recon_weights.resize(vl.weights.size());

        reader.read(&vl.weights[0], vl.weights.size() * sizeof(Byte));
        reader.read(&vl.recon_weights[0], vl.recon_weights.size() * sizeof(Byte));

        vl.reconstruction = Byte_Buffer(num_visible_cells, 0);
    }
}

void Image_Encoder::write_state(
    Stream_Writer &writer
) const {
    writer.write(&hidden_cis[0], hidden_cis.size() * sizeof(int));
}

void Image_Encoder::read_state(
    Stream_Reader &reader
) {
    reader.read(&hidden_cis[0], hidden_cis.size() * sizeof(int));
}

void Image_Encoder::write_weights(
    Stream_Writer &writer
) const {
    for (int vli = 0; vli < visible_layers.size(); vli++) {
        const Visible_Layer &vl = visible_layers[vli];

        writer.write(&vl.weights[0], vl.weights.size() * sizeof(Byte));
        writer.write(&vl.recon_weights[0], vl.recon_weights.size() * sizeof(Byte));
    }
}

void Image_Encoder::read_weights(
    Stream_Reader &reader
) {
    for (int vli = 0; vli < visible_layers.size(); vli++) {
        Visible_Layer &vl = visible_layers[vli];

        reader.read(&vl.weights[0], vl.weights.size() * sizeof(Byte));
        reader.read(&vl.recon_weights[0], vl.recon_weights.size() * sizeof(Byte));
    }
}

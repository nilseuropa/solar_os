// ----------------------------------------------------------------------------
//  AOgmaNeo
//  Copyright(c) 2020-2025 Ogma Intelligent Systems Corp. All rights reserved.
//
//  This copy of AOgmaNeo is licensed to you under the terms described
//  in the AOGMANEO_LICENSE.md file included in this distribution.
// ----------------------------------------------------------------------------

#pragma once

#include "helpers.h"

namespace aon {
// a reinforcement learning layer
class Actor {
public:
    // visible layer descriptor
    struct Visible_Layer_Desc {
        Int3 size; // visible/input size

        int radius; // radius onto input

        // defaults
        Visible_Layer_Desc()
        :
        size(5, 5, 16),
        radius(2)
        {}
    };

    // visible layer
    struct Visible_Layer {
        Float_Buffer value_weights;
        Float_Buffer policy_weights;
    };

    // history sample for delayed updates
    struct History_Sample {
        Array<Int_Buffer> input_cis;
        Int_Buffer hidden_target_cis_prev;
        Float_Buffer hidden_values;

        float reward;
    };

    struct Params {
        float vlr; // value learning rate
        float plr; // policy learning rate
        float smoothing; // smooth value function, = 1 - lambda from TD(lambda)
        float discount; // discount factor
        float td_scale_decay; // decay of max td scale
        float value_range; // maximum range of value estimates in symlog space
        int min_steps; // minimum steps before sample can be used
        int history_iters; // number of iterations over samples

        Params()
        :
        vlr(0.1f),
        plr(0.01f),
        smoothing(0.02f),
        discount(0.99f),
        td_scale_decay(0.999f),
        value_range(10.0f),
        min_steps(16),
        history_iters(8)
        {}
    };

private:
    Int3 hidden_size; // hidden/output/action size
    int value_size; // number of value bins
    int value_num_dendrites_per_cell;
    int policy_num_dendrites_per_cell;

    // current history size - fixed after initialization. determines length of wait before updating
    int history_size;

    Int_Buffer hidden_cis; // hidden states

    Float_Buffer hidden_value_acts;
    Float_Buffer hidden_policy_acts;

    Float_Buffer value_dendrite_acts;
    Float_Buffer policy_dendrite_acts;

    Float_Buffer hidden_values; // hidden value function output buffer

    Float_Buffer hidden_td_scales;

    Circle_Buffer<History_Sample> history_samples; // history buffer, fixed length

    // visible layers and descriptors
    Array<Visible_Layer> visible_layers;
    Array<Visible_Layer_Desc> visible_layer_descs;

    // --- kernels ---

    void forward(
        const Int2 &column_pos,
        const Array<Int_Buffer_View> &input_cis,
        uint64_t* state,
        const Params &params
    );

    void learn(
        const Int2 &column_pos,
        int t,
        float mimic,
        const Params &params
    );

public:
    // initialized randomly
    void init_random(
        const Int3 &hidden_size,
        int value_size,
        int value_num_dendrites_per_cell,
        int policy_num_dendrites_per_cell,
        int history_capacity,
        const Array<Visible_Layer_Desc> &visible_layer_descs
    );

    // step (get actions and update)
    void step(
        const Array<Int_Buffer_View> &input_cis,
        Int_Buffer_View hidden_target_cis_prev,
        bool learn_enabled,
        float reward,
        float mimic,
        const Params &params
    );

    void clear_state();

    // serialization
    long size() const; // returns size in bytes
    long state_size() const; // returns size of state in bytes
    long weights_size() const; // returns size of weights in bytes

    void write(
        Stream_Writer &writer
    ) const;

    void read(
        Stream_Reader &reader
    );

    void write_state(
        Stream_Writer &writer
    ) const;

    void read_state(
        Stream_Reader &reader
    );

    void write_weights(
        Stream_Writer &writer
    ) const;

    void read_weights(
        Stream_Reader &reader
    );

    // get number of visible layers
    int get_num_visible_layers() const {
        return visible_layers.size();
    }

    // get a visible layer
    const Visible_Layer &get_visible_layer(
        int i // index of layer
    ) const {
        return visible_layers[i];
    }

    // get a visible layer descriptor
    const Visible_Layer_Desc &get_visible_layer_desc(
        int i // index of layer
    ) const {
        return visible_layer_descs[i];
    }

    // get hidden state/output/actions
    const Int_Buffer &get_hidden_cis() const {
        return hidden_cis;
    }

    // get hidden activations (probabilities) for actions
    const Float_Buffer &get_hidden_acts() const {
        return hidden_policy_acts;
    }

    // get hidden values
    const Float_Buffer &get_hidden_values() const {
        return hidden_values;
    }

    // get the hidden size
    const Int3 &get_hidden_size() const {
        return hidden_size;
    }

    int get_history_capacity() const {
        return history_samples.size();
    }

    int get_history_size() const {
        return history_size;
    }
};
}

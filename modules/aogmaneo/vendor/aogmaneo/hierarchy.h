// ----------------------------------------------------------------------------
//  AOgmaNeo
//  Copyright(c) 2020-2025 Ogma Intelligent Systems Corp. All rights reserved.
//
//  This copy of AOgmaNeo is licensed to you under the terms described
//  in the AOGMANEO_LICENSE.md file included in this distribution.
// ----------------------------------------------------------------------------

#pragma once

#include "encoder.h"
#include "decoder.h"
#include "actor.h"

namespace aon {
// type of hierarchy input layer
enum IO_Type {
    none = 0,
    prediction = 1,
    action = 2
};

// a sph
class Hierarchy {
public:
    struct IO_Desc {
        Int3 size;
        IO_Type type;

        int num_dendrites_per_cell; // also for policy

        int up_radius; // encoder radius
        int down_radius; // decoder radius, also shared with actor if there is one

        int value_size; // resution of values for RL
        int value_num_dendrites_per_cell; // value function dendrites
        int history_capacity; // max actor credit assignment horizon

        IO_Desc(
            const Int3 &size = Int3(5, 5, 16),
            IO_Type type = prediction,
            int num_dendrites_per_cell = 4,
            int up_radius = 2,
            int down_radius = 2,
            int value_size = 128,
            int value_num_dendrites_per_cell = 2,
            int history_capacity = 512
        )
        :
        size(size),
        type(type),
        num_dendrites_per_cell(num_dendrites_per_cell),
        up_radius(up_radius),
        down_radius(down_radius),
        value_size(value_size),
        value_num_dendrites_per_cell(value_num_dendrites_per_cell),
        history_capacity(history_capacity)
        {}
    };

    // describes a layer for construction. for the first layer, the IO_Desc overrides the parameters that are the same name
    struct Layer_Desc {
        Int3 hidden_size; // size of hidden layer

        int num_dendrites_per_cell;

        int up_radius; // encoder radius
        int recurrent_radius; // encoder onto self radius, -1 to disable
        int down_radius; // decoder radius, also shared with actor if there is one

        Layer_Desc(
            const Int3 &hidden_size = Int3(5, 5, 16),
            int num_dendrites_per_cell = 4,
            int up_radius = 2,
            int recurrent_radius = 0,
            int down_radius = 2
        )
        :
        hidden_size(hidden_size),
        num_dendrites_per_cell(num_dendrites_per_cell),
        up_radius(up_radius),
        recurrent_radius(recurrent_radius),
        down_radius(down_radius)
        {}
    };

    struct Layer_Params {
        Decoder::Params decoder;
        Encoder::Params encoder;

        float recurrent_importance;

        Layer_Params()
        :
        recurrent_importance(0.5f)
        {}
    };

    struct IO_Params {
        Decoder::Params decoder;
        Actor::Params actor;

        // additional
        float importance;

        IO_Params()
        :
        importance(1.0f)
        {}
    };

    struct Params {
        Array<Layer_Params> layers;
        Array<IO_Params> ios;

        Byte anticipation;

        Params()
        :
        anticipation(true)
        {}
    };

private:
    // layers
    Array<Encoder> encoders;
    Array<Array<Decoder>> decoders;
    Array<Actor> actors;
    Array<Int_Buffer> hidden_cis_prev;
    Array<Int_Buffer> feedback_cis_prev;

    // for mapping first layer Decoders
    Int_Buffer i_indices;
    Int_Buffer d_indices;

    // input dimensions
    Array<Int3> io_sizes;
    Array<Byte> io_types;

public:
    // parameters
    Params params;

    // default
    Hierarchy() {}

    Hierarchy(
        const Array<IO_Desc> &io_descs, // input-output descriptors
        const Array<Layer_Desc> &layer_descs // descriptors for layers
    ) {
        init_random(io_descs, layer_descs);
    }
    
    // create a randomly initialized hierarchy
    void init_random(
        const Array<IO_Desc> &io_descs, // input-output descriptors
        const Array<Layer_Desc> &layer_descs // descriptors for layers
    );

    // simulation step/tick
    void step(
        const Array<Int_Buffer_View> &input_cis, // inputs to remember
        bool learn_enabled = true, // whether learning is enabled
        float reward = 0.0f, // reward
        float mimic = 0.0f // mimic mode for bc on actor
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

    // get the number of layers (encoders)
    int get_num_layers() const {
        return encoders.size();
    }

    bool io_layer_exists(
        int i
    ) const {
        return d_indices[i] != -1;
    }

    bool is_layer_recurrent(
        int l
    ) const {
        return (l == 0 ? encoders[l].get_num_visible_layers() > io_sizes.size() : encoders[l].get_num_visible_layers() > 1);
    }

    // retrieve predictions
    const Int_Buffer &get_prediction_cis(
        int i
    ) const {
        if (io_types[i] == action)
            return actors[d_indices[i]].get_hidden_cis();

        return decoders[0][d_indices[i]].get_hidden_cis();
    }

    // retrieve predictions activations
    const Float_Buffer &get_prediction_acts(
        int i
    ) const {
        if (io_types[i] == action)
            return actors[d_indices[i]].get_hidden_acts();

        return decoders[0][d_indices[i]].get_hidden_acts();
    }

    // get value estimate from actor
    const Float_Buffer &get_prediction_values(
        int i
    ) const {
        assert(io_types[i] == action);

        return actors[d_indices[i]].get_hidden_values();
    }

    // number of io layers
    int get_num_io() const {
        return io_sizes.size();
    }

    // get input/output sizes
    const Int3 &get_io_size(
        int i
    ) const {
        return io_sizes[i];
    }

    // get input/output types
    IO_Type get_io_type(
        int i
    ) const {
        return static_cast<IO_Type>(io_types[i]);
    }

    int get_num_encoder_visible_layers(
        int l
    ) const {
        return encoders[l].get_num_visible_layers();
    }

    // retrieve a sparse coding layer
    Encoder &get_encoder(
        int l
    ) {
        return encoders[l];
    }

    // retrieve a sparse coding layer, const version
    const Encoder &get_encoder(
        int l
    ) const {
        return encoders[l];
    }

    int get_num_decoders(
        int l
    ) const {
        return decoders[l].size();
    }

    // retrieve by index
    Decoder &get_decoder(
        int l,
        int i
    ) {
        if (l == 0)
            return decoders[l][d_indices[i]];

        return decoders[l][i];
    }

    const Decoder &get_decoder(
        int l,
        int i
    ) const {
        if (l == 0)
            return decoders[l][d_indices[i]];

        return decoders[l][i];
    }

    Actor &get_actor(
        int i
    ) {
        return actors[d_indices[i]];
    }

    const Actor &get_actor(
        int i
    ) const {
        return actors[d_indices[i]];
    }

    const Int_Buffer &get_i_indices() const {
        return i_indices;
    }

    const Int_Buffer &get_d_indices() const {
        return d_indices;
    }
};
}

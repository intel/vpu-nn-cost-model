// Copyright © 2022 Intel Corporation
// SPDX-License-Identifier: Apache 2.0
// LEGAL NOTICE: Your use of this software and any required dependent software (the “Software Package”)
// is subject to the terms and conditions of the software license agreements for the Software Package,
// which may also include notices, disclaimers, or license terms for third party or open source software
// included in or with the Software Package, and your use indicates your acceptance of all such terms.
// Please refer to the “third-party-programs.txt” or other similarly-named text file included with the
// Software Package for additional details.

#include "inference/model.h"

namespace VPUNN {

InferenceModel::InferenceModel(const char* filename): initialized(false) {
    std::ifstream myFile;

    myFile.open(filename, std::ios::binary | std::ios::in);
    if (myFile.fail()) {
        // File does not exist code here
        return;
    }
    myFile.seekg(0, std::ios::end);
    const auto length = myFile.tellg();
    myFile.seekg(0, std::ios::beg);

    buf.resize(static_cast<size_t>(length));

    myFile.read(buf.data(), length);
    myFile.close();

    flatbuffers::Verifier verifier(reinterpret_cast<const uint8_t*>(buf.data()), buf.size());
    if (!(VPUNN_SCHEMA::VerifyModelBuffer(verifier))) {
        return;
    }

    model = VPUNN_SCHEMA::GetModel(buf.data());
    initialized = true;
}

InferenceModel::InferenceModel(const char* data, size_t length, bool with_copy): initialized(false) {
    flatbuffers::Verifier verifier(reinterpret_cast<const uint8_t*>(data), length);
    if (!(VPUNN_SCHEMA::VerifyModelBuffer(verifier))) {
        return;
    }

    if (with_copy) {
        buf.assign(data, data + length);
        model = VPUNN_SCHEMA::GetModel(buf.data());
    } else {
        model = VPUNN_SCHEMA::GetModel(data);
    }

    initialized = true;
}

void InferenceModel::allocate_tensors(const unsigned int batch) {
    auto tensors = model->tensors();
    auto buffers = model->buffers();
    for (auto it = tensors->begin(); it != tensors->end(); ++it) {
        // Batch only activations
        const int actual_batch = (it->buffer() != 0) ? 1 : batch;
        // Create the new tensor structure
        auto tensor = std::make_shared<VPUNN::Tensor<float>>(parse_vector(it->shape(), actual_batch));
        if (it->buffer() != 0) {
            // in this case, copy the new data
            auto array = buffers->Get(it->buffer())->data();
            tensor->assign((const float*)(array->data()), array->Length());
        } else {
            // Fill with zeros
            tensor->fill(0);
        }
        tensor_map.push_back(tensor);
    }
}

void InferenceModel::predict() {
    auto layers = model->operators();

    for (flatbuffers::uoffset_t idx = 0; idx < layers->Length(); idx++) {
        // Create the new tensor structure
        run_layer(layers->Get(idx));
    }
}

void InferenceModel::run_layer(const VPUNN_SCHEMA::Layer* layer) {
    auto inputs = get_tensors_from_index(layer->inputs());
    auto outputs = get_tensors_from_index(layer->outputs());

    switch (layer->implementation_type()) {
    case VPUNN_SCHEMA::LayerType_FullyConnectedLayer:
        // inputs: activations, weights, bias
        Dense(inputs[1], inputs[0], outputs[0]);

        if (inputs.size() > 2) {
            Bias(inputs[2], outputs[0]);
        }
        break;
    case VPUNN_SCHEMA::LayerType_L2NormalizationLayer:
        L2Normalization(inputs[0], outputs[0]);
        break;
    case VPUNN_SCHEMA::LayerType_kNNLayer:
        // weights, targets, activations
        kNN(inputs[1], inputs[2], inputs[0], outputs[0], layer->implementation_as_kNNLayer()->n_neighbors());
        break;
    default:
        break;
    }

    switch (layer->activation_function()) {
    case VPUNN_SCHEMA::ActivationFunctionType_RELU:
        for (int idx = 0; idx < outputs[0]->size(); idx++) {
            if ((*outputs[0])[idx] < 0)
                (*outputs[0])[idx] = 0;
        }
        break;
    case VPUNN_SCHEMA::ActivationFunctionType_SIGMOID:
        Sigmoid(outputs[0]);
    default:
        break;
    }
}

}  // namespace VPUNN

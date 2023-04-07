// Copyright (C) 2018-2023 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "openvino/frontend/tensorflow/frontend.hpp"

#include "graph_iterator_proto.hpp"
#include "graph_iterator_proto_txt.hpp"
#include "graph_iterator_saved_model.hpp"
#include "helper_transforms/block_lstm_replacer.hpp"
#include "helper_transforms/const_to_result_remover.hpp"
#include "helper_transforms/embedding_segments_feature_fusing.hpp"
#include "helper_transforms/gru_block_cell_replacer.hpp"
#include "helper_transforms/saved_model_unused_remover.hpp"
#include "input_model.hpp"
#include "op_table.hpp"
#include "openvino/frontend/tensorflow/extension/conversion.hpp"
#include "openvino/frontend/tensorflow/graph_iterator.hpp"
#include "openvino/op/util/multi_subgraph_base.hpp"
#include "openvino/pass/manager.hpp"
#include "openvino/util/common_util.hpp"
#include "openvino/util/log.hpp"
#include "so_extension.hpp"
#include "tf_framework_node.hpp"
#include "transformations/common_optimizations/reverse_shape_and_type_infer.hpp"
#include "transformations/transpose_sinking/ts_general.hpp"
#include "translate_session.hpp"
#include "utils.hpp"

using namespace ov;
using namespace ov::frontend::tensorflow;

namespace {

void get_unsupported_operations_and_failures(const std::shared_ptr<Model>& model,
                                             std::vector<std::string>& unsupported_operations,
                                             std::unordered_map<std::string, std::string>& failures) {
    for (const auto& node : model->get_ordered_ops()) {
        if (const auto& fw_node = ov::as_type_ptr<FrameworkNode>(node)) {
            auto op_type = fw_node->get_decoder()->get_op_type();
            auto fw_node_attrs = fw_node->get_attrs();
            if (fw_node_attrs.find(FrameworkNode::failed_conversion_key) != fw_node_attrs.end() &&
                failures.count(op_type) == 0) {
                // save only the first encountered failure that is more improtant for developer
                // that means the translator is found but the conversion is failed
                failures[op_type] = fw_node_attrs.at(FrameworkNode::failed_conversion_key);
            } else if (std::find(unsupported_operations.begin(), unsupported_operations.end(), op_type) ==
                       unsupported_operations.end()) {
                // found new unsupported operation
                unsupported_operations.push_back(op_type);
            }
        }
        if (const auto& fw_node = ov::as_type_ptr<ov::op::util::MultiSubGraphOp>(node)) {
            int subgraphs_size = static_cast<int>(fw_node->get_internal_subgraphs_size());
            for (int i = 0; i < subgraphs_size; ++i) {
                get_unsupported_operations_and_failures(fw_node->get_function(i), unsupported_operations, failures);
            }
        }
    }
}

void translate_framework_node(const std::shared_ptr<FrameworkNode>& node,
                              const TranslatorDictionaryType& op_translators) {
    auto type = node->get_op_type();

    const auto& TRANSLATE_OP_MAP = op_translators;
    auto translator_it = TRANSLATE_OP_MAP.find(type);
    FRONT_END_OP_CONVERSION_CHECK(translator_it != TRANSLATE_OP_MAP.end(), "No translator found for ", type, " node.");

    ov::OutputVector ov_inputs = node->input_values();
    NodeContext node_ctx(node->get_decoder(), ov_inputs);
    auto new_node_outputs = translator_it->second(node_ctx);

    auto new_output = new_node_outputs.begin();
    auto old_outputs = node->outputs();
    auto old_output = old_outputs.begin();

    for (; new_output != new_node_outputs.end() && old_output != old_outputs.end(); ++old_output, ++new_output) {
        old_output->replace(new_output->port);
    }
}
}  // namespace

FrontEnd::FrontEnd() : m_op_translators(tensorflow::op::get_supported_ops()) {}

/// \brief Check if FrontEndTensorflow can recognize model from given parts
bool FrontEnd::supported_impl(const std::vector<ov::Any>& variants) const {
    // Last boolean flag in `variants` (if presented) is reserved for FE configuration
    size_t extra_variants_num = variants.size() > 0 && variants[variants.size() - 1].is<bool>() ? 1 : 0;
    // TODO: Support other TensorFlow formats: SavedModel, .meta, checkpoint, pbtxt
    if (variants.size() != 1 + extra_variants_num)
        return false;

    if (variants[0].is<std::string>()) {
        std::string model_path = variants[0].as<std::string>();
        if (ov::util::ends_with(model_path, ".pb") && GraphIteratorProto::is_supported(model_path)) {
            // handle binary protobuf format
            // for automatic deduction of the frontend to convert the model
            // we have more strict rule that is to have `.pb` extension in the path
            return true;
        } else if (GraphIteratorSavedModel::is_supported(model_path)) {
            return true;
        } else if (GraphIteratorProtoTxt::is_supported(model_path)) {
            // handle text protobuf format
            return true;
        }
    }
#if defined(OPENVINO_ENABLE_UNICODE_PATH_SUPPORT) && defined(_WIN32)
    else if (variants[0].is<std::wstring>()) {
        std::wstring suffix = L".pb";
        std::wstring model_path = variants[0].as<std::wstring>();
        if (ov::util::ends_with(model_path, suffix) && GraphIteratorProto::is_supported(model_path)) {
            // handle binary protobuf format with a path in Unicode
            // for automatic deduction of the frontend to convert the model
            // we have more strict rule that is to have `.pb` extension in the path
            return true;
        } else if (GraphIteratorSavedModel::is_supported(model_path)) {
            return true;
        } else if (GraphIteratorProtoTxt::is_supported(model_path)) {
            // handle text protobuf format
            return true;
        }
    }
#endif
    else if (variants[0].is<GraphIterator::Ptr>()) {
        // this is used for OpenVINO with TensorFlow Integration
        return true;
    }
    return false;
}

ov::frontend::InputModel::Ptr FrontEnd::load_impl(const std::vector<ov::Any>& variants) const {
    // TODO: Support other TensorFlow formats: SavedModel, .meta, checkpoint

    // Last boolean flag in `variants` (if presented) is reserved for FE configuration
    size_t extra_variants_num = variants.size() > 0 && variants[variants.size() - 1].is<bool>() ? 1 : 0;
    FRONT_END_GENERAL_CHECK(variants.size() == 1 + extra_variants_num,
                            "[TensorFlow Frontend] Internal error or inconsistent input model: the frontend supports "
                            "only frozen binary protobuf format.");

    if (variants[0].is<std::string>()) {
        auto model_path = variants[0].as<std::string>();
        if (GraphIteratorProto::is_supported(model_path)) {
            // handle binary protobuf format
            return std::make_shared<InputModel>(std::make_shared<GraphIteratorProto>(model_path), m_telemetry);
        } else if (GraphIteratorSavedModel::is_supported(model_path)) {
            std::shared_ptr<GraphIteratorSavedModel> graph_iterator;
            if (variants.size() > 1 && variants[1].is<std::string>()) {
                graph_iterator = std::make_shared<GraphIteratorSavedModel>(model_path, variants[1].as<std::string>());
            } else {
                graph_iterator = std::make_shared<GraphIteratorSavedModel>(model_path, std::string("serve"));
            }
            return std::make_shared<InputModel>(graph_iterator,
                                                m_telemetry,
                                                graph_iterator->get_variables_index(),
                                                graph_iterator->get_saved_model_input_names(),
                                                graph_iterator->get_saved_model_output_names());
        } else if (GraphIteratorProtoTxt::is_supported(model_path)) {
            // handle text protobuf format
            return std::make_shared<InputModel>(std::make_shared<GraphIteratorProtoTxt>(model_path), m_telemetry);
        }
    }
#if defined(OPENVINO_ENABLE_UNICODE_PATH_SUPPORT) && defined(_WIN32)
    else if (variants[0].is<std::wstring>()) {
        std::wstring model_path = variants[0].as<std::wstring>();
        if (GraphIteratorProto::is_supported(model_path)) {
            // handle binary protobuf format with a path in Unicode
            return std::make_shared<InputModel>(std::make_shared<GraphIteratorProto>(model_path), m_telemetry);
        } else if (GraphIteratorSavedModel::is_supported(model_path)) {
            std::shared_ptr<GraphIteratorSavedModel> graph_iterator;
            if (variants.size() > 1 && variants[1].is<std::string>()) {
                graph_iterator = std::make_shared<GraphIteratorSavedModel>(
                    model_path,
                    ov::util::wstring_to_string(variants[1].as<std::wstring>()));
            } else {
                graph_iterator = std::make_shared<GraphIteratorSavedModel>(model_path, std::string("serve"));
            }
            return std::make_shared<InputModel>(graph_iterator,
                                                m_telemetry,
                                                graph_iterator->get_variables_index(),
                                                graph_iterator->get_saved_model_input_names(),
                                                graph_iterator->get_saved_model_output_names());
        } else if (GraphIteratorProtoTxt::is_supported(model_path)) {
            // handle text protobuf format with a path in Unicode
            return std::make_shared<InputModel>(std::make_shared<GraphIteratorProtoTxt>(model_path), m_telemetry);
        }
    }
#endif
    else if (variants[0].is<GraphIterator::Ptr>()) {
        // this is used for OpenVINO with TensorFlow Integration
        auto graph_iterator = variants[0].as<GraphIterator::Ptr>();
        return std::make_shared<InputModel>(graph_iterator, m_telemetry);
    }

    FRONT_END_GENERAL_CHECK(false,
                            "[TensorFlow Frontend] Internal error or inconsistent input model: the frontend supports "
                            "only frozen binary protobuf format.");

    return nullptr;
}

std::shared_ptr<ov::Model> FrontEnd::convert(const ov::frontend::InputModel::Ptr& model) const {
    auto f = convert_partially(model);

    std::unordered_map<std::string, std::string> failures;
    std::vector<std::string> unsupported_operations;
    get_unsupported_operations_and_failures(f, unsupported_operations, failures);

    std::stringstream exception_message;
    for (const auto& failure : failures) {
        if (m_telemetry) {
            // TODO: 105173 support anonymization of exception message in order to send to telemetry
        }
        exception_message << "[TensorFlow Frontend] Internal error: conversion is failed for " + failure.first +
                                 " operation with a message:\n" + failure.second + "\n";
    }

    if (m_telemetry) {
        for (const auto& unsupported_operation : unsupported_operations) {
            m_telemetry->send_event("error_cause", "tf_" + unsupported_operation);
        }
    }
    // TODO 107500: report the full list of unsupported operations
    // also, communicate with MO for the fallback to the legacy FE
    // via OpConversionFailure exception that will store all failures and unsupported_operations
    if (unsupported_operations.size() > 0) {
        exception_message << "[TensorFlow Frontend] Internal error: No translator found for " +
                                 unsupported_operations[0] + " node.";
    }

    bool is_conversion_successful = ((unsupported_operations.size() == 0) && (failures.size() == 0));
    FRONT_END_OP_CONVERSION_CHECK(is_conversion_successful, exception_message.str());

    return f;
}

std::shared_ptr<ov::Model> FrontEnd::convert_partially(const ov::frontend::InputModel::Ptr& model) const {
    auto model_tf = std::dynamic_pointer_cast<InputModel>(model);
    FRONT_END_GENERAL_CHECK(model_tf != nullptr, "Invalid input model");

    if (!m_transformation_extensions.empty()) {
        auto function = decode(model);

        ov::pass::Manager manager;
        for (const auto& transformation : m_transformation_extensions) {
            transformation->register_pass(manager);
        }
        manager.run_passes(function);
        convert(function);
        return function;
    }

    // create a shared pointer to the cloned dictionary of translators
    auto translator_map = std::make_shared<TranslatorDictionaryType>(m_op_translators);

    std::shared_ptr<ov::Model> f;
    TranslateSession translate_session(model, translator_map, "TensorFlow_Frontend_IR");
    try {
        f = translate_session.get_converted_model();
    } catch (const std::exception&) {
        if (m_telemetry) {
            // TODO: 105173 support anonymization of exception message in order to send to telemetry
        }
        throw;
    }
    normalize(f);

    return f;
}

std::shared_ptr<ov::Model> FrontEnd::decode(const ov::frontend::InputModel::Ptr& model) const {
    auto translator_map = std::make_shared<TranslatorDictionaryType>();

    const std::set<std::string> required_types{"Placeholder", "NoOp"};
    for (const auto& name : required_types) {
        translator_map->emplace(name, m_op_translators.at(name));
    }

    std::shared_ptr<ov::Model> f;
    TranslateSession translate_session(model, translator_map, "TensorFlow_Frontend_IR");
    try {
        f = translate_session.get_converted_model();
    } catch (const std::exception&) {
        if (m_telemetry) {
            // TODO: 105173 support anonymization of exception message in order to send to telemetry
        }
        throw;
    }

    return f;
}

void FrontEnd::convert(const std::shared_ptr<ov::Model>& partiallyConverted) const {
    for (const auto& node : partiallyConverted->get_ordered_ops()) {
        if (ov::is_type<FrameworkNode>(node)) {
            translate_framework_node(std::dynamic_pointer_cast<FrameworkNode>(node), m_op_translators);
        }
    }
    for (const auto& result : partiallyConverted->get_results()) {
        result->validate_and_infer_types();
    }

    normalize(partiallyConverted);
}

void FrontEnd::normalize(const std::shared_ptr<ov::Model>& model) const {
    {
        // run transformations to convert sub-graphs with intermediate (or FrameworkNode) operations
        // into sub-graphs with only OpenVINO operations
        ov::pass::Manager manager;
        manager.register_pass<pass::SavedModelUnusedRemover>();
        manager.register_pass<pass::EmbeddingSegmentSingleFeatureFusion>();
        manager.register_pass<pass::BlockLSTMReplacer>();
        manager.register_pass<pass::GRUBlockCellReplacer>();
        manager.register_pass<pass::ConstToResultRemover>();
        manager.run_passes(model);
    }

    // TODO 107554: TSGeneral can fail on models with Framework nodes (not converted to OV opset)
    std::unordered_map<std::string, std::string> failures;
    std::vector<std::string> unsupported_operations;
    get_unsupported_operations_and_failures(model, unsupported_operations, failures);
    if (unsupported_operations.size() > 0 || failures.size() > 0) {
        return;
    }

    {
        // perform transpose sinking and reverse infer if the model contains only OpenVINO operations
        ov::pass::Manager manager;
        manager.register_pass<ov::pass::TransposeSinkingGeneral>();
        manager.register_pass<ov::pass::ReverseShapeAndTypeInfer>();
        manager.run_passes(model);
    }
}

void FrontEnd::add_extension(const std::shared_ptr<ov::Extension>& extension) {
    if (auto telemetry = std::dynamic_pointer_cast<TelemetryExtension>(extension)) {
        m_telemetry = telemetry;
    } else if (auto transformation = std::dynamic_pointer_cast<DecoderTransformationExtension>(extension)) {
        m_transformation_extensions.push_back(transformation);
    } else if (const auto& so_ext = std::dynamic_pointer_cast<ov::detail::SOExtension>(extension)) {
        add_extension(so_ext->extension());
        m_extensions.push_back(so_ext);
    } else if (auto common_conv_ext = std::dynamic_pointer_cast<ov::frontend::ConversionExtension>(extension)) {
        m_conversion_extensions.push_back(common_conv_ext);
        if (common_conv_ext->get_converter()) {
            m_op_translators[common_conv_ext->get_op_type()] =
                ov::frontend::tensorflow::CreatorFunctionIndexed([=](const tensorflow::NodeContext& context) {
                    return common_conv_ext->get_converter()(context);
                });
        } else if (common_conv_ext->get_converter_named_and_indexed()) {
            m_op_translators[common_conv_ext->get_op_type()] =
                ov::frontend::tensorflow::CreatorFunctionNamedAndIndexed([=](const tensorflow::NodeContext& context) {
                    return common_conv_ext->get_converter_named_and_indexed()(context);
                });
        }
        // Ignore other types of extensions in particular CreatorFunctionNamed which cannot be used with tensorflow
        // frontend
    } else if (const auto& tensorflow_conv_ext =
                   std::dynamic_pointer_cast<ov::frontend::tensorflow::ConversionExtension>(extension)) {
        m_conversion_extensions.push_back(tensorflow_conv_ext);
        m_op_translators[tensorflow_conv_ext->get_op_type()] = tensorflow_conv_ext->get_converter();
    }
}

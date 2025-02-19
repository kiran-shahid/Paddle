// Copyright (c) 2023 PaddlePaddle Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <iostream>

#include "paddle/fluid/ir/transforms/pd_op_to_kernel_pass.h"

#include "paddle/fluid/ir/dialect/kernel_attribute.h"
#include "paddle/fluid/ir/dialect/kernel_dialect.h"
#include "paddle/fluid/ir/dialect/kernel_op.h"
#include "paddle/fluid/ir/dialect/kernel_type.h"
#include "paddle/fluid/ir/dialect/op_yaml_info_util.h"
#include "paddle/fluid/ir/dialect/pd_attribute.h"
#include "paddle/fluid/ir/dialect/pd_dialect.h"
#include "paddle/fluid/ir/dialect/utils.h"
#include "paddle/fluid/ir/interface/op_yaml_info.h"
#include "paddle/fluid/ir/interface/op_yaml_info_parser.h"
#include "paddle/phi/api/lib/data_transform.h"
#include "paddle/phi/api/lib/kernel_dispatch.h"
#include "paddle/phi/common/place.h"
#include "paddle/phi/core/compat/convert_utils.h"
#include "paddle/phi/core/kernel_factory.h"
namespace paddle {
namespace dialect {

const int init_on_gpu_threashold = 1000;

phi::KernelKey GetKernelKey(
    ir::Operation* op,
    const phi::Place& place,
    const std::unordered_map<ir::Value, ir::OpResult>& map_value_pair,
    const dialect::OpYamlInfoParser* op_info_parser = nullptr) {
  if (op->name() == "pd.feed") {
    // NOTE, for now feed op don't need a kernel, so the data type from Op
    // Result the next op use base program datatype
    return {phi::Backend::CPU,
            phi::DataLayout::ANY,
            TransToPhiDataType(
                op->result(0).type().dyn_cast<DenseTensorType>().dtype())};
  }
  phi::Backend kernel_backend = phi::Backend::UNDEFINED;
  phi::DataLayout kernel_layout = phi::DataLayout::UNDEFINED;
  phi::DataType kernel_data_type = phi::DataType::UNDEFINED;

  if (op_info_parser != nullptr) {
    // only suppurt non vector input for now
    int tensor_input_number = op_info_parser->InputTensorNumber();

    auto attr_map = op->attributes();
    auto& data_type_info = op_info_parser->OpRuntimeInfo().kernel_key_dtype;

    if (data_type_info.size() > 0 && data_type_info[0] != "") {
      // only support single input and attribute
      auto slot_name = data_type_info[0];
      auto& input_map = op_info_parser->Name2Id();

      if (input_map.count(slot_name)) {
        // parse from input
        int in_index = input_map.at(slot_name);

        dialect::DenseTensorType type =
            op->operand(in_index)
                .type()
                .dyn_cast<paddle::dialect::DenseTensorType>();
        kernel_data_type = TransToPhiDataType(type.dtype());
      } else {
        PADDLE_ENFORCE_EQ(attr_map.count(slot_name),
                          true,
                          phi::errors::PreconditionNotMet(
                              "[%s] MUST in attribute map", slot_name));

        auto attr_type = op_info_parser->AttrTypeName(slot_name);
        PADDLE_ENFORCE_EQ(attr_type,
                          "paddle::dialect::DataTypeAttribute",
                          phi::errors::PreconditionNotMet(
                              "Type of [%s] should be DataType", slot_name));
        kernel_data_type = attr_map.at(slot_name)
                               .dyn_cast<paddle::dialect::DataTypeAttribute>()
                               .data();
      }
    }

    // parse all the input tensor
    if (tensor_input_number == 0 || op->name() == "pd.full_") {
      // all the information have to get from attribute and context

      if (op->name() == "pd.uniform") {
        // try to process uniform, use shape to determin backend
        // TODO(phlrain): shuold support other initilize op
        auto define_op = op->operand(0).GetDefiningOp();
        if (define_op->name() == "pd.full_int_array") {
          auto shape = define_op->attributes()
                           .at("value")
                           .dyn_cast<dialect::IntArrayAttribute>()
                           .data()
                           .GetData();

          size_t numel = 1;
          for (auto& s : shape) {
            numel *= s;
          }
          if (numel > init_on_gpu_threashold) {
            kernel_backend = phi::Backend::GPU;
          }
        }
      }

      if (kernel_backend == phi::Backend::UNDEFINED) {
        kernel_backend = paddle::experimental::ParseBackend(place);
      }
    }
  }

  if (op->num_operands() > 0) {
    paddle::experimental::detail::KernelKeyParser kernel_key_parser;

    for (size_t i = 0; i < op->num_operands(); ++i) {
      // NOTE, only op with OpYamlInfo can have TensorArr
      if (op_info_parser != nullptr && op_info_parser->IsTensorAttribute(i)) {
        continue;
      }

      auto input_tmp = op->operand(i);
      auto new_input_tmp = map_value_pair.at(input_tmp);

      auto input_type = new_input_tmp.type();
      dialect::AllocatedDenseTensorType type;
      if (input_type.isa<dialect::AllocatedDenseTensorType>()) {
        type = input_type.dyn_cast<dialect::AllocatedDenseTensorType>();
      } else if (input_type.isa<ir::VectorType>()) {
        type = input_type.dyn_cast<ir::VectorType>()[0]
                   .dyn_cast<dialect::AllocatedDenseTensorType>();
      }

      // fake tensor here
      auto ptr = new phi::Allocation(nullptr, 0, type.place());

      std::shared_ptr<phi::Allocation> holder(ptr);

      auto dtype = TransToPhiDataType(type.dtype());

      phi::DenseTensorMeta meta(
          dtype, type.dims(), type.data_layout(), type.lod(), type.offset());

      phi::DenseTensor fake_tensor(holder, meta);

      kernel_key_parser.AssignKernelKeySet(fake_tensor);
    }

    auto kernel_key_set = kernel_key_parser.key_set;

    auto kernel_key = kernel_key_set.GetHighestPriorityKernelKey();

    if (kernel_backend == phi::Backend::UNDEFINED) {
      kernel_backend = kernel_key.backend();
    }
    if (kernel_layout == phi::DataLayout::UNDEFINED) {
      kernel_layout = kernel_key.layout();
    }
    if (kernel_data_type == phi::DataType::UNDEFINED) {
      kernel_data_type = kernel_key.dtype();
    }
  }

  if (kernel_backend == phi::Backend::UNDEFINED) {
    kernel_backend = paddle::experimental::ParseBackend(place);
  }

  phi::KernelKey res(kernel_backend, kernel_layout, kernel_data_type);
  return res;
}

std::unique_ptr<ir::Program> PdOpLowerToKernelPass(ir::Program* prog) {
  auto program = std::make_unique<ir::Program>(ir::IrContext::Instance());

  auto block = prog->block();
  phi::Place cpu_place(phi::AllocationType::CPU);

  ir::IrContext* ctx = ir::IrContext::Instance();
  ctx->GetOrRegisterDialect<paddle::dialect::PaddleDialect>();
  ctx->GetOrRegisterDialect<paddle::dialect::PaddleKernelDialect>();

  std::unordered_map<ir::Operation*, ir::Operation*> map_op_pair;
  std::unordered_map<ir::Value, ir::OpResult> map_value_pair;

  std::string op_name = paddle::dialect::PhiKernelOp::name();

  ir::OpInfo op_info = ctx->GetRegisteredOpInfo(op_name);

  for (auto it = block->begin(); it != block->end(); ++it) {
    VLOG(6) << "op name " << (*it)->name();
    paddle::dialect::OpYamlInfoInterface op_info_interface =
        (*it)->dyn_cast<paddle::dialect::OpYamlInfoInterface>();
    OpYamlInfoParser* op_info_parser = nullptr;
    if (op_info_interface) {
      op_info_parser = new OpYamlInfoParser(op_info_interface.GetOpInfo());
    }
    auto kernel_key =
        GetKernelKey(*it, cpu_place, map_value_pair, op_info_parser);
    VLOG(6) << "kernel type " << kernel_key;
    // create new Op

    // only for single output
    // need update new kernel key layout and data tyep

    std::vector<ir::Type> op_output_types;
    if ((*it)->num_results() > 0) {
      for (size_t i = 0; i < (*it)->num_results(); ++i) {
        auto result_type = (*it)->result(i).type();
        if (result_type.isa<dialect::DenseTensorType>()) {
          auto allocated_dense_tensor_dtype =
              paddle::dialect::AllocatedDenseTensorType::get(
                  ctx,
                  phi::TransToPhiPlace(kernel_key.backend()),
                  result_type.dyn_cast<dialect::DenseTensorType>());
          op_output_types.push_back(allocated_dense_tensor_dtype);
        } else if (result_type.isa<ir::VectorType>()) {
          std::vector<ir::Type> vec_inner_types;
          auto base_types = result_type.dyn_cast<ir::VectorType>().data();
          for (size_t j = 0; j < base_types.size(); ++j) {
            if (base_types[j].isa<dialect::DenseTensorType>()) {
              auto allocated_dense_tensor_dtype =
                  paddle::dialect::AllocatedDenseTensorType::get(
                      ctx,
                      phi::TransToPhiPlace(kernel_key.backend()),
                      base_types[j].dyn_cast<dialect::DenseTensorType>());
              vec_inner_types.push_back(allocated_dense_tensor_dtype);
            } else {
              PADDLE_THROW(phi::errors::Unimplemented(
                  "only support dense tensor in vector type for now"));
            }
          }

          ir::Type t1 = ir::VectorType::get(ctx, vec_inner_types);
          op_output_types.push_back(t1);
        } else {
          PADDLE_THROW(phi::errors::Unimplemented(
              "Result type only support DenseTensorType and VectorType"));
        }
      }
    }

    // constuct input
    std::vector<ir::OpResult> vec_inputs;

    std::string kernel_fn_str;
    if (op_info_parser != nullptr) {
      kernel_fn_str = op_info_parser->OpRuntimeInfo().kernel_func[0];
    }

    if ((*it)->num_operands() > 0) {
      for (size_t i = 0; i < (*it)->num_operands(); ++i) {
        auto cur_in = (*it)->operand(i);
        auto new_in = map_value_pair.at(cur_in);

        auto new_in_type = new_in.type();

        auto& kernel = phi::KernelFactory::Instance().SelectKernelWithGPUDNN(
            kernel_fn_str, kernel_key);

        if (kernel.IsValid()) {
          if (new_in_type.isa<dialect::AllocatedDenseTensorType>()) {
            // allocated type
            auto place =
                new_in_type.dyn_cast<dialect::AllocatedDenseTensorType>()
                    .place();

            bool need_trans =
                (op_info_parser != nullptr &&
                 !op_info_parser->IsTensorAttribute(i)) &&
                (place != phi::TransToPhiPlace(kernel_key.backend()));
            if (need_trans) {
              if (paddle::experimental::NeedTransformPlace(
                      place, kernel.InputAt(i).backend, {})) {
                VLOG(6) << "need trans from " << place << " to "
                        << kernel_key.backend();
                // build memcopy op
                auto copy_kernel_key = kernel_key;
                copy_kernel_key.set_backend(phi::Backend::GPU);
                std::unordered_map<std::string, ir::Attribute> op_attribute{
                    {"op_name", ir::StrAttribute::get(ctx, "pd.memcpy_h2d")},
                    {"kernel_name", ir::StrAttribute::get(ctx, "memcpy_h2d")},
                    {"kernel_key",
                     dialect::KernelAttribute::get(ctx, copy_kernel_key)},
                    {"dst_place_type", ir::Int32Attribute::get(ctx, 1)}};

                ir::Operation* op = ir::Operation::Create(
                    {new_in}, op_attribute, {new_in_type}, op_info);

                program->block()->push_back(op);

                new_in = op->result(0);
              }
            }
          } else if (new_in_type.isa<ir::VectorType>()) {
            // [ todo need update here, support combine data transfomer]
          } else {
            PADDLE_THROW(phi::errors::Unimplemented(
                "only support allocated dense tensor type for now"));
          }
        }
        vec_inputs.push_back(new_in);
      }
    }

    std::unordered_map<std::string, ir::Attribute> op_attribute{
        {"op_name", ir::StrAttribute::get(ctx, (*it)->name())},
        {"kernel_name", ir::StrAttribute::get(ctx, kernel_fn_str)},
        {"kernel_key", dialect::KernelAttribute::get(ctx, kernel_key)}};

    auto op_attr_map = (*it)->attributes();

    for (auto it1 = op_attr_map.begin(); it1 != op_attr_map.end(); ++it1) {
      op_attribute.emplace(it1->first, it1->second);
    }

    ir::Operation* op = ir::Operation::Create(
        vec_inputs, op_attribute, op_output_types, op_info);

    map_op_pair[*it] = op;

    // only deal with single output
    if ((*it)->num_results() > 0) {
      for (size_t i = 0; i < (*it)->num_results(); ++i) {
        map_value_pair[(*it)->result(i)] = op->result(i);
      }
    }

    program->block()->push_back(op);
  }

  return program;
}

}  // namespace dialect
}  // namespace paddle

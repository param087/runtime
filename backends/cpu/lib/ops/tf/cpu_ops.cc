// Copyright 2020 The TensorFlow Runtime Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

//===- tf/cpu_ops.cc --------------------------------------------*- C++ -*-===//
//
// This file defines dispatch functions for CPU implementation of TF ops.
//
//===----------------------------------------------------------------------===//

#include "tfrt/cpu/ops/tf/cpu_ops.h"

#include "../../kernels/cpu_kernels.h"
#include "concat_op.h"
#include "constant_ops.h"
#include "cwise_binary_ops.h"
#include "cwise_unary_ops.h"
#include "matmul_fusion_ops.h"
#include "matmul_ops.h"
#include "shape_ops.h"
#include "softmax_ops.h"
#include "tfrt/common/compat/eigen/eigen_dtype.h"
#include "tfrt/common/ops/tf/metadata_functions.h"
#include "tfrt/core_runtime/op_attrs.h"
#include "tfrt/core_runtime/op_utils.h"
#include "tfrt/cpu/core_runtime/cpu_op_registry.h"
#include "tfrt/host_context/async_value_ref.h"
#include "tfrt/host_context/chain.h"
#include "tfrt/host_context/kernel_utils.h"
#include "tfrt/tensor/dense_host_tensor.h"
#include "tfrt/tensor/dense_host_tensor_view.h"
#include "tfrt/tensor/tensor_serialize_utils.h"
#include "tile_op.h"

namespace tfrt {
namespace {

//===----------------------------------------------------------------------===//
// tf.Const op
//===----------------------------------------------------------------------===//

static Expected<DenseHostTensor> TfConstOp(const OpAttrsRef& attrs,
                                           const TensorMetadata& dest_md,
                                           const ExecutionContext& exec_ctx) {
  auto dest_alloc =
      DenseHostTensor::CreateUninitialized(dest_md, exec_ctx.host());
  if (!dest_alloc) {
    return MakeStringError("out of memory allocating dht tensor");
  }

  auto& dest_tensor = dest_alloc.getValue();

  // Copy data from `value` attribute to dht.
  DenseAttr dense_attr = attrs.GetAsserting<DenseAttr>("value");
  std::memcpy(dest_tensor.data(), dense_attr.GetElements(),
              dest_md.GetHostSizeInBytes());

  return std::move(dest_tensor);
}

//===----------------------------------------------------------------------===//
// tf.Relu op
//===----------------------------------------------------------------------===//

static AsyncValueRef<DenseHostTensor> TfReluOp(
    const DenseHostTensor& A, const TensorMetadata& B_md,
    const ExecutionContext& exec_ctx) {
  HostContext* host = exec_ctx.host();

  auto dest = DenseHostTensor::CreateUninitialized(B_md, host);
  if (!dest) {
    return EmitErrorAsync(exec_ctx, "out of memory allocating result");
  }

  AsyncValueRef<Chain> chain;
  switch (A.dtype().kind()) {
    default:
      chain = EmitErrorAsync(exec_ctx, "unsupported dtype for relu");
      break;
#define DTYPE_NUMERIC(ENUM)                                \
  case DType::ENUM:                                        \
    chain = cpu::Relu<EigenTypeForDTypeKind<DType::ENUM>>( \
        A, dest.getPointer(), exec_ctx);                   \
    break;
#include "tfrt/dtype/dtype.def"  // NOLINT
  }

  return ForwardValue(dest.getValue(), std::move(chain), host);
}

//===----------------------------------------------------------------------===//
// tf.Mean op
//===----------------------------------------------------------------------===//

static Expected<TensorMetadata> TfMeanOutputMd(
    const DenseHostTensor& input, const DenseHostTensor& reduction_indices) {
  // Check if an input dimension is reduced or not.
  DHTArrayView<int32_t> reduction_indices_view(&reduction_indices);
  llvm::SmallVector<bool, 4> reduced_dim(input.shape().GetRank(), false);
  for (auto reduction_index : reduction_indices_view.Elements()) {
    if (reduction_index < 0 || reduction_index >= input.shape().GetRank()) {
      return MakeStringError(
          "tf.Mean reduction index must be in [0, input_rank) range");
    }
    if (reduced_dim[reduction_index]) {
      return MakeStringError("tf.Mean reduction indices must be unique");
    }

    reduced_dim[reduction_index] = true;
  }

  llvm::SmallVector<ssize_t, 4> output_dims;
  for (int i = 0; i < input.shape().GetRank(); ++i) {
    if (!reduced_dim[i])
      output_dims.push_back(input.shape().GetDimensionSize(i));
  }

  return TensorMetadata(input.dtype(), output_dims);
}

static AsyncValueRef<DenseHostTensor> TfMeanOp(
    const DenseHostTensor& input, const DenseHostTensor& reduction_indices,
    const ExecutionContext& exec_ctx) {
  HostContext* host = exec_ctx.host();

  // Compute output tensor metadata from reduction indices.
  auto output_md = TfMeanOutputMd(input, reduction_indices);
  if (auto err = output_md.takeError())
    return EmitErrorAsync(exec_ctx, std::move(err));

  auto output = DenseHostTensor::CreateUninitialized(*output_md, host);
  if (!output) {
    return EmitErrorAsync(exec_ctx, "out of memory allocating tensor");
  }
  DHTArrayView<int32_t> reduction_indices_view(&reduction_indices);

  AsyncValueRef<Chain> chain;
  switch (input.dtype().kind()) {
    default:
      chain = EmitErrorAsync(exec_ctx, "unsupported dtype for TfMeanOp");
      break;
#define DTYPE_FLOAT(ENUM)                                              \
  case DType::ENUM:                                                    \
    chain = cpu::Mean<EigenTypeForDTypeKind<DType::ENUM>>(             \
        input, reduction_indices_view.Elements(), output.getPointer(), \
        exec_ctx);                                                     \
    break;
#include "tfrt/dtype/dtype.def"  // NOLINT
  }

  return ForwardValue(output.getValue(), std::move(chain), host);
}

//===----------------------------------------------------------------------===//
// tf.BiadAdd op
//===----------------------------------------------------------------------===//
// TODO(b/161888722) Use Eigen broadcasting instead of dispatching by rank.
static AsyncValueRef<DenseHostTensor> TfBiasAddOp(
    const DenseHostTensor& input, const DenseHostTensor& bias,
    const TensorMetadata& output_md, const ExecutionContext& exec_ctx) {
  HostContext* host = exec_ctx.host();
  auto output = DenseHostTensor::CreateUninitialized(output_md, host);
  if (!output) {
    return EmitErrorAsync(exec_ctx, "out of memory allocating tensor");
  }

  AsyncValueRef<Chain> chain;
  size_t input_rank = input.shape().GetRank();
  switch (input.dtype().kind()) {
    default:
      chain = EmitErrorAsync(exec_ctx, "unsupported dtype for TfBiasAddOp");
      break;
#define DTYPE_NUMERIC(ENUM)                                          \
  case DType::ENUM:                                                  \
    switch (input_rank) {                                            \
      case 2:                                                        \
        chain = cpu::BiasAdd<EigenTypeForDTypeKind<DType::ENUM>, 2>( \
            input, bias, output.getPointer(), exec_ctx);             \
        break;                                                       \
      case 3:                                                        \
        chain = cpu::BiasAdd<EigenTypeForDTypeKind<DType::ENUM>, 3>( \
            input, bias, output.getPointer(), exec_ctx);             \
        break;                                                       \
      case 4:                                                        \
        chain = cpu::BiasAdd<EigenTypeForDTypeKind<DType::ENUM>, 4>( \
            input, bias, output.getPointer(), exec_ctx);             \
        break;                                                       \
      case 5:                                                        \
        chain = cpu::BiasAdd<EigenTypeForDTypeKind<DType::ENUM>, 5>( \
            input, bias, output.getPointer(), exec_ctx);             \
        break;                                                       \
    }                                                                \
    break;
#include "tfrt/dtype/dtype.def"  // NOLINT
  }

  return ForwardValue(output.getValue(), std::move(chain), host);
}

}  // namespace

void RegisterTfCpuOps(CpuOpRegistry* op_registry) {
  for (const std::pair<llvm::StringRef, OpMetadataFn>& md_function :
       GetAllTFMetadataFunctions()) {
    op_registry->AddMetadataFn(md_function.first, md_function.second);
  }
  op_registry->AddOp("tf.Const", TFRT_CPU_OP(TfConstOp),
                     CpuOpFlags::NoSideEffects, {"value"});
  op_registry->AddOp("tf.Relu", TFRT_CPU_OP(TfReluOp),
                     CpuOpFlags::NoSideEffects);
  op_registry->AddOp("tf.Mean", TFRT_CPU_OP(TfMeanOp),
                     CpuOpFlags::NoSideEffects);
  op_registry->AddOp("tf.BiasAdd", TFRT_CPU_OP(TfBiasAddOp),
                     CpuOpFlags::NoSideEffects);

  RegisterTfConcatCpuOp(op_registry);
  RegisterTfConstantCpuOps(op_registry);
  RegisterTfUnaryCpuOps(op_registry);
  RegisterTfBinaryCpuOps(op_registry);
  RegisterTfShapeCpuOps(op_registry);
  RegisterTfSofmaxCpuOps(op_registry);
  RegisterTfMatmulFusionCpuOps(op_registry);
  RegisterTfMatmulCpuOps(op_registry);
  RegisterTfTileCpuOp(op_registry);
}

}  // namespace tfrt

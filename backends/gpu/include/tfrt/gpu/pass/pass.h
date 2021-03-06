/*
 * Copyright 2020 The TensorFlow Runtime Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// MLIR pass definitions for gpu_ops library

#ifndef TFRT_GPU_PASS_PASS_H_
#define TFRT_GPU_PASS_PASS_H_

#include <memory>

#include "llvm/ADT/STLExtras.h"
#include "mlir/Transforms/DialectConversion.h"

namespace tfrt {
namespace gpu {

namespace internal {
mlir::Value GpuAsyncOpConversionGetStream(mlir::Operation* parent);
mlir::Value GpuAsyncOpConversionGetChain(mlir::Operation* parent);
void GpuAsyncOpConversionSetChain(mlir::Value chain,
                                  mlir::PatternRewriter& rewriter);
}  // namespace internal

// Base class for lowering ops inside a tfrt_gpu_conversion.async.execute op.
template <typename OpTy>
struct GpuAsyncOpConversionPattern : mlir::OpConversionPattern<OpTy> {
  using mlir::OpConversionPattern<OpTy>::OpConversionPattern;

 private:
  mlir::LogicalResult matchAndRewrite(
      OpTy op, mlir::ArrayRef<mlir::Value> operands,
      mlir::ConversionPatternRewriter& rewriter) const final {
    auto* parent = op->getParentOp();
    auto in_chain = internal::GpuAsyncOpConversionGetChain(parent);
    auto stream = internal::GpuAsyncOpConversionGetStream(parent);
    if (!in_chain || !stream)
      return rewriter.notifyMatchFailure(op, "Failed to get chain and stream.");
    auto out_chain =
        matchAndRewriteOp(op, in_chain, stream, operands, rewriter);
    if (failed(out_chain)) return mlir::failure();
    internal::GpuAsyncOpConversionSetChain(*out_chain, rewriter);
    return mlir::success();
  }

  // Lowers 'op' to schedule work on 'stream' and returns chain, or none in case
  // the rewrite failed.
  virtual mlir::FailureOr<mlir::Value> matchAndRewriteOp(
      OpTy op, mlir::Value in_chain, mlir::Value stream,
      mlir::ArrayRef<mlir::Value> operands,
      mlir::ConversionPatternRewriter& rewriter) const = 0;
};

// Adds rewrite patterns that wraps consecutive legal ops as defined by
// `target` into a tfrt_gpu_conversion.async.execute op.
void populateGpuAsyncConversionPatterns(mlir::RewritePatternSet& patterns,
                                        mlir::TypeConverter& converter,
                                        mlir::ConversionTarget& target);

// Adds rewrite patterns that unwrap tfrt_gpu_conversion.async.execute ops
// again and adds !tfrt.chain result and !tfrt.chain, !tfrt_gpu.stream arguments
// to functions.
void populateTfrtConversionPatterns(mlir::RewritePatternSet& patterns,
                                    mlir::ConversionTarget& target);

}  // namespace gpu
}  // namespace tfrt

#endif  // TFRT_GPU_PASS_PASS_H_

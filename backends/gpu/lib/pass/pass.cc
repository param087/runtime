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

#include "tfrt/gpu/pass/pass.h"

#include <memory>

#include "llvm/ADT/STLExtras.h"
#include "mlir/Conversion/AsyncToLLVM/AsyncToLLVM.h"
#include "mlir/Dialect/Async/IR/Async.h"
#include "mlir/Dialect/GPU/GPUDialect.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/StandardOps/IR/Ops.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/OperationSupport.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/IR/Types.h"
#include "mlir/IR/Value.h"
#include "mlir/Support/LLVM.h"
#include "mlir/Support/LogicalResult.h"
#include "mlir/Transforms/DialectConversion.h"
#include "tfrt/basic_kernels/opdefs/basic_kernels.h"
#include "tfrt/basic_kernels/opdefs/tfrt_base.h"
#include "tfrt/basic_kernels/opdefs/types.h"
#include "tfrt/gpu/kernels/gpu_ops.h"

namespace tfrt {
namespace gpu {

Value internal::GpuAsyncOpConversionGetStream(Operation *parent) {
  if (auto exec_op = dyn_cast_or_null<conversion::AsyncExecuteOp>(parent))
    return exec_op.body().getArgument(1);
  return Value();
}
Value internal::GpuAsyncOpConversionGetChain(Operation *parent) {
  if (auto exec_op = dyn_cast_or_null<conversion::AsyncExecuteOp>(parent))
    return exec_op.body().back().getTerminator()->getOperand(0);
  return Value();
}
void internal::GpuAsyncOpConversionSetChain(Value chain,
                                            PatternRewriter &rewriter) {
  Operation *terminator = chain.getParentRegion()->back().getTerminator();
  rewriter.updateRootInPlace(
      terminator, [&] { terminator->setOperands(ValueRange(chain)); });
}

namespace {
// Wraps consecutive legal ops within a block into a
// tfrt_gpu_conversion.async.execute op.
struct WrapInAsyncExecPattern : public OpRewritePattern<FuncOp> {
  WrapInAsyncExecPattern(MLIRContext *context, ConversionTarget &target)
      : OpRewritePattern(context), target(target) {}

 private:
  LogicalResult matchAndRewrite(FuncOp op,
                                PatternRewriter &rewriter) const override;
  LogicalResult matchAndRewriteBlock(Block *block,
                                     PatternRewriter &rewriter) const;
  ConversionTarget &target;
};

// Folds a memref.view of !tfrt_gpu.buffer with zero byte_shift.
struct FoldMemrefViewPattern : public OpConversionPattern<memref::ViewOp> {
  using OpConversionPattern::OpConversionPattern;

 private:
  LogicalResult matchAndRewrite(
      memref::ViewOp op, ArrayRef<Value> operands,
      ConversionPatternRewriter &rewriter) const override;
};

// Moves the body of a tfrt_gpu_conversion.async.execute op into the parent
// block and removes the op.
//
//     %t0 = tfrt_gpu.cast %ch0, %stream : !gpu.async.token
//     %t1 = tfrt_gpu_conversion.async.execute [%t0] {
//       ^bb(%0 : !tfrt.chain, %1 : !tfrt_gpu.stream)
//       ... ops using %0 and %1 ...
//       tfrt.return %n : !tfrt.chain
//     }
//
// will be rewritten to
//
//     %t0 = tfrt_gpu.cast %ch0, %stream : !gpu.async.token
//     ... ops using %ch0 and %stream ...
//     %t1 = tfrt_gpu.cast %n, %stream : !gpu.async.token
//
struct UnwrapAsyncExecPattern
    : public OpConversionPattern<conversion::AsyncExecuteOp> {
  using OpConversionPattern::OpConversionPattern;

 private:
  LogicalResult matchAndRewrite(
      conversion::AsyncExecuteOp op, ArrayRef<Value> operands,
      ConversionPatternRewriter &rewriter) const override;
};

// Rewrites a function to take extra !tfrt.chain and !tfrt_gpu.stream arguments
// and return a !tfrt.chain.
//
//     func @main(...) {
//       ...
//       return
//     }
//
// will be rewritten to
//
//     func @main(!tfrt.chain, !tfrt_gpu.stream, ...) -> !tfrt.chain {
//       ^bb0(%chain : !tfrt.chain, %stream : !tfrt_gpu.stream):
//         ...
//         tfrt.return %chain
//     }
//
struct SignatureRewritePattern : public OpRewritePattern<FuncOp> {
  using OpRewritePattern::OpRewritePattern;

 private:
  LogicalResult matchAndRewrite(FuncOp op,
                                PatternRewriter &rewriter) const override;
};

// A rewrite pattern to convert gpu.wait operations. If the op is in an
// async.execute region, it creates a new stream that is synchronized with the
// parent function's main stream (potentially recursively through
// synchronization with a stream from another dependent async.execute region).
// Otherwise it synchronizes event operands with the function's main stream.
struct WaitOpRewritePattern : public OpConversionPattern<mlir::gpu::WaitOp> {
  using OpConversionPattern::OpConversionPattern;

 private:
  LogicalResult matchAndRewrite(
      mlir::gpu::WaitOp op, ArrayRef<Value> operands,
      ConversionPatternRewriter &rewriter) const override;
};

// A rewrite pattern to convert async.yield operations. Replaces a token operand
// with a newly created event that is recorded on the async.execute's stream.
struct YieldOpRewritePattern : public OpConversionPattern<async::YieldOp> {
  YieldOpRewritePattern(MLIRContext *context,
                        std::unique_ptr<TypeConverter> converter)
      : OpConversionPattern(context), type_converter(std::move(converter)) {}

 private:
  LogicalResult matchAndRewrite(
      async::YieldOp op, ArrayRef<Value> operands,
      ConversionPatternRewriter &rewriter) const override;

  std::unique_ptr<TypeConverter> type_converter;
};

}  // namespace

LogicalResult WrapInAsyncExecPattern::matchAndRewrite(
    FuncOp op, PatternRewriter &rewriter) const {
  rewriter.startRootUpdate(op);
  LogicalResult result = failure();
  op.walk([&](Block *block) {
    if (dyn_cast<conversion::AsyncExecuteOp>(block->getParentOp()))
      return WalkResult::skip();
    if (succeeded(matchAndRewriteBlock(block, rewriter)))
      result = success();  //
    return WalkResult::advance();
  });
  succeeded(result) ? rewriter.finalizeRootUpdate(op)
                    : rewriter.cancelRootUpdate(op);
  return result;
}

// Iterate over ops in block, and whenever we transition from a legal to an
// illegal op, wrap preceding legal ops in !tfrt_gpu_conversion.async.execute.
LogicalResult WrapInAsyncExecPattern::matchAndRewriteBlock(
    Block *block, PatternRewriter &rewriter) const {
  LogicalResult result = failure();
  Operation *legal_begin = nullptr;
  for (Operation *op : llvm::make_pointer_range(block->getOperations())) {
    if (target.isLegal(op)) {
      if (!legal_begin)  // Start of legal op sequence.
        legal_begin = op;
      continue;
    }
    if (!legal_begin)  // Continue in illegal op sequence.
      continue;

    rewriter.setInsertionPoint(legal_begin);
    auto loc = legal_begin->getLoc();
    auto *body = rewriter.create<conversion::AsyncExecuteOp>(loc).getBody();
    // Move sequence of legal ops into !tfrt_gpu_conversion.async.execute body.
    body->getOperations().splice(body->begin(), op->getBlock()->getOperations(),
                                 legal_begin->getIterator(), op->getIterator());
    legal_begin = nullptr;  // Start of illegal op sequence.
    result = success();
  }
  return result;
}

LogicalResult FoldMemrefViewPattern::matchAndRewrite(
    memref::ViewOp op, ArrayRef<Value> operands,
    ConversionPatternRewriter &rewriter) const {
  memref::ViewOpAdaptor adaptor(operands);
  if (!adaptor.source().getType().isa<BufferType>())
    return rewriter.notifyMatchFailure(op, "expected gpu::BufferType source");
  auto byte_shift = adaptor.byte_shift().getDefiningOp<ConstantIndexOp>();
  if (!byte_shift || byte_shift.getValue() != 0)
    return rewriter.notifyMatchFailure(op, "expected const zero byte_shift");
  if (!adaptor.sizes().empty())
    return rewriter.notifyMatchFailure(op, "expected no sizes");
  rewriter.replaceOp(op, {adaptor.source()});
  return success();
}

// Return the defining op of 'value' if it casts a chain and a stream.
static conversion::CastOp GetDefiningCastOp(Value value) {
  auto cast_op = value.getDefiningOp<conversion::CastOp>();
  if (cast_op && cast_op->getNumOperands() == 2 && [](auto types) {
        return types[0].template isa<compiler::ChainType>() &&
               types[1].template isa<StreamType>();
      }(cast_op.getOperandTypes()))
    return cast_op;
  return nullptr;
}

LogicalResult UnwrapAsyncExecPattern::matchAndRewrite(
    conversion::AsyncExecuteOp op, ArrayRef<Value> operands,
    ConversionPatternRewriter &rewriter) const {
  if (operands.empty() || !op.getAsyncToken())
    return rewriter.notifyMatchFailure(op, "no operands or no result");
  auto cast_op = GetDefiningCastOp(operands.front());
  if (!cast_op)
    return rewriter.notifyMatchFailure(op, "expected cast to token");

  // Merge !tfrt_gpu_conversion.async.execute body into parent block.
  Operation *terminator = op.getBody()->getTerminator();
  rewriter.mergeBlockBefore(op.getBody(), op, cast_op->getOperands());
  rewriter.replaceOpWithNewOp<conversion::CastOp>(
      op, rewriter.getType<mlir::gpu::AsyncTokenType>(),
      ValueRange{terminator->getOperand(0), cast_op->getOperand(1)});
  rewriter.eraseOp(terminator);
  rewriter.eraseOp(cast_op);
  return success();
}

LogicalResult SignatureRewritePattern::matchAndRewrite(
    FuncOp op, PatternRewriter &rewriter) const {
  if (op.getNumResults() > 0)
    return rewriter.notifyMatchFailure(op, "Expected no result");

  // Add !tfrt.chain, !tfrt_gpu.stream arguments and !tfrt.chain result.
  Type chain_type = rewriter.getType<compiler::ChainType>();
  SmallVector<Type, 8> input_types;
  input_types.reserve(op.getNumArguments() + 2);
  input_types.push_back(chain_type);
  input_types.push_back(rewriter.getType<StreamType>());
  copy(op.getArgumentTypes(), std::back_inserter(input_types));
  rewriter.updateRootInPlace(op, [&] {
    op.setType(rewriter.getType<mlir::FunctionType>(input_types,
                                                    TypeRange(chain_type)));
  });

  // Add new function arguments to entry block. This is a bit of a dance
  // so that it could be rolled back in case of conversion failure.
  Block *block = &op.body().front();
  Block *entry = rewriter.createBlock(block, input_types);
  auto block_args = entry->getArguments();
  rewriter.mergeBlocks(block, entry, block_args.drop_front(2));

  // Return input chain.
  Operation *terminator = op.body().back().getTerminator();
  rewriter.replaceOpWithNewOp<compiler::ReturnOp>(terminator,
                                                  block_args.front());

  return success();
}

LogicalResult WaitOpRewritePattern::matchAndRewrite(
    mlir::gpu::WaitOp op, ArrayRef<Value> operands,
    ConversionPatternRewriter &rewriter) const {
  // Check that parent function has chain and stream arguments.
  FuncOp func_op = op->getParentOfType<FuncOp>();
  if (!func_op || func_op.getNumArguments() < 2 || ![](auto types) {
        return types[0].template isa<compiler::ChainType>() &&
               types[1].template isa<StreamType>();
      }(func_op.getArgumentTypes())) {
    return rewriter.notifyMatchFailure(
        op, "not in func with chain and stream argument");
  }

  // Check that parent function returns chain.
  Operation *terminator = func_op.getBody().back().getTerminator();
  if (!terminator || terminator->getNumOperands() < 1 ||
      !terminator->getOperand(0).getType().isa<compiler::ChainType>()) {
    return rewriter.notifyMatchFailure(terminator,
                                       "not in func returning chain");
  }

  // If the op has no cast to token operands, use the function's chain and
  // stream argument.
  Value chain = func_op.getArgument(0);
  Value stream = func_op.getArgument(1);

  // Check that operands are events or at most one token casted from chain and
  // stream.
  SmallVector<Value, 1> events;
  for (auto operand : operands) {
    if (operand.getType().isa<EventType>()) {
      events.push_back(operand);
      continue;
    }
    if (auto cast_op = GetDefiningCastOp(operand)) {
      chain = cast_op.getOperand(0);
      stream = cast_op.getOperand(1);
      rewriter.eraseOp(cast_op);
      continue;
    }
    return rewriter.notifyMatchFailure(op, "expected event or cast to token");
  }
  if (events.size() + 1 < op->getNumOperands())
    return rewriter.notifyMatchFailure(op, "more than one token operand");

  Location loc = op->getLoc();

  if (op.asyncToken() && op->getParentOfType<async::ExecuteOp>()) {
    // 'gpu.wait async' inside 'async.execute', create a new chain and stream.
    chain = rewriter.create<compiler::NewChainOp>(loc).getResult();
    Value context =
        rewriter.create<StreamGetContextOp>(loc, stream).getResult();
    // If there are no event operands from dependent async.execute ops,
    // synchronize new stream with function's stream argument.
    if (events.empty()) {
      Value event = rewriter.create<EventCreateOp>(loc, context).getResult();
      chain =
          rewriter.create<EventRecordOp>(loc, event, stream, chain).getResult();
      events.push_back(event);
    }
    stream = rewriter.create<StreamCreateOp>(loc, context).getResult();
  }

  // Synchronize the stream with the event operands.
  for (Value event : events) {
    chain =
        rewriter.create<StreamWaitOp>(loc, stream, event, chain).getResult();
  }

  if (op.asyncToken()) {
    // Replace 'gpu.wait async' with cast to token.
    rewriter.replaceOpWithNewOp<conversion::CastOp>(
        op, rewriter.getType<mlir::gpu::AsyncTokenType>(),
        ValueRange({chain, stream}));
  } else {
    // Update returned 'chain' and erase 'gpu.wait'.
    //
    // The 'gpu.wait' op inserted by 'gpu-async-region' is meant to synchronize
    // its operands with the host. In our case we only synchronize the operands
    // with the function's stream argument, which has happened above.
    // Host-synchronization of the function's stream argument is left up to the
    // caller. We just return the chain that depends on the synchronization with
    // the stream.
    rewriter.updateRootInPlace(terminator,
                               [&] { terminator->setOperand(0, chain); });
    rewriter.eraseOp(op);
  }

  return success();
}

LogicalResult YieldOpRewritePattern::matchAndRewrite(
    async::YieldOp op, ArrayRef<Value> operands,
    ConversionPatternRewriter &rewriter) const {
  if (!any_of(enumerate(operands), [&](auto operand) {
        auto cast_op = GetDefiningCastOp(operand.value());
        if (!cast_op) return false;

        Value chain = cast_op->getOperand(0);
        Value stream = cast_op->getOperand(1);
        Location loc = op->getLoc();

        Value context =
            rewriter.create<StreamGetContextOp>(loc, stream).getResult();
        Value event = rewriter.create<EventCreateOp>(loc, context).getResult();
        rewriter.create<EventRecordOp>(loc, event, stream, chain).getResult();

        rewriter.updateRootInPlace(
            op, [&] { op->setOperand(operand.index(), event); });
        rewriter.eraseOp(cast_op);

        return true;
      })) {
    return rewriter.notifyMatchFailure(op, "no cast to token operand");
  }
  return success();
}

void populateGpuAsyncConversionPatterns(RewritePatternSet &patterns,
                                        TypeConverter &converter,
                                        ConversionTarget &target) {
  patterns.add<WrapInAsyncExecPattern>(patterns.getContext(), target);
  patterns.add<FoldMemrefViewPattern>(converter, patterns.getContext());
}

void populateTfrtConversionPatterns(RewritePatternSet &patterns,
                                    ConversionTarget &target) {
  auto converter = std::make_unique<TypeConverter>();
  converter->addConversion([](Type type) { return type; });
  converter->addConversion([](mlir::gpu::AsyncTokenType type) {
    return EventType::get(type.getContext());
  });
  populateAsyncStructuralTypeConversionsAndLegality(*converter, patterns,
                                                    target);
  patterns.add<UnwrapAsyncExecPattern, SignatureRewritePattern,
               WaitOpRewritePattern>(patterns.getContext());
  patterns.add(std::make_unique<YieldOpRewritePattern>(patterns.getContext(),
                                                       std::move(converter)));

  // Casts are erased by the time conversion completes, but they need to be
  // legal in the interim.
  target.addLegalOp<conversion::CastOp>();

  // Signature needs to be `(!tfrt.chain, !tfrt.stream, ...) -> (!tfrt.chain)`.
  target.addDynamicallyLegalOp<FuncOp>([](FuncOp op) {
    auto type = op.getType();
    return type.getNumResults() == 1 &&
           type.getResult(0).isa<compiler::ChainType>() &&
           type.getNumInputs() >= 2 &&
           type.getInput(0).isa<compiler::ChainType>() &&
           type.getInput(1).isa<StreamType>();
  });
}

}  // namespace gpu
}  // namespace tfrt

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

// This file uses a static constructor to automatically register all of the
// kernels in this directory.  This can be used to simplify clients that don't
// care about selective registration of kernels.

#include "tfrt/common/ops/test/metadata_functions.h"
#include "tfrt/common/ops/tf/metadata_functions.h"
#include "tfrt/cpu/core_runtime/cpu_op_registry.h"
#include "tfrt/cpu/ops/test/cpu_ops_and_kernels.h"
#include "tfrt/host_context/kernel_registry.h"

namespace tfrt {

static void RegisterKernels(KernelRegistry* registry) {
  RegisterBTFIOKernels(registry);
  RegisterCooKernels(registry);
  RegisterMNISTTensorKernels(registry);
  RegisterResNetTensorKernels(registry);
}

static void RegisterMetadataFn(CpuOpRegistry* registry) {
  for (const std::pair<llvm::StringRef, OpMetadataFn>& md_function :
       GetAllTestMetadataFunctions()) {
    registry->AddMetadataFn(md_function.first, md_function.second);
  }
  for (const std::pair<llvm::StringRef, OpMetadataFn>& md_function :
       GetAllTFMetadataFunctions()) {
    registry->AddMetadataFn(md_function.first, md_function.second);
  }
}

static void RegisterDispatchFn(CpuOpRegistry* registry) {
  RegisterCooCpuOps(registry);
  RegisterTestMnistCpuOps(registry);
  RegisterTestCpuOps(registry);
}

TFRT_STATIC_KERNEL_REGISTRATION(RegisterKernels);
TFRT_STATIC_CPU_OP_REGISTRATION(RegisterMetadataFn);
TFRT_STATIC_CPU_OP_REGISTRATION(RegisterDispatchFn);

}  // namespace tfrt

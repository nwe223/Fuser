// clang-format off
/*
 * SPDX-FileCopyrightText: Copyright (c) 2023-present NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
// clang-format on
#pragma once

#include <ir_interface_nodes.h>

namespace nvfuser::optimization {

enum class TORCH_CUDA_CU_API OptimizationPassCategory { PreSegmenter, Null };
using FusionPass = std::function<void(Fusion*)>;

class OptimizationPass {
 public:
  virtual FusionPass func() = 0;
  virtual std::string name() = 0;
};

// higher priority pass runs earlier
// newly registered pass runs at the end of all passes with identical priority
TORCH_CUDA_CU_API void registerOptimizationPass(const OptimizationPassCategory& category, OptimizationPass* pass, int priority = 0);
TORCH_CUDA_CU_API void applyOptimizationPass(const OptimizationPassCategory& category, Fusion* fusion);

} // namespace nvfuser::optimization

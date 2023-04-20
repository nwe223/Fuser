// clang-format off
/*
 * SPDX-FileCopyrightText: Copyright (c) 2023-present NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
// clang-format on
#include <scheduler/reduction.h>

#include <executor_utils.h>
#include <grouped_reduction.h>
#include <instrumentation.h>
#include <ir_all_nodes.h>
#include <ir_iostream.h>
#include <ir_utils.h>
#include <scheduler/normalization_utils.h>
#include <scheduler/reduction_utils.h>
#include <scheduler/registry.h>
#include <scheduler/utils.h>
#include <scheduler/vectorize_helper.h>
#include <transform_replay.h>

#include <ATen/cuda/CUDAContext.h>

#include <cmath>

namespace nvfuser {

namespace {

// round up to multiple of 8 or pow2 whichever smaller
int64_t roundUpPow2Or8(const int64_t x) {
  auto round_up_pow2 = scheduler_utils::lastPow2(x);
  if (round_up_pow2 < x) {
    round_up_pow2 *= 2;
  }
  constexpr int64_t kEight = 8; // clang tidy
  auto round_up_8 = x % kEight == 0 ? x : x + (kEight - x % kEight);
  return std::min(round_up_8, round_up_pow2);
}

// Copied from reduction scheduler, should generalize. Simply needed to take out
// grid reductions.
std::shared_ptr<ReductionParams> innerPersistentHeuristic(
    const int64_t total_reduction_numel,
    const int64_t total_iteration_numel,
    const int64_t inner_most_dimension_numel,
    const int64_t n_tensor_inputs,
    const int64_t max_input_dtype_size,
    const int64_t max_persistent_buffer_size,
    const size_t vectorize_factor) {
  // Set some targets for parallelization
  const int64_t n_elems = total_reduction_numel * total_iteration_numel;

  const int64_t outer_reduction_numel =
      total_reduction_numel / inner_most_dimension_numel;

  const auto dev_prop = at::cuda::getCurrentDeviceProperties();
  // WARNING: At some point we may want to generate heuristics for another
  // device that is not the current device.
  const int64_t device_max_threads_per_multiprocessor =
      (int64_t)dev_prop->maxThreadsPerMultiProcessor;

  const int64_t device_multiprocessor_count =
      (int64_t)dev_prop->multiProcessorCount;

  auto const max_unroll = ceilDiv(
      // Available unrolling based on size of data type
      (int64_t)16 / (int64_t)max_input_dtype_size,
      // Reduce unrolling if we have many inputs, start reduction at 4 inputs
      scheduler_utils::lastPow2(
          std::max((int64_t)n_tensor_inputs >> 2, (int64_t)1)));

  // Conservative value, could be set to larger based on arch if necessary.
  constexpr int64_t l1_cache = 32 * 1024;
  // Could change per generation, but for l1 we want to consider active threads,
  // not resident
  constexpr int64_t active_threads = 1024;

  // if data fits in l2 and we need more parallelization in the reduction dim,
  // we can use a smaller warp size. While thread local data fits in l1, and
  // reduction dim is really small, we can use <32 threads per warp.
  const bool fits_in_l2 =
      n_elems * max_input_dtype_size * n_tensor_inputs < dev_prop->l2CacheSize;

  // If it fits in l2, we just want to make sure each warp uses 32Bytes. Set
  // minimum warp as 16 threads instead of 32 as if we have a small reduction
  // dim going a bit smaller than 32 usually helps.
  const int64_t warp_size_based_on_l2 =
      fits_in_l2 ? (int64_t)32 / max_input_dtype_size : 16;

  // Check how many elements it would take per thread to start thrashing l1
  // set that to minimum number we want to reduce per thread.
  const int64_t warp_size_based_on_l1 = std::min(
      ceilDiv(
          total_reduction_numel,
          scheduler_utils::safeDiv(
              l1_cache,
              n_tensor_inputs * max_input_dtype_size * active_threads)),
      (int64_t)16);

  // Take the smaller, warp_size may be a odd number, e.g. 15
  // Tracked at https://github.com/NVIDIA/Fuser/issues/107
  const int64_t warp_size =
      std::min(warp_size_based_on_l1, warp_size_based_on_l2);

  // Initialization
  int64_t target_blocks = 1;
  int64_t target_unroll = 1;
  int64_t target_iterations = 1;

  // Try to set a minmum amount of work for each thread, as cross thread
  // communication is slow so it shouldn't be done for every element in the
  // reduction.
  int64_t min_target_iterations =
      scheduler_utils::safeDiv(32, max_input_dtype_size);

  // Start trying to break parallelization up across threads,
  // unrolling/iterations, and blocks.

  // max_threads_in_block is the cap on a thread block, the minimum is based on
  // warp_size
  int64_t max_threads_in_block = std::max(
      warp_size, ceilDiv(total_reduction_numel, min_target_iterations));

  // If we have one warp per block, check if that's enough to saturate the SMs
  target_blocks = ceilDiv(n_elems, warp_size);

  // If we have more than a wave of blocks, put parallelism into unrolling and
  // target iterations
  if (target_blocks > device_multiprocessor_count) {
    auto available_unroll = scheduler_utils::safeDiv(
        n_elems, warp_size * device_multiprocessor_count);

    // Spread across unrolling and iterations, want a balance of the two so flip
    // back and forth to alternate adding to them.
    bool flip = true;

    while (available_unroll > 1 &&
           (target_unroll < max_unroll ||
            // Prefer unrolling
            target_iterations < max_unroll)) {
      if (target_unroll * 2 <= max_unroll && flip) {
        target_unroll *= 2;
      }

      if (target_iterations * 2 <= max_unroll && !flip) {
        target_iterations *= 2;
      }

      available_unroll = scheduler_utils::safeDiv(
          n_elems,
          warp_size * device_multiprocessor_count * target_unroll *
              target_iterations);
      flip = !flip;
    }

    // Recompute target blocks
    target_blocks =
        ceilDiv(n_elems, warp_size * target_unroll * target_iterations);
  }

  // Cap target blocks to 4 waves
  target_blocks = std::min(target_blocks, device_multiprocessor_count * 4);

  if (target_blocks * target_unroll * target_iterations < n_elems) {
    if (outer_reduction_numel == 1) {
      // set to hardware limit to use small persistent buffer for large
      // reductions
      max_threads_in_block = std::min(
          ceilDiv(n_elems, target_blocks * target_unroll),
          (int64_t)dev_prop->maxThreadsPerBlock);
    } else {
      // targetting 4 waves, so try to use a quarter of available threads
      max_threads_in_block = std::min(
          ceilDiv(n_elems, target_blocks * target_unroll),
          ceilDiv(device_max_threads_per_multiprocessor, (int64_t)4));
    }
  }

  // Round up to nearest warp.
  if (max_threads_in_block % warp_size != 0) {
    max_threads_in_block += warp_size - max_threads_in_block % warp_size;
    max_threads_in_block =
        std::min(max_threads_in_block, (int64_t)dev_prop->maxThreadsPerBlock);
  }
  // Compute maximum number of reductions we could do in the same kernel based
  // on persistent buffer size
  const int64_t max_multi_reduction_factor = scheduler_utils::safeDiv(
      scheduler_utils::register_file_size, max_persistent_buffer_size);

  // To get to target threads:
  // Prioritize
  // (1) x dim in reduction
  // (2) unrolling in reduction
  // (3) y in output
  // To get target blocks:
  // Prioritize
  // (1) x dim in multiple outputs
  // (2) y dim in multiple reductions

  // Blocks for outputs
  int64_t godim = 1;

  // Threads for reduction
  int64_t bdimx = 1;
  // Threads for outputs
  int64_t bdimy = 1;
  // Threads for outer reduction dimension
  int64_t bdimz = 1;

  // Unroll amount
  int64_t inner_reduction_unroll_factor = 1;
  int64_t outer_reduction_unroll_factor = 1;
  int64_t iter_unroll_factor = 1;

  inner_reduction_unroll_factor =
      vectorize_factor > 1 ? (int64_t)vectorize_factor : 1;

  // Grab what we can out of reduction domain, but don't go over a warp size yet
  bdimx = std::min(
      std::max(
          ceilDiv(inner_most_dimension_numel, inner_reduction_unroll_factor),
          (int64_t)warp_size),
      max_threads_in_block);

  // If we're not just barely covering the dimension, round to a more friendly
  // number
  if (bdimx * inner_reduction_unroll_factor != inner_most_dimension_numel) {
    bdimx = bdimx > warp_size ? bdimx - bdimx % warp_size
                              : scheduler_utils::lastPow2(bdimx);

    // Round bdimx down to multiple of warp size or power 2
    if (bdimx < warp_size) {
      bdimx = scheduler_utils::lastPow2(bdimx);
    } else {
      bdimx = bdimx - bdimx % warp_size;
    }
  }

  // Put everything else in bdimy for now
  bdimy = std::min(
      scheduler_utils::safeDiv(warp_size, bdimx), max_multi_reduction_factor);
  // If 3D fill the rest of the threads into bdimz
  bdimz = std::min(
      std::min(
          scheduler_utils::safeDiv(max_threads_in_block, bdimx * bdimy),
          outer_reduction_numel),
      scheduler_utils::z_block_limit);

  bool vectorize = false;

  // Move unrolling factor into vectorization upto vectorization limit.
  if (vectorize_factor > 1 && inner_reduction_unroll_factor > 1) {
    vectorize = true;
    inner_reduction_unroll_factor = std::min(
        scheduler_utils::lastPow2(inner_reduction_unroll_factor),
        (int64_t)vectorize_factor);
  }

  // start from small block size to minimize expensive inter-threads reduction
  const int threads_after_vectorize =
      inner_most_dimension_numel / inner_reduction_unroll_factor;
  constexpr int scheduler_per_sm = 4;
  if (outer_reduction_numel == 1 && vectorize) {
    bdimx = std::min(
        scheduler_per_sm * dev_prop->warpSize, threads_after_vectorize);
  }

  // If we don't have a full warp, let's do multiple reductions per block.
  // Still keep vectorization as it is important for performance since V100.
  // Limit block size to 4 warps to avoid occupancy and SM wave tail issues.
  if (bdimx * bdimy * bdimz < warp_size) {
    bdimy = std::min(
        scheduler_utils::safeDiv(
            scheduler_per_sm * dev_prop->warpSize, bdimx * bdimz),
        max_multi_reduction_factor);
  }

  // Set size of persistent per thread buffer on inner reduction buffer
  // if too large, will be reduced later to reduce register usage
  int64_t batches_per_block_inner_reduction = ceilDiv(
      inner_most_dimension_numel, bdimx * inner_reduction_unroll_factor);

  // Attempt to put some unrolling into the outer reduction if inner hasn't
  // taken the max unrolling
  if (inner_reduction_unroll_factor < max_unroll) {
    outer_reduction_unroll_factor = std::min(
        ceilDiv(max_unroll, inner_reduction_unroll_factor),
        ceilDiv(outer_reduction_numel, bdimz));
  }

  godim = ceilDiv(total_iteration_numel, bdimy);

  // Prefer putting iterations into unrolling over having a very large
  // persistent buffer.
  while (!vectorize && inner_reduction_unroll_factor < max_unroll &&
         batches_per_block_inner_reduction >= 2) {
    inner_reduction_unroll_factor *= 2;
    batches_per_block_inner_reduction = roundUpPow2Or8(ceilDiv(
        inner_most_dimension_numel, bdimx * inner_reduction_unroll_factor));
  }

  // Set size of persistent per thread buffer on outer reduction buffer
  int64_t batches_per_block_outer_reduction = roundUpPow2Or8(ceilDiv(
      ceilDiv(total_reduction_numel, inner_most_dimension_numel),
      bdimz * outer_reduction_unroll_factor));

  // Prefer putting iterations into unrolling over having a very large
  // persistent buffer.
  while (outer_reduction_unroll_factor < max_unroll &&
         batches_per_block_outer_reduction >= 2) {
    outer_reduction_unroll_factor *= 2;
    batches_per_block_outer_reduction = roundUpPow2Or8(
        ceilDiv(outer_reduction_numel, bdimz * outer_reduction_unroll_factor));
  }

  // If we haven't gotten to the max_unroll case, try to take it out of the
  // iteration domain
  if (inner_reduction_unroll_factor * outer_reduction_unroll_factor <
          max_unroll &&
      scheduler_utils::safeDiv(max_multi_reduction_factor, bdimy) > 2) {
    // Don't go over a combined inner/outer unroll of max_unroll
    auto unroll_available = std::min(
        scheduler_utils::safeDiv(
            max_unroll,
            inner_reduction_unroll_factor * outer_reduction_unroll_factor),
        scheduler_utils::safeDiv(max_multi_reduction_factor, bdimy));
    if (unroll_available > 1 && godim > 2 * device_multiprocessor_count) {
      unroll_available = std::min(
          unroll_available, ceilDiv(godim, 2 * device_multiprocessor_count));
      iter_unroll_factor = unroll_available;
    }
  }

  // Adjust bdimx based on batches_per_block and unroll factor set as they could
  // have moved a bit since they're the free variables, not the buffers
  bdimx = ceilDiv(
      inner_most_dimension_numel,
      inner_reduction_unroll_factor * batches_per_block_inner_reduction);
  bdimz = ceilDiv(
      outer_reduction_numel,
      outer_reduction_unroll_factor * batches_per_block_outer_reduction);

  // Try moving persistent buffer factors into threads until we have too many
  // threads.
  constexpr int batches_per_block_inner_reduction_max = 10;
  while (
      // If block size can be doubled
      bdimx * bdimy * bdimz * 2 <= max_threads_in_block &&
      // And batches_per_block_inner_reduction can be divided by two
      (batches_per_block_inner_reduction >
           batches_per_block_inner_reduction_max ||
       batches_per_block_outer_reduction >= 2)) {
    // Try to decrease per thread register allocation persistence size on inner
    // reduction by reducing buffer size by half. In most cases,
    // inner_most_dimension_numel is evenly divisible by
    // batches_per_block_inner_reduction, thus bdimx will be doubled in each
    // iteration. In nondivisible boundary cases, the difference between reduce
    // by half and directly set to batches_per_block_inner_reduction_max is less
    // than five percent.
    if (batches_per_block_inner_reduction >
        batches_per_block_inner_reduction_max) {
      batches_per_block_inner_reduction /= 2;
      bdimx = ceilDiv(
          inner_most_dimension_numel,
          inner_reduction_unroll_factor * batches_per_block_inner_reduction);
      continue;
    }

    // Try to decrease per thread register allocation persistence size on outer
    // reduction
    if (batches_per_block_outer_reduction >= 2 &&
        batches_per_block_outer_reduction !=
            roundUpPow2Or8(batches_per_block_outer_reduction / 2) &&
        bdimz * 2 <= scheduler_utils::z_block_limit) {
      batches_per_block_outer_reduction =
          roundUpPow2Or8(batches_per_block_outer_reduction / 2);
      bdimz = ceilDiv(
          outer_reduction_numel,
          batches_per_block_outer_reduction * outer_reduction_unroll_factor);
      continue;
    }
    break;
  }

  // Register pressure is really high per thread, which could lead to local
  // memory leaks, if using less than maximum threads, decrease batches per
  // block by a factor of 2
  if (batches_per_block_outer_reduction * batches_per_block_inner_reduction *
              inner_reduction_unroll_factor * outer_reduction_unroll_factor *
              4 >
          255 * 3 &&
      bdimx * bdimy * bdimz * 2 <= max_threads_in_block &&
      batches_per_block_inner_reduction >
          batches_per_block_inner_reduction_max) {
    batches_per_block_inner_reduction = batches_per_block_inner_reduction / 2;
  }

  // Do the same on the outer reduction dimension
  if (batches_per_block_outer_reduction * batches_per_block_inner_reduction *
              inner_reduction_unroll_factor * outer_reduction_unroll_factor *
              4 >
          255 * 3 &&
      bdimx * bdimy * bdimz * 2 <= device_max_threads_per_multiprocessor &&
      batches_per_block_outer_reduction >= 2) {
    batches_per_block_outer_reduction /= 2;
  }

  auto device_warp_size = at::cuda::warp_size();
  auto padded_bdimx = bdimx % device_warp_size == 0
      ? bdimx
      : bdimx + (device_warp_size - bdimx % device_warp_size);

  bool pad_bdimx = bdimx > 16 &&
      padded_bdimx * bdimy * bdimz < (int64_t)dev_prop->maxThreadsPerBlock;

  // estimate register usage and occupancy raito.
  // If occupancy raito is less than a preset occupancy_ratio, reduce register
  // usage register per thread is estimated as overhead + buffer_size /
  // bytes_per_register
  int64_t nvrtc_register_per_thread = 255;
  const int blocksPerKernel = godim;
  // register estimation is only valid for vectorized gmem access
  // we've seen unexpectedly high register counts with vectorization factor less
  // than 4, which would make the below estimate inaccurate.
  // TODO: support the non vectorized case. consider shmem.
  // only need to balance register and occupancy ratio if there are enough
  // blocks and buffers
  if (vectorize && blocksPerKernel > device_multiprocessor_count &&
      batches_per_block_inner_reduction > 1) {
    constexpr int reg_allocation_granularity = 256;
    constexpr float occupancy_ratio = 0.4;
    const int persistent_buffer_size = batches_per_block_inner_reduction *
        inner_reduction_unroll_factor * max_input_dtype_size;
    // persistent_buffer_size = 4*2, 8*2, 32*2, 64*2, 128*2
    // register_used_on_a100  = 27,  40,  62,   73,   105
    // register_used_on_v100  = xx,  xx,  45,   62,   93
    // estimated_register_num = 42,  44,  56,   72,   104
    // safe for both v100 & a100
    constexpr int bytes_per_register = 4;
    constexpr int overhead_register = 40;
    const int estimated_register_count =
        persistent_buffer_size / bytes_per_register + overhead_register;
    // avoid nvcc using too many registers than expected
    nvrtc_register_per_thread = estimated_register_count;

    const int register_per_warp =
        ceilDiv(
            estimated_register_count * device_warp_size,
            reg_allocation_granularity) *
        reg_allocation_granularity;
    const int threadsPerBlock =
        (pad_bdimx ? padded_bdimx : bdimx) * bdimy * bdimz;
    const int warps_per_block = ceilDiv(threadsPerBlock, dev_prop->warpSize);
    const int estimated_warps_per_sm = dev_prop->regsPerMultiprocessor /
        (register_per_warp * warps_per_block) * warps_per_block;
    const int occupancy_warps_per_sm = static_cast<int>(
        dev_prop->maxThreadsPerMultiProcessor / device_warp_size *
        occupancy_ratio);

    if (estimated_warps_per_sm < occupancy_warps_per_sm) {
      const int blocks_per_sm_1 = dev_prop->maxBlocksPerMultiProcessor;
      const int blocks_per_sm_2 =
          ceilDiv(occupancy_warps_per_sm, warps_per_block);
      const int blocks_per_sm = std::min(blocks_per_sm_1, blocks_per_sm_2);
      const int warps_per_sm = blocks_per_sm * warps_per_block;
      const int register_per_warp = dev_prop->regsPerMultiprocessor /
          warps_per_sm / reg_allocation_granularity *
          reg_allocation_granularity;
      const int occupancy_register_count = register_per_warp / device_warp_size;
      // use occupancy_register_count directly may cause register spills
      // only allow 20% drop from estimated_register_count to balance register
      // usage and occupancy
      constexpr float max_adjust_fraction = 0.8;
      nvrtc_register_per_thread = std::max(
          static_cast<int>(estimated_register_count * max_adjust_fraction),
          occupancy_register_count);
    }
  }

  // Will be used once supporting inter-block persistence
  int64_t gdimx = LaunchParams::UNINITIALIZED_VAL;
  int64_t gdimy = LaunchParams::UNINITIALIZED_VAL;
  int64_t gdimz = LaunchParams::UNINITIALIZED_VAL;

  auto rparams = std::make_shared<ReductionParams>();

  rparams->cparams.maxrregcount = nvrtc_register_per_thread;
  rparams->persistent_kernel = true;
  rparams->fastest_dim = true;

  // Inner reduction domain
  rparams->cross_block_inner_reduction = true;
  rparams->block_dim_inner_reduction = ParallelType::TIDx;
  rparams->pad_inner_reduction_to_warp = pad_bdimx;
  rparams->batches_per_block_inner_reduction =
      batches_per_block_inner_reduction;

  // For persistent schedules always have to mark the reduction unrolled
  // otherwise rfactor can fail
  rparams->unroll_factor_inner_reduction = inner_reduction_unroll_factor;
  rparams->vectorize_inner_reduction = vectorize;

  // Iter domain
  rparams->multiple_reds_per_blk = bdimy > 1;
  if (rparams->multiple_reds_per_blk) {
    rparams->block_dim_iter_dom = ParallelType::TIDy;
  }

  if (godim > 1) {
    rparams->grid_dim_iter_dom = ParallelType::BIDx;
    if (godim > scheduler_utils::x_grid_limit) {
      rparams->split_grid_dim_iter_dom_outer = true;
      gdimx = scheduler_utils::x_grid_limit;
    }
  }

  if (iter_unroll_factor > 1) {
    rparams->unroll_factor_iter_dom = iter_unroll_factor;
  }

  // Outer reduction domain
  rparams->schedule_3D = total_reduction_numel != inner_most_dimension_numel;
  if (rparams->schedule_3D) {
    rparams->batches_per_block_outer_reduction =
        batches_per_block_outer_reduction;
    rparams->block_dim_outer_reduction = ParallelType::TIDz;
    rparams->cross_block_outer_reduction = true;
    rparams->unroll_factor_outer_reduction = outer_reduction_unroll_factor;
  }

  rparams->lparams = LaunchParams(
      gdimx,
      gdimy,
      gdimz,
      LaunchParams::UNINITIALIZED_VAL,
      bdimy,
      LaunchParams::UNINITIALIZED_VAL);

  rparams->tag = "Inner Persistent Heuristic.\n";

  if (isDebugDumpEnabled(DebugDumpOption::SchedulerDebug)) {
    std::cerr << "\n===== Reduction Stats ========\n"
              << "total_reduction_numel: " << total_reduction_numel << "\n"
              << "total_iteration_numel: " << total_iteration_numel << "\n"
              << "inner_most_dimension_numel: " << inner_most_dimension_numel
              << "\n"
              << "vectorize_factor: " << vectorize_factor << "\n"
              << "n_tensor_inputs: " << n_tensor_inputs << "\n"
              << "max_input_dtype_size: " << max_input_dtype_size << "\n"
              << "max_persistent_buffer_size: " << max_persistent_buffer_size
              << "\n"
              << "max_multi_reduction_factor: " << max_multi_reduction_factor
              << "\n"
              << "block(" << (pad_bdimx ? padded_bdimx : bdimx) << ", " << bdimy
              << ", " << bdimz << ")";
    std::cerr << rparams->toString() << std::endl;
  }

  return rparams;
}

// Heuristics for grid outer normalizations
std::shared_ptr<ReductionParams> gridOuterPersistentHeuristic(
    const int64_t total_reduction_numel,
    const int64_t total_iteration_numel,
    const int64_t n_tensor_inputs,
    const int64_t max_input_dtype_size,
    const int64_t max_persistent_buffer_size,
    const size_t vectorize_factor) {
  auto outer_params =
      normalization_scheduler_utils::getGridOuterNormalizationParams(
          total_reduction_numel,
          total_iteration_numel,
          vectorize_factor,
          max_persistent_buffer_size);

  TORCH_INTERNAL_ASSERT(outer_params.has_value(), "No valid config found");

  const auto pb_size = outer_params->persistent_buffer_factor;
  const auto unswitch_factor = outer_params->unswitch_factor;

  auto rparams = std::make_shared<ReductionParams>();

  rparams->persistent_kernel = true;
  rparams->cross_block_inner_reduction = true;
  rparams->cross_grid_inner_reduction = true;
  rparams->grid_dim_iter_dom = ParallelType::BIDx;
  rparams->grid_dim_inner_reduction = ParallelType::BIDy;
  rparams->block_dim_inner_reduction = ParallelType::TIDy;
  rparams->batches_per_block_inner_reduction = pb_size;
  rparams->multiple_reds_per_blk = true;
  rparams->vectorize_iter_dom = true;
  rparams->unroll_factor_iter_dom = vectorize_factor;
  rparams->block_dim_iter_dom = ParallelType::TIDx;
  rparams->unroll_factor_inner_reduction = unswitch_factor;
  rparams->split_grid_dim_iter_dom_inner =
      ceilDiv(
          total_iteration_numel / vectorize_factor,
          outer_params->launch_params.bdimx()) >
      outer_params->launch_params.gdimx();
  rparams->compute_persistent_buffer_with_first_consumer = true;
  rparams->static_bdimx = true;
  rparams->static_bdimy = true;

  rparams->lparams = LaunchParams(
      rparams->split_grid_dim_iter_dom_inner
          ? outer_params->launch_params.gdimx()
          : LaunchParams::UNINITIALIZED_VAL,
      LaunchParams::UNINITIALIZED_VAL,
      LaunchParams::UNINITIALIZED_VAL,
      outer_params->launch_params.bdimx(),
      outer_params->launch_params.bdimy(),
      LaunchParams::UNINITIALIZED_VAL);

  if (isDebugDumpEnabled(DebugDumpOption::SchedulerDebug)) {
    std::cerr << "\n===== Reduction Stats ========\n"
              << "total_reduction_numel: " << total_reduction_numel << "\n"
              << "total_iteration_numel: " << total_iteration_numel << "\n"
              << "vectorize_factor: " << vectorize_factor << "\n"
              << "n_tensor_inputs: " << n_tensor_inputs << "\n"
              << "max_input_dtype_size: " << max_input_dtype_size << "\n"
              << "max_persistent_buffer_size: " << max_persistent_buffer_size
              << "\n"
              << "persistent_buffer_factor: " << pb_size << "\n"
              << "block(" << outer_params->launch_params.bdimx() << ", "
              << outer_params->launch_params.bdimy() << ", 1)" << std::endl;
    std::cerr << rparams->toString() << std::endl;
  }

  return rparams;
}

// Copied from reduction scheduler, should generalize. Simply needed to take out
// grid reductions.
// TODO: Check adding iteration domain unrolling
std::shared_ptr<ReductionParams> outerPersistentHeuristic(
    const int64_t total_reduction_numel,
    const int64_t total_iteration_numel,
    const int64_t n_tensor_inputs,
    const int64_t max_input_dtype_size,
    const int64_t max_persistent_buffer_size,
    const size_t vectorize_factor) {
  // Set some targets for parallelization
  const int64_t n_elems = total_reduction_numel * total_iteration_numel;

  // WARNING: Current device for codegen may not be the target device
  const int64_t device_max_threads_per_multiprocessor =
      (int64_t)at::cuda::getCurrentDeviceProperties()
          ->maxThreadsPerMultiProcessor;

  const int64_t device_multiprocessor_count =
      (int64_t)at::cuda::getCurrentDeviceProperties()->multiProcessorCount;

  // If it fits in l2, we just want to make sure each warp uses 32Bytes. Set
  // minimum warp as 16 threads instead of 32 as if we have a small reduction
  // dim going a bit smaller than 32 usually helps.
  const int64_t warp_size = n_elems * max_input_dtype_size * n_tensor_inputs <
          at::cuda::getCurrentDeviceProperties()->l2CacheSize
      ? (int64_t)32 / max_input_dtype_size
      : 16;

  const auto register_file_size =
      at::cuda::getCurrentDeviceProperties()->regsPerBlock * sizeof(int);

  // Each block runs N reductions, where N is defined as:
  // vectorize_factor * blockDim.x. The minimum number of SMs to run
  // this as a persistent kernel is thus defined as:
  const int64_t min_required_sm_per_norm = ceilDiv(
      max_persistent_buffer_size * vectorize_factor *
          normalization_scheduler_utils::PreferredLaunchConfig::kMinBdimx,
      register_file_size);

  if (min_required_sm_per_norm > 1) {
    return gridOuterPersistentHeuristic(
        total_reduction_numel,
        total_iteration_numel,
        n_tensor_inputs,
        max_input_dtype_size,
        max_persistent_buffer_size,
        vectorize_factor);
  }

  int64_t target_blocks = 1;
  int64_t target_unroll = 1;
  int64_t max_threads_in_block = warp_size;

  // If we have one warp per block, check if that's enough to saturate the
  // SMs. Blocks can't come out of reduction dimension, so only use
  // iteration dimension here.
  target_blocks = ceilDiv(total_iteration_numel, (int64_t)warp_size);

  const auto max_unroll = ceilDiv(
      // Available unrolling based on size of data type
      (int64_t)16 / (int64_t)max_input_dtype_size,
      // Reduce unrolling if we have many inputs, start reduction at 4
      // inputs
      scheduler_utils::lastPow2(
          scheduler_utils::safeDiv((int64_t)n_tensor_inputs, 4)));

  // If we have more than a wave of blocks, put parallelism into unrolling
  if (target_blocks > device_multiprocessor_count) {
    target_unroll = std::min(
        max_unroll, ceilDiv(target_blocks, device_multiprocessor_count));
    target_blocks = ceilDiv(target_blocks, target_unroll);
  }

  // Cap target blocks to 4 waves
  target_blocks = std::min(target_blocks, device_multiprocessor_count * 4);

  if (target_blocks * target_unroll * max_threads_in_block < n_elems) {
    // targetting 4 waves, so try to use a quarter of available threads
    max_threads_in_block = std::min(
        ceilDiv(n_elems, target_blocks * target_unroll),
        ceilDiv(device_max_threads_per_multiprocessor, (int64_t)4));
  }

  // Round up to nearest warp.
  if (max_threads_in_block % warp_size != 0) {
    max_threads_in_block += warp_size - max_threads_in_block % warp_size;
  }

  // Compute maximum number of reductions we could do in the same kernel based
  // on persistent buffer size
  const int64_t max_multi_reduction_factor = scheduler_utils::safeDiv(
      scheduler_utils::register_file_size, max_persistent_buffer_size);

  // To get to target threads:
  // Prioritize
  // (1) x dim in iter domain
  // (2) unrolling in iter domain
  // (3) y in reduction domain
  // To get target blocks:
  // Prioritize
  // (1) x dim in multiple outputs
  // (2) y dim in multiple reductions - need to flip unrolling to reduction
  // domain for this

  // Threads for reduction
  int64_t bdimy = 1;
  // Threads for output
  int64_t bdimx = 1;

  // Unroll amount
  int64_t inner_reduction_unroll_factor = 1;
  int64_t iter_unroll_factor = 1;

  // If we only use a warp, can we get iter domain unrolling?
  bdimx = std::min(max_multi_reduction_factor, warp_size);
  // Round down if it didn't hit a full warp
  if (bdimx < warp_size) {
    bdimx = scheduler_utils::lastPow2(bdimx);
  }

  // Prioritize unrolling on iteration domain, but don't sacrifice occupancy,
  // make sure there is at least one wave.
  if (ceilDiv(total_iteration_numel, bdimx) > 2 * device_multiprocessor_count) {
    iter_unroll_factor = std::min(
        std::min(
            scheduler_utils::safeDiv(max_multi_reduction_factor, bdimx),
            max_unroll),
        ceilDiv(device_multiprocessor_count, bdimx));
  }

  // With current setup, is there's at least 2 waves and iter domain space left
  if (max_multi_reduction_factor > bdimx * iter_unroll_factor &&
      ceilDiv(total_iteration_numel, bdimx * iter_unroll_factor) >
          2 * device_multiprocessor_count) {
    // Put more into bdimx
    bdimx = std::min(
        std::min(
            scheduler_utils::safeDiv(
                // Don't exceed multi reduction factor
                max_multi_reduction_factor,
                iter_unroll_factor),
            // Leave a full wave of blocks
            ceilDiv(
                total_iteration_numel,
                iter_unroll_factor * device_multiprocessor_count)),
        // Don't exceed max thread count
        max_threads_in_block);

    // Round bdimx down to multiple of warp size or power 2
    if (bdimx < warp_size) {
      bdimx = scheduler_utils::lastPow2(bdimx);
    } else {
      bdimx = bdimx - bdimx % warp_size;
    }
  }

  // Fill bdimy with left over threads
  bdimy = std::min(
      scheduler_utils::safeDiv(max_threads_in_block, bdimx),
      total_reduction_numel);

  bool vectorize = false;

  // Move unrolling factor into vectorization upto vectorization limit.
  if (vectorize_factor > 1 && iter_unroll_factor > 1) {
    vectorize = true;
    iter_unroll_factor = std::min(
        scheduler_utils::lastPow2(iter_unroll_factor),
        (int64_t)vectorize_factor);
  }

  int64_t sm_required_per_norm_set = ceilDiv(
      max_persistent_buffer_size * bdimx * iter_unroll_factor,
      scheduler_utils::register_file_size);

  TORCH_INTERNAL_ASSERT(
      sm_required_per_norm_set == 1,
      "Tried to use multiple SMs on an outer persistent kernel ",
      "yet this kernel should have been within block persistent.");

  // Since this is persistent and registers will have to be used anyways unroll
  // the reduction dim if it's available
  inner_reduction_unroll_factor =
      std::min(max_unroll, ceilDiv(total_reduction_numel, bdimy));

  // Persistence size from buffers
  int64_t batches_per_block =
      ceilDiv(total_reduction_numel, bdimy * inner_reduction_unroll_factor);

  batches_per_block = roundUpPow2Or8(batches_per_block);

  // Adjust bdimy based on batches_per_block and unroll factor set
  bdimy = ceilDiv(
      total_reduction_numel, inner_reduction_unroll_factor * batches_per_block);

  // Try moving persistent buffers into threads if using less than a quarter of
  // available threads
  while (
      // If using less than a quarter of available threads
      bdimx * bdimy * 2 <=
          ceilDiv(device_max_threads_per_multiprocessor, (int64_t)4) &&
      // And batches_per_block can be divided by two
      batches_per_block >= 2 &&
      // Make sure batches_per_block will be updated
      batches_per_block != roundUpPow2Or8(batches_per_block / 2)) {
    batches_per_block = roundUpPow2Or8(batches_per_block / 2);

    // Adjust bdimy based on batches_per_block and unroll factor set
    bdimy = ceilDiv(
        total_reduction_numel,
        inner_reduction_unroll_factor * batches_per_block);
  }

  // Register pressure is really high per thread and using less than
  // maximum threads, decrease batches per block by a factor of 2
  if ((batches_per_block * inner_reduction_unroll_factor * 4 > 255 * 3 &&
       bdimx * bdimy * 2 <= device_max_threads_per_multiprocessor)) {
    batches_per_block /= 2;
  }

  // If we're close to the limit on the register file size, drop down block dim
  // x so we don't throw an error when we try to launch the kernel.
  while (bdimy * bdimx * inner_reduction_unroll_factor * batches_per_block *
             max_input_dtype_size * 4 >
         scheduler_utils::register_file_size * 3) {
    if (bdimx == 1) {
      TORCH_INTERNAL_ASSERT(false, "Error generating persistent kernel.");
    }
    bdimx = ceilDiv(bdimx, 2);
  }

  int gdimx = ceilDiv(total_iteration_numel, bdimx);

  auto rparams = std::make_shared<ReductionParams>();
  rparams->batches_per_block_inner_reduction = batches_per_block;
  rparams->persistent_kernel = true;

  rparams->fastest_dim = false;
  rparams->cross_block_inner_reduction = true;
  rparams->cross_grid_inner_reduction = false;
  rparams->multiple_reds_per_blk = bdimx > 1;

  if (rparams->multiple_reds_per_blk) {
    rparams->block_dim_iter_dom = ParallelType::TIDx;
  }

  rparams->grid_dim_iter_dom = ParallelType::BIDx;
  rparams->split_grid_dim_iter_dom_outer =
      gdimx > scheduler_utils::x_grid_limit;

  if (rparams->block_dim_iter_dom == ParallelType::TIDx) {
    rparams->block_dim_inner_reduction = ParallelType::TIDy;
  } else {
    rparams->block_dim_inner_reduction = ParallelType::TIDx;
  }

  // Always need to mark inner reduction unroll for rfactor in outer persitent
  // kernels
  rparams->unroll_factor_inner_reduction = inner_reduction_unroll_factor;

  rparams->unroll_factor_iter_dom = iter_unroll_factor;

  if (iter_unroll_factor > 1) {
    rparams->vectorize_iter_dom = vectorize;
  }

  rparams->lparams = LaunchParams(
      LaunchParams::UNINITIALIZED_VAL,
      LaunchParams::UNINITIALIZED_VAL,
      LaunchParams::UNINITIALIZED_VAL,
      rparams->multiple_reds_per_blk ? bdimx : bdimy,
      LaunchParams::UNINITIALIZED_VAL,
      LaunchParams::UNINITIALIZED_VAL);

  rparams->tag = "Outer persistent kernel heuristic.\n";

  if (isDebugDumpEnabled(DebugDumpOption::SchedulerDebug)) {
    std::cerr << "\n===== Reduction Stats ========\n"
              << "total_reduction_numel: " << total_reduction_numel << "\n"
              << "total_iteration_numel: " << total_iteration_numel << "\n"
              << "vectorize_factor: " << vectorize_factor << "\n"
              << "n_tensor_inputs: " << n_tensor_inputs << "\n"
              << "max_input_dtype_size: " << max_input_dtype_size << "\n"
              << "max_persistent_buffer_size: " << max_persistent_buffer_size
              << "\n"
              << "max_multi_reduction_factor: " << max_multi_reduction_factor
              << "\n"
              << "block(" << bdimx << ", " << bdimy << ", 1)" << std::endl;
    std::cerr << rparams->toString() << std::endl;
  }

  return rparams;
}

} // namespace

std::shared_ptr<ReductionParams> persistentHeuristic(
    const int64_t total_reduction_numel,
    const int64_t total_iteration_numel,
    const int64_t inner_most_dimension_numel,
    const bool fastest_dim_reduction,
    const size_t n_tensor_inputs,
    const size_t max_input_dtype_size,
    const int64_t max_persistent_buffer_size,
    size_t vectorize_factor,
    bool project_persistent_buffers) {
  std::shared_ptr<ReductionParams> rparams;
  if (fastest_dim_reduction) {
    rparams = innerPersistentHeuristic(
        total_reduction_numel,
        total_iteration_numel,
        inner_most_dimension_numel,
        n_tensor_inputs,
        max_input_dtype_size,
        max_persistent_buffer_size,
        vectorize_factor);
  } else {
    rparams = outerPersistentHeuristic(
        total_reduction_numel,
        total_iteration_numel,
        n_tensor_inputs,
        max_input_dtype_size,
        max_persistent_buffer_size,
        vectorize_factor);
  }
  rparams->project_persistent_buffers = project_persistent_buffers;
  return rparams;
}

std::shared_ptr<ReductionParams> getPersistentHeuristics(
    Fusion* fusion,
    SchedulerRuntimeInfo& runtime_info,
    HeuristicSummary* data_cache) {
  FUSER_PERF_SCOPE("getPersistentHeuristics");

  FusionGuard fg(fusion);

  auto reduction_tv_entry =
      HeuristicSummaryEntry<HeuristicCompileTime::ReductionTVs>(
          data_cache, [&fusion]() {
            return std::make_unique<std::vector<TensorView*>>(
                scheduler_utils::getReductionTvs(fusion));
          });

  auto& reduction_tvs = reduction_tv_entry.get();

  TORCH_INTERNAL_ASSERT(
      !reduction_tvs.empty(), "Need reduction tensor views to schedule.");

  auto first_red_tv = reduction_tvs[0];

  TORCH_INTERNAL_ASSERT(
      first_red_tv != nullptr, "Reduction TensorView wasn't found.");

  TORCH_INTERNAL_ASSERT(
      first_red_tv->hasReduction(), "TensorView doesn't have a reduction.");
  const auto red_expr = first_red_tv->definition();

  TORCH_INTERNAL_ASSERT(
      ir_utils::isReductionOp(red_expr),
      "TensorView doesn't have a reduction.");

  auto tv_inps = ir_utils::filterByType<TensorView>(fusion->inputs());
  TORCH_INTERNAL_ASSERT(
      std::distance(tv_inps.begin(), tv_inps.end()) > 0,
      "Tried to schedule a fusion with no tensor inputs, currently not supported.");

  auto persistent_buffer_info_entry =
      HeuristicSummaryEntry<HeuristicCompileTime::PersistentBufferInfo>(
          data_cache, [&fusion]() {
            return std::make_unique<scheduler_utils::PersistentBufferInfo>(
                scheduler_utils::persistentBuffers(fusion));
          });

  auto& persistent_buffer_info = persistent_buffer_info_entry.get();
  TORCH_INTERNAL_ASSERT(
      !persistent_buffer_info.persistent_buffers.empty(),
      "Persistent scheduler requires persistent buffers.");

  auto properties =
      scheduler_utils::getProperties(fusion, runtime_info, first_red_tv);

  // Grab persistent buffer sizes
  auto persistent_buffer_size_info = scheduler_utils::persistentBufferSize(
      fusion, runtime_info, persistent_buffer_info, data_cache);
  // If projected persistent buffers are smaller, they will be used.
  // TODO: Fix projected persistent buffers with view
  // https://github.com/csarofeen/pytorch/issues/2054
  auto max_persistent_size = ir_utils::getViewOps(fusion).size() > 0
      ? persistent_buffer_size_info.persistent_buffer_size
      : std::min(
            persistent_buffer_size_info.persistent_buffer_size,
            persistent_buffer_size_info.projected_persistent_buffer_size);

  // Figure out if we want to projet persistent buffers to the inputs for
  // exmaple if we have an input tensor t0 that's fp16:
  //
  // t0 = makeSymbolicTensor(2, DataType::Half)
  // t1 = castOp(DataType::Float, t0)
  // t2 = sum(t1, 1)
  // t3 = broadcast(t2, {false, true})
  // t4 = set(t1)
  // t5 = add(t4, t3)
  // t6 = castOp(DataType::Half, t5)
  //
  // The persistent buffer is detected as being t1, which would save the
  // persistent buffer as a float, however we could obviously just save t0 which
  // is half and would take half the memory. A more complex scenario of this
  // which requires more advanced analysis is batch norm backwards.
  bool project_persistent_buffers =
      persistent_buffer_size_info.projected_persistent_buffer_size <
      persistent_buffer_size_info.persistent_buffer_size;

  auto unrollable_inputs_outputs_entry =
      HeuristicSummaryEntry<HeuristicCompileTime::UnrollableInputsAndOutputs>(
          data_cache, [&first_red_tv]() {
            return std::make_unique<std::vector<TensorView*>>(
                scheduler_utils::getInputsOutputsWithInnerDim(
                    first_red_tv, false, false));
          });

  auto& unrollable_inputs_outputs = unrollable_inputs_outputs_entry.get();

  const auto vectorize_factor = vectorize_helper::getVectorizationFactor(
      runtime_info,
      first_red_tv,
      data_cache,
      (int)(first_red_tv->nDims() - properties.inner_most_dimension_ndims));

  // Base max dtype and n_tensor_inputs on tensors that are vectorizable (i.e.
  // share inner dimension with data pattern we're looking at).
  size_t max_dtype_size = 1;

  // TODO: This might be better if it was the larger of input or outputs. Would
  // be even better if we had better analysis as not all unrolled elements have
  // to be alive at the same time.
  size_t n_tensor_inputs = 0;
  for (auto tv : unrollable_inputs_outputs) {
    if (!tv->isFusionInput()) {
      continue;
    }

    max_dtype_size = std::max(
        max_dtype_size,
        dataTypeSize(tv->getDataType().value(), runtime_info.getIndexType()));
    n_tensor_inputs++;
  }

  // Protect heuristics div by 0:
  n_tensor_inputs = std::max(n_tensor_inputs, (size_t)1);

  auto heuristic = persistentHeuristic(
      properties.total_reduction_numel,
      properties.total_iteration_numel,
      properties.inner_most_dimension_numel,
      properties.fastest_dim_reduction,
      n_tensor_inputs,
      max_dtype_size,
      max_persistent_size,
      vectorize_factor,
      project_persistent_buffers);
  heuristic->cparams.index_type = runtime_info.getIndexType();
  return heuristic;
}

std::shared_ptr<ReductionParams> getPersistentHeuristics(
    Fusion* fusion,
    const at::ArrayRef<c10::IValue>& runtime_inputs,
    HeuristicSummary* data_cache) {
  FUSER_PERF_SCOPE("getPersistentHeuristicsFromIValue");
  SchedulerRuntimeInfo runtime_info(fusion, runtime_inputs);
  return getPersistentHeuristics(fusion, runtime_info, data_cache);
}

// fusion is the input IR that will be modified by this function
void schedulePersistentKernel(Fusion* fusion, const ReductionParams& rparams) {
  FUSER_PERF_SCOPE("schedulePersistentKernel");

  FusionGuard fg(fusion);

  // Project the persistent buffers to the inputs. Inputs will be cached in a
  // later step, this will move them to be in a register buffer as expected.
  // dummy outputs are helper tensors to make sure persistent buffer projection
  // does not create trouble for transform propagation.
  // TODO: Fix projected persistent buffers with view
  // https://github.com/csarofeen/pytorch/issues/2054
  std::vector<TensorView*> dummy_outputs;
  if (rparams.project_persistent_buffers &&
      ir_utils::getViewOps(fusion).empty()) {
    dummy_outputs = reduction_scheduler_utils::projectPersistentBuffers(fusion);
  }

  // Cache tensors before grabbing any references to reductions as cache_before
  // can invalidate the references since when applied to a reduction tensor view
  // the new tensor view contains the reduction and original doesn't.

  bool unroll = rparams.isUnrolled();

  // Cache inputs even if not unrolled, as otherwise we may not create a
  // persistent buffer if that persistent buffer would be the input.
  auto cached_inputs = scheduler_utils::cacheInputs(fusion, true);

  // Cache and fork outputs
  auto cached_outputs = scheduler_utils::cacheAndForkOutputs(fusion, unroll);

  // Make sure we don't have global memory set on intermediate tensors from
  // fusion segmentation
  scheduler_utils::clearMemorySpace(fusion);

  scheduler_utils::prepareForMemoryTypePromotion(fusion);

  auto reduction_tvs = scheduler_utils::getReductionTvs(fusion);

  TORCH_INTERNAL_ASSERT(reduction_tvs.size());
  // Registry assumes the reference tv is the first reduction_tv, if this
  // changes registry needs to change.
  auto reduction_tv = reduction_tvs[0];

  if (ir_utils::getViewOps(fusion).size() > 0) {
    ComputeAtMap ca_map(fusion);
    // Propagate view transforms through the graph, expecially the reference.
    scheduler_utils::propagateViewTransforms(fusion, ca_map);

    // Reorder reference_tv after propagating the view operation. This will
    // reorder for better merging.
    reduction_tv->reorder(
        scheduler_utils::domainReorderAsRfactorMap(reduction_tv));
  }

  if (rparams.persistent_kernel && rparams.cross_grid_inner_reduction &&
      !rparams.fastest_dim && reduction_tvs.size() > 1) {
    groupReductions(reduction_tvs, false);
  }

  auto dim_analysis = scheduler_utils::canonicalDimReduction(
      fusion, reduction_tv, rparams.fastest_dim && rparams.schedule_3D);
  bool has_iter_axis = dim_analysis.first;
  bool has_red_axis = dim_analysis.second;

  TORCH_INTERNAL_ASSERT(
      has_red_axis,
      "Could not find reduction axis in tensor used for reduction scheduler.");

  if (!has_iter_axis) {
    TORCH_INTERNAL_ASSERT(
        rparams.fastest_dim,
        "If all dims are reduction, should be sending it to fastest dim scheduler.");
  }

  TensorView* reference_tv = reduction_scheduler_utils::scheduleReductionTV(
      rparams, reduction_tv, has_iter_axis);

  // Reduction tensor views and rfactor tensor views are setup. Let's finish off
  // the scheduling, particularly inlining and unrolling.
  TORCH_INTERNAL_ASSERT(
      reference_tv != nullptr && reduction_tv != nullptr,
      "Need these two tensor views to finish the scheduling.");

  for (auto output : dummy_outputs) {
    fusion->addOutput(output);
  }

  reduction_scheduler_utils::multiReductionInliner(
      fusion,
      rparams,
      reduction_tv,
      reference_tv,
      reduction_tvs,
      cached_inputs,
      cached_outputs,
      dummy_outputs);

  if (rparams.compute_persistent_buffer_with_first_consumer) {
    TORCH_INTERNAL_ASSERT(
        rparams.persistent_kernel,
        "computeWith should be only used with persistent kernels");
    for (const auto persistent_buffer : cached_inputs) {
      persistent_buffer->computeWith(-1, true);
    }
  }

  scheduler_utils::promoteProducerMemoryTypesOfResizedTensors(
      fusion, cached_inputs);
}

} // namespace nvfuser

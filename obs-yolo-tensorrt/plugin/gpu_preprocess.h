/**
 * gpu_preprocess.h
 *
 * GPU-accelerated preprocessing:
 *   BGRA/RGBA uint8 (OBS texture) → float32 CHW (model input)
 *
 * Operations performed entirely on GPU:
 *   1. Letterbox resize (preserves aspect ratio, pads with grey)
 *   2. BGR → RGB channel reorder
 *   3. Normalise to [0, 1]  (divide by 255)
 *   4. HWC → CHW planar layout
 *
 * All ops are fused into a single CUDA kernel to minimise DRAM traffic.
 * Zero host-device copies are required when the OBS texture is already
 * in a CUDA-registered surface / interop handle.
 */

#pragma once

#include <cuda_runtime.h>
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Launch the letterbox + normalise kernel.
 *
 * @param src_device   RGBA/BGRA uint8 device pointer (OBS frame pixels)
 * @param src_w        source width  (pixels)
 * @param src_h        source height (pixels)
 * @param src_stride   source row stride in bytes (may have padding)
 * @param dst_device   float32 CHW device pointer (model input)
 * @param dst_w        model input width
 * @param dst_h        model input height
 * @param pad_val      letterbox pad value [0,1]  (default 0.5 = grey)
 * @param bgr_input    true if src is BGRA (swap R/B), false for RGBA
 * @param stream       CUDA stream
 */
void gpu_preprocess_letterbox(
    const uint8_t* src_device,
    int src_w, int src_h, int src_stride,
    float* dst_device,
    int dst_w, int dst_h,
    float pad_val,
    bool  bgr_input,
    cudaStream_t stream);

/**
 * Copy an OBS gs_texture into a CUDA device buffer.
 * Uses CUDA/OpenGL (or CUDA/D3D11) interop to avoid CPU round-trips.
 *
 * Returns true on success.
 *
 * @param obs_texture   opaque OBS gs_texture_t* (cast from void*)
 * @param dst_device    destination device uint8 RGBA buffer
 * @param dst_bytes     size of dst_device in bytes
 * @param stream        CUDA stream
 */
bool gpu_texture_to_cuda(
    void*        obs_texture,
    uint8_t*     dst_device,
    size_t       dst_bytes,
    cudaStream_t stream);

#ifdef __cplusplus
}
#endif

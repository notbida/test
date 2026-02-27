/**
 * gpu_preprocess.cu
 *
 * CUDA kernels for letterbox-resize + normalise.
 * Also implements OBS texture → CUDA interop via:
 *   - Linux:   CUDA/OpenGL interop (cudaGraphicsGLRegisterImage)
 *   - Windows: CUDA/D3D11 interop  (cuD3D11GetDevice / cudaGraphicsD3D11RegisterResource)
 */

#include "gpu_preprocess.h"
#include <cuda_runtime.h>

// ── Letterbox + CHW normalise kernel ─────────────────────────────────────────
//
// Grid:  (dst_w / TILE, dst_h / TILE, 1)
// Block: (TILE, TILE, 1)
//
// Each thread writes one output pixel (r,g,b floats into three planes).

#define TILE 16

__global__ void kernel_letterbox_norm(
    const uint8_t* __restrict__ src,
    int src_w, int src_h, int src_stride,
    float* __restrict__ dst,
    int dst_w, int dst_h,
    float pad_val,
    int   bgr_input)
{
    const int dx = blockIdx.x * blockDim.x + threadIdx.x;
    const int dy = blockIdx.y * blockDim.y + threadIdx.y;
    if (dx >= dst_w || dy >= dst_h) return;

    // Compute letterbox scale and offsets
    float scale = fminf((float)dst_w / src_w, (float)dst_h / src_h);
    int   new_w = (int)(src_w * scale + 0.5f);
    int   new_h = (int)(src_h * scale + 0.5f);
    int   pad_x = (dst_w - new_w) / 2;
    int   pad_y = (dst_h - new_h) / 2;

    float r, g, b;

    if (dx < pad_x || dx >= pad_x + new_w ||
        dy < pad_y || dy >= pad_y + new_h) {
        // Padding area
        r = g = b = pad_val;
    } else {
        // Map destination pixel → source pixel (bilinear)
        float sx = (float)(dx - pad_x) / scale;
        float sy = (float)(dy - pad_y) / scale;

        int x0 = (int)sx, y0 = (int)sy;
        int x1 = min(x0 + 1, src_w - 1);
        int y1 = min(y0 + 1, src_h - 1);
        float wx = sx - x0, wy = sy - y0;

        auto px = [&](int x, int y) __device__ -> const uint8_t* {
            return src + y * src_stride + x * 4;
        };

        const uint8_t* p00 = px(x0, y0);
        const uint8_t* p10 = px(x1, y0);
        const uint8_t* p01 = px(x0, y1);
        const uint8_t* p11 = px(x1, y1);

        // BGRA input (OBS default)
        int bi = bgr_input ? 0 : 2;
        int ri = bgr_input ? 2 : 0;
        int gi = 1;

#define BLERP(ch) ( \
    (1-wx)*(1-wy)*p00[ch] + wx*(1-wy)*p10[ch] + \
    (1-wx)*wy   *p01[ch] + wx*wy    *p11[ch])

        b = BLERP(bi) / 255.f;
        g = BLERP(gi) / 255.f;
        r = BLERP(ri) / 255.f;
#undef BLERP
    }

    // Write CHW planar layout  (R plane, G plane, B plane)
    int plane = dst_w * dst_h;
    dst[0 * plane + dy * dst_w + dx] = r;
    dst[1 * plane + dy * dst_w + dx] = g;
    dst[2 * plane + dy * dst_w + dx] = b;
}

// ── Public C wrapper ──────────────────────────────────────────────────────────

void gpu_preprocess_letterbox(
    const uint8_t* src_device,
    int src_w, int src_h, int src_stride,
    float* dst_device,
    int dst_w, int dst_h,
    float pad_val,
    bool  bgr_input,
    cudaStream_t stream)
{
    dim3 block(TILE, TILE);
    dim3 grid((dst_w + TILE - 1) / TILE,
              (dst_h + TILE - 1) / TILE);

    kernel_letterbox_norm<<<grid, block, 0, stream>>>(
        src_device, src_w, src_h, src_stride,
        dst_device, dst_w, dst_h,
        pad_val, (int)bgr_input);
}

// ── OBS texture → CUDA interop ────────────────────────────────────────────────
//
// Platform detection:
//   GS_GL_SHARED_TEXTURE  → Linux/Mac OpenGL path
//   GS_D3D11_SHARED_HANDLE → Windows D3D11 path
//
// We use the OBS graphics subsystem to obtain a native texture handle,
// then register it with CUDA for zero-copy access.
//
// NOTE: This requires the OBS plugin to be loaded in the same process,
//       and the CUDA device to match the GPU running OBS.

#include <obs/graphics/graphics.h>

#ifdef _WIN32
#  include <cuda_d3d11_interop.h>
#  include <d3d11.h>
#else
#  include <cuda_gl_interop.h>
#endif

bool gpu_texture_to_cuda(
    void*        obs_texture,
    uint8_t*     dst_device,
    size_t       dst_bytes,
    cudaStream_t stream)
{
    gs_texture_t* tex = static_cast<gs_texture_t*>(obs_texture);
    if (!tex) return false;

#ifdef _WIN32
    // ── Windows: D3D11 interop ────────────────────────────────────────────
    ID3D11Texture2D* d3d_tex =
        static_cast<ID3D11Texture2D*>(gs_texture_get_obj(tex));
    if (!d3d_tex) return false;

    cudaGraphicsResource_t res = nullptr;
    cudaError_t err = cudaGraphicsD3D11RegisterResource(
        &res, d3d_tex,
        cudaGraphicsRegisterFlagsReadOnly);
    if (err != cudaSuccess) return false;

    cudaGraphicsMapResources(1, &res, stream);

    cudaArray_t arr = nullptr;
    cudaGraphicsSubResourceGetMappedArray(&arr, res, 0, 0);

    // Get dimensions
    D3D11_TEXTURE2D_DESC desc{};
    d3d_tex->GetDesc(&desc);

    cudaMemcpy2DFromArrayAsync(
        dst_device, desc.Width * 4,
        arr, 0, 0,
        desc.Width * 4, desc.Height,
        cudaMemcpyDeviceToDevice, stream);

    cudaGraphicsUnmapResources(1, &res, stream);
    cudaGraphicsUnregisterResource(res);

#else
    // ── Linux: OpenGL interop ─────────────────────────────────────────────
    GLuint gl_tex = *(GLuint*)gs_texture_get_obj(tex);

    cudaGraphicsResource_t res = nullptr;
    cudaError_t err = cudaGraphicsGLRegisterImage(
        &res, gl_tex, GL_TEXTURE_2D,
        cudaGraphicsRegisterFlagsReadOnly);
    if (err != cudaSuccess) return false;

    cudaGraphicsMapResources(1, &res, stream);

    cudaArray_t arr = nullptr;
    cudaGraphicsSubResourceGetMappedArray(&arr, res, 0, 0);

    cudaExtent ext{};
    cudaArrayGetInfo(nullptr, &ext, nullptr, arr);
    size_t w = ext.width, h = ext.height;

    cudaMemcpy2DFromArrayAsync(
        dst_device, w * 4,
        arr, 0, 0,
        w * 4, h,
        cudaMemcpyDeviceToDevice, stream);

    cudaGraphicsUnmapResources(1, &res, stream);
    cudaGraphicsUnregisterResource(res);
#endif

    return true;
}

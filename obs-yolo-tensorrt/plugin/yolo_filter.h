/**
 * yolo_filter.h  –  OBS video filter plugin
 *
 * Registers a "YOLO TensorRT Detection" filter that can be applied to any
 * OBS video source.
 *
 * Pipeline (per-frame, fully async):
 *
 *   OBS render thread
 *     │  video_render() called
 *     │  1. Grab texture from OBS (zero-copy if GPU interop available)
 *     │  2. try_acquire inference token (atomic CAS)
 *     │     ├── acquired: submit GPU work to async_stream_
 *     │     └── busy:     frames_dropped++, pass-through
 *     └──────────────────────────────────────────────────
 *
 *   CUDA async stream completion callback (on GPU driver thread)
 *     │  3. Device→host DMA of output tensor
 *     │  4. Post callback to inference thread
 *     └──────────────────────────────────────────────────
 *
 *   Inference completion thread (dedicated, high priority)
 *     │  5. CPU post-process (decode + NMS)
 *     │  6. ShmWriter::write()
 *     │  7. Release inference token (atomic store)
 *     └──────────────────────────────────────────────────
 *
 * The token is a single std::atomic<bool> that implements the "at-most-one
 * inference in flight" policy.  This keeps latency deterministic.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <obs/obs-module.h>

/** Called from obs_module_load() */
void register_yolo_filter(void);

#ifdef __cplusplus
}
#endif

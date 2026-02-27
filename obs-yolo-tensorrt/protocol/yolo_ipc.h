/**
 * yolo_ipc.h  –  Shared-memory IPC protocol between the OBS plugin and
 *                CVM-colorBot.
 *
 * Layout (single memory-mapped region, YOLO_SHM_SIZE bytes):
 *
 *   [0]       YoloShmHeader   (cache-line aligned)
 *   [256]     YoloDetection   detection[YOLO_MAX_DETECTIONS]
 *
 * Synchronisation – lock-free, single-writer / single-reader:
 *
 *   The writer (OBS plugin) uses a "sequence lock" idiom:
 *     1. Increment write_seq  (odd  → write in progress)
 *     2. Write header fields + detection array
 *     3. Increment write_seq  (even → data valid)
 *
 *   The reader (colorBot) spins on write_seq until it is even, then
 *   copies the data, then verifies write_seq has not changed.
 *
 * This guarantees the reader always sees a consistent snapshot of the
 * LATEST frame without any mutex or condition variable.
 *
 * Memory ordering:
 *   All atomic ops use std::memory_order appropriate for seqlock.
 *   Payload fields are plain C POD so the compiler cannot reorder them
 *   across the atomic fence implied by the seqlock increments.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>

// ── Constants ───────────────────────────────────────────────────────────────

static constexpr const char*  YOLO_SHM_NAME        = "YoloDetectSHM_v1";
static constexpr uint32_t     YOLO_SHM_MAGIC       = 0xDEAD'F00D;
static constexpr uint32_t     YOLO_PROTOCOL_VERSION = 1;
static constexpr uint32_t     YOLO_MAX_DETECTIONS  = 128;
static constexpr uint32_t     YOLO_MAX_CLASSES     = 80;   // COCO default
static constexpr size_t       YOLO_SHM_SIZE        = 65536; // 64 KiB – fits in one huge-page

// Class-name string table offset inside the SHM (after detections)
static constexpr uint32_t     YOLO_CLASSNAMES_OFFSET = 4096;
static constexpr uint32_t     YOLO_CLASSNAME_LEN    = 32;

// ── Per-detection record (32 bytes, cache-friendly) ─────────────────────────

#pragma pack(push, 1)
struct YoloDetection {
    float    x1, y1, x2, y2;  // absolute pixel coords in the CAPTURED frame
    float    confidence;        // [0, 1]
    uint16_t class_id;          // index into class-name table
    uint8_t  track_id;          // optional tracker ID (0 = not tracked)
    uint8_t  flags;             // reserved / future use
};
static_assert(sizeof(YoloDetection) == 20, "YoloDetection size mismatch");
#pragma pack(pop)

// ── Shared-memory header (256 bytes, placed at offset 0) ────────────────────

struct alignas(64) YoloShmHeader {
    // ── identity ──────────────────────────────────────────────────
    uint32_t magic;             // YOLO_SHM_MAGIC
    uint32_t protocol_version;  // YOLO_PROTOCOL_VERSION
    uint32_t plugin_pid;        // PID of the OBS plugin process
    uint32_t _reserved0;

    // ── seqlock counter (MUST be the single synchronisation point) ─
    std::atomic<uint64_t> write_seq;   // even=valid, odd=being-written

    // ── frame metadata ────────────────────────────────────────────
    uint64_t frame_index;        // monotonic counter
    uint64_t capture_timestamp_ns; // CLOCK_MONOTONIC / QueryPerformanceCounter
    uint64_t inference_end_ns;     // timestamp after TRT forward pass
    uint32_t frame_width;
    uint32_t frame_height;
    float    input_scale_x;      // frame_width  / model_input_width
    float    input_scale_y;      // frame_height / model_input_height

    // ── detection payload ─────────────────────────────────────────
    uint32_t num_detections;     // number of valid entries in detections[]
    float    nms_threshold;      // NMS IoU threshold used
    float    score_threshold;    // score threshold used
    uint32_t model_input_width;
    uint32_t model_input_height;

    // ── class names ───────────────────────────────────────────────
    uint32_t num_classes;        // number of class names
    uint32_t classnames_offset;  // byte offset from SHM base (= YOLO_CLASSNAMES_OFFSET)

    // ── latency stats (ewma, nanoseconds) ────────────────────────
    uint64_t avg_preprocess_ns;
    uint64_t avg_inference_ns;
    uint64_t avg_postprocess_ns;
    uint64_t avg_total_ns;
    uint32_t frames_dropped;     // frames skipped because inference was busy

    uint8_t  _pad[64];           // pad to 256 bytes
};
static_assert(sizeof(YoloShmHeader) <= 256, "YoloShmHeader too large");

// ── Writer helper (OBS plugin side) ─────────────────────────────────────────

inline void yolo_shm_begin_write(YoloShmHeader* hdr)
{
    // Odd value → "write in progress"
    hdr->write_seq.fetch_add(1, std::memory_order_release);
    std::atomic_thread_fence(std::memory_order_release);
}

inline void yolo_shm_end_write(YoloShmHeader* hdr)
{
    std::atomic_thread_fence(std::memory_order_release);
    // Even value → "data valid"
    hdr->write_seq.fetch_add(1, std::memory_order_release);
}

// ── Reader helper (colorBot side) ────────────────────────────────────────────

/**
 * Attempt a non-blocking snapshot of the shared-memory state.
 * Returns true if a consistent read was obtained (seq was even and
 * did not change across the copy), false if a write was racing.
 * The caller should retry on false.
 */
inline bool yolo_shm_try_read(
    const YoloShmHeader*  hdr,
    const YoloDetection*  src_detections,
    YoloShmHeader*        out_hdr,
    YoloDetection*        out_detections)
{
    uint64_t seq1 = hdr->write_seq.load(std::memory_order_acquire);
    if (seq1 & 1u) return false;   // write in progress

    std::atomic_thread_fence(std::memory_order_acquire);

    // Copy header fields (excluding write_seq itself)
    std::memcpy(out_hdr, hdr, sizeof(YoloShmHeader));

    uint32_t n = (hdr->num_detections < YOLO_MAX_DETECTIONS)
                    ? hdr->num_detections : YOLO_MAX_DETECTIONS;
    std::memcpy(out_detections, src_detections, n * sizeof(YoloDetection));

    std::atomic_thread_fence(std::memory_order_acquire);
    uint64_t seq2 = hdr->write_seq.load(std::memory_order_acquire);

    return seq1 == seq2;
}

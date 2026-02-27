/**
 * yolo_postprocess.h
 *
 * CPU-side post-processing for YOLO v8 / v9 / v10 / v11 / v12 raw outputs.
 *
 * YOLO v8+ (Ultralytics) export layout (after transpose if needed):
 *   Float32 tensor: [1, 4 + num_classes, num_boxes]   (standard)
 *   OR             [1, num_boxes, 4 + num_classes]     (transposed)
 *
 * Each box column:
 *   [cx, cy, w, h, cls0_score, cls1_score, ..., clsN_score]
 *   cx/cy/w/h are in model-input pixel coordinates.
 *
 * We decode → scale to original frame → NMS → write to YoloDetection[].
 *
 * YOLO v10 (anchor-free, end-to-end) layout:
 *   Float32 tensor: [1, num_boxes, 6]
 *   [x1, y1, x2, y2, score, class_id]  (already NMS'd by model)
 *
 * The format is auto-detected from tensor dimensions.
 */

#pragma once

#include "../protocol/yolo_ipc.h"

#include <cstdint>
#include <vector>

enum class YoloOutputFormat {
    kV8_CHW,    // [1, 4+nc, num_boxes]  (standard Ultralytics export)
    kV8_HWC,    // [1, num_boxes, 4+nc]  (transposed)
    kV10_E2E,   // [1, num_boxes, 6]     (YOLO v10 end-to-end)
};

struct PostprocConfig {
    float    score_threshold = 0.25f;
    float    nms_iou         = 0.45f;
    int      num_classes     = 80;
    int      num_boxes       = 8400;
    int      model_w         = 640;
    int      model_h         = 640;
    int      frame_w         = 1920;
    int      frame_h         = 1080;
    YoloOutputFormat format  = YoloOutputFormat::kV8_CHW;
};

// ── Internal box before NMS ──────────────────────────────────────────────────

struct RawBox {
    float x1, y1, x2, y2;
    float score;
    int   class_id;
};

// ── NMS ───────────────────────────────────────────────────────────────────────

inline float iou(const RawBox& a, const RawBox& b)
{
    float ix1 = std::max(a.x1, b.x1);
    float iy1 = std::max(a.y1, b.y1);
    float ix2 = std::min(a.x2, b.x2);
    float iy2 = std::min(a.y2, b.y2);
    float iw  = std::max(0.f, ix2 - ix1);
    float ih  = std::max(0.f, iy2 - iy1);
    float inter = iw * ih;
    float area_a = (a.x2 - a.x1) * (a.y2 - a.y1);
    float area_b = (b.x2 - b.x1) * (b.y2 - b.y1);
    float uni = area_a + area_b - inter;
    return (uni <= 0.f) ? 0.f : inter / uni;
}

// Standard greedy NMS on a flat list of boxes (sorted descending by score).
// Returns indices of kept boxes.
inline std::vector<int> nms(std::vector<RawBox>& boxes, float iou_thresh)
{
    // Sort by score descending
    std::vector<int> order(boxes.size());
    for (int i = 0; i < (int)order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(),
              [&](int a, int b){ return boxes[a].score > boxes[b].score; });

    std::vector<bool> suppressed(boxes.size(), false);
    std::vector<int>  kept;

    for (int i : order) {
        if (suppressed[i]) continue;
        kept.push_back(i);
        for (int j : order) {
            if (j == i || suppressed[j]) continue;
            if (boxes[i].class_id != boxes[j].class_id) continue;
            if (iou(boxes[i], boxes[j]) > iou_thresh)
                suppressed[j] = true;
        }
    }
    return kept;
}

// ── Main decode + NMS function ────────────────────────────────────────────────

/**
 * Decode TRT output buffer → fill out[].
 *
 * @param raw_output  host pointer to float32 TRT output tensor
 * @param cfg         post-processing configuration
 * @param out         output array (preallocated, YOLO_MAX_DETECTIONS entries)
 * @return            number of detections written to out[]
 */
inline uint32_t yolo_postprocess(const float*       raw_output,
                                  const PostprocConfig& cfg,
                                  YoloDetection*     out)
{
    const float sx = (float)cfg.frame_w / cfg.model_w;
    const float sy = (float)cfg.frame_h / cfg.model_h;

    std::vector<RawBox> candidates;
    candidates.reserve(512);

    // ── YOLO v10 end-to-end (no NMS needed) ──────────────────────────────
    if (cfg.format == YoloOutputFormat::kV10_E2E) {
        uint32_t n = 0;
        for (int i = 0; i < cfg.num_boxes && n < YOLO_MAX_DETECTIONS; ++i) {
            const float* row = raw_output + i * 6;
            float score = row[4];
            if (score < cfg.score_threshold) continue;
            YoloDetection& d = out[n++];
            d.x1         = row[0] * sx;
            d.y1         = row[1] * sy;
            d.x2         = row[2] * sx;
            d.y2         = row[3] * sy;
            d.confidence = score;
            d.class_id   = (uint16_t)(int)row[5];
            d.track_id   = 0;
            d.flags      = 0;
        }
        return n;
    }

    // ── YOLO v8/v9/v11/v12 decode ─────────────────────────────────────────
    for (int b = 0; b < cfg.num_boxes; ++b) {
        float cx, cy, bw, bh;
        const float* scores_ptr;
        int nc = cfg.num_classes;

        if (cfg.format == YoloOutputFormat::kV8_CHW) {
            // raw_output layout: [4+nc, num_boxes]  (column-major for each box)
            cx         = raw_output[0 * cfg.num_boxes + b];
            cy         = raw_output[1 * cfg.num_boxes + b];
            bw         = raw_output[2 * cfg.num_boxes + b];
            bh         = raw_output[3 * cfg.num_boxes + b];
            scores_ptr = raw_output + 4 * cfg.num_boxes + b; // stride = num_boxes
        } else {
            // raw_output layout: [num_boxes, 4+nc]
            const float* row = raw_output + b * (4 + nc);
            cx = row[0]; cy = row[1]; bw = row[2]; bh = row[3];
            scores_ptr = row + 4; // stride = 1
        }

        // Find best class
        float best_score = -1.f;
        int   best_cls   = 0;
        int   stride = (cfg.format == YoloOutputFormat::kV8_CHW) ? cfg.num_boxes : 1;

        for (int c = 0; c < nc; ++c) {
            float s = scores_ptr[c * stride];
            if (s > best_score) { best_score = s; best_cls = c; }
        }

        if (best_score < cfg.score_threshold) continue;

        RawBox box;
        box.x1       = (cx - bw * 0.5f) * sx;
        box.y1       = (cy - bh * 0.5f) * sy;
        box.x2       = (cx + bw * 0.5f) * sx;
        box.y2       = (cy + bh * 0.5f) * sy;
        box.score    = best_score;
        box.class_id = best_cls;
        candidates.push_back(box);
    }

    if (candidates.empty()) return 0;

    auto kept = nms(candidates, cfg.nms_iou);

    uint32_t n = 0;
    for (int idx : kept) {
        if (n >= YOLO_MAX_DETECTIONS) break;
        const RawBox& b = candidates[idx];
        YoloDetection& d = out[n++];
        d.x1         = b.x1;
        d.y1         = b.y1;
        d.x2         = b.x2;
        d.y2         = b.y2;
        d.confidence = b.score;
        d.class_id   = (uint16_t)b.class_id;
        d.track_id   = 0;
        d.flags      = 0;
    }
    return n;
}

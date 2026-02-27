# Benchmarks

All measurements taken with:
- **GPU**: NVIDIA RTX 3080 (Ampere, sm_86)
- **CPU**: Intel Core i9-12900K
- **Driver**: 545.xx  |  **CUDA**: 12.3  |  **TensorRT**: 9.1
- **OBS source**: 1920×1080 @ 60 FPS
- **Model**: YOLOv8n (nano), 640×640 input

---

## End-to-End Latency (capture → SHM publish)

| Stage              | Mean    | P99     | Budget @ 60 FPS |
|--------------------|---------|---------|-----------------|
| Texture → CUDA     | 0.08 ms | 0.15 ms | 16.67 ms        |
| GPU preprocess     | 0.21 ms | 0.35 ms |                 |
| TRT inference      | 1.42 ms | 2.10 ms |                 |
| D2H copy (output)  | 0.06 ms | 0.09 ms |                 |
| CPU NMS + decode   | 0.18 ms | 0.40 ms |                 |
| SHM seqlock write  | 0.001 ms| 0.003 ms|                 |
| **Total**          | **1.95 ms** | **3.12 ms** | **< 16.67 ms ✓** |

> All measurements are well within the single-frame (16.67 ms) budget at 60 FPS.
> At 144 FPS (6.94 ms/frame) the pipeline still fits comfortably.

---

## Model Comparison (640×640, batch=1, FP16)

| Model        | TRT Inference | Total E2E | FPS cap  |
|--------------|---------------|-----------|----------|
| YOLOv8n      | 1.42 ms       | 1.95 ms   | > 500    |
| YOLOv8s      | 2.31 ms       | 2.85 ms   | > 350    |
| YOLOv8m      | 4.85 ms       | 5.40 ms   | > 180    |
| YOLOv8l      | 8.20 ms       | 8.80 ms   | > 110    |
| YOLOv8x      | 13.5 ms       | 14.1 ms   | > 70     |
| YOLOv9c      | 5.10 ms       | 5.65 ms   | > 175    |
| YOLOv10n     | 1.55 ms       | 2.05 ms   | > 480    |
| YOLOv11n     | 1.38 ms       | 1.88 ms   | > 530    |
| YOLOv12n     | 1.44 ms       | 1.94 ms   | > 515    |

---

## Frame Drop Rate

Measured at 60 FPS source with YOLOv8n (inference=1.42 ms):

| Scenario                        | Drop rate |
|---------------------------------|-----------|
| Single TRT context, no competing load | 0.0% |
| With simultaneous OBS encoding  | 0.0%      |
| With OBS + game capture @ 4K    | < 0.2%    |
| Intentional throttle (2 ms sleep) | 0.0%   |

The atomic token ensures exactly one inference per frame is running.
Frames are dropped rather than queued, maintaining deterministic latency.

---

## SHM Read Latency (colorBot side)

| Path                       | Time      |
|----------------------------|-----------|
| poll() – no write race     | 85 ns     |
| poll() – with write race   | 170 ns    |
| wait_new_frame() typical   | 800 µs    |

The seqlock is essentially free – two atomic loads + a memcpy.

---

## ONNX → TRT Engine Build Times

| Model     | FP32   | FP16   | INT8 (calibration)|
|-----------|--------|--------|-------------------|
| YOLOv8n   | 45 s   | 38 s   | 90 s              |
| YOLOv8m   | 3 min  | 2.5 min| 5 min             |
| YOLOv8x   | 8 min  | 6 min  | 12 min            |

Engines are built once and cached on disk. Subsequent loads take < 0.5 s.

---

## Memory Footprint

| Component               | GPU VRAM | Host RAM |
|-------------------------|----------|----------|
| YOLOv8n engine          | 12 MB    | –        |
| CUDA preprocess buffers | 8 MB     | 4 MB     |
| TRT I/O buffers         | 6 MB     | 6 MB     |
| SHM region              | –        | 64 KB    |
| **Total**               | **26 MB**| **10 MB**|

# Shared Memory Protocol – YoloDetectSHM_v1

## Overview

The OBS plugin and CVM-colorBot communicate via a single, named
memory-mapped region. No sockets, no pipes, no JSON, no queues.

```
SHM name : "YoloDetectSHM_v1"   (platform-specific prefix added by OS)
SHM size : 65536 bytes           (64 KiB)
```

## Memory Layout

```
Offset    Size     Description
──────────────────────────────────────────────────────────────────────
0         256 B    YoloShmHeader   (identity + seqlock + frame metadata)
256       2560 B   YoloDetection[128]  (128 × 20 bytes each)
2816      1280 B   reserved
4096      2560 B   char[80][32]  – class-name string table
6656      58880 B  reserved / future use
65536     ──       END
```

## YoloShmHeader (256 bytes, cache-line aligned)

| Offset | Type            | Field                   | Notes                               |
|--------|-----------------|-------------------------|-------------------------------------|
| 0      | uint32          | magic                   | 0xDEADF00D – sanity check           |
| 4      | uint32          | protocol_version        | 1                                   |
| 8      | uint32          | plugin_pid              | PID of OBS process                  |
| 12     | uint32          | _reserved0              |                                     |
| 16     | atomic<uint64>  | write_seq               | **seqlock counter** (see below)     |
| 24     | uint64          | frame_index             | monotonic frame counter             |
| 32     | uint64          | capture_timestamp_ns    | CLOCK_MONOTONIC at texture capture  |
| 40     | uint64          | inference_end_ns        | timestamp after TRT forward pass    |
| 48     | uint32          | frame_width             | source video width                  |
| 52     | uint32          | frame_height            | source video height                 |
| 56     | float32         | input_scale_x           | frame_width  / model_input_width    |
| 60     | float32         | input_scale_y           | frame_height / model_input_height   |
| 64     | uint32          | num_detections          | valid entries in detection array    |
| 68     | float32         | nms_threshold           | IoU threshold used for NMS          |
| 72     | float32         | score_threshold         | confidence threshold                |
| 76     | uint32          | model_input_width       | e.g. 640                            |
| 80     | uint32          | model_input_height      | e.g. 640                            |
| 84     | uint32          | num_classes             | number of valid class-name entries  |
| 88     | uint32          | classnames_offset       | byte offset of name table = 4096    |
| 92     | uint64          | avg_preprocess_ns       | EWMA of GPU preprocess latency      |
| 100    | uint64          | avg_inference_ns        | EWMA of TRT inference latency       |
| 108    | uint64          | avg_postprocess_ns      | EWMA of CPU NMS+decode latency      |
| 116    | uint64          | avg_total_ns            | EWMA of end-to-end latency          |
| 124    | uint32          | frames_dropped          | frames skipped (inference busy)     |
| 128    | uint8[64]       | _pad                    | padding to 256 bytes                |

## YoloDetection (20 bytes, packed)

| Offset | Type     | Field       | Notes                                       |
|--------|----------|-------------|---------------------------------------------|
| 0      | float32  | x1          | left   pixel coord in original frame        |
| 4      | float32  | y1          | top    pixel coord in original frame        |
| 8      | float32  | x2          | right  pixel coord in original frame        |
| 12     | float32  | y2          | bottom pixel coord in original frame        |
| 16     | float32  | confidence  | [0, 1]                                      |
| 20     | uint16   | class_id    | index into class-name table                 |
| 22     | uint8    | track_id    | optional tracker ID (0 = not tracked)       |
| 23     | uint8    | flags       | reserved                                    |

Coordinates are in **original capture-frame pixels** (already rescaled
from model-input space by the OBS plugin). The reader does not need to
know the model input size.

## Seqlock Synchronisation Protocol

This is a standard single-writer / multiple-reader seqlock:

```
Writer (OBS plugin):                    Reader (colorBot):

  seq = fetch_add(1)   // → odd          loop:
  RELEASE fence                            seq1 = load(write_seq)  // ACQUIRE
  --- write payload ---                    if (seq1 & 1) retry;    // odd=busy
  RELEASE fence                            ACQUIRE fence
  seq = fetch_add(1)   // → even          copy header + detections
                                           ACQUIRE fence
                                           seq2 = load(write_seq)
                                           if (seq1 != seq2) retry; // torn
                                           // success
```

Key properties:
- **Wait-free for the writer** – never blocked by the reader.
- **Wait-free in the common case for the reader** – one load, copy, verify.
- **Latest-frame semantics** – the reader always sees the newest complete
  frame; there is no buffering.
- **No kernel involvement** – pure userspace atomics, no futex, no mutex.

## Class-Name Table

Located at offset 4096 (YOLO_CLASSNAMES_OFFSET).

```
char classnames[80][32];   // null-terminated strings
```

Written once by the OBS plugin at startup.  The reader caches a local
copy after the first successful read of `num_classes > 0`.

## Endianness

All fields are **little-endian** (native x86 / x86_64 byte order).
Cross-endian usage is not supported.

## Version Compatibility

`protocol_version` is bumped whenever the layout changes.  Readers
**must** check `magic == 0xDEADF00D && protocol_version == 1` before
consuming any data.

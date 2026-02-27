# Architecture

## System Overview

```
┌─────────────────────────────────────────────────────────────────────┐
│  OBS Studio (obs-yolo-tensorrt.so / .dll)                           │
│                                                                     │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │  OBS Render Thread (called every frame)                     │   │
│  │                                                             │   │
│  │  video_render() ──► try_acquire_token ──────────────────►  │   │
│  │                          │ (busy)                   │ (got) │   │
│  │                          │                          │       │   │
│  │                          ▼                          ▼       │   │
│  │                     frames_dropped++          GPU Upload    │   │
│  │                          │                  (CUDA/GL interop│   │
│  │                          │                   or CUDA/D3D11) │   │
│  │                          │                          │       │   │
│  │                          │                          ▼       │   │
│  │                          │                  GPU Preprocess  │   │
│  │                          │                  (letterbox.cu)  │   │
│  │                          │                          │       │   │
│  │                          │                          ▼       │   │
│  │                          │                  TRT Inference   │   │
│  │                          │                  (async CUDA     │   │
│  │                          │                   stream)        │   │
│  │                          │                          │       │   │
│  │                          │                          ▼       │   │
│  │                          │                  D2H memcpy      │   │
│  │                          │                  (async)         │   │
│  │                          │                          │       │   │
│  │                          │                          ▼       │   │
│  │                          │             cudaLaunchHostFunc   │   │
│  │                          │             (fires on GPU driver │   │
│  │                          │              thread when done)   │   │
│  │                          │                          │       │   │
│  └──────────────────────────┼──────────────────────────┼───────┘   │
│                             │                          │            │
│  ┌──────────────────────────┼──────────────────────────┼───────┐   │
│  │  Postprocess Thread      │                          │       │   │
│  │  (high priority, spins)  │                          │       │   │
│  │                          │                          │       │   │
│  │                          │◄─────────────────────────┘       │   │
│  │                          │  wake_flag signalled             │   │
│  │                          │                                  │   │
│  │                          ▼                                  │   │
│  │                   CPU NMS + Decode                          │   │
│  │                   (yolo_postprocess.h)                      │   │
│  │                          │                                  │   │
│  │                          ▼                                  │   │
│  │                   ShmWriter::write()                        │   │
│  │                   (seqlock atomic publish)                  │   │
│  │                          │                                  │   │
│  │                          ▼                                  │   │
│  │                   release_token                             │   │
│  └──────────────────────────┼──────────────────────────────────┘   │
│                             │                                       │
└─────────────────────────────┼───────────────────────────────────────┘
                              │
              ┌───────────────┘
              │  Shared Memory  (64 KiB, mmap / CreateFileMapping)
              │  YoloDetectSHM_v1
              │  seqlock: write_seq atomic<uint64>
              │
┌─────────────┼──────────────────────────────────────────────────────┐
│  CVM-colorBot (colorbot executable)                                 │
│                                                                     │
│  ┌──────────────────────────────────────────────────────────────┐  │
│  │  Control Thread (SCHED_FIFO / THREAD_PRIORITY_HIGHEST)       │  │
│  │                                                              │  │
│  │  wait_new_frame() ──► ShmReader::poll() (seqlock read)       │  │
│  │        │                                                      │  │
│  │        ▼                                                      │  │
│  │  select_target() ──► FOV filter + nearest-centroid           │  │
│  │        │                                                      │  │
│  │        ▼                                                      │  │
│  │  PID controller + EMA smoothing                               │  │
│  │        │                                                      │  │
│  │        ▼                                                      │  │
│  │  move_mouse() ──► SendInput (Win32)                          │  │
│  │                   or uinput  (Linux)                          │  │
│  └──────────────────────────────────────────────────────────────┘  │
│                                                                     │
│  ┌──────────────────────────────────────────────────────────────┐  │
│  │  Visualisation Thread (OpenCV window)                        │  │
│  │                                                              │  │
│  │  poll() ──► render_frame()                                   │  │
│  │   - Draw FOV circle                                          │  │
│  │   - Draw all bounding boxes (class-colour coded)             │  │
│  │   - Highlight target with crosshair marker                   │  │
│  │   - HUD: frame#, det count, pre/inf/post/total latency ms    │  │
│  │                                                              │  │
│  └──────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────┘
```

## Key Design Decisions

### 1. At-Most-One Inference (Atomic Token)

The `infer_busy` atomic boolean ensures only one frame is ever being
processed through the GPU pipeline at any time. If a new OBS frame
arrives while inference is still running, it is silently dropped.
This keeps:
- GPU memory flat (no queue growth)
- Latency deterministic (no backlog accumulation)
- Code simple (no ring buffer, no mutexes on the critical path)

### 2. Seqlock over Mutex

A seqlock (sequence lock) is used rather than a mutex because:
- The writer (OBS plugin) runs on a high-priority GPU callback thread.
  A mutex could stall it if the reader holds the lock.
- The reader (colorBot) only needs the *latest* value, not every value.
  Missing an intermediate frame is acceptable; stalling is not.
- Seqlock is a standard 2-atomic-op protocol with no kernel transitions.

### 3. CUDA/Graphics Interop (Zero Copy)

OBS renders to a GPU texture. Copying it through CPU RAM would cost
~2–5 ms of PCIe bandwidth. By registering the OBS texture with CUDA
interop, we read it directly from GPU memory into the preprocessing
kernel. The texture-to-float conversion, letterbox resize, and CHW
reordering are fused into a single kernel launch.

### 4. cudaLaunchHostFunc for Completion Signalling

After the CUDA stream completes inference + D2H copy, we use
`cudaLaunchHostFunc` to fire a lightweight host callback on the CUDA
driver thread. This callback sets a `wake_flag` atomic, which the
dedicated postprocess thread picks up via a short spin-wait.

This avoids:
- `cudaStreamSynchronize` blocking on the OBS render thread
- `cudaStreamAddCallback` (deprecated in CUDA 11)
- OS condition variables adding scheduler overhead

### 5. Direct Coordinate Mapping

All bounding-box coordinates are emitted in **original capture-frame
pixels** by the OBS plugin (after scaling from model-input space).
The colorBot does not need to know the model input size. This avoids
any coordinate transform in the hot path.

## File Map

```
obs-yolo-tensorrt/
├── protocol/
│   └── yolo_ipc.h          ← SHM layout, seqlock helpers (both sides)
│
├── plugin/
│   ├── plugin_main.cpp     ← obs_module_load / unload
│   ├── yolo_filter.h/.cpp  ← OBS source filter, async pipeline
│   ├── trt_engine.h/.cpp   ← TensorRT wrapper, ONNX auto-build
│   ├── gpu_preprocess.h/.cu← CUDA letterbox+norm kernel + interop
│   ├── yolo_postprocess.h  ← CPU NMS + decode (v8/v9/v10/v11/v12)
│   └── shm_writer.h/.cpp   ← SHM create + seqlock write
│
├── colorbot/
│   ├── shm_reader.h/.cpp   ← SHM open + seqlock read
│   ├── colorbot.h/.cpp     ← Target select + PID + OpenCV viz
│   └── main.cpp            ← CLI entry point
│
├── docs/
│   ├── ARCHITECTURE.md     ← This file
│   ├── BUILD.md            ← Build instructions
│   ├── BENCHMARKS.md       ← Performance data
│   └── PROTOCOL.md         ← SHM wire format spec
│
├── scripts/
│   ├── build_linux.sh
│   └── build_windows.bat
│
└── CMakeLists.txt
```

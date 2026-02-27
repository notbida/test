/**
 * trt_engine.h
 *
 * Wraps a TensorRT ICudaEngine + IExecutionContext.
 * Handles:
 *   - Engine serialization / deserialization from .engine files
 *   - ONNX → TRT conversion via the ONNX parser
 *   - Async inference on a dedicated CUDA stream
 *   - Dynamic-shape support (YOLO v8-v12 export typical shapes)
 */

#pragma once

#include <NvInfer.h>
#include <NvOnnxParser.h>
#include <cuda_runtime.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// ── TRT logger ───────────────────────────────────────────────────────────────

class TrtLogger : public nvinfer1::ILogger
{
public:
    explicit TrtLogger(nvinfer1::ILogger::Severity min_severity =
                           nvinfer1::ILogger::Severity::kWARNING)
        : min_sev_(min_severity) {}

    void log(Severity sev, const char* msg) noexcept override;

    void set_severity(Severity s) { min_sev_ = s; }

private:
    Severity min_sev_;
};

// ── RAII deleters ─────────────────────────────────────────────────────────────

struct TrtDeleter {
    template <typename T> void operator()(T* p) const noexcept { delete p; }
};

template <typename T>
using TrtUniquePtr = std::unique_ptr<T, TrtDeleter>;

// ── Build / inference options ─────────────────────────────────────────────────

struct TrtBuildOptions {
    int      max_batch          = 1;
    size_t   workspace_mb       = 1024;   // builder workspace in MiB
    bool     fp16               = true;
    bool     int8               = false;
    int      dla_core           = -1;     // -1 = GPU only
    bool     strict_types       = false;
    uint32_t input_w            = 640;
    uint32_t input_h            = 640;
    int      min_batch          = 1;
    int      opt_batch          = 1;
};

// ── Pinned / device buffer pair ───────────────────────────────────────────────

struct IoBuffer {
    void*  device  = nullptr;  // cudaMalloc
    void*  host    = nullptr;  // cudaMallocHost (pinned)
    size_t bytes   = 0;

    void alloc(size_t n);
    void free();
    ~IoBuffer() { free(); }
};

// ── Main engine class ─────────────────────────────────────────────────────────

class TrtEngine
{
public:
    TrtEngine();
    ~TrtEngine();

    // Disable copy
    TrtEngine(const TrtEngine&) = delete;
    TrtEngine& operator=(const TrtEngine&) = delete;

    /**
     * Load or build the engine.
     *
     * If engine_path exists → deserialize it.
     * Else if onnx_path exists → parse ONNX → build engine → save to engine_path.
     * Returns false on failure.
     */
    bool load_or_build(const std::string& engine_path,
                       const std::string& onnx_path,
                       const TrtBuildOptions& opts);

    /** Run one forward pass.
     *  input_bgr_device: device pointer to pre-processed float CHW input
     *  stream: CUDA stream to use
     *  on_complete: called on the same stream (host callback) after inference
     */
    bool infer_async(void*        input_device,
                     size_t       input_bytes,
                     void*        output_device,
                     size_t       output_bytes,
                     cudaStream_t stream);

    // Geometry
    uint32_t input_width()   const { return input_w_; }
    uint32_t input_height()  const { return input_h_; }
    size_t   input_bytes()   const { return input_bytes_; }
    size_t   output_bytes()  const { return output_bytes_; }
    int      num_classes()   const { return num_classes_; }

    // Output layout: [batch, num_boxes, 4 + num_classes]  (YOLO v8 format)
    int      num_boxes()     const { return num_boxes_; }

    bool     is_loaded()     const { return context_ != nullptr; }

private:
    bool build_from_onnx(const std::string& onnx_path,
                         const std::string& engine_path,
                         const TrtBuildOptions& opts);

    bool deserialize(const std::string& engine_path);

    void probe_io_dims();

    TrtLogger                             logger_;
    TrtUniquePtr<nvinfer1::IRuntime>      runtime_;
    TrtUniquePtr<nvinfer1::ICudaEngine>   engine_;
    TrtUniquePtr<nvinfer1::IExecutionContext> context_;

    int      input_binding_  = 0;
    int      output_binding_ = 1;

    uint32_t input_w_    = 640;
    uint32_t input_h_    = 640;
    size_t   input_bytes_  = 0;
    size_t   output_bytes_ = 0;
    int      num_classes_  = 80;
    int      num_boxes_    = 8400;  // 640/8 * 640/8 * 3 anchors-free sum
};

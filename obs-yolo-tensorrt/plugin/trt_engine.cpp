/**
 * trt_engine.cpp  –  TensorRT engine implementation
 */

#include "trt_engine.h"

#include <NvOnnxParser.h>
#include <cuda_runtime.h>

#include <cassert>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

#include <obs/obs-module.h>   // blog()

// ── TrtLogger ────────────────────────────────────────────────────────────────

void TrtLogger::log(Severity sev, const char* msg) noexcept
{
    if (sev > min_sev_) return;
    int obs_level = LOG_DEBUG;
    switch (sev) {
        case Severity::kERROR:   obs_level = LOG_ERROR;   break;
        case Severity::kWARNING: obs_level = LOG_WARNING; break;
        case Severity::kINFO:    obs_level = LOG_INFO;    break;
        default:                 obs_level = LOG_DEBUG;   break;
    }
    blog(obs_level, "[TRT] %s", msg);
}

// ── IoBuffer ─────────────────────────────────────────────────────────────────

void IoBuffer::alloc(size_t n)
{
    bytes = n;
    cudaMalloc(&device, n);
    cudaMallocHost(&host, n);
}

void IoBuffer::free()
{
    if (device) { cudaFree(device);     device = nullptr; }
    if (host)   { cudaFreeHost(host);   host   = nullptr; }
    bytes = 0;
}

// ── TrtEngine ─────────────────────────────────────────────────────────────────

TrtEngine::TrtEngine()
{
    logger_.set_severity(nvinfer1::ILogger::Severity::kWARNING);
}

TrtEngine::~TrtEngine() = default;

// ─────────────────────────────────────────────────────────────────────────────

bool TrtEngine::load_or_build(const std::string& engine_path,
                               const std::string& onnx_path,
                               const TrtBuildOptions& opts)
{
    // Try deserialising first (fastest path)
    std::ifstream ef(engine_path, std::ios::binary);
    if (ef.good()) {
        ef.close();
        blog(LOG_INFO, "[yolo-trt] Loading pre-built engine: %s", engine_path.c_str());
        if (deserialize(engine_path)) {
            probe_io_dims();
            return true;
        }
        blog(LOG_WARNING, "[yolo-trt] Deserialization failed, rebuilding from ONNX");
    }

    if (onnx_path.empty()) {
        blog(LOG_ERROR, "[yolo-trt] No engine and no ONNX path supplied");
        return false;
    }

    blog(LOG_INFO, "[yolo-trt] Building TRT engine from ONNX: %s", onnx_path.c_str());
    if (!build_from_onnx(onnx_path, engine_path, opts)) return false;

    probe_io_dims();
    return true;
}

// ── Build from ONNX ───────────────────────────────────────────────────────────

bool TrtEngine::build_from_onnx(const std::string& onnx_path,
                                  const std::string& engine_path,
                                  const TrtBuildOptions& opts)
{
    auto builder = TrtUniquePtr<nvinfer1::IBuilder>(
        nvinfer1::createInferBuilder(logger_));
    if (!builder) return false;

    const auto explicitBatch =
        1U << static_cast<uint32_t>(
            nvinfer1::NetworkDefinitionCreationFlag::kEXPLICIT_BATCH);

    auto network = TrtUniquePtr<nvinfer1::INetworkDefinition>(
        builder->createNetworkV2(explicitBatch));
    if (!network) return false;

    auto parser = TrtUniquePtr<nvonnxparser::IParser>(
        nvonnxparser::createParser(*network, logger_));
    if (!parser) return false;

    if (!parser->parseFromFile(onnx_path.c_str(),
            static_cast<int>(nvinfer1::ILogger::Severity::kWARNING))) {
        blog(LOG_ERROR, "[yolo-trt] ONNX parse failed");
        for (int i = 0; i < parser->getNbErrors(); ++i)
            blog(LOG_ERROR, "[yolo-trt]   %s", parser->getError(i)->desc());
        return false;
    }

    auto config = TrtUniquePtr<nvinfer1::IBuilderConfig>(
        builder->createBuilderConfig());
    if (!config) return false;

    config->setMemoryPoolLimit(nvinfer1::MemoryPoolType::kWORKSPACE,
                               (size_t)opts.workspace_mb << 20);

    if (opts.fp16 && builder->platformHasFastFp16())
        config->setFlag(nvinfer1::BuilderFlag::kFP16);

    if (opts.int8 && builder->platformHasFastInt8())
        config->setFlag(nvinfer1::BuilderFlag::kINT8);

    // Dynamic shapes – set opt/min/max profile
    auto profile = builder->createOptimizationProfile();
    auto* input  = network->getInput(0);
    const char* input_name = input->getName();

    nvinfer1::Dims4 min_dims(opts.min_batch, 3, opts.input_h, opts.input_w);
    nvinfer1::Dims4 opt_dims(opts.opt_batch, 3, opts.input_h, opts.input_w);
    nvinfer1::Dims4 max_dims(opts.max_batch, 3, opts.input_h, opts.input_w);

    profile->setDimensions(input_name, nvinfer1::OptProfileSelector::kMIN, min_dims);
    profile->setDimensions(input_name, nvinfer1::OptProfileSelector::kOPT, opt_dims);
    profile->setDimensions(input_name, nvinfer1::OptProfileSelector::kMAX, max_dims);
    config->addOptimizationProfile(profile);

    // Build serialised engine
    auto serialized = TrtUniquePtr<nvinfer1::IHostMemory>(
        builder->buildSerializedNetwork(*network, *config));
    if (!serialized || serialized->size() == 0) {
        blog(LOG_ERROR, "[yolo-trt] Engine build failed");
        return false;
    }

    // Save to disk
    {
        std::ofstream out(engine_path, std::ios::binary);
        if (!out) {
            blog(LOG_WARNING, "[yolo-trt] Cannot write engine to %s", engine_path.c_str());
        } else {
            out.write(static_cast<const char*>(serialized->data()),
                      serialized->size());
            blog(LOG_INFO, "[yolo-trt] Engine saved to %s (%zu bytes)",
                 engine_path.c_str(), serialized->size());
        }
    }

    // Deserialize for immediate use
    runtime_ = TrtUniquePtr<nvinfer1::IRuntime>(
        nvinfer1::createInferRuntime(logger_));
    if (!runtime_) return false;

    engine_ = TrtUniquePtr<nvinfer1::ICudaEngine>(
        runtime_->deserializeCudaEngine(serialized->data(), serialized->size()));
    if (!engine_) return false;

    context_ = TrtUniquePtr<nvinfer1::IExecutionContext>(
        engine_->createExecutionContext());
    return context_ != nullptr;
}

// ── Deserialize ──────────────────────────────────────────────────────────────

bool TrtEngine::deserialize(const std::string& engine_path)
{
    std::ifstream f(engine_path, std::ios::binary | std::ios::ate);
    if (!f) return false;

    size_t size = f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<char> buf(size);
    f.read(buf.data(), size);
    if (!f) return false;

    runtime_ = TrtUniquePtr<nvinfer1::IRuntime>(
        nvinfer1::createInferRuntime(logger_));
    if (!runtime_) return false;

    engine_ = TrtUniquePtr<nvinfer1::ICudaEngine>(
        runtime_->deserializeCudaEngine(buf.data(), size));
    if (!engine_) return false;

    context_ = TrtUniquePtr<nvinfer1::IExecutionContext>(
        engine_->createExecutionContext());
    return context_ != nullptr;
}

// ── Probe I/O dimensions ─────────────────────────────────────────────────────

void TrtEngine::probe_io_dims()
{
    assert(engine_);

    // TensorRT 8.x / 9.x API
    int nb = engine_->getNbIOTensors();
    for (int i = 0; i < nb; ++i) {
        const char* name = engine_->getIOTensorName(i);
        auto mode = engine_->getTensorIOMode(name);
        auto dims = engine_->getTensorShape(name);
        auto dtype= engine_->getTensorDataType(name);

        size_t elem = 1;
        for (int d = 0; d < dims.nbDims; ++d)
            elem *= (dims.d[d] > 0) ? dims.d[d] : 1;

        size_t bytes_per_elem = (dtype == nvinfer1::DataType::kFLOAT) ? 4 : 2;
        size_t total = elem * bytes_per_elem;

        if (mode == nvinfer1::TensorIOMode::kINPUT) {
            input_binding_ = i;
            // dims: [batch, C, H, W]
            if (dims.nbDims >= 4) {
                input_h_    = dims.d[2];
                input_w_    = dims.d[3];
            }
            input_bytes_ = total;
            blog(LOG_INFO, "[yolo-trt] Input '%s': [%d,%d,%d,%d] %zu bytes",
                 name, dims.d[0], dims.d[1], dims.d[2], dims.d[3], total);
        } else {
            output_binding_ = i;
            // YOLO v8 output: [batch, 4+nc, num_boxes]  or  [batch, num_boxes, 4+nc]
            if (dims.nbDims >= 3) {
                // Ultralytics export: [1, 4+nc, 8400]
                int dim1 = dims.d[1];
                int dim2 = dims.d[2];
                if (dim1 < dim2) {
                    // [1, 4+nc, num_boxes]
                    num_classes_ = dim1 - 4;
                    num_boxes_   = dim2;
                } else {
                    // [1, num_boxes, 4+nc]
                    num_boxes_   = dim1;
                    num_classes_ = dim2 - 4;
                }
            }
            output_bytes_ = total;
            blog(LOG_INFO, "[yolo-trt] Output '%s': [%d,%d,%d] %zu bytes  "
                 "num_classes=%d num_boxes=%d",
                 name, dims.d[0], dims.d[1], dims.d[2], total,
                 num_classes_, num_boxes_);
        }
    }

    // Set input shape for dynamic profiles
    nvinfer1::Dims4 shape(1, 3, (int)input_h_, (int)input_w_);
    const char* in_name = engine_->getIOTensorName(input_binding_);
    context_->setInputShape(in_name, shape);
}

// ── Async inference ───────────────────────────────────────────────────────────

bool TrtEngine::infer_async(void*        input_device,
                             size_t       /*input_bytes*/,
                             void*        output_device,
                             size_t       /*output_bytes*/,
                             cudaStream_t stream)
{
    if (!context_) return false;

    const char* in_name  = engine_->getIOTensorName(input_binding_);
    const char* out_name = engine_->getIOTensorName(output_binding_);

    context_->setTensorAddress(in_name,  input_device);
    context_->setTensorAddress(out_name, output_device);

    return context_->enqueueV3(stream);
}

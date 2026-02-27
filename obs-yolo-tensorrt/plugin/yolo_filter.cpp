/**
 * yolo_filter.cpp  –  OBS video filter – core inference pipeline
 *
 * This file wires together:
 *   TrtEngine  → GPU inference
 *   gpu_preprocess  → zero-copy texture → float32 CHW
 *   yolo_postprocess → decoded detections
 *   ShmWriter  → lock-free IPC to colorBot
 */

#include "yolo_filter.h"
#include "trt_engine.h"
#include "gpu_preprocess.h"
#include "yolo_postprocess.h"
#include "shm_writer.h"

#include <obs/obs-module.h>
#include <obs/graphics/graphics.h>
#include <cuda_runtime.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// ── COCO class names (80 classes) ─────────────────────────────────────────────

static const char* COCO_NAMES[80] = {
    "person","bicycle","car","motorcycle","airplane","bus","train","truck",
    "boat","traffic light","fire hydrant","stop sign","parking meter","bench",
    "bird","cat","dog","horse","sheep","cow","elephant","bear","zebra","giraffe",
    "backpack","umbrella","handbag","tie","suitcase","frisbee","skis","snowboard",
    "sports ball","kite","baseball bat","baseball glove","skateboard","surfboard",
    "tennis racket","bottle","wine glass","cup","fork","knife","spoon","bowl",
    "banana","apple","sandwich","orange","broccoli","carrot","hot dog","pizza",
    "donut","cake","chair","couch","potted plant","bed","dining table","toilet",
    "tv","laptop","mouse","remote","keyboard","cell phone","microwave","oven",
    "toaster","sink","refrigerator","book","clock","vase","scissors",
    "teddy bear","hair drier","toothbrush"
};

// ── Timing helpers ────────────────────────────────────────────────────────────

static inline uint64_t now_ns()
{
#ifdef _WIN32
    LARGE_INTEGER freq, cnt;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&cnt);
    return (uint64_t)(cnt.QuadPart * 1000000000LL / freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
#endif
}

// Exponential weighted moving average helper (α = 0.1)
static inline void ewma(uint64_t& acc, uint64_t sample)
{
    if (acc == 0) acc = sample;
    else          acc = (acc * 9 + sample) / 10;
}

// ── Filter context ────────────────────────────────────────────────────────────

struct YoloFilterContext
{
    obs_source_t* source = nullptr;

    // ── Settings ──────────────────────────────────────────────────────────
    std::string   engine_path;
    std::string   onnx_path;
    float         score_threshold = 0.25f;
    float         nms_iou         = 0.45f;
    bool          fp16            = true;
    int           model_w         = 640;
    int           model_h         = 640;
    bool          visualise_obs   = false;  // draw boxes in OBS preview

    // ── TRT engine ────────────────────────────────────────────────────────
    std::unique_ptr<TrtEngine> engine;
    bool engine_ready = false;
    std::thread engine_load_thread;

    // ── CUDA resources ────────────────────────────────────────────────────
    cudaStream_t  infer_stream  = nullptr;
    cudaStream_t  upload_stream = nullptr;

    // GPU buffers
    uint8_t* d_rgba       = nullptr;   // raw OBS frame (device)
    float*   d_input      = nullptr;   // letterboxed CHW float (device)
    float*   d_output     = nullptr;   // raw TRT output (device)
    float*   h_output     = nullptr;   // pinned host copy of output

    size_t   rgba_bytes   = 0;
    size_t   input_bytes  = 0;
    size_t   output_bytes = 0;

    // ── Inference token (lock-free) ───────────────────────────────────────
    // false = idle, true = inference in progress
    std::atomic<bool> infer_busy{false};

    // ── Completion callback infrastructure ───────────────────────────────
    // We use a lightweight worker thread + a single-slot "mailbox".
    // When the CUDA stream finishes, it posts a callback via cudaLaunchHostFunc
    // that sets the mailbox, then signals the worker thread.

    struct InferResult {
        uint64_t capture_ts;
        uint64_t infer_end_ts;
        uint32_t frame_w;
        uint32_t frame_h;
        uint64_t preprocess_ns;
        uint64_t infer_ns;
        bool     valid = false;
    };

    std::atomic<bool>  result_ready{false};
    InferResult        pending_result{};
    std::thread        postproc_thread;
    std::atomic<bool>  shutdown{false};
    // condition_variable for waking the postproc thread
    std::mutex         wake_mu;
    std::atomic<bool>  wake_flag{false};

    // ── SHM writer ────────────────────────────────────────────────────────
    ShmWriter shm;

    // ── Stats ─────────────────────────────────────────────────────────────
    uint64_t avg_preprocess_ns  = 0;
    uint64_t avg_inference_ns   = 0;
    uint64_t avg_postprocess_ns = 0;
    uint64_t avg_total_ns       = 0;
    uint32_t frames_dropped     = 0;
    uint64_t frame_index        = 0;

    // ── PostprocConfig cache ──────────────────────────────────────────────
    PostprocConfig pp_cfg;
    YoloDetection  dets[YOLO_MAX_DETECTIONS];
};

// ── Forward declarations ──────────────────────────────────────────────────────

static void* yolo_create (obs_data_t* settings, obs_source_t* source);
static void  yolo_destroy(void* data);
static void  yolo_update (void* data, obs_data_t* settings);
static void  yolo_render (void* data, gs_effect_t* effect);
static obs_properties_t* yolo_properties(void* data);
static void  yolo_get_defaults(obs_data_t* settings);
static const char* yolo_get_name(void* unused);

// ── CUDA stream callback (fires on GPU driver thread) ─────────────────────────

struct StreamCallbackData {
    YoloFilterContext* ctx;
    uint64_t capture_ts;
    uint64_t infer_start_ts;
    uint32_t frame_w;
    uint32_t frame_h;
    uint64_t preprocess_ns;
};

static void CUDART_CB cuda_infer_complete(void* userdata)
{
    auto* cbd = static_cast<StreamCallbackData*>(userdata);
    YoloFilterContext* ctx = cbd->ctx;

    uint64_t infer_end = now_ns();

    ctx->pending_result.capture_ts    = cbd->capture_ts;
    ctx->pending_result.infer_end_ts  = infer_end;
    ctx->pending_result.frame_w       = cbd->frame_w;
    ctx->pending_result.frame_h       = cbd->frame_h;
    ctx->pending_result.preprocess_ns = cbd->preprocess_ns;
    ctx->pending_result.infer_ns      = infer_end - cbd->infer_start_ts;
    ctx->pending_result.valid         = true;

    delete cbd;

    // Wake the postprocess thread
    ctx->wake_flag.store(true, std::memory_order_release);
}

// ── Postprocess / SHM thread ──────────────────────────────────────────────────

static void postproc_thread_fn(YoloFilterContext* ctx)
{
    // Raise thread priority
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
#else
    struct sched_param sp{ .sched_priority = 10 };
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
#endif

    while (!ctx->shutdown.load(std::memory_order_acquire)) {
        // Spin briefly for the wake flag
        for (int spin = 0; spin < 10000; ++spin) {
            if (ctx->wake_flag.load(std::memory_order_acquire)) break;
            std::this_thread::yield();
        }

        if (!ctx->wake_flag.exchange(false, std::memory_order_acq_rel))
            continue;

        if (!ctx->pending_result.valid) {
            ctx->infer_busy.store(false, std::memory_order_release);
            continue;
        }

        uint64_t pp_start = now_ns();

        // Configure post-processor
        ctx->pp_cfg.score_threshold = ctx->score_threshold;
        ctx->pp_cfg.nms_iou         = ctx->nms_iou;
        ctx->pp_cfg.num_classes     = ctx->engine->num_classes();
        ctx->pp_cfg.num_boxes       = ctx->engine->num_boxes();
        ctx->pp_cfg.model_w         = ctx->model_w;
        ctx->pp_cfg.model_h         = ctx->model_h;
        ctx->pp_cfg.frame_w         = (int)ctx->pending_result.frame_w;
        ctx->pp_cfg.frame_h         = (int)ctx->pending_result.frame_h;

        // Detect YOLO v10 end-to-end format (num_classes==1 && num_boxes==300)
        // TODO: expose as a setting if needed
        if (ctx->engine->num_classes() == 0)
            ctx->pp_cfg.format = YoloOutputFormat::kV10_E2E;
        else
            ctx->pp_cfg.format = YoloOutputFormat::kV8_CHW;

        uint32_t num_dets = yolo_postprocess(
            ctx->h_output,
            ctx->pp_cfg,
            ctx->dets);

        uint64_t pp_end = now_ns();
        uint64_t pp_ns  = pp_end - pp_start;

        // Update EWMA stats
        ewma(ctx->avg_preprocess_ns,  ctx->pending_result.preprocess_ns);
        ewma(ctx->avg_inference_ns,   ctx->pending_result.infer_ns);
        ewma(ctx->avg_postprocess_ns, pp_ns);
        ewma(ctx->avg_total_ns,
             pp_end - ctx->pending_result.capture_ts);

        // Publish to shared memory
        YoloShmHeader payload{};
        payload.frame_index          = ++ctx->frame_index;
        payload.capture_timestamp_ns = ctx->pending_result.capture_ts;
        payload.inference_end_ns     = ctx->pending_result.infer_end_ts;
        payload.frame_width          = ctx->pending_result.frame_w;
        payload.frame_height         = ctx->pending_result.frame_h;
        payload.input_scale_x        = (float)ctx->pending_result.frame_w / ctx->model_w;
        payload.input_scale_y        = (float)ctx->pending_result.frame_h / ctx->model_h;
        payload.nms_threshold        = ctx->nms_iou;
        payload.score_threshold      = ctx->score_threshold;
        payload.model_input_width    = ctx->model_w;
        payload.model_input_height   = ctx->model_h;
        payload.avg_preprocess_ns    = ctx->avg_preprocess_ns;
        payload.avg_inference_ns     = ctx->avg_inference_ns;
        payload.avg_postprocess_ns   = ctx->avg_postprocess_ns;
        payload.avg_total_ns         = ctx->avg_total_ns;
        payload.frames_dropped       = ctx->frames_dropped;

        ctx->shm.write(payload, ctx->dets, num_dets);

        ctx->pending_result.valid = false;

        // Release the inference token so the next frame can proceed
        ctx->infer_busy.store(false, std::memory_order_release);
    }
}

// ── Create ────────────────────────────────────────────────────────────────────

static const char* yolo_get_name(void*) { return "YOLO TensorRT Detection"; }

static void* yolo_create(obs_data_t* settings, obs_source_t* source)
{
    auto* ctx = new YoloFilterContext();
    ctx->source = source;

    // CUDA streams
    cudaStreamCreateWithPriority(&ctx->infer_stream,
        cudaStreamNonBlocking, -1);   // highest available priority
    cudaStreamCreateWithPriority(&ctx->upload_stream,
        cudaStreamNonBlocking, 0);

    // Shared memory
    ctx->shm.open();
    ctx->shm.write_classnames(COCO_NAMES, 80);

    // Start postproc thread
    ctx->postproc_thread = std::thread(postproc_thread_fn, ctx);

    yolo_update(ctx, settings);

    return ctx;
}

// ── Destroy ───────────────────────────────────────────────────────────────────

static void yolo_destroy(void* data)
{
    auto* ctx = static_cast<YoloFilterContext*>(data);

    // Signal threads to stop
    ctx->shutdown.store(true);
    ctx->wake_flag.store(true);  // wake postproc thread

    if (ctx->postproc_thread.joinable()) ctx->postproc_thread.join();
    if (ctx->engine_load_thread.joinable()) ctx->engine_load_thread.join();

    // Sync + destroy CUDA resources
    if (ctx->infer_stream)  cudaStreamSynchronize(ctx->infer_stream);
    if (ctx->upload_stream) cudaStreamSynchronize(ctx->upload_stream);

    if (ctx->d_rgba)   cudaFree(ctx->d_rgba);
    if (ctx->d_input)  cudaFree(ctx->d_input);
    if (ctx->d_output) cudaFree(ctx->d_output);
    if (ctx->h_output) cudaFreeHost(ctx->h_output);

    if (ctx->infer_stream)  cudaStreamDestroy(ctx->infer_stream);
    if (ctx->upload_stream) cudaStreamDestroy(ctx->upload_stream);

    ctx->shm.close();

    delete ctx;
}

// ── Update (settings changed) ─────────────────────────────────────────────────

static void yolo_update(void* data, obs_data_t* settings)
{
    auto* ctx = static_cast<YoloFilterContext*>(data);

    ctx->engine_path      = obs_data_get_string(settings, "engine_path");
    ctx->onnx_path        = obs_data_get_string(settings, "onnx_path");
    ctx->score_threshold  = (float)obs_data_get_double(settings, "score_threshold");
    ctx->nms_iou          = (float)obs_data_get_double(settings, "nms_iou");
    ctx->fp16             = obs_data_get_bool(settings, "fp16");
    ctx->model_w          = (int)obs_data_get_int(settings, "model_w");
    ctx->model_h          = (int)obs_data_get_int(settings, "model_h");
    ctx->visualise_obs    = obs_data_get_bool(settings, "visualise_obs");

    // (Re)load the engine on a background thread to avoid blocking OBS UI
    ctx->engine_ready = false;
    if (ctx->engine_load_thread.joinable()) ctx->engine_load_thread.join();

    ctx->engine_load_thread = std::thread([ctx]() {
        ctx->engine = std::make_unique<TrtEngine>();

        TrtBuildOptions opts;
        opts.fp16          = ctx->fp16;
        opts.max_batch     = 1;
        opts.opt_batch     = 1;
        opts.input_w       = ctx->model_w;
        opts.input_h       = ctx->model_h;
        opts.workspace_mb  = 1024;

        if (!ctx->engine->load_or_build(ctx->engine_path, ctx->onnx_path, opts)) {
            blog(LOG_ERROR, "[yolo-filter] Failed to load/build TRT engine");
            return;
        }

        // Allocate / reallocate GPU buffers for the actual I/O sizes
        size_t new_in  = ctx->engine->input_bytes();
        size_t new_out = ctx->engine->output_bytes();

        if (ctx->d_input)  { cudaFree(ctx->d_input);    ctx->d_input  = nullptr; }
        if (ctx->d_output) { cudaFree(ctx->d_output);   ctx->d_output = nullptr; }
        if (ctx->h_output) { cudaFreeHost(ctx->h_output); ctx->h_output = nullptr; }

        cudaMalloc(&ctx->d_input,  new_in);
        cudaMalloc(&ctx->d_output, new_out);
        cudaMallocHost(&ctx->h_output, new_out);

        ctx->input_bytes  = new_in;
        ctx->output_bytes = new_out;

        // Update class names in SHM from model (if >80 classes)
        // For standard COCO we already wrote them in yolo_create

        ctx->model_w = (int)ctx->engine->input_width();
        ctx->model_h = (int)ctx->engine->input_height();

        blog(LOG_INFO, "[yolo-filter] Engine ready – input %dx%d, classes=%d, boxes=%d",
             ctx->model_w, ctx->model_h,
             ctx->engine->num_classes(), ctx->engine->num_boxes());

        ctx->engine_ready = true;
    });
}

// ── Render (called every frame on OBS render thread) ─────────────────────────

static void yolo_render(void* data, gs_effect_t* /*effect*/)
{
    auto* ctx = static_cast<YoloFilterContext*>(data);

    // Always pass through – never block OBS
    obs_source_skip_video_filter(ctx->source);

    if (!ctx->engine_ready) return;

    // Try to acquire the inference token
    bool expected = false;
    if (!ctx->infer_busy.compare_exchange_strong(
            expected, true,
            std::memory_order_acq_rel, std::memory_order_relaxed)) {
        // Inference still running from previous frame – drop this frame
        ctx->frames_dropped++;
        return;
    }

    // ── Capture frame ────────────────────────────────────────────────────

    uint64_t capture_ts = now_ns();

    obs_source_t* target = obs_filter_get_target(ctx->source);
    if (!target) { ctx->infer_busy.store(false); return; }

    uint32_t fw = obs_source_get_base_width(target);
    uint32_t fh = obs_source_get_base_height(target);
    if (!fw || !fh) { ctx->infer_busy.store(false); return; }

    // Resize RGBA device buffer if needed
    size_t need = (size_t)fw * fh * 4;
    if (need > ctx->rgba_bytes) {
        if (ctx->d_rgba) cudaFree(ctx->d_rgba);
        cudaMalloc(&ctx->d_rgba, need);
        ctx->rgba_bytes = need;
    }

    // Obtain the current rendered texture from OBS and copy to CUDA device
    gs_texture_t* tex = obs_source_get_texrender(target)
                            ? gs_texrender_get_texture(obs_source_get_texrender(target))
                            : nullptr;

    bool tex_ok = false;
    if (tex) {
        tex_ok = gpu_texture_to_cuda(tex, ctx->d_rgba, ctx->rgba_bytes,
                                     ctx->upload_stream);
    }

    if (!tex_ok) {
        ctx->infer_busy.store(false);
        return;
    }

    // Wait for upload to complete before preprocessing
    // (upload_stream feeds infer_stream via event)
    cudaStreamSynchronize(ctx->upload_stream);

    // ── Preprocess ───────────────────────────────────────────────────────

    uint64_t pre_start = now_ns();

    gpu_preprocess_letterbox(
        ctx->d_rgba,
        (int)fw, (int)fh, (int)(fw * 4),
        ctx->d_input,
        ctx->model_w, ctx->model_h,
        0.5f,
        true,   // BGRA input (OBS default)
        ctx->infer_stream);

    uint64_t pre_end = now_ns();

    // ── Async inference ──────────────────────────────────────────────────

    uint64_t infer_start = now_ns();

    ctx->engine->infer_async(
        ctx->d_input,  ctx->input_bytes,
        ctx->d_output, ctx->output_bytes,
        ctx->infer_stream);

    // Async D2H copy of output (still on infer_stream, after inference)
    cudaMemcpyAsync(ctx->h_output, ctx->d_output, ctx->output_bytes,
                    cudaMemcpyDeviceToHost, ctx->infer_stream);

    // ── Register completion callback ─────────────────────────────────────

    auto* cbd       = new StreamCallbackData{};
    cbd->ctx        = ctx;
    cbd->capture_ts = capture_ts;
    cbd->infer_start_ts = infer_start;
    cbd->frame_w    = fw;
    cbd->frame_h    = fh;
    cbd->preprocess_ns = pre_end - pre_start;

    cudaLaunchHostFunc(ctx->infer_stream, cuda_infer_complete, cbd);
}

// ── Properties ────────────────────────────────────────────────────────────────

static obs_properties_t* yolo_properties(void* /*data*/)
{
    obs_properties_t* props = obs_properties_create();

    obs_properties_add_path(props, "engine_path", "TensorRT .engine file",
        OBS_PATH_FILE, "TensorRT Engine (*.engine)", nullptr);

    obs_properties_add_path(props, "onnx_path",
        "ONNX model (auto-convert if no .engine)",
        OBS_PATH_FILE, "ONNX Model (*.onnx)", nullptr);

    obs_properties_add_float_slider(props, "score_threshold",
        "Score threshold", 0.01, 1.0, 0.01);

    obs_properties_add_float_slider(props, "nms_iou",
        "NMS IoU threshold", 0.1, 1.0, 0.01);

    obs_properties_add_bool(props, "fp16", "FP16 mode (requires Turing+)");

    obs_properties_add_int(props, "model_w", "Model input width",  128, 2048, 32);
    obs_properties_add_int(props, "model_h", "Model input height", 128, 2048, 32);

    obs_properties_add_bool(props, "visualise_obs",
        "Draw bounding boxes in OBS preview");

    return props;
}

static void yolo_get_defaults(obs_data_t* settings)
{
    obs_data_set_default_string(settings, "engine_path", "");
    obs_data_set_default_string(settings, "onnx_path",   "");
    obs_data_set_default_double(settings, "score_threshold", 0.25);
    obs_data_set_default_double(settings, "nms_iou",         0.45);
    obs_data_set_default_bool  (settings, "fp16",            true);
    obs_data_set_default_int   (settings, "model_w",         640);
    obs_data_set_default_int   (settings, "model_h",         640);
    obs_data_set_default_bool  (settings, "visualise_obs",   false);
}

// ── Registration ─────────────────────────────────────────────────────────────

void register_yolo_filter(void)
{
    struct obs_source_info info = {};
    info.id             = "yolo_trt_filter";
    info.type           = OBS_SOURCE_TYPE_FILTER;
    info.output_flags   = OBS_SOURCE_VIDEO;
    info.get_name       = yolo_get_name;
    info.create         = yolo_create;
    info.destroy        = yolo_destroy;
    info.update         = yolo_update;
    info.video_render   = yolo_render;
    info.get_properties = yolo_properties;
    info.get_defaults   = yolo_get_defaults;

    obs_register_source(&info);
}

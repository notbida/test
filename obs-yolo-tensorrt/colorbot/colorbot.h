/**
 * colorbot.h  –  CVM-colorBot: detection consumer + mouse control
 *
 * Consumes detections from shared memory, selects a target according to
 * configurable priority rules, and moves the mouse toward it.
 *
 * Target selection strategy:
 *   1. Filter by allowed class IDs
 *   2. Sort candidates by (distance_to_crosshair, confidence)
 *   3. Pick nearest
 *
 * Mouse control:
 *   - PID controller driving Win32 mouse_event / uinput deltas
 *   - Smoothing: exponential filter on target centroid
 *   - "FOV" circle: only targets within a radius of screen centre are
 *     considered (prevents edge snapping)
 */

#pragma once

#include "shm_reader.h"

#include <atomic>
#include <cstdint>
#include <set>
#include <string>
#include <thread>
#include <vector>

// ── Mouse control backend ─────────────────────────────────────────────────────

enum class MouseBackend {
    kNone,          // dry-run / visualization only
    kWin32,         // Windows: mouse_event() or SendInput()
    kUinput,        // Linux:   /dev/uinput relative mouse
};

// ── Per-frame targeting result ────────────────────────────────────────────────

struct TargetInfo {
    bool      valid       = false;
    int       det_index   = -1;    // index into snapshot detections[]
    float     cx          = 0;    // centroid x (screen coords)
    float     cy          = 0;    // centroid y
    float     dist        = 1e9f; // distance from aim center
    float     confidence  = 0;
    uint16_t  class_id    = 0;
};

// ── Bot configuration ─────────────────────────────────────────────────────────

struct BotConfig {
    // Aiming
    float     aim_fov_px    = 200.f;  // radius in pixels around aim point
    float     aim_center_x  = 0.f;    // screen x of aim (0 = screen center)
    float     aim_center_y  = 0.f;    // screen y of aim (0 = screen center)

    // Target body-part offset (0.0 = box centre, -0.3 = 30% above centre → head)
    float     aim_offset_y  = -0.15f;

    // PID controller gains
    float     pid_kp = 0.55f;
    float     pid_ki = 0.01f;
    float     pid_kd = 0.10f;

    // Smoothing (EMA α on target centroid)
    float     smooth_alpha = 0.6f;

    // Speed cap (max pixels moved per update)
    float     max_speed_px = 40.f;

    // Score gate
    float     min_confidence = 0.30f;

    // Which classes to aim at (empty = all classes)
    std::set<uint16_t> target_classes;

    // Platform
    MouseBackend mouse_backend = MouseBackend::kNone;

    // Polling interval (microseconds)
    uint32_t  poll_us = 4000;  // ~250 Hz
};

// ── Visualisation config ─────────────────────────────────────────────────────

struct VizConfig {
    bool show_boxes        = true;
    bool show_labels       = true;
    bool show_confidence   = true;
    bool show_latency      = true;
    bool show_fov_circle   = true;
    bool highlight_target  = true;
    int  window_w          = 960;
    int  window_h          = 540;
};

// ── Main bot class ────────────────────────────────────────────────────────────

class ColorBot
{
public:
    explicit ColorBot(BotConfig cfg = {}, VizConfig viz = {});
    ~ColorBot();

    ColorBot(const ColorBot&) = delete;
    ColorBot& operator=(const ColorBot&) = delete;

    /** Connect to the OBS plugin SHM. Retries every 500ms until success. */
    bool connect(const std::string& shm_name = YOLO_SHM_NAME,
                 uint32_t           timeout_ms = 10000);

    /** Start the control + visualisation loops. */
    void run();

    /** Request graceful shutdown. */
    void stop();

    bool is_running() const { return running_.load(); }

    // Runtime config updates (thread-safe via atomics/copies)
    void set_bot_config(const BotConfig& cfg);
    void set_enabled(bool e) { enabled_.store(e); }

private:
    // ── Threads ──────────────────────────────────────────────────────────
    void control_loop();
    void viz_loop();

    // ── Control logic ─────────────────────────────────────────────────────
    TargetInfo select_target(const DetectionSnapshot& snap);
    void       move_mouse(float dx, float dy);

    // ── Visualization ─────────────────────────────────────────────────────
    void render_frame(const DetectionSnapshot& snap,
                      const TargetInfo&        target);
    // Returns per-class color for bounding boxes
    static uint32_t class_color(uint16_t class_id);

    // ── Members ───────────────────────────────────────────────────────────
    ShmReader  shm_;
    BotConfig  cfg_;
    VizConfig  viz_cfg_;

    std::atomic<bool> running_{false};
    std::atomic<bool> enabled_{true};

    std::thread control_thread_;
    std::thread viz_thread_;

    // Smoothed target position
    float smooth_cx_ = 0.f;
    float smooth_cy_ = 0.f;
    bool  has_smooth_ = false;

    // PID state
    float pid_err_x_  = 0.f;
    float pid_err_y_  = 0.f;
    float pid_integ_x_= 0.f;
    float pid_integ_y_= 0.f;

    // Frame tracking
    uint64_t last_frame_index_ = 0;

    // Mouse backend handle
#ifdef _WIN32
    // nothing needed for SendInput
#else
    int uinput_fd_ = -1;
#endif
};

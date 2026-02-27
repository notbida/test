/**
 * colorbot.cpp  –  ColorBot implementation
 *
 * Visualisation uses OpenCV (cv::imshow) for the debug window.
 * Mouse control uses Win32 SendInput on Windows, uinput on Linux.
 */

#include "colorbot.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <chrono>

#include <opencv2/opencv.hpp>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <unistd.h>
#  include <linux/input.h>
#  include <linux/uinput.h>
#  include <sys/ioctl.h>
#endif

// ── Constructor / Destructor ─────────────────────────────────────────────────

ColorBot::ColorBot(BotConfig cfg, VizConfig viz)
    : cfg_(std::move(cfg)), viz_cfg_(std::move(viz))
{
    // If aim_center is zero, we'll set it to screen centre at first frame
}

ColorBot::~ColorBot()
{
    stop();
#ifndef _WIN32
    if (uinput_fd_ >= 0) ::close(uinput_fd_);
#endif
}

// ── connect ───────────────────────────────────────────────────────────────────

bool ColorBot::connect(const std::string& name, uint32_t timeout_ms)
{
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds(timeout_ms);

    while (std::chrono::steady_clock::now() < deadline) {
        if (shm_.open(name)) {
            printf("[colorbot] Connected to SHM: %s\n", name.c_str());
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    fprintf(stderr, "[colorbot] Timed out waiting for OBS plugin SHM\n");
    return false;
}

// ── run / stop ────────────────────────────────────────────────────────────────

void ColorBot::run()
{
    running_.store(true);

#ifndef _WIN32
    // Set up uinput if requested
    if (cfg_.mouse_backend == MouseBackend::kUinput) {
        uinput_fd_ = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
        if (uinput_fd_ < 0) {
            perror("[colorbot] /dev/uinput open failed");
        } else {
            ioctl(uinput_fd_, UI_SET_EVBIT,  EV_REL);
            ioctl(uinput_fd_, UI_SET_RELBIT, REL_X);
            ioctl(uinput_fd_, UI_SET_RELBIT, REL_Y);

            struct uinput_setup usetup{};
            usetup.id.bustype = BUS_USB;
            usetup.id.vendor  = 0x1234;
            usetup.id.product = 0x5678;
            std::strncpy(usetup.name, "colorbot-mouse", UINPUT_MAX_NAME_SIZE);
            ioctl(uinput_fd_, UI_DEV_SETUP, &usetup);
            ioctl(uinput_fd_, UI_DEV_CREATE);
        }
    }
#endif

    control_thread_ = std::thread(&ColorBot::control_loop, this);
    viz_thread_     = std::thread(&ColorBot::viz_loop,     this);
}

void ColorBot::stop()
{
    running_.store(false);
    if (control_thread_.joinable()) control_thread_.join();
    if (viz_thread_.joinable())     viz_thread_.join();
}

// ── control_loop ─────────────────────────────────────────────────────────────

void ColorBot::control_loop()
{
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
#endif

    DetectionSnapshot snap;

    while (running_.load()) {
        auto t0 = std::chrono::steady_clock::now();

        bool got_new = shm_.wait_new_frame(snap, last_frame_index_,
                                            cfg_.poll_us);
        if (!got_new || !snap.valid) {
            std::this_thread::sleep_for(
                std::chrono::microseconds(cfg_.poll_us / 2));
            continue;
        }

        last_frame_index_ = snap.header.frame_index;

        if (!enabled_.load()) continue;

        // Init aim centre to screen centre on first frame
        if (cfg_.aim_center_x == 0.f && cfg_.aim_center_y == 0.f) {
            cfg_.aim_center_x = snap.header.frame_width  * 0.5f;
            cfg_.aim_center_y = snap.header.frame_height * 0.5f;
        }

        TargetInfo tgt = select_target(snap);
        if (!tgt.valid) {
            has_smooth_   = false;
            pid_err_x_    = 0.f;
            pid_err_y_    = 0.f;
            pid_integ_x_  = 0.f;
            pid_integ_y_  = 0.f;
            continue;
        }

        // Target aim point (offset toward head/chest)
        float box_h = snap.detections[tgt.det_index].y2
                    - snap.detections[tgt.det_index].y1;
        float aim_x = tgt.cx;
        float aim_y = tgt.cy + cfg_.aim_offset_y * box_h;

        // EMA smoothing
        if (!has_smooth_) {
            smooth_cx_ = aim_x;
            smooth_cy_ = aim_y;
            has_smooth_ = true;
        } else {
            smooth_cx_ = cfg_.smooth_alpha * aim_x
                       + (1.f - cfg_.smooth_alpha) * smooth_cx_;
            smooth_cy_ = cfg_.smooth_alpha * aim_y
                       + (1.f - cfg_.smooth_alpha) * smooth_cy_;
        }

        // PID
        float err_x = smooth_cx_ - cfg_.aim_center_x;
        float err_y = smooth_cy_ - cfg_.aim_center_y;

        pid_integ_x_ += err_x;
        pid_integ_y_ += err_y;

        float dx = cfg_.pid_kp * err_x
                 + cfg_.pid_ki * pid_integ_x_
                 + cfg_.pid_kd * (err_x - pid_err_x_);
        float dy = cfg_.pid_kp * err_y
                 + cfg_.pid_ki * pid_integ_y_
                 + cfg_.pid_kd * (err_y - pid_err_y_);

        pid_err_x_ = err_x;
        pid_err_y_ = err_y;

        // Speed cap
        float spd = std::sqrt(dx*dx + dy*dy);
        if (spd > cfg_.max_speed_px) {
            dx *= cfg_.max_speed_px / spd;
            dy *= cfg_.max_speed_px / spd;
        }

        move_mouse(dx, dy);

        // Sleep remainder of poll interval
        auto elapsed = std::chrono::steady_clock::now() - t0;
        auto remaining = std::chrono::microseconds(cfg_.poll_us) - elapsed;
        if (remaining.count() > 0)
            std::this_thread::sleep_for(remaining);
    }
}

// ── select_target ─────────────────────────────────────────────────────────────

TargetInfo ColorBot::select_target(const DetectionSnapshot& snap)
{
    TargetInfo best;
    float best_dist = cfg_.aim_fov_px;  // only consider within FOV

    for (uint32_t i = 0; i < snap.header.num_detections; ++i) {
        const YoloDetection& d = snap.detections[i];

        if (d.confidence < cfg_.min_confidence) continue;

        // Class filter
        if (!cfg_.target_classes.empty() &&
            cfg_.target_classes.find(d.class_id) == cfg_.target_classes.end())
            continue;

        float cx = (d.x1 + d.x2) * 0.5f;
        float cy = (d.y1 + d.y2) * 0.5f;

        float dx   = cx - cfg_.aim_center_x;
        float dy   = cy - cfg_.aim_center_y;
        float dist = std::sqrt(dx*dx + dy*dy);

        if (dist < best_dist) {
            best_dist       = dist;
            best.valid      = true;
            best.det_index  = (int)i;
            best.cx         = cx;
            best.cy         = cy;
            best.dist       = dist;
            best.confidence = d.confidence;
            best.class_id   = d.class_id;
        }
    }
    return best;
}

// ── move_mouse ────────────────────────────────────────────────────────────────

void ColorBot::move_mouse(float dx, float dy)
{
    int idx = (int)std::round(dx);
    int idy = (int)std::round(dy);
    if (idx == 0 && idy == 0) return;

    switch (cfg_.mouse_backend) {
#ifdef _WIN32
    case MouseBackend::kWin32: {
        INPUT inp{};
        inp.type           = INPUT_MOUSE;
        inp.mi.dwFlags     = MOUSEEVENTF_MOVE;
        inp.mi.dx          = (LONG)idx;
        inp.mi.dy          = (LONG)idy;
        SendInput(1, &inp, sizeof(INPUT));
        break;
    }
#else
    case MouseBackend::kUinput: {
        if (uinput_fd_ < 0) break;
        auto emit = [&](int type, int code, int val) {
            struct input_event ev{};
            ev.type  = (__u16)type;
            ev.code  = (__u16)code;
            ev.value = val;
            write(uinput_fd_, &ev, sizeof(ev));
        };
        emit(EV_REL, REL_X, idx);
        emit(EV_REL, REL_Y, idy);
        emit(EV_SYN, SYN_REPORT, 0);
        break;
    }
#endif
    default: break;  // kNone – dry run
    }
}

// ── viz_loop ─────────────────────────────────────────────────────────────────

void ColorBot::viz_loop()
{
    DetectionSnapshot snap;
    TargetInfo        tgt;
    uint64_t          last_viz_index = 0;

    cv::namedWindow("colorbot-detect", cv::WINDOW_NORMAL);
    cv::resizeWindow("colorbot-detect", viz_cfg_.window_w, viz_cfg_.window_h);

    while (running_.load()) {
        if (!shm_.poll(snap) || !snap.valid ||
            snap.header.frame_index == last_viz_index) {
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
            cv::waitKey(1);
            continue;
        }

        last_viz_index = snap.header.frame_index;
        tgt = select_target(snap);
        render_frame(snap, tgt);

        if (cv::waitKey(1) == 27) { // ESC
            running_.store(false);
            break;
        }
    }

    cv::destroyWindow("colorbot-detect");
}

// ── render_frame ──────────────────────────────────────────────────────────────

void ColorBot::render_frame(const DetectionSnapshot& snap,
                              const TargetInfo&        tgt)
{
    // Scale factor: snap frame → display window
    float sw = (float)viz_cfg_.window_w / snap.header.frame_width;
    float sh = (float)viz_cfg_.window_h / snap.header.frame_height;

    cv::Mat canvas(viz_cfg_.window_h, viz_cfg_.window_w, CV_8UC3,
                   cv::Scalar(15, 15, 15));

    // Draw FOV circle
    if (viz_cfg_.show_fov_circle) {
        cv::Point centre((int)(cfg_.aim_center_x * sw),
                         (int)(cfg_.aim_center_y * sh));
        cv::circle(canvas, centre, (int)(cfg_.aim_fov_px * sw),
                   cv::Scalar(60, 60, 200), 1, cv::LINE_AA);
        // Crosshair
        cv::line(canvas,
            {centre.x - 10, centre.y}, {centre.x + 10, centre.y},
            {200, 200, 200}, 1);
        cv::line(canvas,
            {centre.x, centre.y - 10}, {centre.x, centre.y + 10},
            {200, 200, 200}, 1);
    }

    // Draw all detections
    for (uint32_t i = 0; i < snap.header.num_detections; ++i) {
        const YoloDetection& d = snap.detections[i];

        cv::Rect r(
            (int)(d.x1 * sw), (int)(d.y1 * sh),
            (int)((d.x2 - d.x1) * sw), (int)((d.y2 - d.y1) * sh));

        bool is_target = viz_cfg_.highlight_target && tgt.valid &&
                         (int)i == tgt.det_index;

        uint32_t col = class_color(d.class_id);
        cv::Scalar color(col & 0xFF, (col >> 8) & 0xFF, (col >> 16) & 0xFF);

        int thick = is_target ? 2 : 1;
        cv::Scalar outline = is_target
            ? cv::Scalar(0, 255, 128)   // bright green for target
            : color;

        cv::rectangle(canvas, r, outline, thick, cv::LINE_AA);

        if (viz_cfg_.show_labels) {
            const char* name = snap.classnames[d.class_id];
            char label[64];
            if (viz_cfg_.show_confidence)
                snprintf(label, sizeof(label), "%s %.0f%%",
                         name[0] ? name : "?",
                         d.confidence * 100.f);
            else
                snprintf(label, sizeof(label), "%s", name[0] ? name : "?");

            cv::putText(canvas, label,
                {r.x + 2, r.y - 4},
                cv::FONT_HERSHEY_SIMPLEX, 0.45,
                outline, 1, cv::LINE_AA);
        }

        // Draw target aim point indicator
        if (is_target) {
            int ax = (int)(tgt.cx * sw);
            int ay = (int)((tgt.cy + cfg_.aim_offset_y *
                           (d.y2 - d.y1)) * sh);
            cv::drawMarker(canvas, {ax, ay},
                cv::Scalar(0, 255, 128),
                cv::MARKER_CROSS, 14, 2, cv::LINE_AA);
        }
    }

    // Latency HUD
    if (viz_cfg_.show_latency && snap.header.avg_total_ns > 0) {
        char hud[256];
        snprintf(hud, sizeof(hud),
            "Frame #%llu  Dets: %u  "
            "Pre: %.1f ms  Inf: %.1f ms  Post: %.1f ms  Total: %.1f ms  "
            "Dropped: %u",
            (unsigned long long)snap.header.frame_index,
            snap.header.num_detections,
            snap.header.avg_preprocess_ns  / 1e6,
            snap.header.avg_inference_ns   / 1e6,
            snap.header.avg_postprocess_ns / 1e6,
            snap.header.avg_total_ns       / 1e6,
            snap.header.frames_dropped);

        cv::putText(canvas, hud,
            {8, 20},
            cv::FONT_HERSHEY_SIMPLEX, 0.40,
            cv::Scalar(200, 200, 200), 1, cv::LINE_AA);
    }

    cv::imshow("colorbot-detect", canvas);
}

// ── class_color ───────────────────────────────────────────────────────────────

uint32_t ColorBot::class_color(uint16_t id)
{
    // Hash the class_id to a deterministic RGB color
    uint32_t h = (uint32_t)id * 2654435761u;
    uint8_t r = 80 + (h & 0x7F);
    uint8_t g = 80 + ((h >> 8) & 0x7F);
    uint8_t b = 80 + ((h >> 16) & 0x7F);
    return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16);
}

// ── set_bot_config ────────────────────────────────────────────────────────────

void ColorBot::set_bot_config(const BotConfig& cfg)
{
    cfg_ = cfg;
    // Reset PID state on config change
    pid_err_x_ = pid_err_y_ = 0.f;
    pid_integ_x_ = pid_integ_y_ = 0.f;
    has_smooth_ = false;
}

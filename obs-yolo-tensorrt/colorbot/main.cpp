/**
 * main.cpp  –  CVM-colorBot entry point
 *
 * Usage:
 *   colorbot [options]
 *
 * Options:
 *   --fov <px>         FOV radius in pixels (default: 200)
 *   --score <f>        Minimum confidence threshold (default: 0.30)
 *   --smooth <f>       EMA smoothing alpha (default: 0.6)
 *   --pid-p <f>        PID proportional gain (default: 0.55)
 *   --pid-i <f>        PID integral gain    (default: 0.01)
 *   --pid-d <f>        PID derivative gain  (default: 0.10)
 *   --backend <name>   Mouse backend: none|win32|uinput (default: none)
 *   --class <id>       Add class ID to target list (repeatable)
 *   --no-viz           Disable visualisation window
 *   --shm <name>       SHM name (default: YoloDetectSHM_v1)
 *   --offset-y <f>     Vertical aim offset fraction (default: -0.15)
 */

#include "colorbot.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>

static ColorBot* g_bot = nullptr;

static void sig_handler(int)
{
    if (g_bot) g_bot->stop();
}

int main(int argc, char** argv)
{
    BotConfig bot{};
    VizConfig viz{};
    std::string shm_name = YOLO_SHM_NAME;
    bool do_viz = true;

    // ── Argument parsing ───────────────────────────────────────────────────
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--fov") && i+1 < argc)
            bot.aim_fov_px = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--score") && i+1 < argc)
            bot.min_confidence = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--smooth") && i+1 < argc)
            bot.smooth_alpha = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--pid-p") && i+1 < argc)
            bot.pid_kp = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--pid-i") && i+1 < argc)
            bot.pid_ki = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--pid-d") && i+1 < argc)
            bot.pid_kd = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--offset-y") && i+1 < argc)
            bot.aim_offset_y = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--class") && i+1 < argc)
            bot.target_classes.insert((uint16_t)atoi(argv[++i]));
        else if (!strcmp(argv[i], "--no-viz"))
            do_viz = false;
        else if (!strcmp(argv[i], "--shm") && i+1 < argc)
            shm_name = argv[++i];
        else if (!strcmp(argv[i], "--backend") && i+1 < argc) {
            const char* b = argv[++i];
#ifdef _WIN32
            if (!strcmp(b, "win32")) bot.mouse_backend = MouseBackend::kWin32;
#else
            if (!strcmp(b, "uinput")) bot.mouse_backend = MouseBackend::kUinput;
#endif
        }
        else if (!strcmp(argv[i], "--help")) {
            printf("colorbot – YOLO detection consumer\n"
                   "Options: --fov, --score, --smooth, --pid-p/i/d, "
                   "--backend, --class, --no-viz, --shm, --offset-y\n");
            return 0;
        }
    }

    if (!do_viz) {
        viz.show_boxes = viz.show_labels = viz.show_latency = false;
    }

    // ── Create and connect ─────────────────────────────────────────────────
    ColorBot bot_inst(bot, viz);
    g_bot = &bot_inst;

    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    printf("[colorbot] Connecting to SHM '%s'...\n", shm_name.c_str());
    if (!bot_inst.connect(shm_name, 30000)) {
        fprintf(stderr, "[colorbot] Could not connect. Is OBS running?\n");
        return 1;
    }

    printf("[colorbot] Running. Press ESC in the visualisation window or "
           "Ctrl+C to quit.\n");

    bot_inst.run();

    // run() blocks internally on the viz window; control loop is on a thread
    // So we need to wait for them
    while (bot_inst.is_running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    printf("[colorbot] Exiting.\n");
    return 0;
}

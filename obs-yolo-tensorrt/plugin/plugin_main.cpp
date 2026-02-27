/**
 * plugin_main.cpp  –  OBS plugin entry point
 *
 * Implements the required obs_module_load / obs_module_unload hooks.
 */

#include "yolo_filter.h"
#include <obs/obs-module.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-yolo-tensorrt", "en-US")

bool obs_module_load(void)
{
    blog(LOG_INFO, "[obs-yolo-tensorrt] Loading plugin v1.0.0");
    register_yolo_filter();
    blog(LOG_INFO, "[obs-yolo-tensorrt] Filter registered: 'YOLO TensorRT Detection'");
    return true;
}

void obs_module_unload(void)
{
    blog(LOG_INFO, "[obs-yolo-tensorrt] Unloading");
}

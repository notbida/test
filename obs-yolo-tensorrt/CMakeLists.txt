cmake_minimum_required(VERSION 3.22)

project(obs-yolo-tensorrt
    VERSION 1.0.0
    DESCRIPTION "Ultra-low-latency YOLO TensorRT OBS plugin"
    LANGUAGES CXX CUDA C)

# ── C++ / CUDA standards ──────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CUDA_STANDARD 17)
set(CMAKE_CUDA_STANDARD_REQUIRED ON)

# ── Build type ────────────────────────────────────────────────────────────────
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# ── Platform detection ────────────────────────────────────────────────────────
if(WIN32)
  add_compile_definitions(_WIN32_WINNT=0x0A00)   # Windows 10
endif()

# ── Find packages ─────────────────────────────────────────────────────────────

# CUDA
find_package(CUDAToolkit REQUIRED)

# TensorRT – look in standard install paths
set(TRT_HINTS
    /usr/local/tensorrt
    /opt/TensorRT
    "C:/Program Files/NVIDIA GPU Computing Toolkit/TensorRT"
    $ENV{TRT_ROOT}
)

find_path(TRT_INCLUDE_DIR
    NAMES NvInfer.h
    HINTS ${TRT_HINTS}
    PATH_SUFFIXES include
    REQUIRED)

find_library(TRT_LIB_INFER
    NAMES nvinfer
    HINTS ${TRT_HINTS}
    PATH_SUFFIXES lib lib64
    REQUIRED)

find_library(TRT_LIB_ONNX
    NAMES nvonnxparser
    HINTS ${TRT_HINTS}
    PATH_SUFFIXES lib lib64
    REQUIRED)

# OBS Studio
find_package(libobs QUIET)
if(NOT libobs_FOUND)
  # Fallback: manual search
  find_path(OBS_INCLUDE_DIR
      NAMES obs/obs-module.h
      HINTS
          /usr/include
          /usr/local/include
          "C:/Program Files/obs-studio/include"
          $ENV{OBS_ROOT}/include
      REQUIRED)

  find_library(OBS_LIB
      NAMES obs
      HINTS
          /usr/lib /usr/local/lib /usr/lib/x86_64-linux-gnu
          "C:/Program Files/obs-studio/bin/64bit"
          $ENV{OBS_ROOT}/lib
      REQUIRED)

  add_library(libobs INTERFACE)
  target_include_directories(libobs INTERFACE ${OBS_INCLUDE_DIR})
  target_link_libraries(libobs INTERFACE ${OBS_LIB})
endif()

# OpenCV (for colorBot visualisation)
find_package(OpenCV REQUIRED COMPONENTS core imgproc highgui videoio)

# ── CUDA architectures (Turing=75, Ampere=86, Ada=89) ─────────────────────────
if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
  set(CMAKE_CUDA_ARCHITECTURES "75;86;89")
endif()

# ── Compiler flags ────────────────────────────────────────────────────────────
if(MSVC)
  add_compile_options(/W3 /O2 /arch:AVX2 /fp:fast)
else()
  add_compile_options(-Wall -Wextra -O3 -march=native -ffast-math)
endif()

# ── ─────────────────────────────────────────────────────────────────────────
#   1.  OBS Plugin  (shared library)
# ──────────────────────────────────────────────────────────────────────────────

add_library(obs-yolo-tensorrt MODULE
    plugin/plugin_main.cpp
    plugin/yolo_filter.cpp
    plugin/trt_engine.cpp
    plugin/shm_writer.cpp
    plugin/gpu_preprocess.cu
)

set_target_properties(obs-yolo-tensorrt PROPERTIES
    PREFIX ""           # no "lib" prefix on Linux
    OUTPUT_NAME "obs-yolo-tensorrt"
    CUDA_SEPARABLE_COMPILATION ON
)

target_include_directories(obs-yolo-tensorrt PRIVATE
    ${TRT_INCLUDE_DIR}
    ${CUDAToolkit_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(obs-yolo-tensorrt PRIVATE
    libobs
    ${TRT_LIB_INFER}
    ${TRT_LIB_ONNX}
    CUDA::cudart
    CUDA::cuda_driver
)

if(WIN32)
  target_link_libraries(obs-yolo-tensorrt PRIVATE d3d11 dxgi)
else()
  target_link_libraries(obs-yolo-tensorrt PRIVATE GL rt)
endif()

# ── ─────────────────────────────────────────────────────────────────────────
#   2.  colorBot  (standalone executable)
# ──────────────────────────────────────────────────────────────────────────────

add_executable(colorbot
    colorbot/main.cpp
    colorbot/colorbot.cpp
    colorbot/shm_reader.cpp
)

target_include_directories(colorbot PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${OpenCV_INCLUDE_DIRS}
)

target_link_libraries(colorbot PRIVATE
    ${OpenCV_LIBS}
)

if(WIN32)
  # nothing extra
else()
  target_link_libraries(colorbot PRIVATE rt pthread)
endif()

# ── Install ───────────────────────────────────────────────────────────────────

if(WIN32)
  set(OBS_PLUGIN_DIR "C:/Program Files/obs-studio/obs-plugins/64bit")
else()
  set(OBS_PLUGIN_DIR "/usr/lib/obs-plugins"
      CACHE PATH "OBS plugin install directory")
endif()

install(TARGETS obs-yolo-tensorrt
    LIBRARY DESTINATION ${OBS_PLUGIN_DIR}
    RUNTIME DESTINATION ${OBS_PLUGIN_DIR}
)

install(TARGETS colorbot
    RUNTIME DESTINATION bin
)

# ── Summary ───────────────────────────────────────────────────────────────────

message(STATUS "")
message(STATUS "=== obs-yolo-tensorrt build summary ===")
message(STATUS "  Build type   : ${CMAKE_BUILD_TYPE}")
message(STATUS "  TRT include  : ${TRT_INCLUDE_DIR}")
message(STATUS "  TRT nvinfer  : ${TRT_LIB_INFER}")
message(STATUS "  TRT onnx     : ${TRT_LIB_ONNX}")
message(STATUS "  CUDA arch    : ${CMAKE_CUDA_ARCHITECTURES}")
message(STATUS "  OpenCV       : ${OpenCV_VERSION}")
message(STATUS "=======================================")
message(STATUS "")

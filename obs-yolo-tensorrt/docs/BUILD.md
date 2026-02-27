# Build Instructions

## Prerequisites

### All Platforms
- **CMake** ≥ 3.22
- **C++17** compiler (GCC 11+, Clang 14+, or MSVC 2022)
- **CUDA Toolkit** 11.8 or 12.x
- **TensorRT** 8.6 or 9.x
- **OBS Studio** development headers (libobs-dev or OBS source)
- **OpenCV** 4.x (core + highgui + imgproc)

---

## Linux (Ubuntu 22.04)

### 1. Install Dependencies

```bash
# NVIDIA driver + CUDA (if not already installed)
sudo apt-get install -y cuda-toolkit-12-3

# TensorRT (via NVIDIA package manager)
# Follow: https://docs.nvidia.com/deeplearning/tensorrt/install-guide
sudo apt-get install -y tensorrt nvinfer-dev nvonnxparser-dev

# OBS dev headers
sudo apt-get install -y libobs-dev

# OpenCV
sudo apt-get install -y libopencv-dev

# Build tools
sudo apt-get install -y cmake ninja-build
```

### 2. Build

```bash
git clone https://github.com/yourname/obs-yolo-tensorrt
cd obs-yolo-tensorrt
chmod +x scripts/build_linux.sh
./scripts/build_linux.sh
```

To specify CUDA architectures (default: 75;86;89):
```bash
./scripts/build_linux.sh --cuda-arch "86;89"
```

To specify a custom TensorRT install path:
```bash
./scripts/build_linux.sh --trt-root /opt/TensorRT-9.1.0
```

### 3. Install

```bash
sudo cmake --install build
# Or manually:
sudo cp build/obs-yolo-tensorrt.so /usr/lib/obs-plugins/
```

---

## Windows (Visual Studio 2022)

### 1. Install Dependencies

1. Install **Visual Studio 2022** with "Desktop development with C++" workload
2. Install **CUDA 12.x** from the NVIDIA CUDA installer
3. Install **TensorRT 9.x**:
   - Download ZIP from developer.nvidia.com
   - Extract to `C:\Program Files\NVIDIA GPU Computing Toolkit\TensorRT`
4. Install **OBS Studio** with developer headers, or build from source
   - Set `%OBS_ROOT%` to the OBS install/build directory
5. Install **OpenCV 4.x**:
   - Download pre-built from opencv.org or build from source
   - Set `%OpenCV_DIR%` to the cmake config dir (e.g. `C:\opencv\build`)

### 2. Build

```bat
scripts\build_windows.bat Release "75;86;89"
```

### 3. Install

```bat
cmake --install build --config Release
```

Or copy manually:
```bat
copy build\Release\obs-yolo-tensorrt.dll "C:\Program Files\obs-studio\obs-plugins\64bit\"
```

---

## Manual CMake Configuration

If the build scripts don't work for your setup:

```bash
mkdir build && cd build

cmake .. \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CUDA_ARCHITECTURES="75;86;89" \
  -DTRT_INCLUDE_DIR=/path/to/tensorrt/include \
  -DTRT_LIB_INFER=/path/to/tensorrt/lib/libnvinfer.so \
  -DTRT_LIB_ONNX=/path/to/tensorrt/lib/libnvonnxparser.so \
  -DOpenCV_DIR=/path/to/opencv/lib/cmake/opencv4

cmake --build . --parallel $(nproc)
```

---

## Model Preparation

### Using a pre-exported ONNX file

Export from Ultralytics:
```python
from ultralytics import YOLO
model = YOLO("yolov8n.pt")
model.export(format="onnx", opset=17, simplify=True,
             imgsz=640, dynamic=False)
```

Place the `.onnx` file anywhere and point the plugin to it.
The engine will be built automatically on first run.

### Using a pre-built TensorRT engine

```bash
trtexec --onnx=yolov8n.onnx \
        --saveEngine=yolov8n.engine \
        --fp16 \
        --workspace=1024 \
        --minShapes=images:1x3x640x640 \
        --optShapes=images:1x3x640x640 \
        --maxShapes=images:1x3x640x640
```

---

## Plugin Configuration (OBS)

1. Open OBS → Sources → right-click a video source → Filters
2. Add filter → "YOLO TensorRT Detection"
3. Set:
   - **TensorRT .engine file**: path to pre-built engine (or leave empty to auto-build)
   - **ONNX model**: path to .onnx file (used only if engine not found)
   - **Score threshold**: 0.25 (default)
   - **NMS IoU threshold**: 0.45 (default)
   - **FP16 mode**: enabled for RTX 20xx+ GPUs

---

## Running colorBot

```bash
# Basic (visualization only, no mouse control)
./colorbot

# With Win32 mouse control, targeting class 0 (person), FOV 300px
./colorbot --backend win32 --class 0 --fov 300

# Full options
./colorbot --help
```

### Keyboard shortcuts in visualization window

| Key  | Action                   |
|------|--------------------------|
| ESC  | Quit                     |
| Space| Toggle mouse control on/off |

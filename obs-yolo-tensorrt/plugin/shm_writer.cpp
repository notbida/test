/**
 * shm_writer.cpp  –  Platform-specific shared-memory implementation
 */

#include "shm_writer.h"

#include <cassert>
#include <cstring>
#include <stdexcept>

#include <obs/obs-module.h>  // blog()

#ifdef _WIN32
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

// ── open ─────────────────────────────────────────────────────────────────────

bool ShmWriter::open(const std::string& name, size_t sz)
{
    size_ = sz;

#ifdef _WIN32
    // Windows: CreateFileMapping
    file_mapping_ = CreateFileMappingA(
        INVALID_HANDLE_VALUE,
        nullptr,
        PAGE_READWRITE,
        (DWORD)(sz >> 32),
        (DWORD)(sz & 0xFFFFFFFF),
        name.c_str());

    if (!file_mapping_) {
        blog(LOG_ERROR, "[yolo-shm] CreateFileMapping failed (%lu)", GetLastError());
        return false;
    }

    base_ = MapViewOfFile(
        file_mapping_,
        FILE_MAP_ALL_ACCESS,
        0, 0, sz);

    if (!base_) {
        blog(LOG_ERROR, "[yolo-shm] MapViewOfFile failed (%lu)", GetLastError());
        CloseHandle(file_mapping_);
        file_mapping_ = nullptr;
        return false;
    }
#else
    // Linux/macOS: shm_open + ftruncate + mmap
    shm_fd_ = shm_open(name.c_str(), O_CREAT | O_RDWR, 0666);
    if (shm_fd_ < 0) {
        blog(LOG_ERROR, "[yolo-shm] shm_open failed: %s", strerror(errno));
        return false;
    }

    if (ftruncate(shm_fd_, (off_t)sz) != 0) {
        blog(LOG_ERROR, "[yolo-shm] ftruncate failed: %s", strerror(errno));
        ::close(shm_fd_);
        shm_fd_ = -1;
        return false;
    }

    base_ = mmap(nullptr, sz, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd_, 0);
    if (base_ == MAP_FAILED) {
        blog(LOG_ERROR, "[yolo-shm] mmap failed: %s", strerror(errno));
        ::close(shm_fd_);
        shm_fd_ = -1;
        base_   = nullptr;
        return false;
    }
#endif

    // Zero the region
    std::memset(base_, 0, sz);

    // Set up sub-region pointers
    hdr_  = reinterpret_cast<YoloShmHeader*>(base_);
    dets_ = reinterpret_cast<YoloDetection*>(
                static_cast<uint8_t*>(base_) + sizeof(YoloShmHeader) + /* align */ 256 - sizeof(YoloShmHeader));

    // Actually detections start right after the 256-byte header
    dets_ = reinterpret_cast<YoloDetection*>(
                static_cast<uint8_t*>(base_) + 256);

    // Initialise header identity fields (no seqlock yet – reader not up)
    hdr_->magic             = YOLO_SHM_MAGIC;
    hdr_->protocol_version  = YOLO_PROTOCOL_VERSION;
    hdr_->plugin_pid        = (uint32_t)
#ifdef _WIN32
        GetCurrentProcessId();
#else
        getpid();
#endif
    hdr_->classnames_offset = YOLO_CLASSNAMES_OFFSET;
    hdr_->write_seq.store(0, std::memory_order_release);

    blog(LOG_INFO, "[yolo-shm] Shared memory opened: %s (%zu bytes)", name.c_str(), sz);
    return true;
}

// ── close ────────────────────────────────────────────────────────────────────

void ShmWriter::close()
{
    if (!base_) return;

#ifdef _WIN32
    UnmapViewOfFile(base_);
    CloseHandle(file_mapping_);
    file_mapping_ = nullptr;
#else
    munmap(base_, size_);
    ::close(shm_fd_);
    shm_fd_ = -1;
#endif

    base_ = nullptr;
    hdr_  = nullptr;
    dets_ = nullptr;
}

// ── write ─────────────────────────────────────────────────────────────────────

void ShmWriter::write(const YoloShmHeader& payload,
                       const YoloDetection* dets,
                       uint32_t             num_dets)
{
    assert(hdr_ && dets_);

    // Begin seqlock write
    yolo_shm_begin_write(hdr_);

    // Copy frame metadata from payload (all fields except write_seq, magic, version)
    hdr_->frame_index          = payload.frame_index;
    hdr_->capture_timestamp_ns = payload.capture_timestamp_ns;
    hdr_->inference_end_ns     = payload.inference_end_ns;
    hdr_->frame_width          = payload.frame_width;
    hdr_->frame_height         = payload.frame_height;
    hdr_->input_scale_x        = payload.input_scale_x;
    hdr_->input_scale_y        = payload.input_scale_y;
    hdr_->num_detections       = num_dets;
    hdr_->nms_threshold        = payload.nms_threshold;
    hdr_->score_threshold      = payload.score_threshold;
    hdr_->model_input_width    = payload.model_input_width;
    hdr_->model_input_height   = payload.model_input_height;
    hdr_->avg_preprocess_ns    = payload.avg_preprocess_ns;
    hdr_->avg_inference_ns     = payload.avg_inference_ns;
    hdr_->avg_postprocess_ns   = payload.avg_postprocess_ns;
    hdr_->avg_total_ns         = payload.avg_total_ns;
    hdr_->frames_dropped       = payload.frames_dropped;

    // Copy detection array
    uint32_t n = (num_dets < YOLO_MAX_DETECTIONS) ? num_dets : YOLO_MAX_DETECTIONS;
    std::memcpy(dets_, dets, n * sizeof(YoloDetection));

    // End seqlock write – reader can now safely consume
    yolo_shm_end_write(hdr_);
}

// ── write_classnames ─────────────────────────────────────────────────────────

void ShmWriter::write_classnames(const char* const* names, uint32_t count)
{
    assert(base_);

    char* table = static_cast<char*>(base_) + YOLO_CLASSNAMES_OFFSET;
    uint32_t nc = (count < YOLO_MAX_CLASSES) ? count : YOLO_MAX_CLASSES;

    for (uint32_t i = 0; i < nc; ++i) {
        char* slot = table + i * YOLO_CLASSNAME_LEN;
        std::strncpy(slot, names[i], YOLO_CLASSNAME_LEN - 1);
        slot[YOLO_CLASSNAME_LEN - 1] = '\0';
    }

    // Update header atomically (only num_classes changes here)
    yolo_shm_begin_write(hdr_);
    hdr_->num_classes = nc;
    yolo_shm_end_write(hdr_);
}

/**
 * shm_reader.cpp  –  Platform-specific SHM reader implementation
 */

#include "shm_reader.h"

#include <cstring>
#include <thread>
#include <chrono>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

// ── open ─────────────────────────────────────────────────────────────────────

bool ShmReader::open(const std::string& name, size_t sz)
{
    size_ = sz;

#ifdef _WIN32
    file_mapping_ = OpenFileMappingA(FILE_MAP_READ, FALSE, name.c_str());
    if (!file_mapping_) return false;

    base_ = MapViewOfFile(file_mapping_, FILE_MAP_READ, 0, 0, sz);
    if (!base_) {
        CloseHandle(file_mapping_);
        file_mapping_ = nullptr;
        return false;
    }
#else
    shm_fd_ = shm_open(name.c_str(), O_RDONLY, 0666);
    if (shm_fd_ < 0) return false;

    base_ = mmap(nullptr, sz, PROT_READ, MAP_SHARED, shm_fd_, 0);
    if (base_ == MAP_FAILED) {
        ::close(shm_fd_);
        shm_fd_ = -1;
        base_   = nullptr;
        return false;
    }
#endif

    hdr_  = reinterpret_cast<const YoloShmHeader*>(base_);
    dets_ = reinterpret_cast<const YoloDetection*>(
                static_cast<const uint8_t*>(base_) + 256);

    return true;
}

// ── close ────────────────────────────────────────────────────────────────────

void ShmReader::close()
{
    if (!base_) return;

#ifdef _WIN32
    UnmapViewOfFile(base_);
    CloseHandle(file_mapping_);
    file_mapping_ = nullptr;
#else
    munmap(const_cast<void*>(base_), size_);
    ::close(shm_fd_);
    shm_fd_ = -1;
#endif

    base_ = nullptr;
    hdr_  = nullptr;
    dets_ = nullptr;
    classnames_loaded_ = false;
}

// ── poll ─────────────────────────────────────────────────────────────────────

bool ShmReader::poll(DetectionSnapshot& snap)
{
    if (!base_) return false;

    // Validate magic
    if (hdr_->magic != YOLO_SHM_MAGIC) return false;

    // Attempt seqlock read (retry up to 8 times before giving up for this call)
    for (int attempt = 0; attempt < 8; ++attempt) {
        if (yolo_shm_try_read(hdr_, dets_,
                               &snap.header, snap.detections)) {
            snap.valid = true;

            // Copy classnames once (they don't change at runtime)
            if (!classnames_loaded_ && snap.header.num_classes > 0) {
                const char* table =
                    static_cast<const char*>(base_) + YOLO_CLASSNAMES_OFFSET;
                uint32_t nc = snap.header.num_classes;
                if (nc > YOLO_MAX_CLASSES) nc = YOLO_MAX_CLASSES;
                for (uint32_t i = 0; i < nc; ++i) {
                    std::strncpy(classnames_[i],
                                 table + i * YOLO_CLASSNAME_LEN,
                                 YOLO_CLASSNAME_LEN - 1);
                    classnames_[i][YOLO_CLASSNAME_LEN - 1] = '\0';
                }
                std::memcpy(snap.classnames, classnames_, sizeof(classnames_));
                classnames_loaded_ = true;
            } else if (classnames_loaded_) {
                std::memcpy(snap.classnames, classnames_, sizeof(classnames_));
            }
            return true;
        }
        // Micro-spin before retry
        for (volatile int i = 0; i < 16; ++i) {}
    }

    return false;
}

// ── wait_new_frame ────────────────────────────────────────────────────────────

bool ShmReader::wait_new_frame(DetectionSnapshot& snap,
                                uint64_t           last_index,
                                uint32_t           timeout_us)
{
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::microseconds(timeout_us);

    while (std::chrono::steady_clock::now() < deadline) {
        if (poll(snap) && snap.header.frame_index > last_index)
            return true;
        std::this_thread::yield();
    }
    return false;
}

// ── classname ────────────────────────────────────────────────────────────────

const char* ShmReader::classname(uint16_t id) const
{
    if (id >= YOLO_MAX_CLASSES) return "unknown";
    return classnames_[id];
}

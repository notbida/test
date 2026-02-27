/**
 * shm_writer.h  –  Shared-memory writer (OBS plugin side)
 *
 * Creates / opens the named shared-memory region and provides a
 * single write() call that updates the SHM atomically using the
 * sequence-lock protocol defined in yolo_ipc.h.
 *
 * Platform support:
 *   Linux:   shm_open + mmap
 *   Windows: CreateFileMapping + MapViewOfFile
 */

#pragma once

#include "../protocol/yolo_ipc.h"

#include <cstdint>
#include <string>

class ShmWriter
{
public:
    ShmWriter()  = default;
    ~ShmWriter() { close(); }

    ShmWriter(const ShmWriter&) = delete;
    ShmWriter& operator=(const ShmWriter&) = delete;

    /** Create or truncate the shared-memory region. */
    bool open(const std::string& name = YOLO_SHM_NAME,
              size_t size = YOLO_SHM_SIZE);

    /** Unmap / close handles. */
    void close();

    bool is_open() const { return base_ != nullptr; }

    /**
     * Atomically publish one frame's detections.
     * Never blocks – uses the seqlock pattern.
     */
    void write(const YoloShmHeader&   hdr_payload,
               const YoloDetection*   dets,
               uint32_t               num_dets);

    /** Write the class-name table once at startup. */
    void write_classnames(const char* const* names, uint32_t count);

    // Raw pointers for direct manipulation if needed
    YoloShmHeader*  header()     { return hdr_; }
    YoloDetection*  detections() { return dets_; }

private:
    void*           base_  = nullptr;
    YoloShmHeader*  hdr_   = nullptr;
    YoloDetection*  dets_  = nullptr;
    size_t          size_  = 0;

#ifdef _WIN32
    void*           file_mapping_ = nullptr;  // HANDLE
#else
    int             shm_fd_ = -1;
#endif
};

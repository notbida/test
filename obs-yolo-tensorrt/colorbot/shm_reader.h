/**
 * shm_reader.h  –  Shared-memory reader (CVM-colorBot side)
 *
 * Opens the named SHM created by the OBS plugin and provides a
 * non-blocking poll() that returns the latest detection snapshot.
 *
 * Usage:
 *   ShmReader reader;
 *   reader.open();
 *   ...
 *   DetectionSnapshot snap;
 *   if (reader.poll(snap)) {
 *       // snap.header and snap.detections[] are valid and consistent
 *   }
 */

#pragma once

#include "../protocol/yolo_ipc.h"

#include <cstdint>
#include <string>

struct DetectionSnapshot {
    YoloShmHeader  header;
    YoloDetection  detections[YOLO_MAX_DETECTIONS];
    char           classnames[YOLO_MAX_CLASSES][YOLO_CLASSNAME_LEN];
    bool           valid = false;
};

class ShmReader
{
public:
    ShmReader()  = default;
    ~ShmReader() { close(); }

    ShmReader(const ShmReader&) = delete;
    ShmReader& operator=(const ShmReader&) = delete;

    /**
     * Open the SHM region.  Will fail if the OBS plugin has not yet
     * created it.  Returns true on success.
     */
    bool open(const std::string& name = YOLO_SHM_NAME,
              size_t size = YOLO_SHM_SIZE);

    void close();
    bool is_open() const { return base_ != nullptr; }

    /**
     * Attempt to get a consistent, up-to-date snapshot.
     *
     * Returns true and fills `snap` if a valid read was obtained.
     * Returns false (without modifying snap) if:
     *   - SHM not open
     *   - Plugin not yet running (magic mismatch)
     *   - seqlock was racing (caller should retry next frame)
     *
     * This function is wait-free in the common case (even seq, no write race).
     */
    bool poll(DetectionSnapshot& snap);

    /**
     * Spin until a new frame (frame_index > last_seen_index) is available
     * or timeout_us microseconds elapse.
     * Returns true if a new frame was obtained.
     */
    bool wait_new_frame(DetectionSnapshot& snap,
                        uint64_t           last_seen_index,
                        uint32_t           timeout_us = 20000);

    const char* classname(uint16_t id) const;

private:
    void*           base_ = nullptr;
    const YoloShmHeader*  hdr_  = nullptr;
    const YoloDetection*  dets_ = nullptr;
    size_t          size_ = 0;

#ifdef _WIN32
    void*           file_mapping_ = nullptr;
#else
    int             shm_fd_ = -1;
#endif

    // Cached classname table
    char classnames_[YOLO_MAX_CLASSES][YOLO_CLASSNAME_LEN]{};
    bool classnames_loaded_ = false;
};

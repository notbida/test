#pragma once

#include <cstdint>
#include <vector>
#include <string>

/**
 * Memory reading interface for reading from target process
 * Abstract class to support different implementations (Windows/Linux)
 */
class IMemoryReader {
public:
    virtual ~IMemoryReader() = default;

    /**
     * Read memory from target process
     * @param address Address to read from
     * @param buffer Buffer to store read data
     * @param size Number of bytes to read
     * @return true if successful, false otherwise
     */
    virtual bool Read(uint64_t address, void* buffer, size_t size) = 0;

    /**
     * Read memory with template type convenience
     */
    template<typename T>
    bool Read(uint64_t address, T* value) {
        return Read(address, value, sizeof(T));
    }

    /**
     * Batch read memory in pages to handle missing pages gracefully
     * @param address Start address
     * @param buffer Output buffer
     * @param size Total size to read
     * @param pageSize Size of each read chunk (default 4KB)
     * @return Number of bytes successfully read
     */
    virtual size_t BatchRead(uint64_t address, uint8_t* buffer, size_t size, size_t pageSize = 0x1000);
};

/**
 * Mock memory reader for testing - reads from local buffer
 */
class MockMemoryReader : public IMemoryReader {
private:
    std::vector<uint8_t> m_data;
    uint64_t m_baseAddress;

public:
    MockMemoryReader(const std::vector<uint8_t>& data, uint64_t baseAddress = 0x140000000);

    bool Read(uint64_t address, void* buffer, size_t size) override;

    void SetData(const std::vector<uint8_t>& data);
};

#ifdef PLATFORM_WINDOWS
/**
 * Windows process memory reader
 */
class WindowsMemoryReader : public IMemoryReader {
private:
    void* m_processHandle;

public:
    WindowsMemoryReader(uint32_t processId);
    ~WindowsMemoryReader() override;

    bool Read(uint64_t address, void* buffer, size_t size) override;
};
#endif

#ifdef PLATFORM_LINUX
/**
 * Linux process memory reader using /proc/pid/mem
 */
class LinuxMemoryReader : public IMemoryReader {
private:
    int m_memFd;
    uint32_t m_pid;

public:
    LinuxMemoryReader(uint32_t processId);
    ~LinuxMemoryReader() override;

    bool Read(uint64_t address, void* buffer, size_t size) override;
};
#endif

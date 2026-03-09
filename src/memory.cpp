#include "memory.h"
#include <cstring>
#include <algorithm>

// Base class implementation
size_t IMemoryReader::BatchRead(uint64_t address, uint8_t* buffer, size_t size, size_t pageSize) {
    size_t bytesRead = 0;
    for (size_t offset = 0; offset < size; offset += pageSize) {
        size_t chunkSize = std::min(pageSize, size - offset);
        if (Read(address + offset, buffer + offset, chunkSize)) {
            bytesRead += chunkSize;
        } else {
            // Fill failed page with zeros
            std::memset(buffer + offset, 0, chunkSize);
        }
    }
    return bytesRead;
}

// MockMemoryReader implementation
MockMemoryReader::MockMemoryReader(const std::vector<uint8_t>& data, uint64_t baseAddress)
    : m_data(data), m_baseAddress(baseAddress) {
}

bool MockMemoryReader::Read(uint64_t address, void* buffer, size_t size) {
    if (address < m_baseAddress) {
        return false;
    }

    uint64_t offset = address - m_baseAddress;
    if (offset + size > m_data.size()) {
        return false;
    }

    std::memcpy(buffer, m_data.data() + offset, size);
    return true;
}

void MockMemoryReader::SetData(const std::vector<uint8_t>& data) {
    m_data = data;
}

#ifdef PLATFORM_WINDOWS
#include <windows.h>

WindowsMemoryReader::WindowsMemoryReader(uint32_t processId) {
    m_processHandle = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, processId);
}

WindowsMemoryReader::~WindowsMemoryReader() {
    if (m_processHandle) {
        CloseHandle(m_processHandle);
    }
}

bool WindowsMemoryReader::Read(uint64_t address, void* buffer, size_t size) {
    if (!m_processHandle) {
        return false;
    }

    SIZE_T bytesRead = 0;
    return ReadProcessMemory(m_processHandle, reinterpret_cast<LPCVOID>(address),
                            buffer, size, &bytesRead) && bytesRead == size;
}
#endif

#ifdef PLATFORM_LINUX
#include <unistd.h>
#include <fcntl.h>
#include <sys/uio.h>

LinuxMemoryReader::LinuxMemoryReader(uint32_t processId) : m_pid(processId) {
    char path[256];
    snprintf(path, sizeof(path), "/proc/%u/mem", processId);
    m_memFd = open(path, O_RDONLY);
}

LinuxMemoryReader::~LinuxMemoryReader() {
    if (m_memFd >= 0) {
        close(m_memFd);
    }
}

bool LinuxMemoryReader::Read(uint64_t address, void* buffer, size_t size) {
    if (m_memFd < 0) {
        return false;
    }

    // Use process_vm_readv for better performance
    struct iovec local[1];
    struct iovec remote[1];

    local[0].iov_base = buffer;
    local[0].iov_len = size;
    remote[0].iov_base = reinterpret_cast<void*>(address);
    remote[0].iov_len = size;

    ssize_t nread = process_vm_readv(m_pid, local, 1, remote, 1, 0);
    return nread == static_cast<ssize_t>(size);
}
#endif

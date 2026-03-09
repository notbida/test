#include "sig_scanner.h"
#include "memory.h"
#include <sstream>
#include <iostream>
#include <algorithm>

SigScanner::SigScanner(IMemoryReader& memory, uint64_t baseAddress)
    : m_memory(memory), m_baseAddress(baseAddress), m_initialized(false) {
}

bool SigScanner::Initialize(uint64_t moduleSize) {
    std::cout << "Initializing scanner, reading module (0x" << std::hex << moduleSize << " bytes)...\n" << std::dec;

    m_moduleData.resize(moduleSize);

    // Batch read in pages to handle missing pages gracefully
    constexpr size_t PAGE_SIZE = 0x1000;
    size_t bytesRead = 0;

    for (size_t offset = 0; offset < moduleSize; offset += PAGE_SIZE) {
        size_t chunkSize = std::min(PAGE_SIZE, moduleSize - offset);
        if (m_memory.Read(m_baseAddress + offset, m_moduleData.data() + offset, chunkSize)) {
            bytesRead += chunkSize;
        } else {
            // Fill failed pages with zeros
            std::fill_n(m_moduleData.data() + offset, chunkSize, 0);
        }

        // Progress indicator
        if (offset % (PAGE_SIZE * 256) == 0) {
            std::cout << "  Read: " << (offset / 1024 / 1024) << " MB / "
                      << (moduleSize / 1024 / 1024) << " MB\r" << std::flush;
        }
    }

    std::cout << "\nModule cached: " << (bytesRead / 1024 / 1024) << " MB read successfully\n";

    m_initialized = true;
    return true;
}

SigScanner::Pattern SigScanner::ParsePattern(const std::string& idaPattern) {
    Pattern result;

    std::istringstream ss(idaPattern);
    std::string token;

    while (ss >> token) {
        if (token == "??" || token == "?") {
            result.bytes.push_back(0x00);
            result.mask += '?';
        } else {
            result.bytes.push_back(static_cast<uint8_t>(std::strtol(token.c_str(), nullptr, 16)));
            result.mask += 'x';
        }
    }

    return result;
}

std::vector<uint32_t> SigScanner::ScanStr(const std::string& idaPattern) {
    Pattern pattern = ParsePattern(idaPattern);
    return Scan(pattern.bytes, pattern.mask);
}

std::vector<uint32_t> SigScanner::Scan(const std::vector<uint8_t>& pattern, const std::string& mask) {
    if (!m_initialized) {
        std::cerr << "Scanner not initialized\n";
        return {};
    }

    if (pattern.empty() || pattern.size() != mask.size()) {
        std::cerr << "Invalid pattern or mask\n";
        return {};
    }

    std::vector<uint32_t> results;
    size_t patternSize = pattern.size();
    size_t searchSize = m_moduleData.size() - patternSize;

    std::cout << "Scanning for pattern (size: " << patternSize << " bytes)...\n";

    for (size_t i = 0; i < searchSize; i++) {
        bool match = true;

        for (size_t j = 0; j < patternSize; j++) {
            if (mask[j] == 'x' && m_moduleData[i + j] != pattern[j]) {
                match = false;
                break;
            }
        }

        if (match) {
            results.push_back(static_cast<uint32_t>(i));
        }

        // Progress indicator for large scans
        if (i % (1024 * 1024) == 0) {
            std::cout << "  Scanned: " << (i / 1024 / 1024) << " MB\r" << std::flush;
        }
    }

    std::cout << "\nFound " << results.size() << " matches\n";

    return results;
}

uint32_t SigScanner::ScanFirst(const std::string& idaPattern) {
    auto results = ScanStr(idaPattern);
    return results.empty() ? 0 : results[0];
}

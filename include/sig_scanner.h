#pragma once

#include <cstdint>
#include <vector>
#include <string>

class IMemoryReader;

/**
 * Signature Scanner - Layer 2
 * IDA-style pattern scanning with wildcards
 */
class SigScanner {
private:
    IMemoryReader& m_memory;
    uint64_t m_baseAddress;
    std::vector<uint8_t> m_moduleData;
    bool m_initialized;

    /**
     * Parse IDA-style pattern string into bytes and mask
     * Example: "F2 0F ?? ?? ??" -> bytes=[0xF2, 0x0F, 0x00, 0x00, 0x00], mask="xx???"
     */
    struct Pattern {
        std::vector<uint8_t> bytes;
        std::string mask;
    };

    Pattern ParsePattern(const std::string& idaPattern);

public:
    SigScanner(IMemoryReader& memory, uint64_t baseAddress);

    /**
     * Initialize by reading entire module into local buffer
     * This caches the module for fast scanning
     * @param moduleSize Size of the module to cache
     * @return true if successful
     */
    bool Initialize(uint64_t moduleSize);

    /**
     * Scan for IDA-style pattern
     * @param idaPattern Pattern like "F2 0F 70 05 ?? ?? ?? ??"
     * @return Vector of RVAs (offsets from base) where pattern was found
     */
    std::vector<uint32_t> ScanStr(const std::string& idaPattern);

    /**
     * Scan for pattern with pre-parsed bytes and mask
     * @param pattern Byte pattern
     * @param mask Mask string ('x' = exact match, '?' = wildcard)
     * @return Vector of RVAs where pattern was found
     */
    std::vector<uint32_t> Scan(const std::vector<uint8_t>& pattern, const std::string& mask);

    /**
     * Get first match for pattern (convenience method)
     * @return RVA of first match, or 0 if not found
     */
    uint32_t ScanFirst(const std::string& idaPattern);

    /**
     * Get cached module data
     */
    const std::vector<uint8_t>& GetModuleData() const { return m_moduleData; }

    /**
     * Check if initialized
     */
    bool IsInitialized() const { return m_initialized; }
};

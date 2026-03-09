#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <map>

class IMemoryReader;

/**
 * PE section information
 */
struct PESection {
    std::string name;
    uint32_t virtualAddress;
    uint32_t virtualSize;
    uint32_t characteristics;

    bool IsReadable() const { return characteristics & 0x40000000; }
    bool IsWritable() const { return characteristics & 0x80000000; }
    bool IsExecutable() const { return characteristics & 0x20000000; }
};

/**
 * PE Header Parser - Layer 1
 * Parses PE headers from target process to find valid memory regions
 */
class PEParser {
private:
    IMemoryReader& m_memory;
    uint64_t m_baseAddress;
    uint64_t m_moduleSize;

    std::vector<PESection> m_sections;
    bool m_initialized;

    // Section boundaries for common sections
    uint32_t m_rdataStart, m_rdataEnd;
    uint32_t m_dataStart, m_dataEnd;
    uint32_t m_textStart, m_textEnd;

public:
    PEParser(IMemoryReader& memory, uint64_t baseAddress);

    /**
     * Parse PE headers and extract section information
     * @return true if successful
     */
    bool Parse();

    /**
     * Get all sections
     */
    const std::vector<PESection>& GetSections() const { return m_sections; }

    /**
     * Get specific section by name
     */
    const PESection* GetSection(const std::string& name) const;

    /**
     * Check if RVA is within .rdata section
     */
    bool IsInRData(uint32_t rva) const;

    /**
     * Check if RVA is within .data section
     */
    bool IsInData(uint32_t rva) const;

    /**
     * Check if RVA is within .text section
     */
    bool IsInText(uint32_t rva) const;

    /**
     * Get module size
     */
    uint64_t GetModuleSize() const { return m_moduleSize; }

    /**
     * Get base address
     */
    uint64_t GetBaseAddress() const { return m_baseAddress; }
};

#include "pe_parser.h"
#include "memory.h"
#include <cstring>
#include <iostream>

PEParser::PEParser(IMemoryReader& memory, uint64_t baseAddress)
    : m_memory(memory), m_baseAddress(baseAddress), m_moduleSize(0),
      m_initialized(false), m_rdataStart(0), m_rdataEnd(0),
      m_dataStart(0), m_dataEnd(0), m_textStart(0), m_textEnd(0) {
}

bool PEParser::Parse() {
    // Read DOS header to find PE signature offset
    uint32_t e_lfanew = 0;
    if (!m_memory.Read(m_baseAddress + 0x3C, &e_lfanew, sizeof(e_lfanew))) {
        std::cerr << "Failed to read DOS header e_lfanew\n";
        return false;
    }

    // Verify PE signature
    uint32_t peSig = 0;
    if (!m_memory.Read(m_baseAddress + e_lfanew, &peSig, sizeof(peSig))) {
        std::cerr << "Failed to read PE signature\n";
        return false;
    }

    if (peSig != 0x00004550) { // "PE\0\0"
        std::cerr << "Invalid PE signature: 0x" << std::hex << peSig << "\n";
        return false;
    }

    // Read COFF header
    uint16_t machine = 0;
    uint16_t numSections = 0;
    if (!m_memory.Read(m_baseAddress + e_lfanew + 4, &machine, sizeof(machine))) {
        return false;
    }
    if (!m_memory.Read(m_baseAddress + e_lfanew + 6, &numSections, sizeof(numSections))) {
        return false;
    }

    // Read optional header size
    uint16_t optHeaderSize = 0;
    if (!m_memory.Read(m_baseAddress + e_lfanew + 20, &optHeaderSize, sizeof(optHeaderSize))) {
        return false;
    }

    // Read SizeOfImage from optional header
    if (!m_memory.Read(m_baseAddress + e_lfanew + 24 + 56, &m_moduleSize, sizeof(uint32_t))) {
        return false;
    }

    std::cout << "PE Header parsed: " << numSections << " sections, module size: 0x"
              << std::hex << m_moduleSize << std::dec << "\n";

    // Parse section headers
    uint64_t sectionHeaderOffset = e_lfanew + 24 + optHeaderSize;

    for (int i = 0; i < numSections; i++) {
        uint64_t secOffset = sectionHeaderOffset + (i * 40);
        uint8_t secHeader[40];

        if (!m_memory.Read(m_baseAddress + secOffset, secHeader, sizeof(secHeader))) {
            continue;
        }

        PESection section;

        // Extract section name (first 8 bytes)
        char name[9] = {0};
        std::memcpy(name, secHeader, 8);
        section.name = name;

        // Extract section properties
        std::memcpy(&section.virtualSize, secHeader + 8, 4);
        std::memcpy(&section.virtualAddress, secHeader + 12, 4);
        std::memcpy(&section.characteristics, secHeader + 36, 4);

        m_sections.push_back(section);

        std::cout << "Section: " << section.name
                  << " VA: 0x" << std::hex << section.virtualAddress
                  << " Size: 0x" << section.virtualSize
                  << " Chars: 0x" << section.characteristics << std::dec << "\n";

        // Identify common sections
        if (section.name == ".rdata" || section.name == ".rodata") {
            m_rdataStart = section.virtualAddress;
            m_rdataEnd = section.virtualAddress + section.virtualSize;
        } else if (section.name == ".data") {
            m_dataStart = section.virtualAddress;
            m_dataEnd = section.virtualAddress + section.virtualSize;
        } else if (section.name == ".text") {
            m_textStart = section.virtualAddress;
            m_textEnd = section.virtualAddress + section.virtualSize;
        }
    }

    m_initialized = true;
    return true;
}

const PESection* PEParser::GetSection(const std::string& name) const {
    for (const auto& section : m_sections) {
        if (section.name == name) {
            return &section;
        }
    }
    return nullptr;
}

bool PEParser::IsInRData(uint32_t rva) const {
    return rva >= m_rdataStart && rva < m_rdataEnd;
}

bool PEParser::IsInData(uint32_t rva) const {
    return rva >= m_dataStart && rva < m_dataEnd;
}

bool PEParser::IsInText(uint32_t rva) const {
    return rva >= m_textStart && rva < m_textEnd;
}

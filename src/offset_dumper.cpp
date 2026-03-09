#include "offset_dumper.h"
#include "memory.h"
#include "pe_parser.h"
#include "sig_scanner.h"
#include "instruction_decoder.h"
#include <iostream>
#include <iomanip>
#include <fstream>
#include <algorithm>

OffsetDumper::OffsetDumper(uint64_t baseAddress)
    : m_baseAddress(baseAddress), m_initialized(false) {
}

bool OffsetDumper::Initialize(std::unique_ptr<IMemoryReader> memory) {
    m_memory = std::move(memory);

    // Initialize PE parser
    m_peParser = std::make_unique<PEParser>(*m_memory, m_baseAddress);
    if (!m_peParser->Parse()) {
        std::cerr << "Failed to parse PE headers\n";
        return false;
    }

    // Initialize signature scanner
    m_scanner = std::make_unique<SigScanner>(*m_memory, m_baseAddress);
    if (!m_scanner->Initialize(m_peParser->GetModuleSize())) {
        std::cerr << "Failed to initialize signature scanner\n";
        return false;
    }

    // Initialize instruction decoder
    m_decoder = std::make_unique<InstructionDecoder>(m_baseAddress);

    // Register default patterns
    RegisterDefaultPatterns();

    m_initialized = true;
    return true;
}

void OffsetDumper::RegisterDefaultPatterns() {
    // GObjects pattern - PSHUFLW + PXOR (SIMD decrypt)
    RegisterPattern({
        "GObjects",
        "F2 0F 70 05 ?? ?? ?? ?? ?? 66 0F EF 05 ?? ?? ?? ??",
        ExtractGObjects
    });

    // FNamePool pattern - FNV-1a hash constants
    RegisterPattern({
        "FNamePool",
        "69 D2 93 01 00 01 81 C2 89 6C 70 69",
        ExtractFNamePool
    });

    // Chunk Decrypt - PEB access
    RegisterPattern({
        "ChunkDecrypt",
        "65 48 03 04 25 60 00 00 00",
        ExtractChunkDecrypt
    });
}

void OffsetDumper::RegisterPattern(const OffsetPattern& pattern) {
    m_patterns.push_back(pattern);
}

bool OffsetDumper::Discover() {
    if (!m_initialized) {
        std::cerr << "OffsetDumper not initialized\n";
        return false;
    }

    std::cout << "\n========================================\n";
    std::cout << "Starting offset discovery...\n";
    std::cout << "========================================\n\n";

    m_results.clear();

    for (const auto& pattern : m_patterns) {
        std::cout << "\n[*] Searching for: " << pattern.name << "\n";
        std::cout << "    Pattern: " << pattern.signature << "\n";

        // Scan for pattern
        auto hits = m_scanner->ScanStr(pattern.signature);

        if (hits.empty()) {
            std::cout << "    [!] No matches found\n";
            OffsetResult result;
            result.name = pattern.name;
            result.found = false;
            result.details = "No pattern matches";
            m_results.push_back(result);
            continue;
        }

        std::cout << "    [+] Found " << hits.size() << " potential matches\n";

        // Extract offset using pattern-specific extractor
        OffsetResult result = pattern.extractor(hits, *m_scanner, *m_decoder, *m_peParser);
        m_results.push_back(result);

        if (result.found) {
            std::cout << "    [+] SUCCESS: 0x" << std::hex << result.address
                      << " (RVA: 0x" << result.rva << ")" << std::dec << "\n";
            if (!result.details.empty()) {
                std::cout << "    [+] Details: " << result.details << "\n";
            }
        } else {
            std::cout << "    [!] FAILED: " << result.details << "\n";
        }
    }

    std::cout << "\n========================================\n";
    std::cout << "Discovery complete!\n";
    std::cout << "========================================\n";

    return !m_results.empty();
}

const OffsetResult* OffsetDumper::GetResult(const std::string& name) const {
    auto it = std::find_if(m_results.begin(), m_results.end(),
                          [&name](const OffsetResult& r) { return r.name == name; });
    return it != m_results.end() ? &(*it) : nullptr;
}

void OffsetDumper::PrintResults() const {
    std::cout << "\n========================================\n";
    std::cout << "OFFSET DISCOVERY RESULTS\n";
    std::cout << "========================================\n\n";

    for (const auto& result : m_results) {
        std::cout << result.name << ":\n";
        if (result.found) {
            std::cout << "  Address: 0x" << std::hex << result.address << std::dec << "\n";
            std::cout << "  RVA:     0x" << std::hex << result.rva << std::dec << "\n";
            if (!result.details.empty()) {
                std::cout << "  Details: " << result.details << "\n";
            }
        } else {
            std::cout << "  Status:  NOT FOUND\n";
            std::cout << "  Reason:  " << result.details << "\n";
        }
        std::cout << "\n";
    }
}

bool OffsetDumper::ExportToJson(const std::string& filename) const {
    std::ofstream file(filename);
    if (!file.is_open()) {
        return false;
    }

    file << "{\n";
    file << "  \"base_address\": \"0x" << std::hex << m_baseAddress << std::dec << "\",\n";
    file << "  \"offsets\": {\n";

    for (size_t i = 0; i < m_results.size(); i++) {
        const auto& result = m_results[i];
        file << "    \"" << result.name << "\": {\n";
        file << "      \"found\": " << (result.found ? "true" : "false") << ",\n";
        if (result.found) {
            file << "      \"address\": \"0x" << std::hex << result.address << std::dec << "\",\n";
            file << "      \"rva\": \"0x" << std::hex << result.rva << std::dec << "\",\n";
            file << "      \"details\": \"" << result.details << "\"\n";
        } else {
            file << "      \"error\": \"" << result.details << "\"\n";
        }
        file << "    }" << (i < m_results.size() - 1 ? "," : "") << "\n";
    }

    file << "  }\n";
    file << "}\n";

    file.close();
    return true;
}

// Built-in extractors
OffsetResult OffsetDumper::ExtractGObjects(const std::vector<uint32_t>& hits,
                                           SigScanner& scanner,
                                           InstructionDecoder& decoder,
                                           PEParser& pe) {
    OffsetResult result;
    result.name = "GObjects";

    // Look for PSHUFLW instruction followed by PXOR
    for (uint32_t rva : hits) {
        // Decode PSHUFLW instruction
        const auto& moduleData = scanner.GetModuleData();
        if (rva + 20 > moduleData.size()) continue;

        auto pshuflw = decoder.Decode(moduleData.data() + rva, 20, rva);
        if (!pshuflw.valid) continue;

        // Check if it has RIP-relative addressing
        if (!decoder.HasRipRelative(pshuflw)) continue;

        // Get the RIP-relative address
        uint64_t address = decoder.GetRipRelativeAddress(pshuflw, 1);
        uint32_t targetRva = static_cast<uint32_t>(address - pe.GetBaseAddress());

        // Validate it's in .data or .rdata
        if (pe.IsInData(targetRva) || pe.IsInRData(targetRva)) {
            result.found = true;
            result.address = address;
            result.rva = targetRva;
            result.details = "Found via PSHUFLW RIP-relative addressing";
            return result;
        }
    }

    result.found = false;
    result.details = "No valid RIP-relative address found in .data/.rdata";
    return result;
}

OffsetResult OffsetDumper::ExtractFNamePool(const std::vector<uint32_t>& hits,
                                            SigScanner& scanner,
                                            InstructionDecoder& decoder,
                                            PEParser& pe) {
    OffsetResult result;
    result.name = "FNamePool";

    // FNV-1a hash constants indicate hashing code
    // Need to find nearby pointer references
    for (uint32_t rva : hits) {
        const auto& moduleData = scanner.GetModuleData();

        // Scan backwards for MOV/LEA instructions
        for (int32_t offset = -50; offset < 50; offset += 1) {
            int32_t checkRva = rva + offset;
            if (checkRva < 0 || checkRva + 15 > static_cast<int32_t>(moduleData.size())) continue;

            auto instr = decoder.Decode(moduleData.data() + checkRva, 15, checkRva);
            if (!instr.valid) continue;

            if (decoder.HasRipRelative(instr)) {
                uint64_t address = decoder.GetRipRelativeAddress(instr, 1);
                uint32_t targetRva = static_cast<uint32_t>(address - pe.GetBaseAddress());

                if (pe.IsInData(targetRva) || pe.IsInRData(targetRva)) {
                    result.found = true;
                    result.address = address;
                    result.rva = targetRva;
                    result.details = "Found near FNV-1a constants";
                    return result;
                }
            }
        }
    }

    result.found = false;
    result.details = "No valid pointer found near hash constants";
    return result;
}

OffsetResult OffsetDumper::ExtractChunkDecrypt(const std::vector<uint32_t>& hits,
                                               SigScanner& scanner,
                                               InstructionDecoder& decoder,
                                               PEParser& pe) {
    OffsetResult result;
    result.name = "ChunkDecrypt";

    // PEB access (gs:0x60) indicates TLS or thread-local usage
    for (uint32_t rva : hits) {
        const auto& moduleData = scanner.GetModuleData();

        // Scan forward for decrypt keys
        for (uint32_t offset = 0; offset < 100; offset += 1) {
            uint32_t checkRva = rva + offset;
            if (checkRva + 15 > moduleData.size()) continue;

            auto instr = decoder.Decode(moduleData.data() + checkRva, 15, checkRva);
            if (!instr.valid) continue;

            if (decoder.HasRipRelative(instr)) {
                uint64_t address = decoder.GetRipRelativeAddress(instr, 1);
                uint32_t targetRva = static_cast<uint32_t>(address - pe.GetBaseAddress());

                if (pe.IsInRData(targetRva)) {
                    result.found = true;
                    result.address = address;
                    result.rva = targetRva;
                    result.details = "Found after PEB access";
                    return result;
                }
            }
        }
    }

    result.found = false;
    result.details = "No valid key found after PEB access";
    return result;
}

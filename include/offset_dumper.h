#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <memory>

class IMemoryReader;
class PEParser;
class SigScanner;
class InstructionDecoder;
struct DecodedInstruction;

/**
 * Result of an offset discovery
 */
struct OffsetResult {
    std::string name;
    uint64_t address;
    uint32_t rva;
    std::string details;
    bool found;

    OffsetResult() : address(0), rva(0), found(false) {}
};

/**
 * Pattern definition for offset discovery
 */
struct OffsetPattern {
    std::string name;
    std::string signature;
    std::function<OffsetResult(const std::vector<uint32_t>&, SigScanner&, InstructionDecoder&, PEParser&)> extractor;
};

/**
 * Main Offset Dumper Orchestrator
 * Combines all three layers to auto-discover game offsets
 */
class OffsetDumper {
private:
    std::unique_ptr<IMemoryReader> m_memory;
    std::unique_ptr<PEParser> m_peParser;
    std::unique_ptr<SigScanner> m_scanner;
    std::unique_ptr<InstructionDecoder> m_decoder;

    uint64_t m_baseAddress;
    std::vector<OffsetPattern> m_patterns;
    std::vector<OffsetResult> m_results;

    bool m_initialized;

    /**
     * Register built-in patterns for common game offsets
     */
    void RegisterDefaultPatterns();

public:
    OffsetDumper(uint64_t baseAddress = 0x140000000);
    ~OffsetDumper();

    /**
     * Initialize with memory reader
     * @param memory Custom memory reader implementation
     * @return true if successful
     */
    bool Initialize(std::unique_ptr<IMemoryReader> memory);

    /**
     * Register a custom pattern
     */
    void RegisterPattern(const OffsetPattern& pattern);

    /**
     * Run all registered patterns and discover offsets
     * @return true if at least one offset was found
     */
    bool Discover();

    /**
     * Get all results
     */
    const std::vector<OffsetResult>& GetResults() const { return m_results; }

    /**
     * Get specific result by name
     */
    const OffsetResult* GetResult(const std::string& name) const;

    /**
     * Print results to stdout
     */
    void PrintResults() const;

    /**
     * Export results to JSON file
     */
    bool ExportToJson(const std::string& filename) const;

    /**
     * Check if initialized
     */
    bool IsInitialized() const { return m_initialized; }

    // Built-in extractor functions
    static OffsetResult ExtractGObjects(const std::vector<uint32_t>& hits, SigScanner& scanner,
                                        InstructionDecoder& decoder, PEParser& pe);
    static OffsetResult ExtractFNamePool(const std::vector<uint32_t>& hits, SigScanner& scanner,
                                         InstructionDecoder& decoder, PEParser& pe);
    static OffsetResult ExtractChunkDecrypt(const std::vector<uint32_t>& hits, SigScanner& scanner,
                                            InstructionDecoder& decoder, PEParser& pe);
};

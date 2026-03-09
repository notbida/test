#include "offset_dumper.h"
#include "memory.h"
#include <iostream>
#include <fstream>
#include <vector>
#include <cstring>

void PrintUsage(const char* programName) {
    std::cout << "Automated Game Offset Dumper\n";
    std::cout << "========================================\n\n";
    std::cout << "Usage:\n";
    std::cout << "  " << programName << " --demo                       Run demo with test data\n";
    std::cout << "  " << programName << " --file <path> [base_addr]    Analyze PE file\n";
#ifdef PLATFORM_WINDOWS
    std::cout << "  " << programName << " --pid <pid> [base_addr]      Attach to process\n";
#endif
    std::cout << "\nOptions:\n";
    std::cout << "  --output <file>      Export results to JSON file\n";
    std::cout << "  --base <address>     Set base address (default: 0x140000000)\n";
    std::cout << "\nExamples:\n";
    std::cout << "  " << programName << " --demo\n";
    std::cout << "  " << programName << " --file game.exe --output offsets.json\n";
#ifdef PLATFORM_WINDOWS
    std::cout << "  " << programName << " --pid 1234 --base 0x7FF700000000\n";
#endif
}

// Create demo data with embedded patterns
std::vector<uint8_t> CreateDemoData() {
    std::vector<uint8_t> data(10 * 1024 * 1024, 0x90); // 10MB of NOPs

    // Fake PE header at start
    data[0] = 'M'; data[1] = 'Z'; // DOS signature
    uint32_t e_lfanew = 0x100;
    std::memcpy(data.data() + 0x3C, &e_lfanew, 4);

    // PE signature at 0x100
    uint32_t peSig = 0x00004550; // "PE\0\0"
    std::memcpy(data.data() + 0x100, &peSig, 4);

    // COFF header
    uint16_t machine = 0x8664; // AMD64
    uint16_t numSections = 3;
    std::memcpy(data.data() + 0x104, &machine, 2);
    std::memcpy(data.data() + 0x106, &numSections, 2);

    // Optional header size
    uint16_t optHeaderSize = 0xF0; // 240 bytes for PE32+
    std::memcpy(data.data() + 0x114, &optHeaderSize, 2);

    // SizeOfImage in optional header
    uint32_t sizeOfImage = data.size();
    std::memcpy(data.data() + 0x100 + 24 + 56, &sizeOfImage, 4);

    // Section headers start at e_lfanew + 24 + optHeaderSize
    uint64_t sectionHeaderOffset = 0x100 + 24 + 0xF0;

    // .text section
    {
        uint8_t secHeader[40] = {0};
        std::memcpy(secHeader, ".text\0\0\0", 8);
        uint32_t vsize = 0x800000;
        uint32_t vaddr = 0x1000;
        uint32_t chars = 0x60000020; // CODE | EXECUTE | READ
        std::memcpy(secHeader + 8, &vsize, 4);
        std::memcpy(secHeader + 12, &vaddr, 4);
        std::memcpy(secHeader + 36, &chars, 4);
        std::memcpy(data.data() + sectionHeaderOffset, secHeader, 40);
    }

    // .rdata section
    {
        uint8_t secHeader[40] = {0};
        std::memcpy(secHeader, ".rdata\0\0", 8);
        uint32_t vsize = 0x100000;
        uint32_t vaddr = 0x801000;
        uint32_t chars = 0x40000040; // INITIALIZED_DATA | READ
        std::memcpy(secHeader + 8, &vsize, 4);
        std::memcpy(secHeader + 12, &vaddr, 4);
        std::memcpy(secHeader + 36, &chars, 4);
        std::memcpy(data.data() + sectionHeaderOffset + 40, secHeader, 40);
    }

    // .data section
    {
        uint8_t secHeader[40] = {0};
        std::memcpy(secHeader, ".data\0\0\0", 8);
        uint32_t vsize = 0x100000;
        uint32_t vaddr = 0x901000;
        uint32_t chars = 0xC0000040; // INITIALIZED_DATA | READ | WRITE
        std::memcpy(secHeader + 8, &vsize, 4);
        std::memcpy(secHeader + 12, &vaddr, 4);
        std::memcpy(secHeader + 36, &chars, 4);
        std::memcpy(data.data() + sectionHeaderOffset + 80, secHeader, 40);
    }

    // Embed GObjects pattern in .text section
    uint32_t gobjectsOffset = 0x5000;
    uint8_t gobjectsPattern[] = {0xF2, 0x0F, 0x70, 0x05, 0x12, 0x34, 0x56, 0x78, 0xAB,
                                  0x66, 0x0F, 0xEF, 0x05, 0x22, 0x33, 0x44, 0x55};
    std::memcpy(data.data() + gobjectsOffset, gobjectsPattern, sizeof(gobjectsPattern));

    // Embed FNamePool pattern
    uint32_t fnameOffset = 0x6000;
    uint8_t fnamePattern[] = {0x69, 0xD2, 0x93, 0x01, 0x00, 0x01,
                              0x81, 0xC2, 0x89, 0x6C, 0x70, 0x69};
    std::memcpy(data.data() + fnameOffset, fnamePattern, sizeof(fnamePattern));

    // Embed ChunkDecrypt pattern
    uint32_t chunkOffset = 0x7000;
    uint8_t chunkPattern[] = {0x65, 0x48, 0x03, 0x04, 0x25, 0x60, 0x00, 0x00, 0x00};
    std::memcpy(data.data() + chunkOffset, chunkPattern, sizeof(chunkPattern));

    std::cout << "[Demo] Created test data with embedded patterns:\n";
    std::cout << "  - GObjects pattern at 0x" << std::hex << gobjectsOffset << "\n";
    std::cout << "  - FNamePool pattern at 0x" << fnameOffset << "\n";
    std::cout << "  - ChunkDecrypt pattern at 0x" << chunkOffset << "\n" << std::dec;

    return data;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        PrintUsage(argv[0]);
        return 1;
    }

    std::string mode = argv[1];
    uint64_t baseAddress = 0x140000000;
    std::string outputFile;

    // Parse arguments
    for (int i = 2; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--output" && i + 1 < argc) {
            outputFile = argv[++i];
        } else if (arg == "--base" && i + 1 < argc) {
            baseAddress = std::strtoull(argv[++i], nullptr, 16);
        }
    }

    std::cout << "========================================\n";
    std::cout << "Automated Game Offset Dumper\n";
    std::cout << "========================================\n\n";

    OffsetDumper dumper(baseAddress);

    if (mode == "--demo") {
        std::cout << "[*] Running demo mode with synthetic data\n\n";

        auto demoData = CreateDemoData();
        auto memory = std::make_unique<MockMemoryReader>(demoData, baseAddress);

        if (!dumper.Initialize(std::move(memory))) {
            std::cerr << "Failed to initialize dumper\n";
            return 1;
        }

        dumper.Discover();
        dumper.PrintResults();

        if (!outputFile.empty()) {
            if (dumper.ExportToJson(outputFile)) {
                std::cout << "\n[+] Results exported to: " << outputFile << "\n";
            } else {
                std::cerr << "\n[!] Failed to export results\n";
            }
        }

    } else if (mode == "--file" && argc >= 3) {
        std::string filename = argv[2];
        std::cout << "[*] Analyzing PE file: " << filename << "\n\n";

        // Read file into memory
        std::ifstream file(filename, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            std::cerr << "Failed to open file: " << filename << "\n";
            return 1;
        }

        size_t fileSize = file.tellg();
        file.seekg(0, std::ios::beg);

        std::vector<uint8_t> fileData(fileSize);
        file.read(reinterpret_cast<char*>(fileData.data()), fileSize);
        file.close();

        auto memory = std::make_unique<MockMemoryReader>(fileData, baseAddress);

        if (!dumper.Initialize(std::move(memory))) {
            std::cerr << "Failed to initialize dumper\n";
            return 1;
        }

        dumper.Discover();
        dumper.PrintResults();

        if (!outputFile.empty()) {
            if (dumper.ExportToJson(outputFile)) {
                std::cout << "\n[+] Results exported to: " << outputFile << "\n";
            } else {
                std::cerr << "\n[!] Failed to export results\n";
            }
        }

#ifdef PLATFORM_WINDOWS
    } else if (mode == "--pid" && argc >= 3) {
        uint32_t pid = std::strtoul(argv[2], nullptr, 10);
        std::cout << "[*] Attaching to process ID: " << pid << "\n\n";

        auto memory = std::make_unique<WindowsMemoryReader>(pid);

        if (!dumper.Initialize(std::move(memory))) {
            std::cerr << "Failed to initialize dumper (check process access rights)\n";
            return 1;
        }

        dumper.Discover();
        dumper.PrintResults();

        if (!outputFile.empty()) {
            if (dumper.ExportToJson(outputFile)) {
                std::cout << "\n[+] Results exported to: " << outputFile << "\n";
            } else {
                std::cerr << "\n[!] Failed to export results\n";
            }
        }
#endif

    } else {
        PrintUsage(argv[0]);
        return 1;
    }

    return 0;
}

# Automated Game Offset Dumper

A fully automated offset discovery system that scans game executables for instruction patterns and extracts decrypt keys/offsets without manual reversing. No more spending hours in IDA after every patch!

## Overview

This tool implements a three-layer approach to automatically discover game offsets:

1. **Layer 1: PE Headers** - Parse PE headers to find valid memory regions
2. **Layer 2: AOB Scanning** - IDA-style pattern scanning with wildcards
3. **Layer 3: Zydis Decoder** - Instruction disassembly to extract actual values

## Features

- ✅ **Fully automated offset discovery** - Run once after each game patch
- ✅ **PE header parsing** - Dynamic section boundary detection
- ✅ **IDA-style pattern scanning** - Support for wildcards like `F2 0F ?? ?? ??`
- ✅ **Zydis instruction decoding** - Extract immediates, RIP-relative addresses, operands
- ✅ **Built-in patterns** - Pre-configured for common game offsets (GObjects, FNamePool, etc.)
- ✅ **Extensible** - Easy to add custom patterns
- ✅ **JSON export** - Save discovered offsets to file
- ✅ **Cross-platform** - Windows and Linux support

## Quick Start

### Build

```bash
mkdir build && cd build
cmake ..
cmake --build .
```

### Run Demo

```bash
./offset_dumper --demo
```

This runs with synthetic test data to demonstrate the functionality.

### Analyze PE File

```bash
./offset_dumper --file game.exe --output offsets.json
```

### Attach to Process (Windows)

```bash
./offset_dumper --pid 1234 --base 0x7FF700000000 --output offsets.json
```

## How It Works

### Layer 1: PE Header Parsing

Instead of hardcoding section boundaries (which break every patch), the tool dynamically parses PE headers from memory:

```cpp
// Read DOS header to find PE signature offset
uint32_t e_lfanew = 0;
mem.Read(gameBase + 0x3C, &e_lfanew, 4);

// Verify PE signature
uint32_t peSig = 0;
mem.Read(gameBase + e_lfanew, &peSig, 4);  // Must be 0x50450000 ("PE\0\0")

// Parse section headers to get .text, .rdata, .data boundaries
```

This gives you dynamic section boundaries to filter garbage hits.

### Layer 2: AOB (Array of Bytes) Scanning

IDA-style patterns with wildcards:

```
F2 0F 70 05 ?? ?? ?? ?? ?? 66 0F EF 05 ?? ?? ?? ??
│  │  │  │  └ wildcard     │  │  │  │  └ wildcard
│  │  │  │    (4 bytes)     │  │  │  │    (4 bytes)
│  │  │  └ PSHUFLW RIP      │  │  │  └ PXOR RIP
```

The scanner caches the entire module once for fast repeated scanning:

```cpp
// Read entire exe into local buffer ONCE
bool Initialize(uint64_t moduleSize);

// Scan for pattern - instant after caching
std::vector<uint32_t> ScanStr("F2 0F 70 05 ?? ?? ?? ??");
```

### Layer 3: Zydis Instruction Decoding

Uses Zydis v4 to decode x86-64 instructions and extract operands:

```cpp
// Decode instruction at RVA
auto decoded = decoder.Decode(data, maxLength, rva);

// Extract RIP-relative address
uint64_t address = decoder.GetRipRelativeAddress(decoded, operandIndex);

// Extract immediate values
uint64_t immediate = decoder.GetImmediate(decoded, operandIndex);
```

No hand-parsing x86 instructions - Zydis handles all the encoding complexity.

## Built-in Patterns

### GObjects

**Pattern:** `F2 0F 70 05 ?? ?? ?? ?? ?? 66 0F EF 05 ?? ?? ?? ??`

Matches PSHUFLW + PXOR (common in SIMD decrypt routines). Extracts the RIP-relative address pointing to the GObjects pointer.

### FNamePool

**Pattern:** `69 D2 93 01 00 01 81 C2 89 6C 70 69`

Matches FNV-1a hash constants. Scans nearby code for pointer references.

### ChunkDecrypt

**Pattern:** `65 48 03 04 25 60 00 00 00`

Matches PEB access (gs:0x60). Scans forward for decrypt keys in .rdata.

## Custom Patterns

You can easily add custom patterns:

```cpp
dumper.RegisterPattern({
    "MyCustomOffset",
    "48 8B 05 ?? ?? ?? ?? 48 85 C0",  // Pattern
    [](const std::vector<uint32_t>& hits, SigScanner& scanner,
       InstructionDecoder& decoder, PEParser& pe) -> OffsetResult {

        OffsetResult result;
        result.name = "MyCustomOffset";

        for (uint32_t rva : hits) {
            // Decode instruction and extract offset
            auto instr = decoder.Decode(scanner.GetModuleData().data() + rva, 15, rva);

            if (decoder.HasRipRelative(instr)) {
                uint64_t address = decoder.GetRipRelativeAddress(instr, 1);
                uint32_t targetRva = static_cast<uint32_t>(address - pe.GetBaseAddress());

                // Validate section
                if (pe.IsInData(targetRva)) {
                    result.found = true;
                    result.address = address;
                    result.rva = targetRva;
                    return result;
                }
            }
        }

        result.found = false;
        result.details = "No valid match found";
        return result;
    }
});
```

## Architecture

```
┌──────────────────────────────────────────────────────┐
│ Layer 1: PE Headers (find valid memory regions)      │
│ - Parse PE headers from target process               │
│ - Get .rdata / .data / .text bounds                  │
│ - Filter out garbage hits later                      │
└────────────────────┬─────────────────────────────────┘
                     │
┌────────────────────▼─────────────────────────────────┐
│ Layer 2: AOB Scanning (find candidate locations)     │
│ - IDA-style patterns like "F2 0F ?? ?? ??"          │
│ - Scans whole exe for matches                        │
│ - Returns list of RVAs                               │
└────────────────────┬─────────────────────────────────┘
                     │
┌────────────────────▼─────────────────────────────────┐
│ Layer 3: Zydis Decoder (extract actual values)       │
│ - Decode instructions at each hit                    │
│ - Pull out immediates, RIP offsets, operands         │
│ - This is where you get the final decrypt params     │
└──────────────────────────────────────────────────────┘
```

## Example Output

```
========================================
Starting offset discovery...
========================================

[*] Searching for: GObjects
    Pattern: F2 0F 70 05 ?? ?? ?? ?? ?? 66 0F EF 05 ?? ?? ?? ??
    [+] Found 2 potential matches
    [+] SUCCESS: 0x7ff7a0801234 (RVA: 0x801234)
    [+] Details: Found via PSHUFLW RIP-relative addressing

[*] Searching for: FNamePool
    Pattern: 69 D2 93 01 00 01 81 C2 89 6C 70 69
    [+] Found 1 potential matches
    [+] SUCCESS: 0x7ff7a0902000 (RVA: 0x902000)
    [+] Details: Found near FNV-1a constants

========================================
Discovery complete!
========================================
```

## Dependencies

- **CMake 3.15+**
- **C++17 compiler** (MSVC, GCC, Clang)
- **Zydis v4.1.0** (automatically fetched via CMake)

## Platform Support

- **Windows**: ReadProcessMemory API for live process reading
- **Linux**: process_vm_readv for live process reading
- **Both**: File-based analysis using MockMemoryReader

## Performance

- Module caching: 30-60 seconds to read entire module once
- Pattern scanning: < 1 second per pattern after caching
- Full discovery: ~1-2 minutes for typical game executable

## Tips

1. **Run after every patch** - Patterns survive most updates, addresses don't
2. **Validate sections** - Always check if extracted addresses are in .data/.rdata
3. **Use context** - Look at surrounding instructions for better hit validation
4. **Export to JSON** - Integrate with your external tools/injectors
5. **Add more patterns** - The more patterns you have, the more robust your discovery

## Troubleshooting

### "Failed to parse PE headers"
- Check base address is correct
- Verify process has proper permissions

### "No matches found"
- Pattern may have changed in new version
- Try broadening wildcards
- Check pattern against IDA/x64dbg first

### "Found but wrong address"
- Add section validation in extractor
- Check nearby instructions for better context
- Use PE parser to validate RVA is in expected section

## License

MIT License - Use freely for security research and game modding

## Contributing

PRs welcome! Especially for:
- New built-in patterns
- Additional extractor strategies
- Performance improvements
- Linux testing

## Credits

Built with:
- [Zydis](https://github.com/zyantific/zydis) - Fast x86/x64 disassembler
- Inspired by countless hours in IDA Pro

---

**No more IDA hell every update!** 🎉

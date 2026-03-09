#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <Zydis/Zydis.h>

/**
 * Decoded instruction information
 */
struct DecodedInstruction {
    ZydisDecodedInstruction instruction;
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
    uint64_t runtimeAddress;
    uint32_t rva;

    bool valid;

    DecodedInstruction() : runtimeAddress(0), rva(0), valid(false) {}
};

/**
 * Instruction Decoder - Layer 3
 * Uses Zydis to decode x86-64 instructions and extract operands
 */
class InstructionDecoder {
private:
    ZydisDecoder m_decoder;
    ZydisFormatter m_formatter;
    uint64_t m_baseAddress;

public:
    InstructionDecoder(uint64_t baseAddress);

    /**
     * Decode instruction at given RVA
     * @param data Pointer to instruction bytes
     * @param maxLength Maximum bytes to decode
     * @param rva RVA of the instruction
     * @return Decoded instruction information
     */
    DecodedInstruction Decode(const uint8_t* data, size_t maxLength, uint32_t rva);

    /**
     * Extract immediate value from instruction
     * @param decoded Decoded instruction
     * @param operandIndex Which operand to extract (default 0)
     * @return Immediate value, or 0 if not found
     */
    uint64_t GetImmediate(const DecodedInstruction& decoded, size_t operandIndex = 0);

    /**
     * Extract RIP-relative address from instruction
     * Used for patterns like "mov rax, [rip + offset]"
     * @param decoded Decoded instruction
     * @param operandIndex Which operand to extract (default 0)
     * @return Calculated absolute address
     */
    uint64_t GetRipRelativeAddress(const DecodedInstruction& decoded, size_t operandIndex = 0);

    /**
     * Extract RIP-relative offset (before calculation)
     * @param decoded Decoded instruction
     * @param operandIndex Which operand to extract
     * @return RIP-relative offset value
     */
    int64_t GetRipRelativeOffset(const DecodedInstruction& decoded, size_t operandIndex = 0);

    /**
     * Format instruction to human-readable string
     * @param decoded Decoded instruction
     * @return Formatted instruction string
     */
    std::string Format(const DecodedInstruction& decoded);

    /**
     * Check if instruction has RIP-relative operand
     */
    bool HasRipRelative(const DecodedInstruction& decoded);

    /**
     * Get instruction length
     */
    uint8_t GetLength(const DecodedInstruction& decoded);
};

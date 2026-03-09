#include "instruction_decoder.h"
#include <iostream>
#include <cstring>

InstructionDecoder::InstructionDecoder(uint64_t baseAddress)
    : m_baseAddress(baseAddress) {
    // Initialize Zydis decoder for x86-64
    ZydisDecoderInit(&m_decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);

    // Initialize formatter for Intel syntax
    ZydisFormatterInit(&m_formatter, ZYDIS_FORMATTER_STYLE_INTEL);
}

DecodedInstruction InstructionDecoder::Decode(const uint8_t* data, size_t maxLength, uint32_t rva) {
    DecodedInstruction result;
    result.rva = rva;
    result.runtimeAddress = m_baseAddress + rva;

    // Decode instruction
    ZyanStatus status = ZydisDecoderDecodeFull(&m_decoder, data, maxLength,
                                                &result.instruction, result.operands);

    if (ZYAN_SUCCESS(status)) {
        result.valid = true;
    } else {
        result.valid = false;
    }

    return result;
}

uint64_t InstructionDecoder::GetImmediate(const DecodedInstruction& decoded, size_t operandIndex) {
    if (!decoded.valid || operandIndex >= decoded.instruction.operand_count) {
        return 0;
    }

    const ZydisDecodedOperand& operand = decoded.operands[operandIndex];

    if (operand.type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
        if (operand.imm.is_signed) {
            return static_cast<uint64_t>(operand.imm.value.s);
        } else {
            return operand.imm.value.u;
        }
    }

    return 0;
}

uint64_t InstructionDecoder::GetRipRelativeAddress(const DecodedInstruction& decoded, size_t operandIndex) {
    if (!decoded.valid || operandIndex >= decoded.instruction.operand_count) {
        return 0;
    }

    const ZydisDecodedOperand& operand = decoded.operands[operandIndex];

    if (operand.type == ZYDIS_OPERAND_TYPE_MEMORY && operand.mem.base == ZYDIS_REGISTER_RIP) {
        // Calculate: RIP + instruction_length + displacement
        uint64_t nextInstruction = decoded.runtimeAddress + decoded.instruction.length;
        int64_t displacement = operand.mem.disp.value;
        return nextInstruction + displacement;
    }

    return 0;
}

int64_t InstructionDecoder::GetRipRelativeOffset(const DecodedInstruction& decoded, size_t operandIndex) {
    if (!decoded.valid || operandIndex >= decoded.instruction.operand_count) {
        return 0;
    }

    const ZydisDecodedOperand& operand = decoded.operands[operandIndex];

    if (operand.type == ZYDIS_OPERAND_TYPE_MEMORY && operand.mem.base == ZYDIS_REGISTER_RIP) {
        return operand.mem.disp.value;
    }

    return 0;
}

std::string InstructionDecoder::Format(const DecodedInstruction& decoded) {
    if (!decoded.valid) {
        return "<invalid>";
    }

    char buffer[256];
    ZydisFormatterFormatInstruction(&m_formatter, &decoded.instruction,
                                    decoded.operands, decoded.instruction.operand_count_visible,
                                    buffer, sizeof(buffer), decoded.runtimeAddress, ZYAN_NULL);

    return std::string(buffer);
}

bool InstructionDecoder::HasRipRelative(const DecodedInstruction& decoded) {
    if (!decoded.valid) {
        return false;
    }

    for (size_t i = 0; i < decoded.instruction.operand_count; i++) {
        const ZydisDecodedOperand& operand = decoded.operands[i];
        if (operand.type == ZYDIS_OPERAND_TYPE_MEMORY && operand.mem.base == ZYDIS_REGISTER_RIP) {
            return true;
        }
    }

    return false;
}

uint8_t InstructionDecoder::GetLength(const DecodedInstruction& decoded) {
    return decoded.valid ? decoded.instruction.length : 0;
}

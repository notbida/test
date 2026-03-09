# Common Game Engine Patterns

This document contains useful patterns for common game engines and frameworks.

## Unreal Engine

### GObjects (SIMD Decrypt)

```
Pattern: F2 0F 70 05 ?? ?? ?? ?? ?? 66 0F EF 05 ?? ?? ?? ??
Description: PSHUFLW + PXOR instruction sequence
Location: Typically in .text section
Extract: RIP-relative address from PSHUFLW operand
Validates: Should point to .data or .rdata
```

### FNamePool (Hash Constants)

```
Pattern: 69 D2 93 01 00 01 81 C2 89 6C 70 69
Description: FNV-1a hash multiplication and addition constants
Location: .text section (hashing code)
Extract: Scan nearby for MOV/LEA with RIP-relative addressing
Validates: Pointer should be in .data section
```

### GWorld Pointer

```
Pattern: 48 8B 05 ?? ?? ?? ?? 48 85 C0 74 ?? 48 8B 88 ?? ?? ?? ??
Description: mov rax, [rip+offset]; test rax, rax; jz; mov rcx, [rax+offset]
Location: .text section
Extract: RIP-relative from first MOV instruction
Validates: Should point to .data
```

### UObject::ProcessEvent

```
Pattern: 40 55 56 57 41 54 41 55 41 56 41 57 48 81 EC ?? ?? ?? ?? 48 8D 6C 24
Description: Function prologue with stack allocation
Location: .text section
Extract: Function RVA directly
Note: May need to find via vtable or name references
```

## Unity Engine

### GameObjectManager

```
Pattern: 48 8B 0D ?? ?? ?? ?? 48 85 C9 74 ?? E8 ?? ?? ?? ?? 48 8B 0D
Description: mov rcx, [rip+offset]; test rcx, rcx; jz; call; mov rcx, [rip+...]
Location: .text section
Extract: RIP-relative from first MOV
Validates: Should be in .data
```

### Il2CppClass Metadata

```
Pattern: 48 89 5C 24 ?? 57 48 83 EC 20 48 8B DA 48 8B F9 E8 ?? ?? ?? ?? 48 8B D3
Description: Common Il2Cpp class access pattern
Location: .text section
Extract: Function start
```

## Source Engine

### Client Class Head

```
Pattern: 8B 0D ?? ?? ?? ?? 8B 01 FF 50 ?? 85 C0 75 ?? 8B 0D
Description: mov ecx, [offset]; mov eax, [ecx]; call [eax+offset]; test eax, eax
Location: .text section
Extract: Absolute address from first MOV (32-bit)
```

### Global Entity List

```
Pattern: 48 8B 0D ?? ?? ?? ?? 48 8B 14 D1 48 85 D2
Description: mov rcx, [rip+offset]; mov rdx, [rcx+rdx*8]; test rdx, rdx
Location: .text section
Extract: RIP-relative from first MOV
Validates: Should be in .data
```

## CryEngine

### Entity System

```
Pattern: 48 8B 0D ?? ?? ?? ?? 48 85 C9 74 ?? 48 8B 01 FF 90 ?? ?? ?? ??
Description: mov rcx, [rip+offset]; test rcx, rcx; jz; mov rax, [rcx]; call [rax+offset]
Location: .text section
Extract: RIP-relative from first MOV
```

## General Patterns

### Virtual Function Table

```
Pattern: 48 8D 05 ?? ?? ?? ?? 48 89 01 48 89 51 08 C3
Description: lea rax, [rip+offset]; mov [rcx], rax; mov [rcx+8], rdx; ret
Location: Constructor function
Extract: RIP-relative from LEA
Validates: Should point to .rdata (vtable location)
```

### Singleton Instance

```
Pattern: 48 89 5C 24 ?? 48 89 74 24 ?? 57 48 83 EC 20 48 8B 05 ?? ?? ?? ??
Description: Common singleton getter pattern
Location: .text section
Extract: RIP-relative from MOV near end
```

### String Pool Reference

```
Pattern: 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ??
Description: lea rdx, [rip+offset]; lea rcx, [rip+offset]; call
Location: .text section
Extract: Both RIP-relative addresses
Validates: First should be in .rdata (string data)
```

### TLS (Thread Local Storage) Access

```
Pattern: 65 48 8B 04 25 ?? ?? ?? ?? 48 8B 88 ?? ?? ?? ??
Description: mov rax, gs:[offset]; mov rcx, [rax+offset]
Location: .text section
Extract: Both offsets (TLS index and struct offset)
```

### Decrypt Key in Data Section

```
Pattern: 48 8B 05 ?? ?? ?? ?? 48 33 C1 48 89 44 24 ?? 48 8B 39
Description: mov rax, [rip+offset]; xor rax, rcx; mov [rsp+offset], rax
Location: .text section, near security cookies
Extract: RIP-relative address
Validates: Should be in .data or .rdata
```

## Anti-Cheat Patterns

### Heartbeat Function

```
Pattern: 48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 57 48 83 EC 20 48 8B 3D
Description: Common heartbeat/check-in function prologue
Location: .text section
Extract: RIP-relative pointer near end
```

### Integrity Check

```
Pattern: 48 8D 0D ?? ?? ?? ?? BA ?? ?? ?? ?? E8 ?? ?? ?? ?? 85 C0 0F 85
Description: lea rcx, [rip+offset]; mov edx, size; call check_func; test eax, eax; jnz
Location: .text section
Extract: Target address from LEA, size from MOV
```

## Tips for Finding Patterns

1. **Use IDA's "Search > Sequence of bytes"** with wildcards
2. **Look for unique instruction combinations** (SIMD, TLS, specific constants)
3. **Anchor patterns with fixed bytes** at start and end
4. **Use enough wildcards** for offsets that change between compilations
5. **Validate hits** by checking what section they point to
6. **Test on multiple versions** to ensure pattern stability

## Pattern Writing Best Practices

1. **Include context** - Don't just match one instruction
2. **Use fixed prefixes** - SIMD prefixes (F2 0F, 66 0F) are stable
3. **Wildcard all offsets** - Any immediate or displacement
4. **Keep 8-12 bytes minimum** - Shorter patterns have false positives
5. **Document what you're matching** - Future you will thank you
6. **Test the extractor** - Ensure it handles edge cases

## Common Mistakes

- ❌ Pattern too short (< 8 bytes) → Too many false positives
- ❌ Including function-specific offsets → Breaks on recompile
- ❌ Not validating section → Matches garbage data
- ❌ Hardcoded immediate values → Breaks when constants change
- ✅ Use wildcards liberally for offsets and immediates
- ✅ Validate extracted addresses against PE sections
- ✅ Test on multiple game versions

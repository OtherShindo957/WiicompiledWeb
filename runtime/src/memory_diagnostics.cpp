#include "memory.h"
#include "memory_diagnostics.h"
#include "ppc_runtime.h"
#include "runtime_log.h"
#include "system_bridge.h"
#include <array>
#include <iomanip>
#include <iostream>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#endif

void ReportUnmappedGuestAccess(uint32_t address, size_t length) {
    // Emit extra context to help diagnose early-boot accesses that miss the map.
    if (auto* cpu = TryGetCpuContext()) {
        const uint32_t active = RecompMod::CurrentTranslatedExecutionAddress();
        RT_LOG(RT_TAG_MEMORY) << "AccessViolation ctx=" << cpu
                  << " pc=0x" << std::hex << cpu->pc
                  << " active=0x" << active
                  << " lr=0x" << cpu->lr
                  << " r1=0x" << cpu->gpr[1]
                  << " r8=0x" << cpu->gpr[8]
                  << " r9=0x" << cpu->gpr[9]
                  << " r12=0x" << cpu->gpr[12]
                  << " r30=0x" << cpu->gpr[30]
                  << " r31=0x" << cpu->gpr[31]
                  << std::dec
                  << " addr=0x" << std::hex << address
                  << " len=" << length << std::dec
                  << " reason=no mapped region" << std::endl;
    } else {
        RT_LOG(RT_TAG_MEMORY) << "AccessViolation ctx=(null) addr=0x" << std::hex << address
                  << " len=" << length << std::dec << " reason=no mapped region" << std::endl;
    }
    RT_LOG(RT_TAG_MEMORY) << "===== DUMPING CPU STATE =====" << std::endl;
    SystemBridge::DumpCpuState(TryGetCpuContext());
    // Hexdump guest memory around pointer-carrying registers so a corrupted
    // structure's surroundings (e.g. ASCII sprayed over a link pointer) are
    // visible in the report without a debugger attached.
    if (auto* cpu = TryGetCpuContext()) {
        for (const int reg : {4, 5, 6, 7, 8, 26, 27, 28, 29, 30, 31}) {
            const uint32_t base = cpu->gpr[reg];
            if (base < 0x80000000u || base >= 0x94000000u)
                continue;
            const uint32_t start = (base - 0x40u) & ~0xFu;
            RT_LOG(RT_TAG_MEMORY) << "hexdump around r" << reg << "=0x" << std::hex << base << ":" << std::endl;
            for (uint32_t row = 0; row < 16; ++row) {
                const uint32_t rowAddr = start + row * 16u;
                RT_LOG(RT_TAG_MEMORY) << "  0x" << std::hex << rowAddr << ":";
                char ascii[17] = {};
                for (uint32_t i = 0; i < 16; ++i) {
                    uint8_t byte = 0;
                    if (!MemoryInline::TryReadGuestScalar(rowAddr + i, byte)) {
                        std::cerr << " ??";
                        ascii[i] = '?';
                        continue;
                    }
                    std::cerr << " " << std::setw(2) << std::setfill('0') << static_cast<uint32_t>(byte);
                    ascii[i] = (byte >= 0x20 && byte < 0x7F) ? static_cast<char>(byte) : '.';
                }
                std::cerr << "  |" << ascii << "|" << std::dec << std::setfill(' ') << std::endl;
            }
        }
    }
#if defined(_WIN32)
    void* frames[32]{};
    const USHORT captured = CaptureStackBackTrace(0, static_cast<DWORD>(std::size(frames)), frames, nullptr);
    const auto imageBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    HANDLE process = GetCurrentProcess();
    static const bool symbolsReady = [] {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
        if (SymInitialize(GetCurrentProcess(), nullptr, TRUE) != FALSE)
            return true;
        // The runtime crash reporter may already own the process-wide DbgHelp
        // session. Reuse it rather than treating ERROR_INVALID_PARAMETER as
        // symbol unavailability.
        return GetLastError() == ERROR_INVALID_PARAMETER;
    }();
    RT_LOG(RT_TAG_MEMORY) << "host stack at first invalid guest access:" << std::endl;
    for (USHORT index = 0; index < captured; ++index) {
        const auto addressValue = reinterpret_cast<uintptr_t>(frames[index]);
        RT_LOG(RT_TAG_MEMORY) << "  #" << index << " absolute=0x" << std::hex << addressValue;
        if (imageBase != 0 && addressValue >= imageBase)
            std::cerr << " image+0x" << (addressValue - imageBase);
        std::array<char, sizeof(SYMBOL_INFO) + MAX_SYM_NAME> symbolBuffer{};
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(symbolBuffer.data());
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = MAX_SYM_NAME;
        DWORD64 displacement = 0;
        if (symbolsReady && SymFromAddr(process, addressValue, &displacement, symbol))
            std::cerr << " " << symbol->Name << "+0x" << displacement;
        std::cerr << std::dec << std::endl;
    }
#endif
    std::cerr.flush();
}

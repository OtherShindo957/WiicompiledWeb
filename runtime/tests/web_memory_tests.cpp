// Handwritten data only. Link the real Memory implementation and compact backing.
// GX and mod-policy dependencies are test doubles, not a browser GX/mod port.
#include "memory.h"
#include <array>
#include <cstdio>
#include <limits>
#include <stdexcept>

namespace {
void Require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
template<class F> void Reject(F fn, const char* message) {
    bool rejected = false;
    try { fn(); } catch (const std::exception&) { rejected = true; }
    Require(rejected, message);
}
unsigned fifoWrites = 0;
uint32_t fifoValue = 0;
unsigned guardedWrites = 0;
constexpr uint32_t guardedAddress = 0x80004000;
unsigned deferredCalls = 0;
bool Deferred(void*) { ++deferredCalls; *Memory::GetPointer(0x80007000) = 0x5a; return true; }
bool FailedDeferred(void*) { return false; }
}
namespace RecompMod {
std::atomic<bool> g_executableWriteGuardEnabled{false};
std::atomic<uint8_t> g_executableWriteGuardPages[kExecutableWriteGuardPageCount]{};
std::atomic<uint8_t> g_executableWriteGuardCoarsePages[kExecutableWriteGuardCoarsePageCount]{};
std::atomic<uint8_t> g_executableWriteGuardMidPages[kExecutableWriteGuardMidPageCount]{};
uint32_t CurrentTranslatedExecutionAddress() noexcept { return 0; }
bool HandleExecutableWrite(uint32_t address, size_t length, uint64_t) {
    if (address <= guardedAddress && uint64_t(address) + length > guardedAddress) {
        ++guardedWrites;
        return true;
    }
    return false;
}
void CheckExecutableWrite(uint32_t address, size_t length, uint64_t value) {
    HandleExecutableWrite(address, length, value);
}
}
extern "C" {
void GX_HLE_FIFO_Write8(uint8_t v) { ++fifoWrites; fifoValue = v; }
void GX_HLE_FIFO_Write16(uint16_t v) { ++fifoWrites; fifoValue = v; }
void GX_HLE_FIFO_Write32(uint32_t v) { ++fifoWrites; fifoValue = v; }
void GX_HLE_FIFO_WriteFloat(float) { ++fifoWrites; }
void GX_HLE_FIFO_WriteBurst(const uint8_t*, uint32_t) { ++fifoWrites; }
}

int main() {
    try {
        using GuestFlat::Backing;
        Require(GuestFlat::RequiresCheckedAccess(), "checked backend selected");
        // Validate malformed layouts before initialization; each failure must be atomic.
        Reject([] { GuestFlat::Initialize({{0xfffffff0, 32, Backing::Owned}}); }, "wrapped mapping accepted");
        Reject([] { GuestFlat::Initialize({{0x80000000, 64, Backing::Mem1}, {0x80000020, 64, Backing::Owned}}); }, "overlap accepted");
        Reject([] { GuestFlat::Initialize({{0xcc008000, 16, Backing::Owned}}); }, "MMIO mapped as RAM");
        Reject([] { GuestFlat::Initialize({{0x817ffff0, 32, Backing::Mem1}}); }, "oversized MEM1 accepted");
        Reject([] { GuestFlat::Initialize({{0xa0000000, 16, Backing::Mem1}}); }, "invalid alias accepted");
        Require(!GuestFlat::IsActive(), "failed initialization published state");
        auto config = Memory::Config::WiiDefaults();
        // Exercise sub-page owned mappings and the final valid guest byte too.
        config.regions.push_back({"synthetic tail", 0xfffffff0, 16});
        Memory::Init(config);
        Require(GuestFlat::IsActive(), "backing not initialized");
        Require(Memory::GetPointer(0) == Memory::GetPointer(0x80000000) &&
                Memory::GetPointer(0) == Memory::GetPointer(0xc0000000), "MEM1 aliases copied instead of shared");
        Require(Memory::GetPointer(0x10000000) == Memory::GetPointer(0x90000000) &&
                Memory::GetPointer(0x10000000) == Memory::GetPointer(0xd0000000), "MEM2 aliases copied instead of shared");
        Require(Memory::Contains(0x97ffffff), "upstream 128 MiB MEM2 lost");
        Require(!Memory::Contains(0x98000000), "MEM2 end incorrectly mapped");
        Require(Memory::Read32(0x80001000) == 0, "backing not zero initialized");
        MemoryInline::FlatWriteRam32(0x80001001, 0x12345678);
        Require(MemoryInline::FlatRead32(0xc0001001) == 0x12345678, "unaligned alias read/write");
        const auto* bytes = Memory::GetPointer(0x1001);
        Require(bytes[0] == 0x12 && bytes[3] == 0x78, "guest big endian bytes");
        MemoryInline::FlatStore<uint64_t>(0x900ffffd, 0x0123456789abcdefULL);
        Require(MemoryInline::FlatLoad<uint64_t>(0xd00ffffd) == 0x0123456789abcdefULL, "cross-page 64-bit transfer");
        MemoryInline::FlatWriteRamFloat32(0x80001010, 1.5);
        Require(MemoryInline::FlatReadFloat32(0x80001010) == 1.5f, "single storage conversion");
        MemoryInline::FlatWriteRamFloat64(0x80001018, -2.25);
        Require(MemoryInline::FlatReadFloat64(0x80001018) == -2.25, "double storage conversion");
        Memory::Write8(0xffffffff, 0xab);
        Require(Memory::Read8(0xffffffff) == 0xab, "last guest byte");
        Reject([] { Memory::Read32(0xfffffffe); }, "wrapped read accepted");
        Reject([] { MemoryInline::FlatWriteRam32(0xfffffffe, 0); }, "wrapped proven-RAM write accepted");
        Require(!Memory::Contains(0x80000001, std::numeric_limits<size_t>::max()), "length overflow accepted");
        Require(!Memory::Contains(0x80000000, 0x100000000ull - 1), "oversized guest range accepted");
        Require(MemoryInline::ResolveRangeHost(0x80000000, 0, 4, true, true) == nullptr, "unsafe resolved flat pointer");
        Reject([] { Memory::Read32(0xcc008000); }, "write-only FIFO read accepted");
        Reject([] { Memory::Write32(0xcc000000, 1); }, "unsupported MMIO write accepted");
        MemoryInline::FlatWrite32(0xcc008000, 0xdecafbad);
        Require(fifoWrites == 1 && fifoValue == 0xdecafbad, "FIFO policy bypassed");
        Require(Memory::RegisterDeferredRead(0x80007000, 4, Deferred, nullptr) != 0, "deferred registration");
        Require(MemoryInline::FlatRead8(0x80007000) == 0x5a && deferredCalls == 1, "deferred read not materialized");
        MemoryInline::FlatRead8(0x80007000);
        Require(deferredCalls == 1, "deferred callback repeated");
        Memory::RegisterDeferredRead(0x80008000, 4, FailedDeferred, nullptr);
        Reject([] { MemoryInline::FlatRead32(0x80008000); }, "failed materialization ignored");
        RecompMod::g_executableWriteGuardEnabled = true;
        RecompMod::g_executableWriteGuardPages[guardedAddress >> 12] = 1;
        RecompMod::g_executableWriteGuardMidPages[guardedAddress >> 16] = 1;
        RecompMod::g_executableWriteGuardCoarsePages[guardedAddress >> 20] = 1;
        Memory::RefreshWritableFastPathsForExecutableRanges();
        MemoryInline::FlatWriteRam32(guardedAddress, 0xffffffff);
        Require(guardedWrites == 1 && Memory::Read32(guardedAddress) == 0, "proven-RAM store bypassed executable guard");
        auto invalid = config;
        invalid.regions.push_back({"cannot remap", 0x20000000, 16});
        Reject([&] { Memory::Init(invalid); }, "live layout remapped");
        Require(Memory::Read32(0x80001001) == 0x12345678, "rejected remap destroyed active table");
        Memory::Init(config);
        Require(Memory::Read32(0x80001001) == 0x12345678, "idempotent initialization cleared backing");
        Memory::Reset();
        Require(!Memory::Contains(0x80001001), "reset left mapped pointers");
        Memory::Init(config);
        Require(Memory::Read32(0x80001001) == 0x12345678, "reset changed native backing lifetime semantics");
        std::puts("[INFO] Guest memory contracts passed (real Memory implementation; synthetic GX/mod hooks)");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[ERROR] Memory contract test: %s\n", error.what());
        return 1;
    }
}

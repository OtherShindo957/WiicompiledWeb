#include "memory.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <optional>
#include "memory_diagnostics.h"
#include "runtime_log.h"
#include <mutex>
#include <sstream>
#include <unordered_map>

MemoryInline::PageEntry MemoryInline::g_pageTable[MemoryInline::kPageCount]{};
uintptr_t MemoryInline::g_fullPageBias[MemoryInline::kPageCount]{};
uintptr_t MemoryInline::g_fullReadablePageBias[MemoryInline::kPageCount]{};
uintptr_t MemoryInline::g_fullWritablePageBias[MemoryInline::kPageCount]{};
const MemoryInline::SparseWritablePageTable*
    MemoryInline::g_sparseWritablePageTables[MemoryInline::kPageCount]{};
uint8_t MemoryInline::g_deferredReadCoveredPages[MemoryInline::kPageCount]{};
template <typename T>
T MemoryInline::ReadResolvedFallback(uint32_t addr) {
    if constexpr (sizeof(T) == 1) return Memory::Read8(addr);
    if constexpr (sizeof(T) == 2) return Memory::Read16(addr);
    if constexpr (sizeof(T) == 4) return Memory::Read32(addr);
    return Memory::Read64(addr);
}
template uint8_t MemoryInline::ReadResolvedFallback<uint8_t>(uint32_t);
template uint16_t MemoryInline::ReadResolvedFallback<uint16_t>(uint32_t);
template uint32_t MemoryInline::ReadResolvedFallback<uint32_t>(uint32_t);
template uint64_t MemoryInline::ReadResolvedFallback<uint64_t>(uint32_t);
float MemoryInline::ReadResolvedFallbackFloat32(uint32_t addr) { return Memory::ReadFloat32(addr); }
double MemoryInline::ReadResolvedFallbackFloat64(uint32_t addr) { return Memory::ReadFloat64(addr); }
template <typename T>
void MemoryInline::WriteResolvedFallback(uint32_t addr, T value) {
    if constexpr (sizeof(T) == 1) Memory::Write8(addr, value);
    else if constexpr (sizeof(T) == 2) Memory::Write16(addr, value);
    else if constexpr (sizeof(T) == 4) Memory::Write32(addr, value);
    else Memory::Write64(addr, value);
}
template void MemoryInline::WriteResolvedFallback<uint8_t>(uint32_t, uint8_t);
template void MemoryInline::WriteResolvedFallback<uint16_t>(uint32_t, uint16_t);
template void MemoryInline::WriteResolvedFallback<uint32_t>(uint32_t, uint32_t);
template void MemoryInline::WriteResolvedFallback<uint64_t>(uint32_t, uint64_t);
void MemoryInline::WriteResolvedFallbackFloat32(uint32_t addr, double val) { Memory::WriteFloat32(addr, val); }
void MemoryInline::WriteResolvedFallbackFloat64(uint32_t addr, double val) { Memory::WriteFloat64(addr, val); }
namespace {
struct DeferredRead {
    uint64_t token = 0;
    uint32_t start = 0;
    size_t length = 0;
    Memory::DeferredReadCallback callback = nullptr;
    void* user = nullptr;
};

std::mutex& DeferredReadMutex() {
    static std::mutex mutex;
    return mutex;
}

std::vector<DeferredRead>& DeferredReads() {
    static std::vector<DeferredRead> reads;
    return reads;
}

uint64_t& NextDeferredReadToken() {
    static uint64_t token = 1;
    return token;
}

bool RangesOverlap(uint32_t firstStart, size_t firstLength,
                   uint32_t secondStart, size_t secondLength) {
    const uint64_t firstEnd = static_cast<uint64_t>(firstStart) + firstLength;
    const uint64_t secondEnd = static_cast<uint64_t>(secondStart) + secondLength;
    return static_cast<uint64_t>(firstStart) < secondEnd &&
           static_cast<uint64_t>(secondStart) < firstEnd;
}

void RefreshDeferredReadPage(uint32_t page) {
    const uint32_t pageStart = page << MemoryInline::kPageShift;
    const auto& reads = DeferredReads();
    const bool covered = std::any_of(reads.begin(), reads.end(), [pageStart](const DeferredRead& read) {
        return RangesOverlap(pageStart, MemoryInline::kPageSize, read.start, read.length);
    });
    MemoryInline::g_fullReadablePageBias[page] =
        !covered ? MemoryInline::g_fullPageBias[page] : 0;
    MemoryInline::g_deferredReadCoveredPages[page] = covered ? 1 : 0;
}

struct Region {
    Memory::RegionConfig config;
    uint8_t* storagePtr = nullptr;
    size_t storageSize = 0;
};

std::vector<Region>& Regions() {
    static std::vector<Region> regions;
    return regions;
}

std::mutex& RegionMutex() {
    static std::mutex mutex;
    return mutex;
}

std::array<std::unique_ptr<MemoryInline::SparseWritablePageTable>,
           MemoryInline::kPageCount>& SparseWritablePageTableStorage() {
    static std::array<std::unique_ptr<MemoryInline::SparseWritablePageTable>,
                      MemoryInline::kPageCount> storage;
    return storage;
}

void ClearPageTable() {
    auto* table = MemoryInline::g_pageTable;
    for (uint32_t i = 0; i < MemoryInline::kPageCount; ++i) {
        auto& entry = table[i];
        entry.base = nullptr;
        entry.limit = 0;
        MemoryInline::g_fullPageBias[i] = 0;
        MemoryInline::g_fullReadablePageBias[i] = 0;
        MemoryInline::g_fullWritablePageBias[i] = 0;
        MemoryInline::g_deferredReadCoveredPages[i] = 0;
        MemoryInline::g_sparseWritablePageTables[i] = nullptr;
        SparseWritablePageTableStorage()[i].reset();
    }
}

} // namespace

uint64_t Memory::RegisterDeferredRead(uint32_t addr, size_t length,
                                      DeferredReadCallback callback, void* user) {
    if (length == 0 || callback == nullptr ||
        static_cast<uint64_t>(addr) + length > (uint64_t{1} << 32)) {
        return 0;
    }

    std::lock_guard lock{DeferredReadMutex()};
    uint64_t token = NextDeferredReadToken()++;
    if (token == 0) token = NextDeferredReadToken()++;
    DeferredReads().push_back({token, addr, length, callback, user});
    const uint32_t firstPage = addr >> MemoryInline::kPageShift;
    const uint32_t lastPage = static_cast<uint32_t>(
        (static_cast<uint64_t>(addr) + length - 1) >> MemoryInline::kPageShift);
    for (uint32_t page = firstPage; page <= lastPage; ++page) {
        MemoryInline::g_fullReadablePageBias[page] = 0;
        MemoryInline::g_deferredReadCoveredPages[page] = 1;
    }
    // Clearing the readable bias only intercepts the checked path. A flat read
    // needs the host pages themselves to trap, which is what PAGE_NOACCESS on
    // the guest view does; the vectored handler materializes the copy, restores
    // the protection and re-runs the access.
    GuestFlat::ProtectDeferredRange(addr, length);
    return token;
}

void Memory::ClearDeferredReads() {
    std::lock_guard lock{DeferredReadMutex()};
    auto& reads = DeferredReads();
    std::vector<uint32_t> pages;
    for (const auto& read : reads) {
        const uint32_t firstPage = read.start >> MemoryInline::kPageShift;
        const uint32_t lastPage = static_cast<uint32_t>(
            (static_cast<uint64_t>(read.start) + read.length - 1) >> MemoryInline::kPageShift);
        for (uint32_t page = firstPage; page <= lastPage; ++page) pages.push_back(page);
        GuestFlat::UnprotectDeferredRange(read.start, read.length);
    }
    reads.clear();
    std::sort(pages.begin(), pages.end());
    pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
    for (uint32_t page : pages) RefreshDeferredReadPage(page);
}

bool MemoryInline::ResolveDeferredReads(uint32_t addr, size_t length) {
    std::vector<DeferredRead> matches;
    std::vector<uint32_t> affectedPages;
    {
        std::lock_guard lock{DeferredReadMutex()};
        auto& reads = DeferredReads();
        for (auto it = reads.begin(); it != reads.end();) {
            if (!RangesOverlap(addr, length, it->start, it->length)) {
                ++it;
                continue;
            }
            const uint32_t firstPage = it->start >> kPageShift;
            const uint32_t lastPage = static_cast<uint32_t>(
                (static_cast<uint64_t>(it->start) + it->length - 1) >> kPageShift);
            for (uint32_t page = firstPage; page <= lastPage; ++page) affectedPages.push_back(page);
            matches.push_back(*it);
            // Idempotent: the vectored handler already dropped the protection
            // for the range whose trap brought us here.
            GuestFlat::UnprotectDeferredRange(it->start, it->length);
            it = reads.erase(it);
        }
        std::sort(affectedPages.begin(), affectedPages.end());
        affectedPages.erase(std::unique(affectedPages.begin(), affectedPages.end()), affectedPages.end());
        for (uint32_t page : affectedPages) RefreshDeferredReadPage(page);
    }

    // The range is removed before entering renderer code so any memory reads
    // made while submitting the copy cannot recursively trigger it.
    for (const auto& match : matches) {
        if (!match.callback(match.user)) {
            throw Memory::AccessViolation(addr, length, "deferred read materialization failed");
        }
    }
    return true;
}

namespace {

void BuildPageTable() {
    auto* table = MemoryInline::g_pageTable;
    for (const auto& region : Regions()) {
        const uint64_t base = region.config.baseAddress;
        const uint64_t size = region.storageSize;
        const uint64_t end = base + size;
        if (size == 0) {
            continue;
        }

        const uint32_t startPage = static_cast<uint32_t>(base >> MemoryInline::kPageShift);
        const uint32_t endPage = static_cast<uint32_t>((end - 1) >> MemoryInline::kPageShift);
        for (uint32_t page = startPage; page <= endPage; ++page) {
            const uint64_t pageBase = static_cast<uint64_t>(page) << MemoryInline::kPageShift;
            const uint64_t offset = pageBase - base;
            if (offset >= size) {
                continue;
            }
            const uint32_t limit = static_cast<uint32_t>(std::min<uint64_t>(MemoryInline::kPageSize, size - offset));
            table[page].base = region.storagePtr + static_cast<size_t>(offset);
            table[page].limit = limit;
        }
    }

    // The last page of each contiguous mapping remains on the checked path so
    // an access straddling its end cannot escape the mapped region. Every
    // preceding full page can safely service native accesses up to 8 bytes,
    // including a cross-page access into its contiguous successor.
    for (uint32_t page = 0; page + 1 < MemoryInline::kPageCount; ++page) {
        const auto& current = table[page];
        const auto& next = table[page + 1];
        if (!current.base || current.limit != MemoryInline::kPageSize ||
            !next.base || next.limit < MemoryInline::kMaxFastScalarSize - 1u ||
            next.base != current.base + MemoryInline::kPageSize)
            continue;
        const uintptr_t guestPageBase = static_cast<uintptr_t>(page) << MemoryInline::kPageShift;
        const uintptr_t bias = reinterpret_cast<uintptr_t>(current.base) - guestPageBase;
        MemoryInline::g_fullPageBias[page] = bias + 1u;
    }
}

void RefreshFastPathTables() {
    auto& sparseStorage = SparseWritablePageTableStorage();
    for (uint32_t page = 0; page < MemoryInline::kPageCount; ++page) {
        const uintptr_t bias = MemoryInline::g_fullPageBias[page];
        MemoryInline::g_fullReadablePageBias[page] = bias;
        MemoryInline::g_deferredReadCoveredPages[page] = 0;
        MemoryInline::g_sparseWritablePageTables[page] = nullptr;
        sparseStorage[page].reset();

        const bool coarseExecutable =
            RecompMod::g_executableWriteGuardCoarsePages[page].load(
                std::memory_order_relaxed) != 0;
        MemoryInline::g_fullWritablePageBias[page] = coarseExecutable ? 0 : bias;
        if (!coarseExecutable || bias == 0)
            continue;

        auto sparse = std::make_unique<MemoryInline::SparseWritablePageTable>();
        bool hasExecutableSubPage = false;
        bool hasWritableSubPage = false;
        const uint32_t firstExactPage =
            page * MemoryInline::kWritableSubPagesPerPage;
        for (uint32_t subPage = 0;
             subPage < MemoryInline::kWritableSubPagesPerPage; ++subPage) {
            const bool executable =
                RecompMod::g_executableWriteGuardPages[firstExactPage + subPage].load(
                    std::memory_order_relaxed) != 0;
            hasExecutableSubPage |= executable;
            hasWritableSubPage |= !executable;
            sparse->encodedBias[subPage] = executable ? 0 : bias;
        }

        // A homogeneous executable page has no writable fast path; a
        // homogeneous data page already uses g_fullWritablePageBias. Retain an
        // allocation only for the intended mixed case.
        if (hasExecutableSubPage && hasWritableSubPage) {
            MemoryInline::g_sparseWritablePageTables[page] = sparse.get();
            sparseStorage[page] = std::move(sparse);
        }
    }
    std::lock_guard lock{DeferredReadMutex()};
    for (const auto& read : DeferredReads()) {
        const uint32_t firstPage = read.start >> MemoryInline::kPageShift;
        const uint32_t lastPage = static_cast<uint32_t>(
            (static_cast<uint64_t>(read.start) + read.length - 1) >> MemoryInline::kPageShift);
        for (uint32_t page = firstPage; page <= lastPage; ++page) {
            MemoryInline::g_fullReadablePageBias[page] = 0;
            MemoryInline::g_deferredReadCoveredPages[page] = 1;
        }
    }
}

Region* TryResolveRegion(uint32_t address, size_t length) {
    auto& regions = Regions();
    for (auto& region : regions) {
        const uint64_t base = region.config.baseAddress;
        const uint64_t limit = base + region.storageSize;
        const uint64_t addr = address;
        if (addr >= base && addr <= limit && length <= limit - addr) {
            return &region;
        }
    }
    return nullptr;
}

Region& ResolveRegion(uint32_t address, size_t length) {
    if (Region* region = TryResolveRegion(address, length)) {
        return *region;
    }
    ReportUnmappedGuestAccess(address, length);
    throw Memory::AccessViolation(address, length, "no mapped region");
}

GuestFlat::Backing ClassifyBacking(uint32_t baseAddress) {
    // MEM1: physical (0x00000000), cached (0x80000000), uncached (0xC0000000)
    if ((baseAddress >= Memory::kMem1PhysicalBase &&
         baseAddress < Memory::kMem1PhysicalBase + Memory::kMem1Size) ||
        (baseAddress >= Memory::kMem1CachedBase &&
         baseAddress < Memory::kMem1CachedBase + Memory::kMem1Size) ||
        (baseAddress >= Memory::kMem1UncachedBase &&
         baseAddress < Memory::kMem1UncachedBase + Memory::kMem1Size)) {
        return GuestFlat::Backing::Mem1;
    }

    // NDEV-sized MEM2: physical (0x10000000), cached (0x90000000),
    // uncached (0xD0000000). Mario Kart Wii detects this configuration and
    // creates its original EGGRootDebug expansion heap.
    if ((baseAddress >= Memory::kMem2PhysicalBase &&
         baseAddress < Memory::kMem2PhysicalEnd) ||
        (baseAddress >= Memory::kMem2CachedBase &&
         baseAddress < Memory::kMem2CachedEnd) ||
        (baseAddress >= Memory::kMem2UncachedBase &&
         baseAddress < Memory::kMem2UncachedEnd)) {
        return GuestFlat::Backing::Mem2;
    }

    return GuestFlat::Backing::Owned;
}


template <typename T>
T ReadScalar(uint32_t address) {
    // This is the cold *Slow path; Memory::GetPointer already tries the fast
    // probe first, so repeating it here only guaranteed a second miss.
    auto* ptr = Memory::GetPointer(address, sizeof(T));
    if constexpr (sizeof(T) == 1) {
        return *ptr;
    } else {
        T value = 0;
        std::memcpy(&value, ptr, sizeof(T));
        return MemoryInline::MaybeByteSwap(value);
    }
}

template <typename T>
void WriteScalar(uint32_t address, T value) {
    if (RecompMod::HandleExecutableWrite(address, sizeof(T), static_cast<uint64_t>(value))) {
        return;
    }

    auto* ptr = Memory::GetPointer(address, sizeof(T));
    if constexpr (sizeof(T) == 1) {
        *ptr = static_cast<uint8_t>(value);
    } else {
        const T swapped = MemoryInline::MaybeByteSwap(value);
        std::memcpy(ptr, &swapped, sizeof(T));
    }
}

} // namespace

Memory::AccessViolation::AccessViolation(uint32_t address, size_t length, std::string_view reason)
    : std::runtime_error([&]() {
          std::ostringstream oss;
          oss << "Memory access violation at 0x" << std::hex << std::uppercase << address
              << " (+0x" << length << ") :: " << reason;
          return oss.str();
      }()),
      address_(address),
      length_(length),
      reason_(reason) {}

void Memory::RefreshWritableFastPathsForExecutableRanges() {
    std::lock_guard<std::mutex> lock(RegionMutex());
    RefreshFastPathTables();
}

Memory::Config Memory::Config::WiiDefaults() {
    // Physical / cached / uncached views of MEM1 and MEM2, then the locked cache.
    Config config;

    config.regions.push_back(RegionConfig{
        .name = "MEM1_PHYS",
        .baseAddress = Memory::kMem1PhysicalBase,
        .sizeBytes = Memory::kMem1Size,
    });

    config.regions.push_back(RegionConfig{
        .name = "MEM1",
        .baseAddress = Memory::kMem1CachedBase,
        .sizeBytes = Memory::kMem1Size,
    });

    config.regions.push_back(RegionConfig{
        .name = "MEM1_UNCACHED",
        .baseAddress = Memory::kMem1UncachedBase,
        .sizeBytes = Memory::kMem1Size,
    });

    config.regions.push_back(RegionConfig{
        .name = "MEM2_PHYS",
        .baseAddress = Memory::kMem2PhysicalBase,
        .sizeBytes = Memory::kMem2Size,
    });

    config.regions.push_back(RegionConfig{
        .name = "MEM2",
        .baseAddress = Memory::kMem2CachedBase,
        .sizeBytes = Memory::kMem2Size,
    });

    config.regions.push_back(RegionConfig{
        .name = "MEM2_UNCACHED",
        .baseAddress = Memory::kMem2UncachedBase,
        .sizeBytes = Memory::kMem2Size,
    });

    // Kamek module overlay: 2 MiB above MEM1, which the game believes ends at 0x81800000, so this
    // costs no arena space (unlike the old in-arena reservation that shrank the race scene heaps).
    // Base must stay within +/-32 MiB of every DOL/StaticR hook site for Kamek Rel24 branches to encode.
    config.regions.push_back(RegionConfig{
        .name = "MEM1_KAMEK_OVERLAY",
        .baseAddress = 0x81800000,
        .sizeBytes = 0x200000,
    });

    // Locked cache (THP decoder fast RAM) is really 16KB at 0xE0000000, but a sub-page mapping can
    // never enter the coarse 1MiB bias tables, forcing every THP load/store through the checked
    // fallback (the dominant cost of THP-heavy screens). Back a full 1MiB page plus the successor
    // page the bias builder needs, so locked-cache access stays on the two-instruction native path.
    constexpr size_t lcSize = MemoryInline::kPageSize + 4096u;
    config.regions.push_back(RegionConfig{
        .name = "LOCKED_CACHE",
        .baseAddress = 0xE0000000,
        .sizeBytes = lcSize,
    });

    return config;
}

void Memory::Init(size_t mem1Size) {
    Config config;
    config.regions.push_back(RegionConfig{.name = "MEM1", .baseAddress = 0x80000000, .sizeBytes = mem1Size});
    Init(config);
}

void Memory::Init(const Config& config) {
    std::lock_guard<std::mutex> lock(RegionMutex());
    auto& regions = Regions();

    // The flat reservation backs every guest view from one shared section object, so cached/uncached/
    // physical mirrors alias as before. GetPointer still hands out the unprotected host alias, so
    // native code (image loading, DVD reads, HLE) is unaffected by the guest view's protections.
    {
        std::vector<GuestFlat::RegionRequest> flatRegions;
        flatRegions.reserve(config.regions.size());
        for (const auto& regionConfig : config.regions) {
            flatRegions.push_back(GuestFlat::RegionRequest{
                regionConfig.baseAddress, regionConfig.sizeBytes,
                ClassifyBacking(regionConfig.baseAddress)});
        }
        GuestFlat::Initialize(flatRegions);
    }

    regions.clear();
    ClearPageTable();
    regions.reserve(config.regions.size());

    // Map virtual regions onto that backing store, with mirroring.
    for (const auto& regionConfig : config.regions) {
        Region instance;
        instance.config = regionConfig;

        instance.storagePtr = GuestFlat::HostPointer(regionConfig.baseAddress);
        if (instance.storagePtr == nullptr && regionConfig.sizeBytes != 0) {
            throw std::runtime_error("Flat guest mapping is missing region '" + regionConfig.name + "'");
        }
        instance.storageSize = regionConfig.sizeBytes;

        regions.emplace_back(std::move(instance));
    }

    BuildPageTable();
    RefreshFastPathTables();
}

void Memory::Reset() {
    ClearDeferredReads();
    std::lock_guard<std::mutex> lock(RegionMutex());
    Regions().clear();
    ClearPageTable();
}

namespace {
// MMIO reads have no backing store or generic HLE. Answering zero (the old sparse fallback) turned
// unimplemented devices into silent hangs, so unmapped reads throw like MMIO writes; every device
// we do implement is hooked at the function level, so a hit here means a missing HLE hook.
[[noreturn]] void ThrowMmioReadBlocked(uint32_t addr, size_t length) {
    std::ostringstream reason;
    reason << (MemoryInline::IsGpuFifoAddress(addr)
                   ? "GPU FIFO read blocked (the gather pipe is write-only)"
                   : "MMIO read blocked (non-GPU)")
           << "; add HLE for this device instead of answering zero"
           << " (active=0x" << std::hex << std::uppercase
           << RecompMod::CurrentTranslatedExecutionAddress() << std::dec << std::nouppercase << ")";
    throw Memory::AccessViolation(addr, length, reason.str());
}
} // namespace

uint8_t MemoryInline::Read8Slow(uint32_t addr) {
    if (IsMmioAddress(addr)) {
        ThrowMmioReadBlocked(addr, sizeof(uint8_t));
    }
    ResolveDeferredReads(addr, sizeof(uint8_t));
    return ReadScalar<uint8_t>(addr);
}

uint16_t MemoryInline::Read16Slow(uint32_t addr) {
    if (IsMmioAddress(addr)) {
        ThrowMmioReadBlocked(addr, sizeof(uint16_t));
    }
    ResolveDeferredReads(addr, sizeof(uint16_t));
    return ReadScalar<uint16_t>(addr);
}

uint32_t MemoryInline::Read32Slow(uint32_t addr) {
    if (IsMmioAddress(addr)) {
        ThrowMmioReadBlocked(addr, sizeof(uint32_t));
    }
    ResolveDeferredReads(addr, sizeof(uint32_t));
    const uint32_t value = ReadScalar<uint32_t>(addr);
    return value;
}

uint64_t MemoryInline::Read64Slow(uint32_t addr) {
    if (IsMmioAddress(addr)) {
        ThrowMmioReadBlocked(addr, sizeof(uint64_t));
    }
    ResolveDeferredReads(addr, sizeof(uint64_t));
    return ReadScalar<uint64_t>(addr);
}

float MemoryInline::ReadFloat32Slow(uint32_t addr) {
    if (IsMmioAddress(addr)) {
        ThrowMmioReadBlocked(addr, sizeof(float));
    }
    ResolveDeferredReads(addr, sizeof(uint32_t));
    const auto bits = ReadScalar<uint32_t>(addr);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

double MemoryInline::ReadFloat64Slow(uint32_t addr) {
    if (IsMmioAddress(addr)) {
        ThrowMmioReadBlocked(addr, sizeof(double));
    }
    ResolveDeferredReads(addr, sizeof(uint64_t));
    const auto bits = ReadScalar<uint64_t>(addr);
    double value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void MemoryInline::Write8Slow(uint32_t addr, uint8_t val) {
    if (IsGpuFifoAddress(addr)) {
        GX_HLE_FIFO_Write8(val);
        return;
    }
    if (IsMmioAddress(addr)) {
        throw Memory::AccessViolation(addr, sizeof(val), "MMIO write blocked (non-GPU)");
    }
    WriteScalar(addr, val);
}

void MemoryInline::Write16Slow(uint32_t addr, uint16_t val) {
    if (IsGpuFifoAddress(addr)) {
        GX_HLE_FIFO_Write16(val);
        return;
    }
    if (IsMmioAddress(addr)) {
        throw Memory::AccessViolation(addr, sizeof(val), "MMIO write blocked (non-GPU)");
    }
    WriteScalar(addr, val);
}

void MemoryInline::Write32Slow(uint32_t addr, uint32_t val) {
    if (IsGpuFifoAddress(addr)) {
        GX_HLE_FIFO_Write32(val);
        return;
    }
    if (IsMmioAddress(addr)) {
        throw Memory::AccessViolation(addr, sizeof(val), "MMIO write blocked (non-GPU)");
    }
    WriteScalar(addr, val);
}

void MemoryInline::Write64Slow(uint32_t addr, uint64_t val) {
    if (IsGpuFifoAddress(addr)) {
        // The gather pipe is a byte-stream FIFO; a 64-bit store is two big-endian
        // 32-bit pushes, high word first. Without this arm the write fell through
        // to WriteScalar, which found no mapped region behind 0xCC008000 and threw
        // "no mapped region" instead of reaching GX.
        GX_HLE_FIFO_Write32(static_cast<uint32_t>(val >> 32));
        GX_HLE_FIFO_Write32(static_cast<uint32_t>(val));
        return;
    }
    if (IsMmioAddress(addr)) {
        throw Memory::AccessViolation(addr, sizeof(val), "MMIO write blocked (non-GPU)");
    }
    WriteScalar(addr, val);
}

void MemoryInline::WriteFloat32Slow(uint32_t addr, double val) {
    // stfs stores the IEEE-754 bit pattern of the single-precision value, never a
    // truncated integer.
    const uint32_t bits = ConvertPpcDoubleToSingleBits(val);
    if (IsGpuFifoAddress(addr)) {
        GX_HLE_FIFO_WriteFloat(PpcSingleBitsToFloat(bits));
        return;
    }
    if (IsMmioAddress(addr)) {
        throw Memory::AccessViolation(addr, sizeof(float), "MMIO write blocked (non-GPU)");
    }

    WriteScalar(addr, bits);
}

void MemoryInline::WriteFloat64Slow(uint32_t addr, double val) {
    // stfd stores the full 64-bit FPR bit pattern - load-bearing for the
    // fctiwz->stfd->lwz idiom, where the integer result lives in the low word.
    uint64_t bits;
    std::memcpy(&bits, &val, sizeof(bits));

    if (IsGpuFifoAddress(addr)) {
        GX_HLE_FIFO_WriteFloat(static_cast<float>(val));
        return;
    }
    if (IsMmioAddress(addr)) {
        throw Memory::AccessViolation(addr, sizeof(double), "MMIO write blocked (non-GPU)");
    }

    WriteScalar(addr, bits);
}

uint8_t* Memory::GetPointer(uint32_t addr) {
    return GetPointer(addr, 1);
}

uint8_t* Memory::GetPointer(uint32_t addr, size_t length) {
    if (auto* ptr = MemoryInline::GetPointerFast(addr, length)) {
        return ptr;
    }
    auto& region = ResolveRegion(addr, length);
    auto offset = static_cast<size_t>(addr - region.config.baseAddress);
    return region.storagePtr + offset;
}

bool Memory::Contains(uint32_t addr, size_t length) {
    if (MemoryInline::GetPointerFast(addr, length)) {
        return true;
    }
    // Contains is a membership predicate: callers probe arbitrary guest values
    // (e.g. gfx-node slots that may hold floats), so a miss must stay silent
    // rather than take ResolveRegion's crash-dump path.
    return TryResolveRegion(addr, length) != nullptr;
}

std::vector<Memory::RegionConfig> Memory::DescribeRegions() {
    std::lock_guard<std::mutex> lock(RegionMutex());
    std::vector<RegionConfig> info;
    info.reserve(Regions().size());
    for (const auto& region : Regions()) {
        info.push_back(RegionConfig{region.config.name, region.config.baseAddress, region.storageSize});
    }
    return info;
}

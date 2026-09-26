#include "guest_flat_memory.h"
#include "memory.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

#ifndef MKW_CHECKED_GUEST_MEMORY
#error "Compact guest backing requires checked access helpers"
#endif

namespace GuestFlat {
namespace {
struct Store {
    Backing kind;
    uint32_t owned;
    size_t size;
    std::unique_ptr<uint8_t[]> bytes;
};
struct Mapping { uint32_t base; uint64_t size; uint8_t* bytes; };
std::vector<Store> stores;
std::vector<Mapping> mappings;
std::vector<RegionRequest> layout;
bool active = false;

uint64_t Offset(const RegionRequest& r) {
    if (r.backing == Backing::Owned) return 0;
    const uint32_t physical = r.base & 0x1fffffffu;
    const uint32_t base = r.backing == Backing::Mem1 ? 0 : Memory::kMem2PhysicalBase;
    const size_t size = r.backing == Backing::Mem1 ? Memory::kMem1Size : Memory::kMem2Size;
    const uint32_t segment = r.base & 0xe0000000u;
    if ((segment != 0 && segment != 0x80000000u && segment != 0xc0000000u) ||
        physical < base || uint64_t(physical - base) + r.size > size)
        throw std::invalid_argument("Guest RAM alias exceeds its backing window");
    return physical - base;
}
bool SameLayout(const std::vector<RegionRequest>& requested) {
    return requested.size() == layout.size() && std::equal(requested.begin(), requested.end(), layout.begin(),
        [](const auto& a, const auto& b) { return a.base == b.base && a.size == b.size && a.backing == b.backing; });
}
}

bool IsActive() { return active; }

void Initialize(const std::vector<RegionRequest>& requested) {
    // Startup/scheduler-thread ownership, matching Memory::Init's serialized contract.
    // Existing raw host pointers must never be invalidated by a later reconfiguration.
    if (active) {
        if (!SameLayout(requested)) throw std::runtime_error("Guest backing layout cannot be remapped");
        return;
    }
    std::vector<Store> nextStores;
    std::vector<Mapping> nextMappings;
    for (size_t index = 0; index < requested.size(); ++index) {
        const auto& r = requested[index];
        if (r.size > kGuestSpaceSize - r.base)
            throw std::invalid_argument("Guest mapping wraps the 32-bit address space");
        if (!r.size) continue;
        if (r.backing != Backing::Owned && r.backing != Backing::Mem1 && r.backing != Backing::Mem2)
            throw std::invalid_argument("Unknown guest backing kind");
        // MMIO must reach policy handlers, never an allocated RAM fast path.
        if (uint64_t(r.base) < 0xce000000ull && uint64_t(r.base) + r.size > 0xcc000000ull)
            throw std::invalid_argument("Guest RAM mapping overlaps MMIO");
        for (size_t other = 0; other < index; ++other) {
            const auto& p = requested[other];
            if (p.size && uint64_t(r.base) < uint64_t(p.base) + p.size && uint64_t(p.base) < uint64_t(r.base) + r.size)
                throw std::invalid_argument("Overlapping guest mappings");
        }
        const uint64_t need = Offset(r) + r.size;
        if (need > std::numeric_limits<size_t>::max()) throw std::length_error("Guest backing exceeds host size_t");
        const uint32_t owned = r.backing == Backing::Owned ? r.base : 0;
        auto found = std::find_if(nextStores.begin(), nextStores.end(), [&](const Store& s) {
            return s.kind == r.backing && s.owned == owned;
        });
        if (found == nextStores.end()) nextStores.push_back({r.backing, owned, static_cast<size_t>(need), {}});
        else found->size = std::max(found->size, static_cast<size_t>(need));
    }
    for (auto& store : nextStores) store.bytes = std::make_unique<uint8_t[]>(store.size);
    for (const auto& r : requested) {
        if (!r.size) continue;
        const uint32_t owned = r.backing == Backing::Owned ? r.base : 0;
        auto found = std::find_if(nextStores.begin(), nextStores.end(), [&](const Store& s) {
            return s.kind == r.backing && s.owned == owned;
        });
        nextMappings.push_back({r.base, r.size, found->bytes.get() + Offset(r)});
    }
    // Commit only after every validation/allocation succeeds.
    auto nextLayout = requested;
    stores = std::move(nextStores);
    mappings = std::move(nextMappings);
    layout = std::move(nextLayout);
    active = true;
}

uint8_t* HostPointer(uint32_t address) {
    for (const auto& mapping : mappings)
        if (address >= mapping.base && uint64_t(address - mapping.base) < mapping.size)
            return mapping.bytes + (address - mapping.base);
    return nullptr;
}

// No host page protection exists here. Checked Memory::* access owns deferred
// reads and RecompMod owns executable-write policy, as on checked native paths.
void ProtectDeferredRange(uint32_t, size_t) {}
void UnprotectDeferredRange(uint32_t, size_t) {}
void RegisterExecutableRange(uint32_t, uint32_t) {}
FaultCounters Counters() { return {}; }
void LogFaultSummary() noexcept {}
bool HandleAccessViolation(void*, bool) noexcept { return false; }
}

#include "memory_diagnostics.h"
#include <cstdio>

void ReportUnmappedGuestAccess(uint32_t address, size_t length) {
    // CPU state reporting awaits the browser CPU/context port; never swallow the error.
    std::fprintf(stderr, "[ERROR] Unmapped guest access at 0x%08x, length %zu\n", address, length);
}

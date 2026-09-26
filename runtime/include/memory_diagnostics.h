#pragma once
#include <cstddef>
#include <cstdint>

// Host diagnostics are separate from guest memory policy.
void ReportUnmappedGuestAccess(uint32_t address, size_t length);

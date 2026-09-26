# Milestone 2a: compact checked guest memory

This increment addresses the first audit blocker: the real `runtime/src/memory.cpp`
now builds and executes as wasm32 without a fixed native 4 GiB mapping. It is an
experimental memory adapter with a conformance harness, **not a complete browser
runtime or game boot**. The accepted clear-screen launcher remains unchanged.

## Implementation

`runtime/src/platform/web/guest_memory.cpp` implements the existing `GuestFlat`
backing contract with zero-initialized ordinary allocations. Physical, cached and
uncached MEM1/MEM2 views point into the same stores. It preserves upstream's 24 MiB
MEM1, 128 MiB MEM2, 2 MiB overlay and enlarged locked-cache configuration, rather
than allocating one store per alias or a 4 GiB array. Owned mappings have independent
backing. Overlapping, wrapped, out-of-window and MMIO mappings are rejected before
any state is published. Initialization is startup-thread-only and the live layout
cannot be remapped, matching the native backing lifetime. `Memory::Reset` clears
access tables but does not destroy backing; initializing the identical layout
restores access to retained bytes, as before.

CMake selects `MKW_CHECKED_GUEST_MEMORY`; native builds keep their existing fixed
mapping and helper code. Existing translated helper names are retained. General
and proven-RAM flat scalar operations route through checked `Memory::*` on this
target. Range resolution returns null so existing fallback operations retain their
policy checks. Quantized-pointer helpers already reject the checked path; the
quantized/FP implementation itself has **not** been ported or verified on WASM.

MMIO and deferred/executable-write behavior stays in shared memory code. The web
backend has no page-protection mechanism: its protect/unprotect functions are
intentionally empty, as on the existing checked macOS path. This is safe only
with checked access selected for every guest access; it is not permission to
write through arbitrary raw pointers. Host/HLE `GetPointer` remains the trusted
raw backing API, as upstream. Native fault handlers can commit certain invalid
unmapped touches; this checked backend instead throws an explicit access error.
It does not reproduce native fault-recovery heuristics or certify compatibility
with guest code that depends on them.

Two narrow shared corrections accompany the port: use subtraction for range
bounds to prevent `size_t` overflow on wasm32, and validate the backing layout
before clearing active access tables so a rejected remap leaves the old mapping
usable. Native crash diagnostics were moved verbatim into
`runtime/src/memory_diagnostics.cpp`; the browser implementation emits address and
length without importing unsupported CPU/SIMD headers. CPU-context dumps await
the context/FP port. The translator's lean native test source list includes the
new diagnostics file.

The native recursive source list now excludes `src/platform/web/`, preventing both
the previously added probe `main` and the new adapters from entering desktop
products. The root web build selects its sources explicitly.

## Validation

With Emscripten 6.0.10:

```sh
source /path/to/emsdk/emsdk_env.sh
emcmake cmake -S . -B build-web -DWIICOMPILED_WEB=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-web -j2
ctest --test-dir build-web --output-on-failure
```

Local results: **3/3** WASM tests passed (input expressions, SC serial, memory).
The new tests compile the real `Memory` implementation and compact adapter.
Handwritten GX FIFO and RecompMod policy hooks are test doubles, clearly isolated
in `runtime/tests/web_memory_tests.cpp`; they verify dispatch and guards, not GX
rendering or the complete mod loader. Fixtures contain no game code or assets.

Covered behavior:

- Invalid-layout rollback, overlap/MMIO/alias-window rejection, fixed-layout lifetime.
- Shared MEM1/MEM2 pointers, zero initialization, upstream 128 MiB MEM2 extent.
- Unaligned big-endian reads/writes, cross-page 64-bit access and float storage.
- Final 32-bit address, wrapping accesses and oversized/overflowing lengths.
- FIFO write dispatch, blocked FIFO reads and unsupported MMIO writes.
- Deferred materialization, one-shot callback and explicit callback failure.
- Proven-RAM stores reaching executable-write policy and rejected remap preserving tables.
- Reset/reinitialization matching the retained native backing lifetime.

The browser companion `web_memory_browser.mjs` runs these same contracts in
Chromium via `web/tests/smoke.cjs`, alongside the accepted real WebGPU clear/pixel,
resize and failure tests. All passed locally with software WebGPU. Memory tests
need no GPU, though the combined smoke suite tests the renderer separately.
The memory modules cap WASM linear memory at 256 MiB: the full default backing and
synthetic tail fit within that limit. This is a test budget, not a game memory
budget or a performance measurement.

The same memory tests passed natively with AddressSanitizer and UBSan (local leak
checking disabled for sandbox compatibility). The modified native memory and
native diagnostics translation units also compile with x86-64 Clang; the moved
diagnostic body was checked against the baseline verbatim. A full native game
build and .NET translator suite were not run locally. Browser CI now repeats the
three WASM tests, browser memory execution, existing GPU smoke tests and native
memory sanitizer checks. Consult its run result for remote status.

Browser contract binaries are test-only: the release artifact still allowlists
the original six game-free launcher/probe files. A successful memory test does
not cause the probe to load translated game code. No Nintendo data is required,
added, uploaded or used by this increment.

## Remaining dependencies

This closes the fixed-address allocation requirement **for the checked memory
subsystem**, not the entire memory/CPU integration. Real executable-write hooks,
renderer-driven deferred EFB behavior, asynchronous completion and all translated
instruction paths require integration tests after their dependencies are ported.
Existing alias-specific policy semantics are retained; no new alias-wide
executable/deferred policy is claimed.

Next: preserve cooperative guest-context semantics with a browser-capable fiber
backend and synthetic scheduler tests; then port FP/paired-single helpers with
semantic comparisons; then adapt Aurora's native Dawn initialization/readbacks.
These are separate changes. No game boot, physics synchronization, audio, saves,
controller support or hardware performance is claimed by this PR.

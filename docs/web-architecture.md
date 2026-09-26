# WiiCompiled Web architecture decision — Milestone 0

Status: proposed direction, not a completed runtime port. Based on fork commit
`85f250155f8f397c1c463dc51dd4dc2707371d06`; see [audit](web-port-audit.md).

## Decision and limits

Keep PowerPC decoding, IR/SSA, translation semantics, HLE, GX state/shader generation and static product selection upstream-compatible. Use wasm32/Emscripten and browser WebGPU initially. No Dolphin embedding, PowerPC interpreter or runtime PowerPC JIT.

Use **user-local build-time AOT** for early correctness work: user's supported PAL dump → existing translator → private generated C++/data → Emscripten → private WASM package → browser. This allows work on runtime portability without first shipping a C++ compiler inside a page. It does **not** meet the final “select ISO and play entirely in browser” workflow. Public artifacts contain only handwritten/open-source host code. No generated Nintendo-derived module goes into releases, PRs, build logs or CI artifacts.

The browser-only installation decision remains gated on measurements and prototype conformance. A clear-screen success must not be advertised as Milestone 1's full runtime-compiled criterion.

## Pipeline alternatives

### Build-time static recompilation

Reuse the .NET translator and C++ emitter on the user's machine; compile a private package with a pinned Emscripten version. Lowest semantic drift, mature optimizing compiler and easiest differential debugging. Costs: external installation, potentially long compilation and no browser-only setup. Publisher-side compilation of the game is rejected because distributing its WASM/data would violate the content boundary. Development tooling may distribute compiler/runtime sources, never game outputs.

### Client-side static recompilation (Option A)

Browser reads bounded ISO ranges, validates the revision and decrypts/extracts the required executables locally. A browser-hosted translator emits the current C++ representation; a WASM-hosted LLVM/Clang/linker compiles all reachable translated code before gameplay. Cache the private result by game digest, translator/runtime/toolchain versions and ABI. No upload.

Advantages: closest to the existing semantic pipeline, reaches the requested local browser workflow. Costs: .NET browser-host integration, C++ compiler distribution, filesystem emulation, enormous transient source/IR/linking memory, startup time, quota and browser process limits. Merely compiling the translator to WASM is insufficient. Before selection, benchmark synthetic workloads scaling toward the observed source graph, with bounded temporary storage, cancellation, recovery and peak memory measurements. Never claim feasibility from a small “hello world.”

### Direct static WASM emitter / browser-native representation (Option B)

Keep the PowerPC frontend and canonical IR, add a WASM backend that emits function bodies, data and a linked dispatch table during a one-time local preparation stage. Complete translation before executing the game; persist a private module. This remains AOT rather than a runtime PPC JIT. JavaScript/WASM engines compiling that module are the normal execution platform, not a PowerPC emulator.

Advantages: avoids distributing a full C++ compiler and writing huge C++ source trees. Costs: new lowering/linking/optimization backend, structured control flow, indirect branches/calls, inferred ABIs, relocations, exception/suspension boundaries and FP fidelity. A custom bytecode loop executing PowerPC-derived instructions at runtime would be an interpreter and is rejected. “Metadata” is not a shortcut around semantics or copyright. Prove scalar/paired-single helpers and synthetic multi-function ABI cases against the existing emitter before committing to this substantial compiler project.

### Pre-generated purportedly non-copyrightable metadata

Public version/checksum identifiers and independently authored format schemas may assist selection. Game-specific control-flow graphs, immediates, instruction traces, data tables or reconstructable IR can encode the executable. Do not assume generated translation metadata is non-copyrightable merely because it is JSON or omits raw opcode bytes. This is **not an approved distribution path** without provenance/legal assessment; default to deriving it locally. It cannot by itself execute the game without a local AOT step or an impermissible distributed code payload.

### Hybrid

Ship a reusable open-source WASM runtime/compiler frontend and derive/link only game-specific code locally, either using a desktop helper first or an eventual browser AOT backend. The ABI between game code, runtime, memory, callbacks and function tables must be versioned and exactly checked. A monolithic private WASM build is simplest for early tests; modular WASM linking adds table/shared-memory and type-signature risks. Remote compilation of a game dump is outside scope and not a fallback.

Selection gates: measure source/module size, translation/compile wall time, peak memory, OPFS quota needs, invalid-image handling and deterministic helper tests. Until those exist, use local AOT for development and keep Option A versus Option B explicitly unresolved for final browser-only delivery.

## Platform boundaries

Extend the existing `runtime/include/platform` and `runtime/src/platform` layout. Select implementations with CMake, not broad shared-code Emscripten conditionals. The small probe belongs in `runtime/src/platform/web` with a distinct `web/` shell and does not link desktop runtime sources.

Required contracts, introduced incrementally:

- Surface: async initialization, canvas resize/DPI, fullscreen, format/limits, loss/recovery and presentation; retain separate guest aspect/internal resolution.
- Timing: monotonic host time and scheduling; guest VI/OS clocks remain semantic authorities. Browser presentation callbacks do not increment simulation by definition.
- Input: stable key/controller snapshots mapped through existing expressions at the same guest polling boundary; clear held buttons on blur/disconnect.
- Audio: guest PCM producer, bounded ring and AudioWorklet consumer; mute/gain, user-gesture unlock, suspended context handling and underrun counters.
- Filesystem: mount/load/save/flush lifecycle with errors; OPFS/IndexedDB backing, versioned config, transactional save publication and explicit export/import.
- Disc: bounded random access with 64-bit offsets, cancellation and exact reads; browser File/Blob or worker OPFS implementation plus a native file adapter for tests.
- Guest memory: compact region backing and alias translation; explicit MMIO/EFB/write guards using existing checked-access semantics.
- Guest contexts: cooperative stack suspension adapter, separately tested from host Workers/pthreads.
- Network: explicit offline adapter initially. Clipboard and log export are user-triggered browser services. Configuration serialization remains shared.

Do not invent a giant interface hierarchy before a second implementation is needed. The probe only establishes lifecycle and surface boundaries.

## Scheduling and threading

Main browser thread owns launcher/DOM. A future runtime worker can host game work and pthreads where cross-origin isolation is available. Choose a single GPU owner; do not replicate native presenter/frame-worker sharing of Dawn objects. Worker-to-UI messages must not hold locks while awaiting synchronous UI service.

Guest fibers cannot become one OS thread each without changing scheduler semantics. First investigate Emscripten's [Asyncify fiber API](https://emscripten.org/docs/api_reference/fiber.h.html) with synthetic nested yields, TLS/FPSCR restoration, alarms and stack pressure. Explicit continuations are an alternative with higher translator changes. JSPI can suspend asynchronous calls but must not be assumed to provide all required symmetric fiber semantics. [Asyncify/JSPI documentation](https://emscripten.org/docs/porting/asyncify.html) describes their different mechanisms; benchmark any instrumentation overhead separately.

Yield at guest waits/retraces. Keep existing video-mode deadlines and callback ordering; simulation is normally 60 Hz for the intended mode, independently of 60/120/144 Hz display presentation. Pause on hidden-tab suspension with an explicit resume policy; do not silently discard simulation steps to catch up. Preserve interpolation as optional presentation work.

Threaded builds require HTTPS (localhost for development), SharedArrayBuffer and cross-origin isolation. Serve:

```
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
```

Keep assets same-origin or give dependencies compatible CORS/CORP policies. Verify `crossOriginIsolated` at runtime before selecting a threaded artifact. The initial probe is single-threaded. A future single-thread runtime is a **separate build**, not a switch inside a pthread binary, and feasibility depends on cooperative scheduling/audio budgets. See [Emscripten pthread requirements](https://emscripten.org/docs/porting/pthreads.html). A generic `python -m http.server` is inadequate for validating threaded operation; provide a header-aware dev server serving only the browser build directory.

## Aurora migration

Retain GX → Aurora → WebGPU. First align the pinned Aurora C++ WebGPU headers with a browser binding (investigate Emdawnwebgpu), then split native instance/surface/cache code from portable renderer state. Replace timed waits with continuations for adapter/device/readback; remove native Dawn cache/toggle descriptors. Browser shader compilation/driver caches are opaque; persist only application-owned, content-safe descriptors when useful.

Prove synthetic triangle/clear, GX state changes, texture formats, EFB copies/readback ordering and resize/device loss before game use. Native backend enum support is not browser build support. No new GX renderer is justified by current evidence. WebGL2 is deferred: it requires its own compatibility/feature assessment, not a cosmetic “fallback” toggle.

## Disc and persistent data

Proposed native-facing shape:

```cpp
class DiscReader {
public:
    virtual ~DiscReader() = default;
    virtual bool read(uint64_t offset, void* dst, size_t size) = 0;
};
```

This synchronous contract is usable only on a worker or a suspendable caller with a proven async bridge. On the UI thread expose async reads instead. Validate `offset <= length` and `size <= length - offset`, destination bounds and integer conversions. Pass offsets as BigInt or checked high/low words across JS; avoid narrowing to wasm32 pointers. Keep bounded decrypted block caching and FST indexing. `File.slice()` avoids multi-GiB memory loads, but Wii partition crypto/extraction still needs implementation. ISO first; other formats require independent decompressor/random-access review.

Mount persistence before reading Config.toml-equivalent; debounce writes with explicit completion, flush after save/config transactions, and do not rely solely on unload. OPFS synchronous handles belong in dedicated workers; IndexedDB APIs are async. Catch quota/permission errors, request persistence where supported, provide save export and a clear-cache action that separates saves from disposable extraction/shader caches. Never retain user disc data silently. No game-selection control until it has a working local path; eventual copy: “Your game file stays on this device and is processed locally.”

## Memory, correctness and compatibility gates

Start wasm32, measure and cap allocations rather than choosing memory64 prematurely. Preserve 24 MiB MEM1 and 128 MiB MEM2 backing with shared aliases, then budget guest contexts, renderer staging, decoded textures, PCM rings and transient translation separately. Bound disc cache and worker queues; avoid holding compiler and game heaps concurrently. Reacquire JS heap views after memory growth. Report CPU/WASM and GPU memory estimates separately.

Pure synthetic tests should cover endian helpers, alias/MMIO access, cross-page bounds, paired-single/FP behavior, function tables, timer/input ordering, config parsing, disc range/EOF and storage recovery. Use browser automation for actual WASM start, GPU clear/readback, missing WebGPU, null adapter, device failure/loss, resize and responsive UI. Mocked WebGPU proves error handling only. Hardware performance targets remain unmeasured.

Initial compatibility: modern desktop Chromium with WebGPU in a secure context; detect capabilities rather than trusting browser names. Firefox/Safari expansion requires actual tests of WebGPU, suspension, threads and storage. No claims for mobile or Wii Remotes initially. Gamepad mapping/HID access varies by browser/device. There is no supplied Web logo asset in this task; retain a text title until the actual logo is available.

## Delivery boundaries

Milestone 0: these documents and evidence only. Small Milestone 1 experiment: handwritten main → web adapter → async GPU initialization → visible canvas clear → native success log. No game, Aurora, audio, saves or translated code. Only declare success after GPU submission and validation complete; visible errors on unsupported browsers.

Full Milestone 1 remains open until actual WiiCompiled runtime code compiles. Milestones 2–7 remain future work. Add CI only after the probe builds locally; allowlist its three web outputs so inherited assets/generated code cannot enter artifacts. Never use broad repository/build-tree uploads. Commit audit and experiment separately, preserve desktop entry points and open a focused PR with exact verification limitations.

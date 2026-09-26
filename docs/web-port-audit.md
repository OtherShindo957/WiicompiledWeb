# WiiCompiled Web: Milestone 0 audit

Audit date: 2026-09-26. Fork: `OtherShindo957/WiicompiledWeb`.
Source baseline: `85f250155f8f397c1c463dc51dd4dc2707371d06`.
This is a source audit, not proof of game compatibility. No game dump was supplied or used.
The vendored Aurora in this commit, rather than a newer upstream Aurora, is the integration target.

## Result

A small independent Emscripten executable is feasible. Compiling the existing game runtime for wasm32 is **not** a build-flag change. Fixed-address guest memory, fault interception, native cooperative stacks, host floating-point control, and synchronous Dawn operations are hard blockers. Preserve the translator and GX implementation; isolate replacements at those contracts. Do not begin game boot until these contracts have synthetic conformance tests.

## Scope and evidence

All 1,189 tracked paths were inventoried. Entry points, build definitions, first-party translator/runtime, vendored Aurora platform/GPU paths and dependency declarations were inspected. Search inventories in [web-audit](web-audit/) record baseline file/line evidence for threads, filesystem, sockets and platform APIs. They are discovery aids, not a claim that every dependency implementation was reviewed. Downloaded SDL/Dawn/nod internals and their transitive worker pools remain unknown until exact dependencies are built. No submodules are declared; `aurora-main/` is an in-tree fork.

Repository structure:

- `translator/`: .NET 8 solution, CLI, core PowerPC pipeline, synthetic tests.
- `projects/`: versioned YAML inputs, profiles, runtime assumptions and example manifests.
- `runtime/`: C++ runtime, HLE, platform services, tests, native product build and third-party code.
- `aurora-main/`: GX/GD/VI/PAD/SI/OS/CARD compatibility, renderer, SDL window/input, CMake dependency providers, examples and tests.
- `Launcher/`: C# desktop installers plus PowerShell/shell extraction, compiler and packaging orchestration.
- `.github/workflows/`: existing native build/recompilation/package workflows.
- `docs/`: existing macOS build guide. No root CMake target or browser launcher existed at the baseline.

## Subsystem classification

### Translator and generated output — Major adaptation required

`translator/src/Translator.Cli/Program.cs` is the CLI entry point. `Translator.Core/Parsing/{Dol,Rel}` reads executables and relocations; `Disassembly`, `Lifting`, `Ir`, `Translation`, `Representation` and `CodeGen` decode, lift, analyze and emit C++. `Mods` implements static overlay/patch translation. Preserve these semantic passes.

The CLI workflow is `translate-recursive`, `generate-data-init`, then `emit-build-shards`. Outputs include `generated/functions/*.cpp`, translation JSON, `RuntimeConfig.h`, data initializers, symbol/dispatch sources and `generated/build_shards/shards.cmake`. `Build/TranslatedBuildShardEmitter.cs` owns the aggregate graph. Native CMake describes roughly 28,000 generated function files, not a measured browser binary size. Generated executable code **and data initializers** are user-derived private artifacts, never release inputs.

`CodeGen/CxxLinearCodeGenerator.FlatGuestMemory.cs` emits native flat accesses. Other generated code relies on runtime helpers, function-pointer signatures and inferred/state-free ABIs. Browser output needs an explicit checked-memory target and ABI verification; simply compiling the C# CLI to WASM does not compile its C++ output. CLI file/process assumptions and parallel work scheduling also need review for a browser host.

### Runtime entry, HLE and products — Major adaptation required

`runtime/src/main.cpp:1306` (`RuntimeMain`, called by `main` at 1519) loads config/logging, initializes `SystemBridge`, Aurora, persistent CPU state and `GuestFiberManager`, then calls `InvokeIndirectCpu(entry->address, &cpu)`. Game control flow owns execution; there is no simple top-level update/render loop to replace. `product/{base,retro_rewind}_product.cpp` selects product behavior. `hle/` supplies OS, VI, GX, audio, input, storage and network services. Preserve HLE contracts; split host effects from guest semantics.

### Guest memory and MMIO — Needs browser replacement

`include/guest_flat_memory.h:16` declares a 4 GiB guest space at fixed native bases (16 TiB on x86-64). `guest_flat_memory.cpp` uses VirtualAlloc2/MapViewOfFile3 or mmap/mprotect, aliased host/guest views, executable-write guards and deferred EFB read faults. `main.cpp` has Windows vectored/POSIX signal crash and fault handling. `guest_flat_memory_macos.cpp` supplies another native mapping implementation. WASM linear memory cannot preserve this virtual-memory scheme.

`memory.cpp` and `include/memory.h` already model regions and checked access, providing a starting contract, but initialization still relies on flat backing. Current sizes are 24 MiB MEM1 and **128 MiB MEM2**, with physical/cached/uncached aliases and a 2 MiB Kamek overlay. Do not substitute retail MEM2 size. Implement aliased offsets over compact backing with explicit MMIO, deferred reads and write notifications. Never reserve 4 GiB merely to imitate guest addresses.

### CPU helpers, math and compiler ABI — Major adaptation required

`include/isa/ppc_isa_config.h` explicitly errors outside x86-64/AArch64; SSE/AVX/NEON paired-single helpers need scalar or wasm SIMD equivalents. `ppc_isa_fpenv.h` uses MXCSR/FPCR to implement FPSCR NI and errors on WASM. WASM has no equivalent mutable host FP control register: software semantics and regression tests are required. Audit NaNs, signed zero, subnormals, fused operations, quantized loads and rounding before performance work. Never use relaxed SIMD or fast-math without proof of equivalent behavior.

Clang-specific vector types/attributes, Windows `__regcall`, state-free return vectors, function pointer casts and tail-call patterns require compile/ABI tests. Native flags and pointer-width assumptions must not leak into wasm32. Endian integer logic and pure parsers are candidates for reuse, **not yet verified portable as a complete runtime**.

### Aurora integration and GX — Major adaptation required

`runtime/CMakeLists.txt` includes `aurora-main` and native products link `aurora::gx`, `pad`, `si`, `vi`, `mtx`. Runtime GX HLE under `src/hle/gx` feeds the compatibility layer. Aurora's `lib/gx`, `lib/dolphin/gx` and `lib/gfx` implement command/state handling, textures, EFB copies, pipeline/shader generation and interpolation. Shader sources already use WGSL (`lib/gfx/clear.cpp`, `depth_peek.cpp`, `tex_copy_conv.cpp`). Keep that renderer.

`BACKEND_WEBGPU` maps to `wgpu::BackendType::WebGPU`, but no first-party Emscripten path was found. `cmake/aurora_core.cmake` sets `WEBGPU_DAWN` and links `dawn::webgpu_dawn`. `lib/webgpu/gpu.cpp` creates SDL/native surfaces through `lib/dawn/BackendBinding.cpp`, asks for `TimedWaitAny`, blocks for adapter/device requests and queue completion, and uses Dawn cache/toggle extensions. It also probes `ImplicitDeviceSynchronization` for concurrent encoding/presentation. These are not evidence of browser support.

Needs: browser WebGPU C API binding/provider, async adapter/device and mapping operations, canvas surface, one clearly owned GPU execution context, removal of native Dawn extension requirements, feature/limit checks and device-loss handling. GPU readbacks in `gfx/{depth_peek,efb_ram_copy,common}.cpp` and screenshot capture in `aurora.cpp` cannot busy-wait while their JS completions require the event loop. Preserve guest-visible ordering when yielding. Pipeline compatibility and shader limits remain **Blocked / unknown** until synthetic GX tests run in-browser.

### Window, presentation, timing and main loop — Needs browser replacement

`aurora-main/lib/window.cpp`, `input.cpp`, `aurora.cpp` use SDL windows, native handles/events, surface locks and presenter workers. `runtime/src/hle/vi.cpp` contains retrace state, deadlines, sleeps/spin waits and presentation coordination. It starts near 60 Hz and includes video-mode-dependent timing; preserve actual upstream mode behavior rather than blindly making every callback a 60 Hz tick. Interpolation is separate work in `aurora-main/lib/gx/frame_interpolation.cpp`.

Browser host scheduling must yield at existing guest wait/retrace boundaries. requestAnimationFrame controls presentation only. Maintain simulation deadlines, callback order and input snapshots, and pause deliberately on tab suspension without silently dropping guest ticks. Canvas resize, DPI, fullscreen and aspect policies belong in a web surface adapter. Preserve configurable internal resolution independently from CSS/backing size.

### Audio — Major adaptation required

`runtime/src/hle/audio/{audio,ax_mix,ax_dsp,ax_effects,ax_memory}` owns guest mixing. `audio_backend.cpp` submits signed 16-bit PCM through SDL3 audio streams with gain/mute. `ax_mix.cpp` starts a mix worker and `ax_mix_kernels.h` contains architecture optimizations. Preserve the mixer; use a bounded PCM bridge to AudioWorklet/WebAudio, handle rate conversion, underrun/overrun, volume/mute, pause/resume and gesture unlock. SDL web audio may help, but its exact AudioWorklet/latency behavior is unverified. `music_attenuation.cpp` watches Windows media sessions/Linux MPRIS through OS APIs; cross-application music ducking needs disabling on web, with an explicit explanation.

### Input and configuration — Minor adaptation required for pure logic; Needs browser replacement for host access

`input_expr.cpp` is self-contained expression evaluation with tests. `input_bindings.cpp`, `settings_overlay.cpp`, `controller_mapping_wizard.cpp`, `wii_remote_input.cpp` and HLE PAD/WPAD/KPAD depend on SDL and native devices. Aurora `lib/input.cpp` and PAD/SI implementations also participate. Reuse mapping semantics; normalize keyboard and standard Gamepad API devices into that model. Poll once at the simulation input boundary; handle disconnection, focus loss, dead zones and nonstandard mappings. Xbox/PlayStation/USB support is conditional on browser mapping. Raw libusb GameCube adapters and real Wii Remotes are later work, not guaranteed browser gamepads.

`include/runtime_config.h` and TOML-related headers/settings code retain the config model. Browser storage mount readiness must precede config load, and writes must be intentionally flushed with visible failure handling. Clipboard becomes the browser clipboard API with explicit user activation.

### Filesystem, disc and saves — Needs browser replacement

`src/platform/host_platform.cpp` abstracts a few paths/process IDs only; it is not a complete platform interface. `system_bridge.cpp`, config helpers, logging, NAND/ISFS, DVD, Riivolution, mod loading and Aurora caches still call host filesystem APIs directly. See [filesystem inventory](web-audit/filesystem.txt).

`hle/storage/dvd.cpp` indexes extracted FST/files and performs synchronous offset reads using `DvdReadContract`; the runtime is not currently an ISO reader. `Launcher/.../DiscTool.cs` shells out to nod tooling for extraction. ISO access must include Wii partition/decryption/FST handling, not just Blob slicing. Format identification/checksums must preserve supported PAL revision validation.

NAND code includes rename/copy/hard-link and rollback assumptions (`nand_api.cpp`) that require transactional browser equivalents. Persist user saves/config/mappings/metadata in IndexedDB or OPFS. Cache eviction, quota failures, export/import and crash consistency need tests. Avoid storing full temporary extraction indefinitely. Implement 64-bit disc offsets with bounded reads and explicit overflow/EOF checks; do not read the full image into WASM.

### Thread inventory — Major adaptation required

First-party creation sites and their proposed disposition (not implemented):

- `runtime/src/main.cpp:490,493`: two POSIX transcript pipe readers; replace with direct bounded browser log events.
- `runtime/src/hle/audio/ax_mix.cpp:1351`: mix worker; retain as pthread/Worker when isolated, feed AudioWorklet ring buffer; evaluate serial mixing separately.
- `runtime/src/music_attenuation.cpp:455,458`: detached Windows/Linux media watchers; omit on web.
- `runtime/src/hle/net/network_deferred.cpp:274`: detached resolver work; replace with asynchronous browser networking later; no native DNS/socket workers in offline milestone.
- `aurora-main/lib/aurora.cpp:351`: frame worker; candidate pthread with command ownership preserved, or serialize into GPU-owning worker.
- `aurora-main/lib/aurora.cpp:1118`: presenter; merge into GPU owner/browser presentation scheduling, not a separate thread calling browser GPU objects concurrently.
- `aurora-main/lib/gx/frame_interpolation.cpp:99`: interpolation pool; optional bounded pthread pool, serial fallback candidate.
- `aurora-main/lib/gfx/pipeline_cache.cpp:1180`: pipeline workers; replace GPU work with async creation on GPU owner; CPU preparation may remain workers.
- `aurora-main/lib/gfx/pipeline_cache.cpp:1136`: cache writer; replace with serialized asynchronous persistence/OPFS worker.
- `runtime/tests/nand_settings_tests.cpp`: test-only contention threads; retain native tests and later add isolated-browser equivalents.
- Aurora optional nod DVD sets `preloader_threads=1`; SDL audio/HID, Dawn drivers, nod and other downloaded libraries may spawn additional threads. Audit after dependency selection; not claimed exhaustive below that boundary.

Guest OSThreads are **fibers, not pthreads**. `fiber_manager.cpp` allocates 1 MiB host stacks per guest fiber and delegates to `host_context.cpp`: Win32 Fibers, Linux libco, macOS AArch64 assembly. This contract is **Blocked / unknown** for browser runtime until Emscripten Asyncify fibers or explicit resumable continuations pass synthetic scheduler tests. Moving main to a pthread does not solve cooperative guest stack switching.

### Networking — Needs browser replacement, deferred

`hle/net/network_{core,socket,deferred,ssl}.cpp` uses Winsock/POSIX TCP/UDP, DNS, polling, blocking completion and Schannel/mbedTLS. `discord_presence.cpp` is another desktop integration. Browsers cannot directly preserve arbitrary native socket semantics. Return explicit offline behavior first; later assess a documented WebSocket/WebRTC/HTTP protocol/service adapter. Do not introduce a hidden proxy or upload game files. Desktop setup download/HTTP code is not a browser game networking implementation.

### OS-specific APIs and dynamic libraries — Needs browser replacement

Windows: native fibers, virtual mappings/protection, vectored exception handling, DbgHelp, process pipes, known folders, WinRT/media sessions, Schannel, Winsock and DLL packaging. Linux: libco assembly contexts, mmap/mprotect, signals/ucontext, fd/poll/socket APIs, dlopen/dlsym of libdbus for MPRIS, home/current-directory assumptions. macOS: custom AArch64 context assembly, Mach/dyld paths, Objective-C/Metal surface paths and mmap. None should be selected by a generic non-Windows fallback on Emscripten.

`guest_flat_memory.cpp` dynamically resolves Windows mapping functions; `music_attenuation.cpp` loads DBus. Aurora provider/copy-DLL CMake modules package native shared SDL/Dawn/nod where configured. These are host dependencies, not proof of runtime-translated game DLL loading: translated products are statically linked via shard manifests. Browser builds should statically link open-source runtime code; WASM dynamic module loading is a separate design decision, not `dlopen` compatibility.

### Build system and libraries — Major adaptation required

`runtime/CMakeLists.txt` requires 64-bit Clang, Release and specific native OS/ISA combinations before creating targets. `runtime/cmake/PublicProducts.cmake` consumes generated code, native CPU flags and libraries. Native setup runs .NET/nod/CMake/compiler subprocesses, which a web page cannot copy directly.

Dependency inventory and initial disposition:

- SDL3 3.4.4: adapt/build web provider; window/audio/input behavior must be tested.
- Dawn v20260603.191052: replace native library/provider with compatible browser WebGPU bindings; C++ API version alignment unknown.
- nod v2.0.0-alpha.8 (optional Aurora DVD and desktop extraction tool): browser extraction build feasibility unknown; begin ISO only.
- libusb 1.0.30: omit raw USB controller path initially.
- libco: replace guest-context backend; native assembly cannot target WASM.
- Crypto++ 8.9.0: assembly disabled in native build already; CSPRNG and wasm build still need tests.
- mbedTLS/Schannel: omit offline web closure; future networking replacement changes TLS ownership.
- toml11, pugixml, fmt, xxhash, magic_enum: source-level reuse candidates, minor build adaptation expected.
- Abseil, zlib, libpng, FreeType, ImGui, SQLite, zstd: retain only needed functionality; thread/filesystem/build providers require wasm validation, especially caches.
- Tracy: disable native instrumentation transport initially.
- Aurora CARD/GCI file handling: unused paths should be excluded where possible, otherwise persistence adaptation required.

Pure algorithms/config parsers are likely portable; no complete subsystem is labeled **Already portable** without compiling/testing its selected browser dependency closure. **Already portable** at source level: the YAML project descriptions and source-only documentation have no execution-platform dependency; their referenced paths still require web setup adaptation.

## Memory audit and measurements still needed

Known baseline: 152 MiB of MEM1+MEM2 backing, plus overlay/other regions; avoid tripling this for aliases. Native 4 GiB reservation is virtual, not the physical RAM requirement. Guest stacks cost 1 MiB each on the host. Texture caches/uploads, EFB render/readback buffers, frame/interpolation queues, shader/pipeline caches, audio queues, C++ containers and translation IR/source all add separate peaks. Relevant code: `memory.cpp`, `fiber_manager.cpp`, Aurora `gfx/{texture,pipeline_cache,efb_ram_copy,common}.cpp`, `webgpu/gpu_cache.cpp`, `gx/frame_interpolation.cpp`, `hle/audio/ax_mix.cpp` and translator CLI file reads.

Actual translated WASM size, compiler working set, initial/peak heap and GPU cache budgets are unknown without private local translation. Measure stages separately; memory growth invalidates cached JS typed-array views. Do not assert 1080p/60 FPS or a small startup download from the clear-screen probe.

## Content boundary findings

The inherited tree contains `runtime/assets/wii/shared2/wc24/*`, `assets/dsp/dsp_coef.bin`, and `assets/pipeline/initial_pipeline_cache.db`. README/notices attribute bootstrap/coefficient data to Dolphin; attribution alone does not establish suitability under this project's stricter no-NAND/no-game-texture policy. The cache's content provenance is also unverified. Flag these inherited assets for a separate provenance/removal decision before publishing a game-capable build. Do not redistribute them with the probe. This audit did not delete inherited assets or certify the entire repository copyright-clean. No supplied WiiCompiled Web logo was present; the existing README links an upstream logo and the only tracked PNG is Aurora's logo. Do not substitute either as the requested new branding.

## Validation and next gates

Milestone 0 changes documentation only. The companion [architecture](web-architecture.md) selects a local AOT development path and preserves the browser-only setup question as unresolved. A separately scoped Milestone 1 probe may compile only its own handwritten source, initialize JS/WASM, create WebGPU canvas and clear it. That is **not** proof that the upstream runtime or Aurora compiles to WASM.

Before game boot: synthetic guest memory/MMIO/alias tests; FP/paired-single differential tests; guest scheduler ordering/stack tests; Aurora synthetic GX/readback tests; bounded disc-reader ISO extraction tests; storage crash/quota tests; then locally supplied supported PAL image. No Nintendo data in CI. Record compiler/browser versions, failures and actual measurements in the probe verification report.

# UE / Moer RDG CPU comparison

This is a CPU graph-management benchmark, not a renderer or GPU benchmark.
Both implementations import pre-created textures (64x64, 4 mips, 2 layers) and
4096-byte buffers. Pass `p` accesses resource `(p * 3 + a) % resources` for each
access `a`; every fourth pass writes and the others read. Both retain all passes
and validate the callback count. Each callback only increments that count.

The primary lifecycle interval includes construction, declarations, compilation,
recording and graph destruction. UE does some dependency preparation in AddPass,
so comparing only functions named Compile would omit work. Separate build and
execution-entry intervals are diagnostic, not identical internal phases.

UE uses Development, NullRHI, no tracing, and serial setup/compile/execute/destruction.
Moer uses Release, fake bound resources, no active ProfileDump producer, and serial
recording. UE drains its native RHI command storage **after** the timed interval;
Moer destroys the frontend command list **inside** it. UE's development validation
remains compiled in, as do Moer's normal runtime checks. These differences prevent
treating the result as an exact implementation-independent overhead ratio.

Example PowerShell setup (UE 5.7.1 and MSVC):

```powershell
cmake -S . -B build/rdg-release -G 'Visual Studio 17 2022' -A x64 `
  -DMOER_IGNORE_ENABLE_FEATURES=ON -DWITH_CUDA=OFF -DWITH_NRD=OFF `
  -Dmoer_build_test=ON -DMOER_VULKAN_TEST_SUPPORT=ON `
  -DBINARY_ROOT_DIR=D:/C++_Files/MoerEngine/build/rdg-release-target
cmake --build build/rdg-release --config Release --target MoerEditor TestRenderGraphCompilerBenchmark --parallel 20

# Use a new, empty destination; build generated UE files outside the source tree.
Copy-Item tools/rdg_benchmark/unreal build/ue-rdg-benchmark -Recurse
& D:/UE_5.7/Engine/Build/BatchFiles/Build.bat RdgCpuBenchmarkEditor Win64 Development `
  -Project=D:/C++_Files/MoerEngine/build/ue-rdg-benchmark/RdgCpuBenchmark.uproject -WaitMutex -NoHotReload
powershell -NoProfile -ExecutionPolicy Bypass -File tools/rdg_benchmark/run_comparison.ps1
```

The script writes results under `build/rdg-ue-comparison`. Each of three matrix
rounds measures 16/64/128 passes, the same number of resources of each kind, and
4/16/16 accesses of each kind per pass. It uses 100 warmups and 1000 samples.
The script pins both benchmark threads to logical CPU 2 (`-Cpu` overrides it).
Moer's `--cpu` switch is Windows-only; omitting it leaves affinity unchanged.
UE's three rounds run in one commandlet process; Moer starts one process per
case/phase/round. Report the run distribution and median of run medians.
Do not build or run another editor while sampling. `-SkipUnreal` reruns only Moer.

Moer's original `--lower`, `--record` and default Compile modes remain available.
`--lifecycle` measures the whole graph lifetime and does not enable allocation hooks.
UE Whole-buffer access does not express Moer's byte-range hazard model, so the
cross-engine matrix intentionally excludes `--subranges`; use it for Moer A/B
regression measurement instead. Record the exact engine revision, build flags,
binary hashes and runtime limitations with any published numbers.

## Function, stage, and allocation diagnosis

The detailed investigation and its limitations are recorded in
[`docs/RDG_PROFILING_2026_10.md`](../../docs/RDG_PROFILING_2026_10.md).

For stack sampling, keep the Release optimization flags and add symbols:

```powershell
cmake -S . -B build/rdg-release `
  '-DCMAKE_CXX_FLAGS_RELEASE=/O2 /Ob2 /DNDEBUG /Zi' `
  '-DCMAKE_C_FLAGS_RELEASE=/O2 /Ob2 /DNDEBUG /Zi' `
  '-DCMAKE_EXE_LINKER_FLAGS_RELEASE=/INCREMENTAL:NO /DEBUG:FULL /OPT:REF /OPT:ICF' `
  '-DCMAKE_SHARED_LINKER_FLAGS_RELEASE=/INCREMENTAL:NO /DEBUG:FULL /OPT:REF /OPT:ICF'
cmake --build build/rdg-release --config Release --target TestRenderGraphCompilerBenchmark --parallel 20
cmake -S tools/rdg_benchmark -B build/rdg-profile-tools -A x64
cmake --build build/rdg-profile-tools --config Release
New-Item -ItemType Directory -Force build/rdg-profile | Out-Null
$env:_NT_SYMBOL_PATH = "$PWD/build/rdg-release-target/bin/Release"
& ./build/rdg-profile-tools/Release/RdgSampleProcess.exe `
  --output build/rdg-profile/moer-sample --delay-ms 300 -- `
  ./build/rdg-release-target/bin/Release/TestRenderGraphCompilerBenchmark.exe `
  --lifecycle --cpu 2 --passes 128 --resources 128 --accesses 16 --warmup 200 --samples 16000
python tools/rdg_benchmark/analyze_stacks.py build/rdg-profile/moer-sample.folded
```

The fallback sampler launches its own hidden child, samples its main thread (or
`--thread RenderThread` for UE), and writes the exact command, child log, folded
stacks, exit status and suspension time. This is intrusive wall-time sampling;
do not use sampled benchmark timings as performance measurements. Keep matching
PDBs. A nonzero child exit status invalidates a completed-run claim. When passing
UE arguments through PowerShell and the sampler, quote each complete argument,
for example `"-Output=$OutputPath"`, to preserve paths after the drive colon.

For independent diagnostic builds, repeat the optimized configure command with
`-B build/rdg-profile-diagnostics`,
`-DBINARY_ROOT_DIR=D:/C++_Files/MoerEngine/build/rdg-profile-target` and
`-DMOER_RDG_DIAGNOSTICS=ON`. Preserve all feature and symbol flags above. The local
Agility SDK can be reused with `-DMOER_AGILITYSDK_SOURCE_DIR=<existing SDK source>`.
Create `build/rdg-profile-target/bin/Release` before the first build because the
third-party DLL copy targets can otherwise race directory creation.

```powershell
cmake --build build/rdg-profile-diagnostics --config Release --target TestRenderGraphCompilerBenchmark --parallel 20
powershell -NoProfile -ExecutionPolicy Bypass -File tools/rdg_benchmark/run_diagnostics.ps1
```

The script creates a new result directory, records binary hashes, checks probe
coverage, calibrates the instrumented executable against ordinary Release, and
runs `--diagnose timing`, `operations`, and `allocations` separately. Allocation
counts cover ordinary new/delete in Render and the benchmark, default PMR upstream
requests, and the Memory API; they do not cover all DLL or direct allocator calls.
Requested bytes are cumulative, not peak live memory. Allocation stacks use fixed
storage and report any dropped sites. All probes and allocation overrides are
absent when `MOER_RDG_DIAGNOSTICS=OFF` (the default).

The UE commandlet additionally reports graph destruction, command drain, and a
total including drain. `-WorkStats` reads existing RenderCore diagnostic counters
through the installed engine's matching PDB, outside timed iterations. It does
not patch the engine or depend on FRDGBuilder's private layout. Missing symbols
fail the diagnostic run. Counter deltas cover warmup plus measured iterations;
`GRDGStatMemoryWatermark` is a maximum and must not be divided by iteration count.
UE transition entries, batches and dependency counts differ semantically from
Moer's barrier descriptions and explicit resource-reason edges.

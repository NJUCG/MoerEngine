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

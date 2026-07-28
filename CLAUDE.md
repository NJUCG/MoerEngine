# MoerEngine

Real-time Vulkan rendering engine (C++20, Vulkan 1.3) with raster and ray-tracing pipelines, ImGui-based editor.

## Rule: strict-engineering

You are a strict software engineer. Follow these constraints in every response:

1. **No fallbacks (不做兜底).** If a design is wrong, fix it at the source — don't paper over it with defensive checks, silent clamping, or default values that hide the real problem. A crash is better than silently wrong behavior.

2. **Concise implementation (实现简洁).** Write the minimum code that correctly solves the problem. Don't add parameters, overloads, or branches "just in case." Delete dead code; don't comment it out.

3. **No unnecessary static wrappers (不做无效static封装).** Free functions in `.cpp` files are fine. Don't wrap stateless functions in classes or create helper classes with a single method. Don't add `static` to functions just to hide them from the linker — anonymous namespaces exist for that.

4. **No random helpers (不乱写helper).** A helper function must have a well-defined, reusable contract. If a piece of logic is called exactly once and is shorter than the comment that would explain it, inline it. Don't extract "helpers" that just pass through to another function with one argument changed.

5. **No try-catch.** Never use try-catch for control flow. Handle error conditions with early returns or sentinel values. For string conversion, replace invalid code points with U+FFFD instead of throwing.

6. **Use MoerString/MoerChar for string operations.** All string manipulation uses wide-character PlatformString/StringView. Avoid UTF-8 conversion unless required by an external API. Debug logging with resource names uses `LOG_INFO(MOER_TEXT("..."), texture->GetName())` — `WideToUtf8` safely replaces invalid chars.

7. **Prove it works.** When you make a change, state what observable behavior confirms correctness. If the change touches Vulkan, reference the specific VU or spec section.

## Skills

Use `.claude/skills/<name>/SKILL.md` for domain-specific guidance:
- `vulkan` — Vulkan API usage: spec-driven, no guessing, modern features
- `powershell` — PowerShell 5.1 strict compatibility
- `git` — Branch strategy, merge priority, commit format
- `run-moerengine` — Build, test, launch the engine

## Build System

- **Build directory:** `build/clang-debug/` (Ninja + Clang 22.x, Debug)
- **Output:** `target/bin/{Config}/MoerEditor.exe`
- **Compiler:** `F:/LLVM_22.1.4/bin/clang++` (pure Clang, not clang-cl)
- **CMake:** `F:/CMake/bin/cmake` 3.26+
- **Ninja:** `/f/Ninja/ninja`
- **Vulkan SDK:** `F:/VulkanSDK/1.4.341.1/`

### Build Commands

```bash
# Full build
cmake --build build/clang-debug --config Debug --target MoerEditor TestRHITranslate -j20

# Single target
cmake --build build/clang-debug --config Debug --target TestRHITranslate -j20
```

## Key Constraints

- `just` is not installed — use `cmake --build` directly
- Tests must run from `target/bin/{Config}/` (they look for `MoerEngine.toml` and `asset/` relative to CWD)
- Renderer toggle in `MoerEngine.toml`: `default_render_method = "Raster"` or `"Raytracing"`
- Shader cache: `asset/shader_cache/` — delete if corrupt
- D3D12 backend (`TestDxRhi`) is partial and may crash — focus on Vulkan (`TestRHITranslate`)
- `TestStringSystem` fails to compile (pre-existing Clang/MSVC `std::apply` + `wchar_t` incompatibility)

## Current Branch: `dev_parallel_rhi`

Active development branch. See `.claude/skills/git/SKILL.md` for branch/merge policy.

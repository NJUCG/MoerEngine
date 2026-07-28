---
name: powershell
description: PowerShell 5.1 strict compatibility. All PowerShell commands must work on Windows PowerShell 5.1 — no PS7+ features.
---

# PowerShell Skill

## Core Rule

All PowerShell code must run on **Windows PowerShell 5.1** (the built-in `powershell.exe` on Windows 10/11). Test with `$PSVersionTable.PSVersion` — it must show `Major=5`.

## Forbidden Syntax (PS 7+ only)

| Forbidden | Reason | 5.1 Alternative |
|---|---|---|
| `??` null-coalescing | PS 7+ | `if ($null -eq $x) { $default } else { $x }` |
| `?:` ternary | PS 7+ | `if ($cond) { $a } else { $b }` |
| `&&` / `\|\|` pipeline chain | PS 7+ | `if ($?) { ... }` or `; if ($LASTEXITCODE ...)` |
| `ForEach-Object -Parallel` | PS 7+ | `ForEach-Object` (sequential) or `Start-Job` |
| `Get-Content -AsByteStream` | PS 7+ | `Get-Content -Encoding Byte` |
| `ConvertFrom-Json -AsHashtable` | PS 7+ | `ConvertFrom-Json` then manual hashtable conversion |
| `Write-Information` | PS 5+ (rarely used) | `Write-Host` or `Write-Output` |
| `Join-String` | PS 7+ | `-join` operator |

## String Quoting and Escaping

PowerShell 5.1 quoting is strict. When escaping for nested shells, you pay double-escape tax.

### Single-quoted strings (`'...'`): literal, no variable expansion
- To embed a single quote: double it `'It''s working'`
- Backticks inside single quotes are LITERAL — no escape meaning

### Double-quoted strings (`"..."`): variables expand, backtick escapes
- `` `n `` newline, `` `t `` tab, `` `" `` literal quote
- `$var` / `${var}` expand
- Inside double quotes: backticks escape. Outside: backticks are escape chars but ONLY for newlines — otherwise they're literal.
  - ``Write-Host `"hello`"`` prints `"hello"` (backtick needed inside double quotes)
  - But `$file = 'C:\test''s'` — single quote doubled

### Nested escaping (PowerShell inside Bash inside PowerShell)

When writing a Bash command that calls PowerShell:
```
Bash invocation: "..." (JSON string)
  → shell sees: powershell -Command "..."
    → Command string: double-quoted PS string
      → inner quotes: \`" (backtick escaped, then backslash escaped for JSON)
```

### Pattern: Start-Process with redirect paths

In PS 5.1:
```powershell
$p = Start-Process -FilePath './MoerEditor.exe' -RedirectStandardOutput 'F:/out.log' -RedirectStandardError 'F:/err.log' -PassThru
Start-Sleep -Seconds 25
if (-not $p.HasExited) {
    Stop-Process -Id $p.Id -Force
    Write-Host 'Process killed'
}
```

## Path Handling

- Always use forward slashes `/` in PS paths — 5.1 accepts them
- `Join-Path` is safe for cross-platform path construction
- `$env:TEMP` / `$env:USERPROFILE` for temp/user directories

## Error Handling (5.1)

```powershell
# When calling .exe files, $LASTEXITCODE is set
& './myapp.exe'
if ($LASTEXITCODE -ne 0) {
    Write-Host "Failed with exit code: $LASTEXITCODE"
    exit 1
}

# For try/catch, use -ErrorAction Stop
try {
    Get-Content 'file.txt' -ErrorAction Stop
} catch {
    Write-Host "Error: $($_.Exception.Message)"
}
```

## Common MoerEngine PowerShell Patterns

### Run editor and collect logs
```powershell
$p = Start-Process -FilePath './MoerEditor.exe' -RedirectStandardOutput 'stdout.txt' -RedirectStandardError 'stderr.txt' -PassThru -NoNewWindow
Start-Sleep -Seconds 25
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
Get-Content 'stderr.txt' -Encoding Unicode | Where-Object { $_ -match '\[error\]' }
```

### Filter validation errors
```powershell
Get-Content 'stdout.txt' -Encoding Unicode | Where-Object { $_ -match 'Validation Error|SYNC-HAZARD|VUID' }
```

### Run a test executable
```powershell
Push-Location 'target/bin/Debug'
& './TestRHITranslate.exe'
$exitCode = $LASTEXITCODE
Pop-Location
exit $exitCode
```

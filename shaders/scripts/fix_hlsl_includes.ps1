# fix_hlsl_includes.ps1
# Automatically fix #include paths in HLSL files
# [Location] Must be run from shaders/scripts/ directory

Write-Host "Building file index..." -ForegroundColor Cyan

# 1. Path Logic
# -----------------------------------------------------------
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$rootPath = (Get-Item $scriptDir).Parent.FullName

Write-Host "Script Location: $scriptDir" -ForegroundColor Gray
Write-Host "Shaders Root:    $rootPath" -ForegroundColor Gray

if (-not (Test-Path $rootPath)) {
    Write-Error "Error: Cannot locate 'shaders' directory!"
    exit
}

$fileMap = @{}
$duplicateFiles = @()

# 2. Build Index
# -----------------------------------------------------------
$allShaderFiles = Get-ChildItem -Path $rootPath -Include *.hlsl, *.hlsli -Recurse -File

foreach ($file in $allShaderFiles) {
    $fileName = $file.Name
    # Get relative path using forward slashes
    $relativePath = $file.FullName.Substring($rootPath.Length + 1).Replace("\", "/")
    
    if ($fileMap.ContainsKey($fileName)) {
        $duplicateFiles += $fileName
        Write-Warning "Duplicate file found: $fileName (Skipping)"
    } else {
        $fileMap[$fileName] = $relativePath
    }
}

Write-Host "Index built. Found $($fileMap.Count) unique files." -ForegroundColor Green

# 3. Execute Replacement
# -----------------------------------------------------------
Write-Host "`nScanning and replacing #include..." -ForegroundColor Cyan

$processedCount = 0
$utf8Encoding = [System.Text.Encoding]::UTF8

foreach ($file in $allShaderFiles) {
    try {
        $content = [System.IO.File]::ReadAllText($file.FullName, $utf8Encoding)
    } catch {
        Write-Warning "Cannot read file: $($file.Name)"
        continue
    }

    if ([string]::IsNullOrEmpty($content)) {
        Write-Host "Skipping empty file: $($file.Name)" -ForegroundColor DarkGray
        continue
    }

    $originalContent = $content
    
    # Regex: Match both "..." and <...>
    $regex = '(#include\s+)(["<])(.+?)([">])'
    
    $newContent = [Regex]::Replace($content, $regex, {
        param($match)
        $prefix = $match.Groups[1].Value
        $startChar = $match.Groups[2].Value
        $oldPath = $match.Groups[3].Value
        $endChar = $match.Groups[4].Value
        
        # Pair check
        if (($startChar -eq '"' -and $endChar -ne '"') -or 
            ($startChar -eq '<' -and $endChar -ne '>')) {
            return $match.Value
        }

        $targetFileName = [System.IO.Path]::GetFileName($oldPath)
        
        if ($fileMap.ContainsKey($targetFileName)) {
            if ($duplicateFiles -contains $targetFileName) {
                return $match.Value
            }
            
            $newPath = $fileMap[$targetFileName]
            
            if ($oldPath -eq $newPath) {
                return $match.Value
            }
            
            return "$prefix$startChar$newPath$endChar"
        } else {
            return $match.Value
        }
    })
    
    if ($newContent -ne $originalContent) {
        [System.IO.File]::WriteAllText($file.FullName, $newContent, $utf8Encoding)
        Write-Host "Fixed: $($file.Name)" -ForegroundColor Gray
        $processedCount++
    }
}

Write-Host "`n--------------------------------------------------"
Write-Host "Done! Modified $processedCount files." -ForegroundColor Yellow
Write-Host "--------------------------------------------------"
Read-Host "Press Enter to exit..."
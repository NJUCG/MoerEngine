# fix_cpp_includes.ps1
# Automatically fix HLSL file paths in C++ source code
# [Location] Must be run from shaders/scripts/ directory

Write-Host "Initializing C++ path fix (UTF-8 Safe)..." -ForegroundColor Cyan

# 1. Path Logic (Based on script location)
# -----------------------------------------------------------
# Get script directory (.../shaders/scripts)
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition

# Find Project Root (Up 2 levels: .../shaders/scripts -> .../shaders -> .../ProjectRoot)
$projectRoot = (Get-Item $scriptDir).Parent.Parent.FullName

# Find Shaders Root
$shaderRoot = Join-Path $projectRoot "shaders"

Write-Host "Script Location: $scriptDir" -ForegroundColor Gray
Write-Host "Project Root:    $projectRoot" -ForegroundColor Gray
Write-Host "Shaders Root:    $shaderRoot" -ForegroundColor Gray

# Safety Check
if (-not (Test-Path $shaderRoot)) {
    Write-Error "Error: Cannot locate 'shaders' directory!"
    Write-Error "Please ensure this script is located in 'shaders/scripts/'."
    exit
}

# 2. Build Shader Index
# -----------------------------------------------------------
$fileMap = @{}
$duplicateFiles = @()

Write-Host "Scanning shaders directory for index..." -ForegroundColor Gray
$allShaderFiles = Get-ChildItem -Path $shaderRoot -Include *.hlsl, *.hlsli -Recurse -File

foreach ($file in $allShaderFiles) {
    $fileName = $file.Name
    # Calculate relative path from shaders root
    $relativePath = $file.FullName.Substring($shaderRoot.Length + 1).Replace("\", "/")
    
    if ($fileMap.ContainsKey($fileName)) {
        $duplicateFiles += $fileName
    } else {
        $fileMap[$fileName] = $relativePath
    }
}
Write-Host "Index built. Found $($fileMap.Count) shader files." -ForegroundColor Green

# 3. Scan C++ Files and Replace
# -----------------------------------------------------------
Write-Host "`nScanning C++ files (.cpp, .h, .hpp)..." -ForegroundColor Cyan

# Start scanning from Project Root
# Exclude build folders and the shaders folder itself
$cppFiles = Get-ChildItem -Path $projectRoot -Include *.cpp, *.h, *.hpp, *.c, *.cc -Recurse -File | 
    Where-Object { 
        $_.FullName -notmatch "\\build\\" -and 
        $_.FullName -notmatch "\\\.git\\" -and 
        $_.FullName -notmatch "\\\.vs\\" -and
        $_.FullName -notmatch "\\out\\" -and
        $_.FullName -notmatch "\\target\\" -and
        $_.FullName -notmatch "\\shaders\\" 
    }

$processedCount = 0
$utf8Encoding = [System.Text.Encoding]::UTF8

foreach ($file in $cppFiles) {
    try {
        $content = [System.IO.File]::ReadAllText($file.FullName, $utf8Encoding)
    } catch {
        Write-Warning "Cannot read file: $($file.Name)"
        continue
    }
    if ([string]::IsNullOrEmpty($content)) { continue }
    
    $originalContent = $content
    
    # Regex: Match L"..." strings
    $regex = '(L?")(.+?)(")'
    
    $newContent = [Regex]::Replace($content, $regex, {
        param($match)
        $prefix = $match.Groups[1].Value
        $oldPath = $match.Groups[2].Value
        $suffix = $match.Groups[3].Value
        
        # Only process strings ending in .hlsl or .hlsli
        if (-not ($oldPath.EndsWith(".hlsl", [System.StringComparison]::OrdinalIgnoreCase) -or 
                  $oldPath.EndsWith(".hlsli", [System.StringComparison]::OrdinalIgnoreCase))) {
            return $match.Value
        }

        $targetFileName = [System.IO.Path]::GetFileName($oldPath)
        
        if ($fileMap.ContainsKey($targetFileName)) {
            if ($duplicateFiles -contains $targetFileName) { return $match.Value }
            $newRelativePath = $fileMap[$targetFileName]
            
            # Smart prefix handling
            if ($oldPath -match "^shaders/") {
                $finalPath = "shaders/" + $newRelativePath
            } else {
                $finalPath = $newRelativePath
            }
            
            if ($oldPath -eq $finalPath) { return $match.Value }
            
            return "$prefix$finalPath$suffix"
        }
        return $match.Value
    })
    
    if ($newContent -ne $originalContent) {
        [System.IO.File]::WriteAllText($file.FullName, $newContent, $utf8Encoding)
        Write-Host "Updated C++ file: $($file.Name)" -ForegroundColor Yellow
        $processedCount++
    }
}

Write-Host "`n--------------------------------------------------"
Write-Host "C++ path fix completed (UTF-8 Safe)! Modified $processedCount files." -ForegroundColor Yellow
Write-Host "--------------------------------------------------"
Read-Host "Press Enter to exit..."
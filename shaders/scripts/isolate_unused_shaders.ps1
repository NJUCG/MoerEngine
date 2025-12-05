# isolate_unused_shaders.ps1
# Find unused shader files and move them to shaders/deprecated
# [Location] Must be run from shaders/scripts/ directory

Write-Host "Initializing unused shader isolation..." -ForegroundColor Cyan

# 1. Path Logic (Based on script location)
# -----------------------------------------------------------
# Get script directory (shaders/scripts)
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition

# Find Project Root (Up 2 levels: .../shaders/scripts -> .../shaders -> .../ProjectRoot)
$projectRoot = (Get-Item $scriptDir).Parent.Parent.FullName

# Find Shaders Root
$shaderRoot = Join-Path $projectRoot "shaders"
$deprecatedRoot = Join-Path $shaderRoot "deprecated"

Write-Host "Script Location: $scriptDir" -ForegroundColor Gray
Write-Host "Project Root:    $projectRoot" -ForegroundColor Gray
Write-Host "Shaders Root:    $shaderRoot" -ForegroundColor Gray

if (-not (Test-Path $shaderRoot)) {
    Write-Error "Error: Cannot locate 'shaders' directory!"
    Write-Error "Please ensure this script is located in 'shaders/scripts/'."
    exit
}

# Create deprecated directory
if (-not (Test-Path $deprecatedRoot)) {
    New-Item -ItemType Directory -Path $deprecatedRoot -Force | Out-Null
}

# 2. Get Shader List (Exclude already deprecated)
# -----------------------------------------------------------
Write-Host "Collecting shader file list..." -ForegroundColor Gray

$allShaders = Get-ChildItem -Path $shaderRoot -Include *.hlsl, *.hlsli -Recurse -File | 
    Where-Object { $_.FullName -notmatch [regex]::Escape($deprecatedRoot) }

if ($allShaders.Count -eq 0) {
    Write-Warning "No shader files found (or all are deprecated)."
    exit
}

Write-Host "Found $($allShaders.Count) active shader files." -ForegroundColor Green

# 3. Build Search Index (Project-wide text scan)
# -----------------------------------------------------------
Write-Host "Building project-wide search index..." -ForegroundColor Gray

# Start scanning from Project Root
# Exclude build folders, deprecated folder, and scripts folder
$searchFiles = Get-ChildItem -Path $projectRoot -Include *.cpp, *.h, *.hpp, *.c, *.cc, *.hlsl, *.hlsli, *.cmake, CMakeLists.txt -Recurse -File | 
    Where-Object { 
        $_.FullName -notmatch "\\build\\" -and 
        $_.FullName -notmatch "\\\.git\\" -and 
        $_.FullName -notmatch "\\\.vs\\" -and
        $_.FullName -notmatch "\\out\\" -and
        $_.FullName -notmatch "\\target\\" -and
        $_.FullName -notmatch [regex]::Escape($deprecatedRoot) -and
        $_.FullName -notmatch [regex]::Escape($scriptDir) 
    }

$sb = [System.Text.StringBuilder]::new()
$utf8Encoding = [System.Text.Encoding]::UTF8

foreach ($file in $searchFiles) {
    try {
        $text = [System.IO.File]::ReadAllText($file.FullName, $utf8Encoding)
        [void]$sb.Append($text)
        [void]$sb.Append(" ") 
    } catch {}
}
$allContent = $sb.ToString()

# 4. Analyze and Move
# -----------------------------------------------------------
Write-Host "Analyzing and moving unused files..." -ForegroundColor Cyan

$movedCount = 0

foreach ($shader in $allShaders) {
    $fileName = $shader.Name
    
    # Check if filename exists in project content
    if ($allContent.IndexOf($fileName, [System.StringComparison]::OrdinalIgnoreCase) -eq -1) {
        
        # --- Determine Move Path ---
        # Calculate relative path (e.g. core\math\Old.hlsli)
        $relativePath = $shader.FullName.Substring($shaderRoot.Length + 1)
        
        # Build destination path (e.g. shaders\deprecated\core\math\Old.hlsli)
        $destPath = Join-Path $deprecatedRoot $relativePath
        $destDir = Split-Path $destPath -Parent
        
        # Ensure dest dir exists
        if (-not (Test-Path $destDir)) {
            New-Item -ItemType Directory -Path $destDir -Force | Out-Null
        }
        
        # Move file
        Move-Item -Path $shader.FullName -Destination $destPath -Force
        
        Write-Host "Isolating: $relativePath" -ForegroundColor Yellow
        $movedCount++
    }
}

# 5. Summary
# -----------------------------------------------------------
Write-Host "`n========================================"
if ($movedCount -eq 0) {
    Write-Host "Perfect! No unused code found." -ForegroundColor Green
} else {
    Write-Host "Cleanup complete! Isolated $movedCount files." -ForegroundColor Yellow
    Write-Host "Moved to: shaders/deprecated/"
    Write-Host "Original directory structure preserved."
}
Write-Host "========================================"
Read-Host "Press Enter to exit..."
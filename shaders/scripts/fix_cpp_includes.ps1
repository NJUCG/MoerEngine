# fix_cpp_includes.ps1
# 专门用于修正 C++ 代码中引用的 HLSL 文件路径字符串 (修复中文乱码问题)
# [位置] 本脚本必须放在 shaders/scripts/ 目录下

Write-Host "正在初始化 C++ 路径修正 (UTF-8 安全版)..." -ForegroundColor Cyan

# 1. 路径定位逻辑 (基于脚本文件物理位置)
# -----------------------------------------------------------
# 获取脚本文件所在的目录 (即 .../shaders/scripts)
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition

# 向上两级找到项目根目录 (.../shaders/scripts -> .../shaders -> .../ProjectRoot)
$projectRoot = (Get-Item $scriptDir).Parent.Parent.FullName

# 推断 Shaders 根目录
$shaderRoot = Join-Path $projectRoot "shaders"

Write-Host "脚本位置: $scriptDir" -ForegroundColor Gray
Write-Host "定位项目根目录: $projectRoot" -ForegroundColor Gray
Write-Host "定位 Shaders 目录: $shaderRoot" -ForegroundColor Gray

# 安全检查
if (-not (Test-Path $shaderRoot)) {
    Write-Error "错误：无法定位 shaders 目录！"
    Write-Error "请确认此脚本是否放置在 'shaders/scripts/' 目录下。"
    exit
}

# 2. 建立 Shader 索引
# -----------------------------------------------------------
$fileMap = @{}
$duplicateFiles = @()

Write-Host "正在扫描 shaders 目录建立索引..." -ForegroundColor Gray
$allShaderFiles = Get-ChildItem -Path $shaderRoot -Include *.hlsl, *.hlsli -Recurse -File

foreach ($file in $allShaderFiles) {
    $fileName = $file.Name
    # 计算相对于 shaders 根目录的路径
    $relativePath = $file.FullName.Substring($shaderRoot.Length + 1).Replace("\", "/")
    
    if ($fileMap.ContainsKey($fileName)) {
        $duplicateFiles += $fileName
    } else {
        $fileMap[$fileName] = $relativePath
    }
}
Write-Host "索引构建完成。共 $($fileMap.Count) 个 Shader 文件。" -ForegroundColor Green

# 3. 扫描 C++ 文件并替换字符串
# -----------------------------------------------------------
Write-Host "`n开始扫描 C++ 文件 (.cpp, .h, .hpp)..." -ForegroundColor Cyan

# [关键] 使用计算出的 $projectRoot 作为扫描起点
# 排除无关目录
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
        Write-Warning "无法读取文件: $($file.Name)"
        continue
    }
    if ([string]::IsNullOrEmpty($content)) { continue }
    
    $originalContent = $content
    
    # 正则逻辑
    $regex = '(L?")(.+?)(")'
    
    $newContent = [Regex]::Replace($content, $regex, {
        param($match)
        $prefix = $match.Groups[1].Value
        $oldPath = $match.Groups[2].Value
        $suffix = $match.Groups[3].Value
        
        if (-not ($oldPath.EndsWith(".hlsl", [System.StringComparison]::OrdinalIgnoreCase) -or 
                  $oldPath.EndsWith(".hlsli", [System.StringComparison]::OrdinalIgnoreCase))) {
            return $match.Value
        }
        $targetFileName = [System.IO.Path]::GetFileName($oldPath)
        
        if ($fileMap.ContainsKey($targetFileName)) {
            if ($duplicateFiles -contains $targetFileName) { return $match.Value }
            $newRelativePath = $fileMap[$targetFileName]
            
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
        Write-Host "已更新 C++ 文件: $($file.Name)" -ForegroundColor Yellow
        $processedCount++
    }
}

Write-Host "`n--------------------------------------------------"
Write-Host "C++ 路径修正完成 (UTF-8 模式)！共修改了 $processedCount 个文件。" -ForegroundColor Yellow
Write-Host "--------------------------------------------------"
Read-Host "按回车键退出..."
# fix_hlsl_includes.ps1
# 自动修正 HLSL 文件中的 #include 路径 (支持 "" 和 <>)
# [位置] 请将此脚本放在 shaders/scripts/ 目录下运行

Write-Host "正在构建文件索引..." -ForegroundColor Cyan

# 1. 路径定位逻辑 (关键修改)
# -----------------------------------------------------------
# 获取脚本当前所在目录 (shaders/scripts)
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition

# 推断 Shaders 根目录 (脚本的上一级: shaders/scripts -> shaders)
$rootPath = (Get-Item $scriptDir).Parent.FullName

Write-Host "脚本位置: $scriptDir" -ForegroundColor Gray
Write-Host "Shaders根目录: $rootPath" -ForegroundColor Gray

if (-not (Test-Path $rootPath)) {
    Write-Error "错误：无法定位 shaders 目录！"
    exit
}

$fileMap = @{}
$duplicateFiles = @()

# 2. 建立索引 (从 $rootPath 开始扫描)
# -----------------------------------------------------------
# [关键修改] Path 改为 $rootPath
$allShaderFiles = Get-ChildItem -Path $rootPath -Include *.hlsl, *.hlsli -Recurse -File

foreach ($file in $allShaderFiles) {
    $fileName = $file.Name
    # 获取相对路径，统一为正斜杠
    # 这里的 Substring 是基于 $rootPath 的长度
    $relativePath = $file.FullName.Substring($rootPath.Length + 1).Replace("\", "/")
    
    if ($fileMap.ContainsKey($fileName)) {
        $duplicateFiles += $fileName
        Write-Warning "发现重名文件: $fileName (将被跳过)"
    } else {
        $fileMap[$fileName] = $relativePath
    }
}

Write-Host "索引构建完成。共找到 $($fileMap.Count) 个唯一文件。" -ForegroundColor Green

# 3. 执行替换
# -----------------------------------------------------------
Write-Host "`n开始扫描并替换 #include..." -ForegroundColor Cyan

$processedCount = 0
# 定义 UTF-8 编码对象 (带 BOM)
$utf8Encoding = [System.Text.Encoding]::UTF8

foreach ($file in $allShaderFiles) {
    # [修复] 使用 .NET 读取以处理编码
    try {
        $content = [System.IO.File]::ReadAllText($file.FullName, $utf8Encoding)
    } catch {
        Write-Warning "无法读取文件: $($file.Name)"
        continue
    }

    if ([string]::IsNullOrEmpty($content)) {
        Write-Host "跳过空文件: $($file.Name)" -ForegroundColor DarkGray
        continue
    }

    $originalContent = $content
    
    # 正则表达式：同时匹配 "..." 和 <...>
    $regex = '(#include\s+)(["<])(.+?)([">])'
    
    $newContent = [Regex]::Replace($content, $regex, {
        param($match)
        $prefix = $match.Groups[1].Value  # "#include "
        $startChar = $match.Groups[2].Value # " 或 <
        $oldPath = $match.Groups[3].Value   # 旧路径
        $endChar = $match.Groups[4].Value   # " 或 >
        
        # 简单的配对检查
        if (($startChar -eq '"' -and $endChar -ne '"') -or 
            ($startChar -eq '<' -and $endChar -ne '>')) {
            return $match.Value
        }

        # 提取文件名
        $targetFileName = [System.IO.Path]::GetFileName($oldPath)
        
        # 查表
        if ($fileMap.ContainsKey($targetFileName)) {
            if ($duplicateFiles -contains $targetFileName) {
                return $match.Value
            }
            
            $newPath = $fileMap[$targetFileName]
            
            if ($oldPath -eq $newPath) {
                return $match.Value
            }
            
            # 构造新字符串，保留原来的引号或尖括号
            return "$prefix$startChar$newPath$endChar"
        } else {
            return $match.Value
        }
    })
    
    if ($newContent -ne $originalContent) {
        # [修复] 使用 .NET 写入以保持 UTF-8 BOM
        [System.IO.File]::WriteAllText($file.FullName, $newContent, $utf8Encoding)
        Write-Host "已修正: $($file.Name)" -ForegroundColor Gray
        $processedCount++
    }
}

Write-Host "`n--------------------------------------------------"
Write-Host "处理完成！共修改了 $processedCount 个文件。" -ForegroundColor Yellow
Write-Host "--------------------------------------------------"
Read-Host "按回车键退出..."
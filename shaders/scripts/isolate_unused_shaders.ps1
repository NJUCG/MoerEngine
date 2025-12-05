# isolate_unused_shaders.ps1
# 查找未引用的 Shader 文件并将其移动到 shaders/deprecated 目录
# [位置] 请将此脚本放在 shaders/scripts/ 目录下运行

Write-Host "正在初始化僵尸代码隔离..." -ForegroundColor Cyan

# 1. 路径定位逻辑 (基于脚本位置)
# -----------------------------------------------------------
# 获取脚本所在目录 (shaders/scripts)
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition

# 推断项目根目录 (往上两级: shaders/scripts -> shaders -> 项目根)
$projectRoot = (Get-Item $scriptDir).Parent.Parent.FullName

# 推断 Shaders 根目录
$shaderRoot = Join-Path $projectRoot "shaders"
$deprecatedRoot = Join-Path $shaderRoot "deprecated"

Write-Host "脚本位置: $scriptDir" -ForegroundColor Gray
Write-Host "项目根目录: $projectRoot" -ForegroundColor Gray
Write-Host "Shaders目录: $shaderRoot" -ForegroundColor Gray

if (-not (Test-Path $shaderRoot)) {
    Write-Error "错误：无法定位 shaders 目录！请确认脚本是否放在 shaders/scripts/ 下。"
    exit
}

# 创建 deprecated 目录
if (-not (Test-Path $deprecatedRoot)) {
    New-Item -ItemType Directory -Path $deprecatedRoot -Force | Out-Null
}

# 2. 获取所有 Shader 文件名单 (排除已经 deprecated 的文件)
# -----------------------------------------------------------
Write-Host "正在收集 Shader 文件名单..." -ForegroundColor Gray

# [关键] 使用 $shaderRoot 而不是默认路径
$allShaders = Get-ChildItem -Path $shaderRoot -Include *.hlsl, *.hlsli -Recurse -File | 
    Where-Object { $_.FullName -notmatch [regex]::Escape($deprecatedRoot) }

if ($allShaders.Count -eq 0) {
    Write-Warning "没有找到任何 Shader 文件（或者都在 deprecated 里了）。"
    exit
}

Write-Host "共找到 $($allShaders.Count) 个活跃 Shader 文件。" -ForegroundColor Green

# 3. 准备搜索范围 (构建全项目文本索引)
# -----------------------------------------------------------
Write-Host "正在构建全项目搜索索引..." -ForegroundColor Gray

# [关键] 使用 $projectRoot 作为搜索起点
# 排除 build, .git, .vs, out, target 以及 deprecated 目录
# 同时也排除 shaders/scripts 目录本身，避免脚本自己被误扫
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
# 使用 UTF-8 读取以防止编码问题导致匹配失败
$utf8Encoding = [System.Text.Encoding]::UTF8

foreach ($file in $searchFiles) {
    try {
        $text = [System.IO.File]::ReadAllText($file.FullName, $utf8Encoding)
        [void]$sb.Append($text)
        [void]$sb.Append(" ") 
    } catch {}
}
$allContent = $sb.ToString()

# 4. 分析引用并移动
# -----------------------------------------------------------
Write-Host "正在分析并移动未引用文件..." -ForegroundColor Cyan

$movedCount = 0

foreach ($shader in $allShaders) {
    $fileName = $shader.Name
    
    # 检查文件名是否在项目内容中出现过
    if ($allContent.IndexOf($fileName, [System.StringComparison]::OrdinalIgnoreCase) -eq -1) {
        
        # --- 确定移动路径 ---
        # 计算相对于 shaders 根目录的路径 (例如: core\math\Old.hlsli)
        $relativePath = $shader.FullName.Substring($shaderRoot.Length + 1)
        
        # 构建目标全路径 (例如: shaders\deprecated\core\math\Old.hlsli)
        $destPath = Join-Path $deprecatedRoot $relativePath
        $destDir = Split-Path $destPath -Parent
        
        # 确保目标子目录存在
        if (-not (Test-Path $destDir)) {
            New-Item -ItemType Directory -Path $destDir -Force | Out-Null
        }
        
        # 移动文件
        Move-Item -Path $shader.FullName -Destination $destPath -Force
        
        Write-Host "隔离: $relativePath" -ForegroundColor Yellow
        $movedCount++
    }
}

# 5. 总结
# -----------------------------------------------------------
Write-Host "`n========================================"
if ($movedCount -eq 0) {
    Write-Host "完美！没有发现僵尸代码。" -ForegroundColor Green
} else {
    Write-Host "清理完成！共隔离了 $movedCount 个文件。" -ForegroundColor Yellow
    Write-Host "它们已被移动到: shaders/deprecated/"
    Write-Host "原有目录结构已保留，如果发现误删，可以直接移回去。"
}
Write-Host "========================================"
Read-Host "按回车键退出..."
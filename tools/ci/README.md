# 本地可选功能矩阵测试

这个目录提供本机一键验证，不需要 GitHub Actions 或常驻构建机。

## 一次性配置

完整四组合需要在本机安装 CUDA、LibTorch、TensorRT 和 NRD。脚本按以下顺序查找路径：

1. 命令行参数；
2. `MOER_FEATURE_*` 环境变量；
3. 现有的 `LIBTORCH_DIR`、`TENSORRT_DIR`、`NRD_ROOT` 环境变量。

可以为当前 Windows 用户设置一次：

```powershell
[Environment]::SetEnvironmentVariable(
  "MOER_FEATURE_LIBTORCH_DIR", "C:\SDK\libtorch", "User")
[Environment]::SetEnvironmentVariable(
  "MOER_FEATURE_TENSORRT_DIR", "C:\SDK\TensorRT-10.12.0.36", "User")
[Environment]::SetEnvironmentVariable(
  "MOER_FEATURE_NRD_ROOT", "C:\SDK\NRD", "User")
```

设置后重新打开 PowerShell。

## 一键运行

顺序测试 CUDA/NRD 的四种组合，并对每个组合执行
Raster → Raytracing → Raster 生命周期验证：

```powershell
.\tools\ci\Test-LocalFeatureMatrix.ps1
```

如果不想设置环境变量，也可以直接传路径：

```powershell
.\tools\ci\Test-LocalFeatureMatrix.ps1 `
  -LibTorchDir "C:\SDK\libtorch" `
  -TensorRtDir "C:\SDK\TensorRT-10.12.0.36" `
  -NrdRoot "C:\SDK\NRD"
```

常用选项：

```powershell
# 普通改动只跑 OFF/OFF
.\tools\ci\Test-LocalFeatureMatrix.ps1 -Scope Core

# 四组合只编译，不启动编辑器
.\tools\ci\Test-LocalFeatureMatrix.ps1 -SkipRuntime

# 查看将测试哪些组合，不做任何环境检查或构建
.\tools\ci\Test-LocalFeatureMatrix.ps1 -DryRun
```

四个构建目录和产物都独立保存在 `build/local-feature-matrix/`，后续运行可复用增量构建，
同时不会误用其他组合生成的可执行文件。
本次结果保存在 `target/validation/local-feature-matrix/<时间>/`，总表为其中的
`summary.md`。某个组合失败后脚本会继续测试其余组合，最后统一返回失败。

单独排查一个组合时仍可使用：

```powershell
.\tools\ci\Invoke-FeatureValidation.ps1 `
  -WithCuda OFF `
  -WithNrd ON
```

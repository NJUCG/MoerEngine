#
# 这个文件是命令行工具just的配置文件
# 此模板适用于windows、msvc编译器环境
#
# 使用方式：
# - just setup                => 初始化MoerEngine Config、下载默认场景
# - just gbr                  => 以Debug模式 生成、构建、运行MoerEditor
# - just gbr Release TestEnTT => 以Release模式 生成、构建、运行测试用例TestEnTT
# - just br Debug             => 以Debug模式 构建、运行MoerEditor
# - just r Release            => 以Release模式 运行MoerEditor
# - just clean                => 清除build和target目录
#

set windows-shell := ["powershell", "-c"]
set shell := ["bash", "-c"]

# MARK: Directories

dir_build := "./build/"
dir_target := "./target/"
dir_bin := dir_target + "bin/"
exe_suffix := ".exe"

default_threads := "30"

# MARK: debug printf

# export VK_LAYER_PRINTF_ENABLE := "1" # 如果想关闭shader的debug printf，可以将此项设置为0
# export VK_LAYER_PRINTF_TO_STDOUT := "1"

# MARK: default

default: generate build run

# MARK: internal functions

_rm path:
    if (Test-Path "{{path}}") { Remove-Item -Force "{{path}}" }

_rm_exe config="Debug" exe="MoerEditor":
    if (Test-Path "{{dir_bin}}{{config}}/{{exe}}{{exe_suffix}}") { Remove-Item -Force "{{dir_bin}}{{config}}/{{exe}}{{exe_suffix}}" }

_generate:
    cmake -B build
    # cmake -B build -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++

_build config="Debug" threads="30" target="MoerEditor":
    cmake --build build -j{{threads}} --config {{config}} --target {{target}}

_run config="Debug" exe="MoerEditor":
    {{dir_bin}}{{config}}/{{exe}}{{exe_suffix}}

# MARK: setup

dir_sponza := "./asset/scenes/sponza"
dir_config := "./source/configs/MoerEngine.toml"
setup:
    git config --local blame.ignoreRevsFile .git-blame-ignore-revs
    if (!(Test-Path {{dir_sponza}})) { git clone --branch sponza-scene-files --depth 1 git@github.com:NJUCG/MoerEngine.git {{dir_sponza}} }
    if (!(Test-Path {{dir_config}})) { cp source/configs/template.MoerEngine.toml {{dir_config}} }

# MARK: gbr

clean-exe config="Debug": (_rm_exe config)

clean: (_rm dir_build) (_rm dir_target)

generate: (_generate)

build config="Debug" exe="MoerEditor" threads=default_threads: (_build config threads exe)

run config="Debug" exe="MoerEditor" threads=default_threads: (_run config exe)

build-run config="Debug" exe="MoerEditor" threads=default_threads: (build config exe threads) (run config exe threads)

generate-build-run config="Debug" exe="MoerEditor" threads=default_threads: (generate) (build config exe threads) (run config exe threads)

# MARK: aliases

alias gbr := generate-build-run
alias br := build-run
alias g := generate
alias b := build
alias r := run
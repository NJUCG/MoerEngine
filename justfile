# Only support Powershell currently

set windows-shell := ["powershell", "-c"]
set shell := ["bash", "-c"]

alias gen := generate
alias g := generate
alias b := build
alias r := run
alias cr := compile-run
alias rr := run-release
alias crr := compile-run-release

# MARK: Assignments
editor_debug_dir := "./target/bin/Debug/MoerEditor.exe"
editor_release_dir := "./target/bin/Release/MoerEditor.exe"
build_dir := "./build"
target_dir := "./target"

threads := "30"

# MARK: default

default: generate build run

# MARK: clean

clean-exe:
    if (Test-Path {{editor_debug_dir}}) { Remove-Item -Force {{editor_debug_dir}} }

clean-exe-release:
    if (Test-Path {{editor_release_dir}}) { Remove-Item -Force {{editor_release_dir}} }

clean:
    if (Test-Path {{build_dir}}) { Remove-Item -Recurse -Force {{build_dir}} }
    if (Test-Path {{target_dir}}) { Remove-Item -Recurse -Force {{target_dir}} }

# MARK: build

generate:
    cmake -B build

build:
    cmake --build build -j{{threads}}

build-release:
    cmake --build build -j{{threads}} --config Release

# MARK: debug

run:
    {{editor_debug_dir}}

compile-run: clean-exe build run

# MARK: release

run-release:
    {{editor_release_dir}}


compile-run-release: clean-exe-release build-release run-release

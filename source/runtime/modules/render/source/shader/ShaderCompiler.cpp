#include "shader/ShaderCompiler.h"
#include "spirv_cross.hpp"
#include <filesystem>
#include <iostream>
#define NOMINMAX 1
#include "Windows.h"
#include "dxc/dxcapi.h"
#include "log/LogSystem.h"
#include "shader/ShaderCommon.h"
#include <vector>
#include <d3d12.h>
#include <atlcomcli.h>
#include <winnt.h>
#include <sstream>

void ShaderCompiler::ShaderConductorTest() {
    const char* file_name_c = "TestVert.vert";
    HRESULT     hres;

    std::filesystem::path file_path = g_global_shader_resource_root_dir;
    file_path /= file_name_c;

    std::wstring file_name = file_path.generic_wstring();

    // Initialize DXC library
    CComPtr<IDxcLibrary>
        library;
    hres = DxcCreateInstance(CLSID_DxcLibrary, IID_PPV_ARGS(&library));
    if (FAILED(hres)) {
        throw std::runtime_error("Could not init DXC Library");
    }

    // Initialize DXC compiler
    CComPtr<IDxcCompiler3> compiler;
    hres = DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler));
    if (FAILED(hres)) {
        throw std::runtime_error("Could not init DXC Compiler");
    }

    // Initialize DXC utility
    CComPtr<IDxcUtils> utils;
    hres = DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils));
    if (FAILED(hres)) {
        throw std::runtime_error("Could not init DXC Utiliy");
    }

    // Load the HLSL text shader from disk
    uint32_t                  codePage = DXC_CP_ACP;
    CComPtr<IDxcBlobEncoding> sourceBlob;
    hres = utils->LoadFile(file_name.c_str(), &codePage, &sourceBlob);
    if (FAILED(hres)) {
        throw std::runtime_error("Could not load shader file");
    }

    // Select target profile based on shader file extension
    LPCWSTR targetProfile{};
    size_t  idx = file_name.rfind('.');
    if (idx != std::string::npos) {
        std::wstring extension = file_name.substr(idx + 1);
        if (extension == L"vert") {
            targetProfile = L"vs_6_1";
        }
        if (extension == L"frag") {
            targetProfile = L"ps_6_1";
        }
        // Mapping for other file types go here (cs_x_y, lib_x_y, etc.)
    }

    // Configure the compiler arguments for compiling the HLSL shader to SPIR-V
    std::vector<LPCWSTR> arguments = {
        // (Optional) name of the shader file to be displayed e.g. in an error message
        file_name.c_str(),
        // Shader main entry point
        L"-E",
        L"main",
        // Shader target profile
        L"-T",
        targetProfile,
        // Compile to SPIRV
        L"-spirv",
        L"-fspv-reflect",
        DXC_ARG_ALL_RESOURCES_BOUND,
        DXC_ARG_DEBUG};

    // Compile shader
    DxcBuffer buffer{};
    buffer.Encoding = DXC_CP_ACP;
    buffer.Ptr      = sourceBlob->GetBufferPointer();
    buffer.Size     = sourceBlob->GetBufferSize();

    CComPtr<IDxcResult> result{nullptr};
    hres = compiler->Compile(
        &buffer,
        arguments.data(),
        (uint32_t)arguments.size(),
        nullptr,
        IID_PPV_ARGS(&result));

    if (SUCCEEDED(hres)) {
        result->GetStatus(&hres);
    }

    // Output error if compilation failed
    if (FAILED(hres) && (result)) {
        CComPtr<IDxcBlobEncoding> errorBlob;
        hres = result->GetErrorBuffer(&errorBlob);
        std::stringstream error_stream;
        if (SUCCEEDED(hres) && errorBlob) {

            error_stream << "Shader compilation failed :\n\n"
                         << (const char*)errorBlob->GetBufferPointer();
            LOG_INFO(error_stream.str());
        }
    }

    // Get compilation result
    IDxcBlob* code;
    result->GetResult(&code);
    const uint32_t*              data = (uint32_t*)code->GetBufferPointer();
    uint32_t                     size = code->GetBufferSize();
    spirv_cross::Compiler        comp(data, size);
    spirv_cross::ShaderResources res = comp.get_shader_resources();
    for (const spirv_cross::Resource& resource : res.stage_inputs) {
        unsigned location = comp.get_decoration(resource.id, spv::DecorationLocation);
        LOG_INFO(location);
    }
    LOG_INFO("?????????????");
}
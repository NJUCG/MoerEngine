#include "D3D12Backend.h"
#include <algorithm>

namespace Moer::Render::D3D12 {
namespace {
void Append(ConstantLayout& dst, const ConstantLayout& src, uint32 packed, uint32 native) {
    for (auto copy : src.copies) {
        copy.source += packed;
        copy.destination += native;
        if (!dst.copies.empty() && dst.copies.back().source + dst.copies.back().size == copy.source &&
            dst.copies.back().destination + dst.copies.back().size == copy.destination)
            dst.copies.back().size += copy.size;
        else
            dst.copies.push_back(copy);
    }
    dst.packed_size = std::max(dst.packed_size, packed + src.packed_size);
    dst.native_size = std::max(dst.native_size, native + src.native_size);
}
ConstantLayout Describe(ID3D12ShaderReflectionType* type, bool element = false) {
    D3D12_SHADER_TYPE_DESC desc{};
    DX_CHECK_HRESULT(type->GetDesc(&desc));
    ConstantLayout result;
    if (desc.Elements && !element) {
        const auto   single        = Describe(type, true);
        const uint32 native_stride = uint32(Align(single.native_size, 16));
        for (uint32 i = 0; i < desc.Elements; ++i)
            Append(result, single, single.packed_size * i, native_stride * i);
    } else if (desc.Class == D3D_SVC_STRUCT) {
        uint32 offset = 0;
        for (uint32 member = 0; member < desc.Members; ++member) {
            auto*                  child = type->GetMemberTypeByIndex(member);
            D3D12_SHADER_TYPE_DESC child_desc{};
            DX_CHECK_HRESULT(child->GetDesc(&child_desc));
            const auto layout = Describe(child);
            Append(result, layout, offset, child_desc.Offset);
            offset += layout.packed_size;
        }
    } else if (desc.Class == D3D_SVC_MATRIX_ROWS || desc.Class == D3D_SVC_MATRIX_COLUMNS) {
        const uint32 vectors = desc.Class == D3D_SVC_MATRIX_ROWS ? desc.Rows : desc.Columns;
        const uint32 width   = desc.Class == D3D_SVC_MATRIX_ROWS ? desc.Columns : desc.Rows;
        for (uint32 i = 0; i < vectors; ++i) {
            ConstantLayout vector{width * 4, width * 4, {{0, 0, width * 4}}};
            Append(result, vector, i * width * 4, i * 16);
        }
    } else if (desc.Class == D3D_SVC_SCALAR || desc.Class == D3D_SVC_VECTOR) {
        // Engine shared headers use 32-bit scalar/vector components and 4-byte
        // C++ alignment. Keep that contract explicit rather than guessing bool
        // or native 16-bit C++ representations.
        if (desc.Type != D3D_SVT_FLOAT && desc.Type != D3D_SVT_UINT && desc.Type != D3D_SVT_INT)
            throw std::invalid_argument("D3D12 shared constants require 32-bit float/int/uint components");
        result.packed_size = result.native_size = desc.Rows * desc.Columns * 4;
        result.copies.push_back({0, 0, result.packed_size});
    } else
        throw std::invalid_argument("Unsupported D3D12 constant member type");
    return result;
}
} // namespace
ConstantLayout ReflectConstantLayout(ID3D12ShaderReflectionType* type) {
    return Describe(type);
}
ConstantLayout ReflectBufferLayout(ID3D12ShaderReflectionConstantBuffer* buffer) {
    D3D12_SHADER_BUFFER_DESC desc{};
    DX_CHECK_HRESULT(buffer->GetDesc(&desc));
    ConstantLayout result;
    uint32         packed = 0;
    for (uint32 i = 0; i < desc.Variables; ++i) {
        auto*                      variable = buffer->GetVariableByIndex(i);
        D3D12_SHADER_VARIABLE_DESC info{};
        DX_CHECK_HRESULT(variable->GetDesc(&info));
        const auto layout = Describe(variable->GetType());
        Append(result, layout, packed, info.StartOffset);
        packed += layout.packed_size;
    }
    result.native_size = desc.Size;
    return result;
}
ID3D12ShaderReflectionType* FindConstantType(ID3D12ShaderReflection* shader, std::string_view name) {
    auto*                    wrapper = shader->GetConstantBufferByName(ReflectParamInfo::constants_name.data());
    D3D12_SHADER_BUFFER_DESC desc{};
    if (SUCCEEDED(wrapper->GetDesc(&desc)) && desc.Variables == 1) {
        auto*                  type = wrapper->GetVariableByIndex(0)->GetType();
        D3D12_SHADER_TYPE_DESC info{};
        DX_CHECK_HRESULT(type->GetDesc(&info));
        for (uint32 i = 0; i < info.Members; ++i)
            if (name == type->GetMemberTypeName(i))
                return type->GetMemberTypeByIndex(i);
    }
    auto* buffer = shader->GetConstantBufferByName(std::string(name).c_str());
    DX_CHECK_HRESULT(buffer->GetDesc(&desc));
    if (desc.Variables != 1)
        throw std::invalid_argument("Logical D3D12 constant must have one reflected root variable");
    return buffer->GetVariableByIndex(0)->GetType();
}
} // namespace Moer::Render::D3D12

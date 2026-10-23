#include "rhi/RHICommon.h"
#include "serialize/Serializer.h"

namespace Moer::Render {
namespace {

// Cache tags have fixed values independent of std::variant alternative order.
enum class ReflectionBackendTag : uint8_t {
    Spirv = 0,
    Dxil  = 1
};
enum class ReflectionParameterTag : uint8_t {
    Resource      = 0,
    Constant      = 1,
    BindlessArray = 2
};

ReflectParamInfo::CustomFlags ReadActiveFlag(InputStream& stream) {
    uint active = 0;
    stream >> active;
    if (active > 1) {
        throw std::invalid_argument("Invalid shader reflection active flag");
    }
    return {.active = active};
}

void WriteBindlessArray(OutputStream& stream, const ReflectParamInfo::BindlessArray& bindings) {
    for (const auto* binding :
         {&bindings.array,
          &bindings.buffer,
          &bindings.image,
          &bindings.sampler,
          &bindings.acceleration_structure}) {
        stream << uint8_t(binding->has_value());
        if (!binding->has_value()) {
            continue;
        }
        const auto& value = binding->value();
        stream << value.set << value.binding << value.count << value.desc_type << value.resource_type
               << value.stage_bits << uint(value.custom_flag.active);
    }
    stream << uint8_t(bindings.index_table_address.has_value());
    if (bindings.index_table_address) {
        const auto& address = *bindings.index_table_address;
        stream << address.offset << address.size << address.padded_size << uint(address.custom_flag.active);
    }
}

ReflectParamInfo::BindlessArray ReadBindlessArray(InputStream& stream) {
    ReflectParamInfo::BindlessArray bindings;
    for (auto* binding :
         {&bindings.array,
          &bindings.buffer,
          &bindings.image,
          &bindings.sampler,
          &bindings.acceleration_structure}) {
        uint8_t present = 0;
        stream >> present;
        if (present > 1) {
            throw std::invalid_argument("Invalid shader reflection bindless presence flag");
        }
        if (present == 0) {
            continue;
        }
        ReflectParamInfo::Bindless value{};
        stream >> value.set >> value.binding >> value.count >> value.desc_type >> value.resource_type >>
            value.stage_bits;
        value.custom_flag = ReadActiveFlag(stream);
        *binding          = value;
    }
    uint8_t address_present = 0;
    stream >> address_present;
    if (address_present > 1) {
        throw std::invalid_argument("Invalid bindless address presence flag");
    }
    if (address_present) {
        ReflectParamInfo::Constant address{};
        stream >> address.offset >> address.size >> address.padded_size;
        address.custom_flag = ReadActiveFlag(stream);
        bindings.index_table_address = address;
    }
    return bindings;
}

} // namespace

OutputStream& ReflectParamInfo::operator<<(OutputStream& stream) const {
    const auto* spirv_reflection = std::get_if<SpirvParameterReflection>(&backend_data);
    if (spirv_reflection == nullptr) {
        const auto& dxil_reflection = GetDxilReflection();
        stream << ReflectionBackendTag::Dxil << dxil_reflection.slot << dxil_reflection.space
               << dxil_reflection.count << uint16_t(dxil_reflection.type)
               << uint16_t(dxil_reflection.byte_size);
        return stream;
    }

    stream << ReflectionBackendTag::Spirv;
    if (const auto* resource = spirv_reflection->TryGetResource()) {
        stream << ReflectionParameterTag::Resource << spirv_reflection->stage_bits << resource->set
               << resource->binding << resource->sampled << resource->desc_type << resource->resource_type
               << resource->count << uint(resource->custom_flag.active) << uint(resource->format);
    } else if (const auto* constant = spirv_reflection->TryGetConstant()) {
        stream << ReflectionParameterTag::Constant << spirv_reflection->stage_bits << constant->offset
               << constant->size << constant->padded_size << uint(constant->custom_flag.active);
    } else {
        stream << ReflectionParameterTag::BindlessArray << spirv_reflection->stage_bits;
        WriteBindlessArray(stream, std::get<BindlessArray>(spirv_reflection->data));
    }
    return stream;
}

InputStream& ReflectParamInfo::operator>>(InputStream& stream) {
    auto backend = static_cast<ReflectionBackendTag>(0xff);
    stream >> backend;
    switch (backend) {
        case ReflectionBackendTag::Dxil: {
            DxilParameterReflection dxil_reflection{};
            uint16_t type      = 0;
            uint16_t byte_size = 0;
            stream >> dxil_reflection.slot >> dxil_reflection.space >> dxil_reflection.count >> type >>
                byte_size;
            dxil_reflection.type      = type;
            dxil_reflection.byte_size = byte_size;
            backend_data              = dxil_reflection;
            return stream;
        }
        case ReflectionBackendTag::Spirv:
            break;
        default:
            throw std::invalid_argument("Invalid shader reflection backend tag");
    }

    auto  kind = static_cast<ReflectionParameterTag>(0xff);
    SpirvParameterReflection spirv_reflection;
    stream >> kind >> spirv_reflection.stage_bits;
    switch (kind) {
        case ReflectionParameterTag::Resource: {
            Resource resource{};
            uint     format = 0;
            stream >> resource.set >> resource.binding >> resource.sampled >> resource.desc_type >>
                resource.resource_type >> resource.count;
            resource.custom_flag = ReadActiveFlag(stream);
            stream >> format;
            resource.format = static_cast<EPixelFormat>(format);
            spirv_reflection.data = resource;
            break;
        }
        case ReflectionParameterTag::Constant: {
            Constant constant{};
            stream >> constant.offset >> constant.size >> constant.padded_size;
            constant.custom_flag = ReadActiveFlag(stream);
            spirv_reflection.data = constant;
            break;
        }
        case ReflectionParameterTag::BindlessArray:
            spirv_reflection.data = ReadBindlessArray(stream);
            break;
        default:
            throw std::invalid_argument("Invalid shader reflection parameter tag");
    }
    backend_data = std::move(spirv_reflection);
    return stream;
}

} // namespace Moer::Render

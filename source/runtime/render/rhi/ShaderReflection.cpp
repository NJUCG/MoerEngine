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
    return bindings;
}

} // namespace

OutputStream& ReflectParamInfo::operator<<(OutputStream& stream) const {
    const auto* spirv = std::get_if<Spirv>(&backend_data);
    if (spirv == nullptr) {
        const auto& dxil = GetDxil();
        stream << ReflectionBackendTag::Dxil << dxil.slot << dxil.space << dxil.count << uint16_t(dxil.type)
               << uint16_t(dxil.byte_size);
        return stream;
    }

    stream << ReflectionBackendTag::Spirv;
    if (const auto* resource = spirv->TryGetResource()) {
        stream << ReflectionParameterTag::Resource << spirv->stage_bits << resource->set << resource->binding
               << resource->sampled << resource->desc_type << resource->resource_type << resource->count
               << uint(resource->custom_flag.active) << uint(resource->format);
    } else if (const auto* constant = spirv->TryGetConstant()) {
        stream << ReflectionParameterTag::Constant << spirv->stage_bits << constant->offset << constant->size
               << constant->padded_size << uint(constant->custom_flag.active);
    } else {
        stream << ReflectionParameterTag::BindlessArray << spirv->stage_bits;
        WriteBindlessArray(stream, std::get<BindlessArray>(spirv->data));
    }
    return stream;
}

InputStream& ReflectParamInfo::operator>>(InputStream& stream) {
    auto backend = static_cast<ReflectionBackendTag>(0xff);
    stream >> backend;
    switch (backend) {
        case ReflectionBackendTag::Dxil: {
            Dxil     dxil{};
            uint16_t type      = 0;
            uint16_t byte_size = 0;
            stream >> dxil.slot >> dxil.space >> dxil.count >> type >> byte_size;
            dxil.type      = type;
            dxil.byte_size = byte_size;
            backend_data   = dxil;
            return stream;
        }
        case ReflectionBackendTag::Spirv:
            break;
        default:
            throw std::invalid_argument("Invalid shader reflection backend tag");
    }

    auto  kind = static_cast<ReflectionParameterTag>(0xff);
    Spirv spirv;
    stream >> kind >> spirv.stage_bits;
    switch (kind) {
        case ReflectionParameterTag::Resource: {
            Resource resource{};
            uint     format = 0;
            stream >> resource.set >> resource.binding >> resource.sampled >> resource.desc_type >>
                resource.resource_type >> resource.count;
            resource.custom_flag = ReadActiveFlag(stream);
            stream >> format;
            resource.format = static_cast<EPixelFormat>(format);
            spirv.data      = resource;
            break;
        }
        case ReflectionParameterTag::Constant: {
            Constant constant{};
            stream >> constant.offset >> constant.size >> constant.padded_size;
            constant.custom_flag = ReadActiveFlag(stream);
            spirv.data           = constant;
            break;
        }
        case ReflectionParameterTag::BindlessArray:
            spirv.data = ReadBindlessArray(stream);
            break;
        default:
            throw std::invalid_argument("Invalid shader reflection parameter tag");
    }
    backend_data = std::move(spirv);
    return stream;
}

} // namespace Moer::Render

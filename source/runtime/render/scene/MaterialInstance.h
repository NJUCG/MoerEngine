#pragma once

#include "misc/CountableRef.h"
#include "rhi/RHIResource.h"

namespace Moer {
class UniformBuffer;
class Material;
using MaterialRef = CountableRef<Material>;
using MaterialID  = uint32_t;

class RENDER_API MaterialInstance : public CountableResource {
public:
    MaterialInstance(const MaterialInstance& rhs)            = delete;
    MaterialInstance& operator=(const MaterialInstance& rhs) = delete;
    MaterialInstance(MaterialRef material);
    template<typename T>
    using is_supported_parameter_t = typename std::enable_if<
        std::is_same<float, T>::value || std::is_same<int32_t, T>::value ||
        std::is_same<uint32_t, T>::value || std::is_same<Vector2i, T>::value ||
        std::is_same<Vector3i, T>::value || std::is_same<Vector4i, T>::value ||
        std::is_same<Vector2ui, T>::value || std::is_same<Vector3ui, T>::value ||
        std::is_same<Vector4ui, T>::value || std::is_same<Vector2f, T>::value ||
        std::is_same<Vector3f, T>::value || std::is_same<Vector4f, T>::value ||
        std::is_same<Matrix4x4f, T>::value ||
        // these types are slower as they need a layout conversion
        std::is_same<bool, T>::value || std::is_same<Matrix3x3f, T>::value>::type;

    template<typename T, typename = is_supported_parameter_t<T>>
    void SetParameter(const std::string& name, T const& value);
    void SetParameter(const std::string& name, const void* value, size_t size);
    void SetUnifomBuffer(const void* data, size_t size);

    MaterialRef GetMaterial() const;

    const UniformBuffer& GetUniformBuffer() const;
    const std::string&   GetName() const;
    void                 SetName(const std::string& name);

    template<typename T>
    T GetParameter(const std::string& _name) {
        T value;
        GetParameter(_name, &value, sizeof(T));
        return value;
    }

    void GetParameter(const std::string& _name, void* _value, size_t _size);

protected:
    class Impl;
    Impl* m_impl;
};

template<typename T, typename>
void MaterialInstance::SetParameter(const std::string& name, T const& value) {
    SetParameter(name, &value, sizeof(T));
}

using MaterialInstanceRef = CountableRef<MaterialInstance>;

} // namespace Moer
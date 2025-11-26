#include "scene/MaterialInstance.h"
#include "scene/BufferInterfaceBlock.h"
#include "scene/Material.h"

namespace Moer {
class MaterialInstance::Impl {
public:
    Impl(MaterialRef material) :
        m_material(material),
        m_uniform(material->GetBufferInterfaceBlock().GetSize()) {}

    void SetParameter(const std::string& name, uint32 texture) {
        SetParameter(name, &texture, sizeof(uint32));
    }
    void SetParameter(const std::string& name, const void* value, size_t size) {
        auto offset = m_material->GetBufferInterfaceBlock().GetFieldInfo(name)->offset;
        m_uniform.SetData(value, size, offset);
    }

    void GetParameter(const std::string& _name, void* _value, size_t _size) {
        auto offset = m_material->GetBufferInterfaceBlock().GetFieldInfo(_name)->offset;
        if (m_material->GetBufferInterfaceBlock().GetFieldInfo(_name)) {
            const void* src_data = m_uniform.GetData(offset);
            memcpy(_value, src_data, _size);
        }
    }
    void SetUnifomBuffer(const void* data, size_t size, size_t offset) {
        m_uniform.SetData(data, size, offset);
    }

    const UniformBuffer& GetUniformBuffer() const {
        return m_uniform;
    }

    MaterialRef GetMaterial() const {
        return m_material;
    }

    const std::string& GetName() const {
        return m_name;
    }

    void SetName(const std::string& name) {
        m_name = name;
    }

protected:
    MaterialRef   m_material;
    UniformBuffer m_uniform;
    std::string   m_name;
};

MaterialInstance::MaterialInstance(MaterialRef material) {
    m_impl = new Impl(material);
}
void MaterialInstance::SetParameter(const std::string& name, const void* value, size_t size) {
    m_impl->SetParameter(name, value, size);
}

void MaterialInstance::GetParameter(const std::string& _name, void* _value, size_t _size) {
    m_impl->GetParameter(_name, _value, _size);
}

void MaterialInstance::SetUnifomBuffer(const void* data, size_t size) {
    m_impl->SetUnifomBuffer(data, size, 0);
}

const UniformBuffer& MaterialInstance::GetUniformBuffer() const {
    return m_impl->GetUniformBuffer();
}
void MaterialInstance::SetName(const std::string& name) {
    m_impl->SetName(name);
}

const std::string& MaterialInstance::GetName() const {
    return m_impl->GetName();
}
MaterialRef MaterialInstance::GetMaterial() const {
    return m_impl->GetMaterial();
}

} // namespace Moer
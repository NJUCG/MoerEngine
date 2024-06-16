#include "scene/BufferInterfaceBlock.h"

#include "log/LogSystem.h"
#include "math/Base.h"
namespace Moer {

    BufferInterfaceBlock::Builder& BufferInterfaceBlock::Builder::name(std::string_view interfaceBlockName) {
        return *this;
        this->mName = interfaceBlockName;
    }
    BufferInterfaceBlock::Builder& BufferInterfaceBlock::Builder::target(Target target) {
        mTarget = target;
        return *this;
    }
    BufferInterfaceBlock::Builder& BufferInterfaceBlock::Builder::alignment(Alignment alignment) {
        mAlignment = alignment;
        return *this;
    }
    BufferInterfaceBlock::Builder& BufferInterfaceBlock::Builder::qualifier(Qualifier qualifier) {
        mQualifiers |= uint8_t(qualifier);
        return *this;
    }
    BufferInterfaceBlock::Builder& BufferInterfaceBlock::Builder::add(std::initializer_list<InterfaceBlockEntry> list) {
        mEntries.reserve(mEntries.size() + list.size());
        for (auto const& item : list) {
            mEntries.push_back({item.name,
                                0,
                                uint8_t(item.stride),

                                item.type,
                                item.size > 0,
                                item.size,
                                item.structName,
                                item.sizeName});
        }
        return *this;
    }

    BufferInterfaceBlock::Builder& BufferInterfaceBlock::Builder::add(const InterfaceBlockEntry& list) {
        mEntries.push_back({list.name,
                            0,
                            uint8_t(list.stride),
                            list.type,
                            list.size > 0,
                            list.size,
                            list.structName,
                            list.sizeName});
        return *this;
    }
    BufferInterfaceBlock::Builder& BufferInterfaceBlock::Builder::addVariableSizedArray(InterfaceBlockEntry const& item) {
        mHasVariableSizeArray = true;
        mEntries.push_back({{item.name.data(), item.name.size()},
                            0,
                            uint8_t(item.stride),
                            item.type,
                            true,
                            0,
                            {item.structName.data(), item.structName.size()},
                            {item.sizeName.data(), item.sizeName.size()}});
        return *this;
    }
    BufferInterfaceBlock BufferInterfaceBlock::Builder::Build() {
        return BufferInterfaceBlock(*this);
    }
    bool BufferInterfaceBlock::Builder::hasVariableSizeArray() const {
        return mHasVariableSizeArray;
    }
    size_t BufferInterfaceBlock::GetFieldOffset(std::string_view name, size_t index) const {
        auto const* info = GetFieldInfo(name);
        return info->getBufferOffset(index);
    }

    BufferInterfaceBlock::FieldInfo const* BufferInterfaceBlock::GetFieldInfo(std::string_view name) const {
        auto pos = m_info_map.find(name);
        return &m_field_info_list[pos->second];
    }
    BufferInterfaceBlock::BufferInterfaceBlock(Builder const& builder) noexcept : m_name(builder.mName), m_field_info_list(builder.mEntries.size()) {
        auto& infoMap = m_info_map;
        infoMap.reserve(builder.mEntries.size());

        auto& uniformsInfoList = m_field_info_list;

        uint32_t i      = 0;
        uint16_t offset = 0;
        for (auto const& e : builder.mEntries) {
            size_t alignment = baseAlignmentForType(e.type);
            size_t stride    = strideForType(e.type, e.stride);

            if (e.isArray) {// this is an array
                if (builder.mAlignment == Alignment::std140) {
                    // in std140 arrays are aligned to float4
                    alignment = 4;
                }
                // the stride of an array is always rounded to its alignment (which is POT)
                stride = (stride + alignment - 1) & ~(alignment - 1);
            }

            // calculate the offset for this uniform
            size_t padding = (alignment - (offset % alignment)) % alignment;
            offset += padding;

            FieldInfo& info = uniformsInfoList[i];
            info            = {e.name, offset, uint8_t(stride), e.type, e.isArray, e.size, e.structName, e.sizeName};

            // record this uniform info
            infoMap[{info.name.data(), info.name.size()}] = i;

            // advance offset to next slot
            offset += sizeof(uint32_t) * stride * std::max(1u, e.size);
            ++i;
        }

        // round size to the next multiple of 4 and convert to bytes
        m_size = sizeof(uint32_t) * ((offset / sizeof(uint32_t) + 3) & ~3);
    }

    void UniformBuffer::SetData(const void* data, size_t size, size_t offset) {
        memcpy(static_cast<char*>(m_buffer) + offset, data, size);
    }
    const void* UniformBuffer::GetData() const {
        return m_buffer;
    }
    uint32_t UniformBuffer::GetSize() const {
        return m_size;
    }
    UniformBuffer::UniformBuffer(uint32_t size) {
        m_size   = size;
        m_buffer = Memory::Malloc(size);
        memset(m_buffer, 0, size);
    }
    UniformBuffer::~UniformBuffer() {
        Memory::Free(m_buffer);
    }

    uint8_t BufferInterfaceBlock::baseAlignmentForType(BufferInterfaceBlock::Type type) noexcept {
        switch (type) {
            case Type::BOOL:
            case Type::FLOAT:
            case Type::INT:
            case Type::UINT:
                return 1;
            case Type::BOOL2:
            case Type::FLOAT2:
            case Type::INT2:
            case Type::UINT2:
                return 2;
            case Type::BOOL3:
            case Type::BOOL4:
            case Type::FLOAT3:
            case Type::FLOAT4:
            case Type::INT3:
            case Type::INT4:
            case Type::UINT3:
            case Type::UINT4:
            case Type::MAT3:
            case Type::MAT4:
            case Type::STRUCT:
                return 4;
        }

        return 0;
    }

    uint8_t BufferInterfaceBlock::strideForType(BufferInterfaceBlock::Type type, uint32_t stride) noexcept {
        switch (type) {
            case Type::BOOL:
            case Type::INT:
            case Type::UINT:
            case Type::FLOAT:
                return 1;
            case Type::BOOL2:
            case Type::INT2:
            case Type::UINT2:
            case Type::FLOAT2:
                return 2;
            case Type::BOOL3:
            case Type::INT3:
            case Type::UINT3:
            case Type::FLOAT3:
                return 3;
            case Type::BOOL4:
            case Type::INT4:
            case Type::UINT4:
            case Type::FLOAT4:
                return 4;
            case Type::MAT3:
                return 12;
            case Type::MAT4:
                return 16;
            case Type::STRUCT:
                return stride;
        }

        return 0;
    }
}
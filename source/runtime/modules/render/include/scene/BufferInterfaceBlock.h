#pragma once
#include "RenderAPI.h"
#include "math/Base.h"
#include "misc/STL.h"

#include <string_view>

namespace Moer {

    enum class UniformType : uint8_t {
        BOOL,
        BOOL2,
        BOOL3,
        BOOL4,
        FLOAT,
        FLOAT2,
        FLOAT3,
        FLOAT4,
        INT,
        INT2,
        INT3,
        INT4,
        UINT,
        UINT2,
        UINT3,
        UINT4,
        MAT3,//!< a 3x3 float matrix
        MAT4,//!< a 4x4 float matrix
        STRUCT
    };

    class BufferInterfaceBlock {
    public:
        struct InterfaceBlockEntry {
            std::string name;
            uint32_t    size;
            UniformType type;
            std::string structName{};
            uint32_t    stride{};
            std::string sizeName{};
        };

        BufferInterfaceBlock() = default;

        BufferInterfaceBlock(const BufferInterfaceBlock& rhs)            = delete;
        BufferInterfaceBlock& operator=(const BufferInterfaceBlock& rhs) = delete;

        BufferInterfaceBlock(BufferInterfaceBlock&& rhs) noexcept            = default;
        BufferInterfaceBlock& operator=(BufferInterfaceBlock&& rhs) noexcept = default;

        ~BufferInterfaceBlock() noexcept = default;

        using Type = UniformType;
        struct FieldInfo {
            std::string name;  // name of this field
            uint16_t    offset;// offset in "uint32_t" of this field in the buffer
            uint8_t     stride;// stride in "uint32_t" to the next element
            Type        type;
            bool        isArray;   // true if the field is an array
            uint32_t    size;      // size of the array in elements, or 0 if not an array
            std::string structName;// name of this field structure if type is STRUCT
            std::string sizeName;  // name of the size parameter in the shader
            // returns offset in bytes of this field (at index if an array)
            inline size_t getBufferOffset(size_t index = 0) const {
                return (offset + stride * index) * sizeof(uint32_t);
            }
        };

        enum class Alignment : uint8_t {
            std140,
            std430
        };

        enum class Target : uint8_t {
            UNIFORM,
            SSBO
        };

        enum class Qualifier : uint8_t {
            COHERENT  = 0x01,
            WRITEONLY = 0x02,
            READONLY  = 0x04,
            VOLATILE  = 0x08,
            RESTRICT  = 0x10
        };

        class Builder {
        public:
            Builder() noexcept  = default;
            ~Builder() noexcept = default;

            Builder(Builder const& rhs)                = default;
            Builder(Builder&& rhs) noexcept            = default;
            Builder& operator=(Builder const& rhs)     = default;
            Builder& operator=(Builder&& rhs) noexcept = default;

            // Give a name to this buffer interface block
            Builder& name(std::string_view interfaceBlockName);

            // Buffer target
            Builder& target(Target target);

            // build and return the BufferInterfaceBlock
            Builder& alignment(Alignment alignment);

            // add a qualifier
            Builder& qualifier(Qualifier qualifier);

            // a list of this buffer's fields
            Builder& add(std::initializer_list<InterfaceBlockEntry> list);
            Builder& add(const InterfaceBlockEntry& list);

            // add a variable-sized array. must be the last entry.
            Builder& addVariableSizedArray(InterfaceBlockEntry const& item);

            BufferInterfaceBlock Build();

            bool hasVariableSizeArray() const;

        private:
            friend class BufferInterfaceBlock;
            std::string            mName;
            Moer::Array<FieldInfo> mEntries;
            Alignment              mAlignment            = Alignment::std140;
            Target                 mTarget               = Target::UNIFORM;
            uint8_t                mQualifiers           = 0;
            bool                   mHasVariableSizeArray = false;
        };

        // name of this BufferInterfaceBlock interface block
        std::string_view getName() const noexcept { return {m_name.data(), m_name.size()}; }

        // size needed for the buffer in bytes
        size_t GetSize() const noexcept { return m_size; }

        // list of information records for each field
        Moer::Array<FieldInfo> const& GetFieldInfoList() const noexcept {
            return m_field_info_list;
        }

        // negative value if name doesn't exist or Panic if exceptions are enabled
        size_t GetFieldOffset(std::string_view name, size_t index) const;

        FieldInfo const* GetFieldInfo(std::string_view name) const;

        bool HasField(std::string_view name) const noexcept {
            return m_info_map.find(name) != m_info_map.end();
        }

        bool IsEmpty() const noexcept { return m_field_info_list.empty(); }

        Alignment GetAlignment() const noexcept { return m_alignment; }

        Target GetTarget() const noexcept { return m_target; }

        uint8_t getQualifier() const noexcept { return m_qualifiers; }

    private:
        friend class Builder;
        friend class SceneCache;

        explicit BufferInterfaceBlock(Builder const& builder) noexcept;

        static uint8_t baseAlignmentForType(Type type) noexcept;
        static uint8_t strideForType(Type type, uint32_t stride) noexcept;

        std::string m_name;
        uint32_t    m_size       = 0;// size in bytes rounded to multiple of 4
        Alignment   m_alignment  = Alignment::std140;
        Target      m_target     = Target::UNIFORM;
        uint8_t     m_qualifiers = 0;

        Moer::Array<FieldInfo>                         m_field_info_list;
        Moer::UnorderedMap<std::string_view, uint32_t> m_info_map;
    };

    class RENDER_API UniformBuffer {
    public:
        void        SetData(const void* data, size_t size, size_t offset);
        const void* GetData() const;
        uint32_t    GetSize() const;
        UniformBuffer(uint32_t size);
        ~UniformBuffer();
        UniformBuffer(const UniformBuffer& rhs)            = delete;
        UniformBuffer& operator=(const UniformBuffer& rhs) = delete;
        UniformBuffer(UniformBuffer&& rhs)                 = delete;
        UniformBuffer& operator=(UniformBuffer&& rhs)      = delete;

    protected:
        void*    m_buffer;
        uint32_t m_size;
    };
}
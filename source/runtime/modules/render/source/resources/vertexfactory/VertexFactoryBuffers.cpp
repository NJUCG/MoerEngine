#include "resources/vertexfactory/VertexFactoryBuffers.h"

#include <tuple>
#include <limits>
#include <sstream>

#include "log/LogSystem.h"
#include "math/Math.h"

namespace Moer {

    // public:

    VertexFactoryBuffers::VertexFactoryBuffers() {}// In this case, VFB will be initialized by InputStream

    VertexFactoryBuffers::VertexFactoryBuffers(
        const Moer::Array<EVertexAttributes>& attributes)
        : m_num_of_attributes(attributes.size()),

          m_buffers(m_num_of_attributes),
          m_attributes(m_num_of_attributes),

          m_pixel_formats(m_num_of_attributes),
          m_attribute_sizes(m_num_of_attributes) {

        for (size_t i = 0; i < VA_NUM; i++) {
            m_attribute_to_index_map[i] = std::numeric_limits<size_t>::max();
        }

        for (size_t i = 0; i < m_num_of_attributes; i++) {
            m_attributes[i]                                                = attributes[i];
            m_attribute_to_index_map[static_cast<size_t>(m_attributes[i])] = i;

            m_pixel_formats[i]   = VertexAttributesTool::GetPixelFormat(m_attributes[i]);
            m_attribute_sizes[i] = VertexAttributesTool::GetSize(m_attributes[i]);
        }

        m_attributes_bitmask = VertexAttributesTool::GetBitmaskFromArray(m_attributes);
    }

    bool VertexFactoryBuffers::HasAttribute(EVertexAttributes attr) const {
        return m_attribute_to_index_map[static_cast<size_t>(attr)] < m_num_of_attributes;
    }

    VertexAttributesBitmask VertexFactoryBuffers::GetAttributesBitmask() const {
        return m_attributes_bitmask;
    }

    size_t VertexFactoryBuffers::GetAttributesCount() const {
        return m_num_of_attributes;
    }

    EVertexAttributes VertexFactoryBuffers::GetAttribute(size_t idx) const {
        return m_attributes[idx];
    }

    size_t VertexFactoryBuffers::GetSizeOfAttribute(EVertexAttributes attr) const {
        return m_attribute_sizes[GetAttributeIndex(attr)];
    }

    EPixelFormat VertexFactoryBuffers::GetPixelFormatOfAttribute(EVertexAttributes attr) const {
        return m_pixel_formats[GetAttributeIndex(attr)];
    }

    size_t VertexFactoryBuffers::GetBufferLength(EVertexAttributes attr) const {
        size_t idx = GetAttributeIndex(attr);
        return m_buffers[idx].size() / m_attribute_sizes[idx];
    }

    size_t VertexFactoryBuffers::GetBufferByteSize(EVertexAttributes attr) const {
        size_t idx = GetAttributeIndex(attr);
        return m_buffers[idx].size();
    }

    const void* VertexFactoryBuffers::GetBufferData(EVertexAttributes attr) const {
        return m_buffers[GetAttributeIndex(attr)].data();
    }

    void* VertexFactoryBuffers::GetBufferData(EVertexAttributes attr) {
        return m_buffers[GetAttributeIndex(attr)].data();
    }

    void VertexFactoryBuffers::SetBufferLength(EVertexAttributes attr, size_t length) {
        auto idx = GetAttributeIndex(attr);
        m_buffers[idx].resize(length * m_attribute_sizes[idx]);
    }

    void VertexFactoryBuffers::AssignBuffer(EVertexAttributes attr, const void* data, size_t length) {
        size_t idx = GetAttributeIndex(attr);
        m_buffers[idx].resize(length * m_attribute_sizes[idx]);
        memcpy(m_buffers[idx].data(), data, length * m_attribute_sizes[idx]);
    }

    void VertexFactoryBuffers::AssignAllBuffers(const Array<std::tuple<EVertexAttributes, const void*, size_t>>& data) {
        assert(data.size() == m_num_of_attributes && "The number of attributes is inconsistent");
        for (const auto& [attr, ptr, length] : data) {
            AssignBuffer(attr, ptr, length);
        }
    }

    InputStream& VertexFactoryBuffers::operator>>(InputStream& _stream) {
        _stream >> m_num_of_attributes;

        m_buffers.resize(m_num_of_attributes);
        m_attributes.resize(m_num_of_attributes);

        m_pixel_formats.resize(m_num_of_attributes);
        m_attribute_sizes.resize(m_num_of_attributes);

        for (size_t i = 0; i < VA_NUM; i++) {
            m_attribute_to_index_map[i] = std::numeric_limits<size_t>::max();
        }

        for (size_t i = 0; i < m_num_of_attributes; i++) {
            size_t tmp;
            _stream >> tmp;
            m_attributes[i]                                                = static_cast<EVertexAttributes>(tmp);
            m_attribute_to_index_map[static_cast<size_t>(m_attributes[i])] = i;

            m_pixel_formats[i]   = VertexAttributesTool::GetPixelFormat(m_attributes[i]);
            m_attribute_sizes[i] = VertexAttributesTool::GetSize(m_attributes[i]);
        }

        m_attributes_bitmask = VertexAttributesTool::GetBitmaskFromArray(m_attributes);

        for (size_t i = 0; i < m_num_of_attributes; i++) {
            _stream >> m_buffers[i];
        }
        return _stream;
    }

    OutputStream& VertexFactoryBuffers::operator<<(OutputStream& _stream) const {
        _stream << m_num_of_attributes;
        for (size_t i = 0; i < m_num_of_attributes; i++) {
            _stream << static_cast<size_t>(m_attributes[i]);
        }
        for (size_t i = 0; i < m_num_of_attributes; i++) {
            _stream << m_buffers[i];
        }
        return _stream;
    }

    // private:

    size_t VertexFactoryBuffers::GetAttributeIndex(EVertexAttributes attr) const {
        if (m_attribute_to_index_map[static_cast<size_t>(attr)] >= m_num_of_attributes) {
            if (m_num_of_attributes == 0) {
                LOG_ERROR("VertexFactoryBuffers is not initialized correctly");
                assert(false);
            } else {
                LOG_ERROR(
                    "Invalid EVertexAttributes: {}, and m_attribute_to_index_map[{}] = {}",
                    static_cast<size_t>(attr),
                    static_cast<size_t>(attr),
                    m_attribute_to_index_map[static_cast<size_t>(attr)]);
                assert(false);
            }
        }
        return m_attribute_to_index_map[static_cast<size_t>(attr)];
    }

    // test:

    void TestVertexFactoryBuffers() {
        LOG_INFO("TestVertexFactoryBuffers Start!");

        VertexFactoryBuffers default_vf({EVertexAttributes::VA_POSITION,
                                         EVertexAttributes::VA_TANGENT,
                                         EVertexAttributes::VA_NORMAL,
                                         EVertexAttributes::VA_TEXCOORD0});

        VertexFactoryBuffers only_position_and_normal_vf({EVertexAttributes::VA_POSITION,
                                                          EVertexAttributes::VA_NORMAL});

        assert(default_vf.GetAttributesCount() == 4);
        assert(only_position_and_normal_vf.GetAttributesCount() == 2);

        assert(default_vf.GetAttribute(0) == EVertexAttributes::VA_POSITION);
        assert(default_vf.GetAttribute(1) == EVertexAttributes::VA_TANGENT);
        assert(default_vf.GetAttribute(2) == EVertexAttributes::VA_NORMAL);
        assert(default_vf.GetAttribute(3) == EVertexAttributes::VA_TEXCOORD0);

        assert(only_position_and_normal_vf.GetAttribute(0) == EVertexAttributes::VA_POSITION);
        assert(only_position_and_normal_vf.GetAttribute(1) == EVertexAttributes::VA_NORMAL);

        assert(default_vf.GetSizeOfAttribute(EVertexAttributes::VA_POSITION) == sizeof(float3));
        assert(default_vf.GetSizeOfAttribute(EVertexAttributes::VA_TANGENT) == sizeof(float));
        assert(default_vf.GetSizeOfAttribute(EVertexAttributes::VA_NORMAL) == sizeof(float));
        assert(default_vf.GetSizeOfAttribute(EVertexAttributes::VA_TEXCOORD0) == sizeof(float2));

        assert(only_position_and_normal_vf.GetSizeOfAttribute(EVertexAttributes::VA_POSITION) == sizeof(float3));
        assert(only_position_and_normal_vf.GetSizeOfAttribute(EVertexAttributes::VA_NORMAL) == sizeof(float));

        assert(default_vf.GetPixelFormatOfAttribute(EVertexAttributes::VA_POSITION) == PF_R32G32B32_SFLOAT);
        assert(default_vf.GetPixelFormatOfAttribute(EVertexAttributes::VA_TANGENT) == PF_R32_UINT);
        assert(default_vf.GetPixelFormatOfAttribute(EVertexAttributes::VA_NORMAL) == PF_R32_UINT);
        assert(default_vf.GetPixelFormatOfAttribute(EVertexAttributes::VA_TEXCOORD0) == PF_R32G32_SFLOAT);

        assert(only_position_and_normal_vf.GetPixelFormatOfAttribute(EVertexAttributes::VA_POSITION) == PF_R32G32B32_SFLOAT);
        assert(only_position_and_normal_vf.GetPixelFormatOfAttribute(EVertexAttributes::VA_NORMAL) == PF_R32_UINT);

        for (size_t i = 0; i < default_vf.GetAttributesCount(); i++) {
            assert(default_vf.GetBufferLength(default_vf.GetAttribute(i)) == 0);
        }
        for (size_t i = 0; i < only_position_and_normal_vf.GetAttributesCount(); i++) {
            assert(only_position_and_normal_vf.GetBufferLength(only_position_and_normal_vf.GetAttribute(i)) == 0);
        }

        LOG_INFO("TestVertexFactoryBuffers 1");

        {// safe method to assign buffers

            Array<float3> default_position_data = {float3(1, 2, 3), float3(4, 5, 6)};
            Array<uint>   default_tangent_data  = {1, 2};
            Array<uint>   default_normal_data   = {3, 4};
            Array<float2> default_texcoord_data = {float2(1, 2), float2(3, 4)};

            Array<float3> only_position_data = {float3(1, 2, 3), float3(4, 5, 6), float3(7, 8, 9), float3(10, 11, 12)};
            Array<uint>   only_normal_data   = {1, 2, 3, 4};

            default_vf.AssignAllBuffers({{EVertexAttributes::VA_POSITION, default_position_data.data(), default_position_data.size()},
                                         {EVertexAttributes::VA_TANGENT, default_tangent_data.data(), default_tangent_data.size()},
                                         {EVertexAttributes::VA_NORMAL, default_normal_data.data(), default_normal_data.size()},
                                         {EVertexAttributes::VA_TEXCOORD0, default_texcoord_data.data(), default_texcoord_data.size()}});

            only_position_and_normal_vf.AssignAllBuffers({{EVertexAttributes::VA_POSITION, only_position_data.data(), only_position_data.size()},
                                                          {EVertexAttributes::VA_NORMAL, only_normal_data.data(), only_normal_data.size()}});

            LOG_INFO("TestVertexFactoryBuffers 1.1");

            // length check
            for (size_t i = 0; i < default_vf.GetAttributesCount(); i++) {
                assert(default_vf.GetBufferLength(default_vf.GetAttribute(i)) == default_position_data.size());
            }
            for (size_t i = 0; i < only_position_and_normal_vf.GetAttributesCount(); i++) {
                assert(only_position_and_normal_vf.GetBufferLength(only_position_and_normal_vf.GetAttribute(i)) == only_position_data.size());
            }

            LOG_INFO("TestVertexFactoryBuffers 1.2");

            // 如果你想将void* Buffer转换成特定类型的Buffer（比如float3*），下面这种方式是比较合适的，因为你只需要传入一个EVertexAttributes即可
            //     具体类型，是在编译期，由VertexAttributesType<EVertexAttributes>确定的，这使得我们不会不小心传入错误的类型
            //     此外，因为这种方式是编译期决定类型的，所以你没法用for循环来实现类似的效果。类型信息在VertexFactoryBuffers中并不存在，必须在编译期决定
            //
            // 如果你觉得太麻烦，你也可以手动指定类型，一个方便但是容易出错的方法：
            //     auto buf = reinterpret_cast<float3*>(default_vf.GetBufferData(EVertexAttributes::VA_POSITION));
            //
            // 如果还是觉得麻烦，可以用：
            //     auto buf = (float3*)default_vf.GetBufferData(EVertexAttributes::VA_POSITION);

            auto default_position_buffer =
                reinterpret_cast<VertexAttributesType<EVertexAttributes::VA_POSITION>::type*>(
                    default_vf.GetBufferData(EVertexAttributes::VA_POSITION));
            auto default_position_buffer_length = default_vf.GetBufferLength(EVertexAttributes::VA_POSITION);

            auto default_tangent_buffer =
                reinterpret_cast<VertexAttributesType<EVertexAttributes::VA_TANGENT>::type*>(
                    default_vf.GetBufferData(EVertexAttributes::VA_TANGENT));
            auto default_tangent_buffer_length = default_vf.GetBufferLength(EVertexAttributes::VA_TANGENT);

            auto default_normal_buffer =
                reinterpret_cast<VertexAttributesType<EVertexAttributes::VA_NORMAL>::type*>(
                    default_vf.GetBufferData(EVertexAttributes::VA_NORMAL));
            auto default_normal_buffer_length = default_vf.GetBufferLength(EVertexAttributes::VA_NORMAL);

            auto default_texcoord_buffer =
                reinterpret_cast<VertexAttributesType<EVertexAttributes::VA_TEXCOORD0>::type*>(
                    default_vf.GetBufferData(EVertexAttributes::VA_TEXCOORD0));
            auto default_texcoord_buffer_length = default_vf.GetBufferLength(EVertexAttributes::VA_TEXCOORD0);

            LOG_INFO("TestVertexFactoryBuffers 1.3");

            for (size_t i = 0; i < default_position_buffer_length; i++) {
                for (size_t j = 0; j < 3; j++) {
                    assert(Compare(default_position_data[i][j], default_position_buffer[i][j]) == 0);
                }
            }
            for (size_t i = 0; i < default_tangent_buffer_length; i++) {
                assert(default_tangent_data[i] == default_tangent_buffer[i]);
            }
            for (size_t i = 0; i < default_normal_buffer_length; i++) {
                assert(default_normal_data[i] == default_normal_buffer[i]);
            }
            for (size_t i = 0; i < default_texcoord_buffer_length; i++) {
                for (size_t j = 0; j < 2; j++) {
                    assert(Compare(default_texcoord_data[i][j], default_texcoord_buffer[i][j]) == 0);
                }
            }

            LOG_INFO("TestVertexFactoryBuffers 1.4");

            auto only_position_buffer        = reinterpret_cast<float3*>(only_position_and_normal_vf.GetBufferData(EVertexAttributes::VA_POSITION));
            auto only_position_buffer_length = only_position_and_normal_vf.GetBufferLength(EVertexAttributes::VA_POSITION);

            auto only_normal_buffer        = reinterpret_cast<uint*>(only_position_and_normal_vf.GetBufferData(EVertexAttributes::VA_NORMAL));
            auto only_normal_buffer_length = only_position_and_normal_vf.GetBufferLength(EVertexAttributes::VA_NORMAL);

            for (size_t i = 0; i < only_position_buffer_length; i++) {
                for (size_t j = 0; j < 3; j++) {
                    assert(Compare(only_position_data[i][j], only_position_buffer[i][j]) == 0);
                }
            }
            for (size_t i = 0; i < only_normal_buffer_length; i++) {
                assert(only_normal_data[i] == only_normal_buffer[i]);
            }

            LOG_INFO("TestVertexFactoryBuffers 1.5");
        }

        LOG_INFO("TestVertexFactoryBuffers 2");

        {// unsafe method to assign buffers

            // test different lengths
            default_vf.SetBufferLength(EVertexAttributes::VA_POSITION, 3);
            default_vf.SetBufferLength(EVertexAttributes::VA_TANGENT, 3);
            default_vf.SetBufferLength(EVertexAttributes::VA_NORMAL, 0);
            default_vf.SetBufferLength(EVertexAttributes::VA_TEXCOORD0, 2);

            Array<float3> default_position_data = {float3(1, 2, 3), float3(4, 5, 6), float3(7, 8, 9)};
            Array<uint>   default_tangent_data  = {1, 2, 3};
            Array<uint>   default_normal_data   = {};
            Array<float2> default_texcoord_data = {float2(1, 2), float2(3, 4)};

            auto position_buffer =
                reinterpret_cast<VertexAttributesType<EVertexAttributes::VA_POSITION>::type*>(
                    default_vf.GetBufferData(EVertexAttributes::VA_POSITION));
            auto tangent_buffer =
                reinterpret_cast<VertexAttributesType<EVertexAttributes::VA_TANGENT>::type*>(
                    default_vf.GetBufferData(EVertexAttributes::VA_TANGENT));
            auto normal_buffer =
                reinterpret_cast<VertexAttributesType<EVertexAttributes::VA_NORMAL>::type*>(
                    default_vf.GetBufferData(EVertexAttributes::VA_NORMAL));
            auto texcoord_buffer =
                reinterpret_cast<VertexAttributesType<EVertexAttributes::VA_TEXCOORD0>::type*>(
                    default_vf.GetBufferData(EVertexAttributes::VA_TEXCOORD0));

            memcpy(position_buffer, default_position_data.data(), default_position_data.size() * default_vf.GetSizeOfAttribute(EVertexAttributes::VA_POSITION));
            memcpy(tangent_buffer, default_tangent_data.data(), default_tangent_data.size() * default_vf.GetSizeOfAttribute(EVertexAttributes::VA_TANGENT));
            memcpy(normal_buffer, default_normal_data.data(), default_normal_data.size() * default_vf.GetSizeOfAttribute(EVertexAttributes::VA_NORMAL));
            memcpy(texcoord_buffer, default_texcoord_data.data(), default_texcoord_data.size() * default_vf.GetSizeOfAttribute(EVertexAttributes::VA_TEXCOORD0));

            for (size_t i = 0; i < default_vf.GetBufferLength(EVertexAttributes::VA_POSITION); i++) {
                for (size_t j = 0; j < 3; j++) {
                    assert(Compare(default_position_data[i][j], position_buffer[i][j]) == 0);
                }
            }
            for (size_t i = 0; i < default_vf.GetBufferLength(EVertexAttributes::VA_TANGENT); i++) {
                assert(default_tangent_data[i] == tangent_buffer[i]);
            }
            for (size_t i = 0; i < default_vf.GetBufferLength(EVertexAttributes::VA_NORMAL); i++) {
                assert(default_normal_data[i] == normal_buffer[i]);
            }
            for (size_t i = 0; i < default_vf.GetBufferLength(EVertexAttributes::VA_TEXCOORD0); i++) {
                for (size_t j = 0; j < 2; j++) {
                    assert(Compare(default_texcoord_data[i][j], texcoord_buffer[i][j]) == 0);
                }
            }
        }

        LOG_INFO("TestVertexFactoryBuffers 3");

        {// input stream & output stream test
            std::stringstream ss;

            InputStream  is(ss);
            OutputStream os(ss);

            os << default_vf;

            VertexFactoryBuffers new_vf;

            is >> new_vf;

            assert(default_vf.GetAttributesCount() == new_vf.GetAttributesCount());

            for (size_t i = 0; i < default_vf.GetAttributesCount(); i++) {
                assert(default_vf.GetAttribute(i) == new_vf.GetAttribute(i));
                assert(default_vf.GetBufferLength(default_vf.GetAttribute(i)) == new_vf.GetBufferLength(new_vf.GetAttribute(i)));
                assert(default_vf.GetSizeOfAttribute(default_vf.GetAttribute(i)) == new_vf.GetSizeOfAttribute(new_vf.GetAttribute(i)));
                assert(default_vf.GetPixelFormatOfAttribute(default_vf.GetAttribute(i)) == new_vf.GetPixelFormatOfAttribute(new_vf.GetAttribute(i)));
            }

            auto default_position_buffer = (uint8*)default_vf.GetBufferData(EVertexAttributes::VA_POSITION);
            auto default_tangent_buffer  = (uint8*)default_vf.GetBufferData(EVertexAttributes::VA_TANGENT);
            auto default_normal_buffer   = (uint8*)default_vf.GetBufferData(EVertexAttributes::VA_NORMAL);
            auto default_texcoord_buffer = (uint8*)default_vf.GetBufferData(EVertexAttributes::VA_TEXCOORD0);

            auto new_position_buffer = (uint8*)new_vf.GetBufferData(EVertexAttributes::VA_POSITION);
            auto new_tangent_buffer  = (uint8*)new_vf.GetBufferData(EVertexAttributes::VA_TANGENT);
            auto new_normal_buffer   = (uint8*)new_vf.GetBufferData(EVertexAttributes::VA_NORMAL);
            auto new_texcoord_buffer = (uint8*)new_vf.GetBufferData(EVertexAttributes::VA_TEXCOORD0);

            auto position_buf_size = default_vf.GetBufferLength(EVertexAttributes::VA_POSITION) * default_vf.GetSizeOfAttribute(EVertexAttributes::VA_POSITION);
            for (size_t i = 0; i < position_buf_size; i++) {
                assert(default_position_buffer[i] == new_position_buffer[i]);
            }

            auto tangent_buf_size = default_vf.GetBufferLength(EVertexAttributes::VA_TANGENT) * default_vf.GetSizeOfAttribute(EVertexAttributes::VA_TANGENT);
            for (size_t i = 0; i < tangent_buf_size; i++) {
                assert(default_tangent_buffer[i] == new_tangent_buffer[i]);
            }

            auto normal_buf_size = default_vf.GetBufferLength(EVertexAttributes::VA_NORMAL) * default_vf.GetSizeOfAttribute(EVertexAttributes::VA_NORMAL);
            for (size_t i = 0; i < normal_buf_size; i++) {
                assert(default_normal_buffer[i] == new_normal_buffer[i]);
            }

            auto texcoord_buf_size = default_vf.GetBufferLength(EVertexAttributes::VA_TEXCOORD0) * default_vf.GetSizeOfAttribute(EVertexAttributes::VA_TEXCOORD0);
            for (size_t i = 0; i < texcoord_buf_size; i++) {
                assert(default_texcoord_buffer[i] == new_texcoord_buffer[i]);
            }
        }

        LOG_INFO("TestVertexFactoryBuffers Passed!");

        // the following code will produce an assertion error

        // only_position_and_normal_vf.GetPixelFormatOfAttribute(EVertexAttributes::VA_TANGENT);
    }

}// namespace Moer
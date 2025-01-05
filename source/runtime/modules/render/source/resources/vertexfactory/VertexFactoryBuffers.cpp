#include "resources/vertexfactory/VertexFactoryBuffers.h"

#include <tuple>
#include <limits>
#include <sstream>

#include "log/LogSystem.h"

namespace Moer {

    // public:

    VertexFactoryBuffers::VertexFactoryBuffers() {}// In this case, VFB will be initialized by InputStream

    VertexFactoryBuffers::VertexFactoryBuffers(
        const std::initializer_list<EVertexAttributes>& attributes)
        : m_num_of_attributes(attributes.size()),
          m_buffers_length(0),

          m_buffers(m_num_of_attributes),
          m_attributes(m_num_of_attributes),

          m_pixel_formats(m_num_of_attributes),
          m_attribute_sizes(m_num_of_attributes) {

        for (size_t i = 0; i < VA_NUM; i++) {
            m_attributes_map[i] = std::numeric_limits<size_t>::max();
        }

        for (size_t i = 0; i < m_num_of_attributes; i++) {
            m_attributes[i]                                        = *(attributes.begin() + i);
            m_attributes_map[static_cast<size_t>(m_attributes[i])] = i;

            m_pixel_formats[i]   = VertexAttributesTool::GetPixelFormat(m_attributes[i]);
            m_attribute_sizes[i] = VertexAttributesTool::GetSize(m_attributes[i]);
        }
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

    size_t VertexFactoryBuffers::GetBufferLength() const {
        return m_buffers_length;
    }

    const void* VertexFactoryBuffers::GetBufferData(EVertexAttributes attr) const {
        return m_buffers[GetAttributeIndex(attr)].data();
    }

    void* VertexFactoryBuffers::GetBufferData(EVertexAttributes attr) {
        return m_buffers[GetAttributeIndex(attr)].data();
    }

    void VertexFactoryBuffers::SetBufferLength(size_t length) {
        m_buffers_length = length;
        for (size_t i = 0; i < m_num_of_attributes; i++) {
            m_buffers[i].resize(length * m_attribute_sizes[i]);
        }
    }

    void VertexFactoryBuffers::AssignAllBuffers(const Array<std::tuple<EVertexAttributes, const void*, size_t>>& data) {
        assert(data.size() == m_num_of_attributes && "The number of attributes is inconsistent");
        m_buffers_length = std::get<2>(data[0]);
        for (const auto& [attr, ptr, length] : data) {
            assert(m_buffers_length == length && "Inconsistent buffer length");
            AssignBuffer(attr, ptr, length);
        }
    }

    InputStream& VertexFactoryBuffers::operator>>(InputStream& _stream) {
        _stream >> m_num_of_attributes;
        m_buffers_length = 0;

        m_buffers.resize(m_num_of_attributes);
        m_attributes.resize(m_num_of_attributes);

        m_pixel_formats.resize(m_num_of_attributes);
        m_attribute_sizes.resize(m_num_of_attributes);

        for (size_t i = 0; i < VA_NUM; i++) {
            m_attributes_map[i] = std::numeric_limits<size_t>::max();
        }

        for (size_t i = 0; i < m_num_of_attributes; i++) {
            size_t tmp;
            _stream >> tmp;
            m_attributes[i]                                        = static_cast<EVertexAttributes>(tmp);
            m_attributes_map[static_cast<size_t>(m_attributes[i])] = i;

            m_pixel_formats[i]   = VertexAttributesTool::GetPixelFormat(m_attributes[i]);
            m_attribute_sizes[i] = VertexAttributesTool::GetSize(m_attributes[i]);
        }

        for (size_t i = 0; i < m_num_of_attributes; i++) {
            _stream >> m_buffers[i];
        }
        m_buffers_length = m_buffers[0].size() / m_attribute_sizes[0];
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
        if (m_attributes_map[static_cast<size_t>(attr)] >= m_num_of_attributes) {
            if (m_num_of_attributes == 0) {
                LOG_ERROR("VertexFactoryBuffers is not initialized correctly");
                assert(false);
            } else {
                LOG_ERROR(
                    "Invalid EVertexAttributes: {}, and m_attributes_map[{}] = {}",
                    static_cast<size_t>(attr),
                    static_cast<size_t>(attr),
                    m_attributes_map[static_cast<size_t>(attr)]);
                assert(false);
            }
        }
        return m_attributes_map[static_cast<size_t>(attr)];
    }

    void VertexFactoryBuffers::AssignBuffer(EVertexAttributes attr, const void* data, size_t length) {
        size_t idx = GetAttributeIndex(attr);
        m_buffers[idx].resize(length * m_attribute_sizes[idx]);
        memcpy(m_buffers[idx].data(), data, length * m_attribute_sizes[idx]);
    }

    // test:

    void TestVertexFactoryBuffers() {
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

        assert(default_vf.GetBufferLength() == 0);
        assert(only_position_and_normal_vf.GetBufferLength() == 0);

        {// safe method
            Array<float3> default_position_data = {float3(1, 2, 3), float3(4, 5, 6)};
            Array<float>  default_tangent_data  = {1, 2};
            Array<float>  default_normal_data   = {3, 4};
            Array<float2> default_texcoord_data = {float2(1, 2), float2(3, 4)};

            Array<float3> only_position_data = {float3(1, 2, 3), float3(4, 5, 6), float3(7, 8, 9), float3(10, 11, 12)};
            Array<float>  only_normal_data   = {1, 2, 3, 4};

            default_vf.AssignAllBuffers({{EVertexAttributes::VA_POSITION, default_position_data.data(), default_position_data.size()},
                                         {EVertexAttributes::VA_TANGENT, default_tangent_data.data(), default_tangent_data.size()},
                                         {EVertexAttributes::VA_NORMAL, default_normal_data.data(), default_normal_data.size()},
                                         {EVertexAttributes::VA_TEXCOORD0, default_texcoord_data.data(), default_texcoord_data.size()}});

            only_position_and_normal_vf.AssignAllBuffers({{EVertexAttributes::VA_POSITION, only_position_data.data(), only_position_data.size()},
                                                          {EVertexAttributes::VA_NORMAL, only_normal_data.data(), only_normal_data.size()}});

            assert(default_vf.GetBufferLength() == 2);
            assert(only_position_and_normal_vf.GetBufferLength() == 4);

            auto default_position_buffer = default_vf.GetBufferData(EVertexAttributes::VA_POSITION);
            auto default_tangent_buffer  = default_vf.GetBufferData(EVertexAttributes::VA_TANGENT);
            auto default_normal_buffer   = default_vf.GetBufferData(EVertexAttributes::VA_NORMAL);
            auto default_texcoord_buffer = default_vf.GetBufferData(EVertexAttributes::VA_TEXCOORD0);

            auto only_position_buffer = only_position_and_normal_vf.GetBufferData(EVertexAttributes::VA_POSITION);
            auto only_normal_buffer   = only_position_and_normal_vf.GetBufferData(EVertexAttributes::VA_NORMAL);

            for (size_t i = 0; i < default_vf.GetBufferLength(); i++) {
                for (size_t j = 0; j < 3; j++) {
                    assert(default_position_data[i][j] == ((float3*)default_position_buffer)[i][j]);
                }
                assert(default_tangent_data[i] == ((float*)default_tangent_buffer)[i]);
                assert(default_normal_data[i] == ((float*)default_normal_buffer)[i]);
                for (size_t j = 0; j < 2; j++) {
                    assert(default_texcoord_data[i][j] == ((float2*)default_texcoord_buffer)[i][j]);
                }
            }

            for (size_t i = 0; i < only_position_and_normal_vf.GetBufferLength(); i++) {
                for (size_t j = 0; j < 3; j++) {
                    assert(only_position_data[i][j] == ((float3*)only_position_buffer)[i][j]);
                }
                assert(only_normal_data[i] == ((float*)only_normal_buffer)[i]);
            }
        }

        {// unsafe method
            default_vf.SetBufferLength(3);

            Array<float3> default_position_data = {float3(1, 2, 3), float3(4, 5, 6), float3(7, 8, 9)};
            Array<float>  default_tangent_data  = {1, 2, 3};
            Array<float>  default_normal_data   = {4, 5, 6};
            Array<float2> default_texcoord_data = {float2(1, 2), float2(3, 4), float2(5, 6)};

            auto position_data_ptr = default_vf.GetBufferData(EVertexAttributes::VA_POSITION);
            auto tangent_data_ptr  = default_vf.GetBufferData(EVertexAttributes::VA_TANGENT);
            auto normal_data_ptr   = default_vf.GetBufferData(EVertexAttributes::VA_NORMAL);
            auto texcoord_data_ptr = default_vf.GetBufferData(EVertexAttributes::VA_TEXCOORD0);

            memcpy(position_data_ptr, default_position_data.data(), default_position_data.size() * default_vf.GetSizeOfAttribute(EVertexAttributes::VA_POSITION));
            memcpy(tangent_data_ptr, default_tangent_data.data(), default_tangent_data.size() * default_vf.GetSizeOfAttribute(EVertexAttributes::VA_TANGENT));
            memcpy(normal_data_ptr, default_normal_data.data(), default_normal_data.size() * default_vf.GetSizeOfAttribute(EVertexAttributes::VA_NORMAL));
            memcpy(texcoord_data_ptr, default_texcoord_data.data(), default_texcoord_data.size() * default_vf.GetSizeOfAttribute(EVertexAttributes::VA_TEXCOORD0));

            for (size_t i = 0; i < default_vf.GetBufferLength(); i++) {
                for (size_t j = 0; j < 3; j++) {
                    assert(default_position_data[i][j] == ((float3*)position_data_ptr)[i][j]);
                }
                assert(default_tangent_data[i] == ((float*)tangent_data_ptr)[i]);
                assert(default_normal_data[i] == ((float*)normal_data_ptr)[i]);
                for (size_t j = 0; j < 2; j++) {
                    assert(default_texcoord_data[i][j] == ((float2*)texcoord_data_ptr)[i][j]);
                }
            }
        }

        {// input stream & output stream test
            std::stringstream ss;

            InputStream  is(ss);
            OutputStream os(ss);

            os << default_vf;

            VertexFactoryBuffers new_vf;

            is >> new_vf;

            assert(default_vf.GetAttributesCount() == new_vf.GetAttributesCount());
            assert(default_vf.GetBufferLength() == new_vf.GetBufferLength());

            for (size_t i = 0; i < default_vf.GetAttributesCount(); i++) {
                assert(default_vf.GetAttribute(i) == new_vf.GetAttribute(i));
                assert(default_vf.GetSizeOfAttribute(default_vf.GetAttribute(i)) == new_vf.GetSizeOfAttribute(new_vf.GetAttribute(i)));
                assert(default_vf.GetPixelFormatOfAttribute(default_vf.GetAttribute(i)) == new_vf.GetPixelFormatOfAttribute(new_vf.GetAttribute(i)));
            }

            auto default_position_buffer = default_vf.GetBufferData(EVertexAttributes::VA_POSITION);
            auto default_tangent_buffer  = default_vf.GetBufferData(EVertexAttributes::VA_TANGENT);
            auto default_normal_buffer   = default_vf.GetBufferData(EVertexAttributes::VA_NORMAL);
            auto default_texcoord_buffer = default_vf.GetBufferData(EVertexAttributes::VA_TEXCOORD0);

            auto new_position_buffer = new_vf.GetBufferData(EVertexAttributes::VA_POSITION);
            auto new_tangent_buffer  = new_vf.GetBufferData(EVertexAttributes::VA_TANGENT);
            auto new_normal_buffer   = new_vf.GetBufferData(EVertexAttributes::VA_NORMAL);
            auto new_texcoord_buffer = new_vf.GetBufferData(EVertexAttributes::VA_TEXCOORD0);

            for (size_t i = 0; i < default_vf.GetBufferLength(); i++) {
                for (size_t j = 0; j < 3; j++) {
                    assert(((float3*)default_position_buffer)[i][j] == ((float3*)new_position_buffer)[i][j]);
                }
                assert(((float*)default_tangent_buffer)[i] == ((float*)new_tangent_buffer)[i]);
                assert(((float*)default_normal_buffer)[i] == ((float*)new_normal_buffer)[i]);
                for (size_t j = 0; j < 2; j++) {
                    assert(((float2*)default_texcoord_buffer)[i][j] == ((float2*)new_texcoord_buffer)[i][j]);
                }
            }
        }

        // the following code will produce an assertion error

        // only_position_and_normal_vf.GetPixelFormatOfAttribute(EVertexAttributes::VA_TANGENT);
    }

}// namespace Moer
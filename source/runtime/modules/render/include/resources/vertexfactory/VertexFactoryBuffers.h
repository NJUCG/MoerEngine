#pragma once

#include <concepts>
#include <type_traits>

#include "RenderAPI.h"
#include "misc/STL.h"
#include "misc/CompileTimeString.h"
#include "resources/vertexfactory/VertexAttributes.h"
#include "serialize/Serializer.h"

namespace Moer {

    /**
     * Vertex Factory Buffers
     * 
     * Store vertex buffer data and provide corresponding attribute information
     * 
     * Support any number and type of attribute combinations.
     * Attributes must be determined when instantiating VertexFactory (or deserialized through InputStream)
     * 
     * The buffer length and data can be dynamically set after instantiation
     */
    class RENDER_API VertexFactoryBuffers {

    private:
        // assert m_num_of_attributes == m_buffers.size() == m_attributes.size() == m_pixel_formats.size()
        size_t m_num_of_attributes;

        Array<Array<uint8>>         m_buffers;// Buffer length == m_buffers[0].length() / m_attribute_sizes[0]
        Array<EVertexAttributes>    m_attributes;
        StaticArray<size_t, VA_NUM> m_attributes_map;

        Array<EPixelFormat> m_pixel_formats;  // cache, could be instead by VertexAttributesTool
        Array<size_t>       m_attribute_sizes;// cache, could be instead by VertexAttributesTool

    public:
        VertexFactoryBuffers();

        VertexFactoryBuffers(const std::initializer_list<EVertexAttributes>& attributes);

        size_t GetAttributesCount() const;

        EVertexAttributes GetAttribute(size_t idx) const;

        size_t GetSizeOfAttribute(EVertexAttributes attr) const;

        EPixelFormat GetPixelFormatOfAttribute(EVertexAttributes attr) const;

        size_t GetBufferLength(EVertexAttributes attr) const;

        const void* GetBufferData(EVertexAttributes attr) const;

        void* GetBufferData(EVertexAttributes attr);

        /**
         * Only call this method when you are sure that the buffer length is correct!
         * 
         * Example: TestVertexFactoryBuffers => unsafe method
         */
        void SetBufferLength(EVertexAttributes attr, size_t length);

        /**
         * Safe but slow method. (Need an extra memory copy)
         * 
         * Example: TestVertexFactoryBuffers => safe method
         * 
         * If you want a fast but unsafe method, you can write your own AssignAllBuffers.
         */
        void AssignBuffer(EVertexAttributes attr, const void* data, size_t size);

        /**
         * Safe but slow method. (Need an extra memory copy)
         * 
         * Example: TestVertexFactoryBuffers => safe method
         * 
         * If you want a fast but unsafe method, you can write your own AssignAllBuffers.
         */
        void AssignAllBuffers(const Array<std::tuple<EVertexAttributes, const void*, size_t>>& data);

        InputStream& operator>>(InputStream& stream);

        OutputStream& operator<<(OutputStream& stream) const;

    private:
        size_t GetAttributeIndex(EVertexAttributes attr) const;
    };

    void RENDER_API TestVertexFactoryBuffers();

}// namespace Moer
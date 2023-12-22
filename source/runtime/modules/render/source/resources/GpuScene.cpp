#include "resources/GpuScene.h"

#include "rhi/RHI.h"
#include "rhi/RHICommand.h"

namespace Moer  {

 class GpuPrimitiveBuilder::Impl {
 public:
     void   Vertex(const std::vector<float> * data );
     void   Index(const std::vector<uint32_t> * data);
     void   Attribute(VertexAttributeFlags attribute);
     static void InitBuild();
     static void EndBuild();
     RHIRenderPrimitiveRef Build();
     bool Validate() const;
 protected:
     static RHICommandQueue* copy_queue;
     static RHICopyCommandList* copy_cmd_list;

     const std::vector<float> * m_vertex_data{nullptr};
     const  std::vector<uint32_t> * m_index_data{nullptr};
     uint8_t m_attribute{0};
 };

RHICommandQueue*  GpuPrimitiveBuilder::Impl::copy_queue = nullptr;  
RHICopyCommandList*  GpuPrimitiveBuilder::Impl::copy_cmd_list = nullptr;  

void  GpuPrimitiveBuilder::Impl::Vertex(const std::vector<float>* data) {
 this->m_vertex_data = data;
}

void GpuPrimitiveBuilder::Impl::Index(const std::vector<uint32_t>* data) {
    this->m_index_data = data;
}

void GpuPrimitiveBuilder::Impl::Attribute(VertexAttributeFlags attribute) {
    this->m_attribute = attribute;
}

RHIRenderPrimitiveRef GpuPrimitiveBuilder::Impl::Build() {
     auto * cmd_list = copy_cmd_list;
     cmd_list->BeginRecording();

     uint32_t vertex_buffer_size = m_vertex_data->size()*sizeof(float);
     uint32_t index_buffer_size = m_index_data->size()*sizeof(uint32_t);

     RHIBufferCreateInfo  vertex_buffer_create_info(vertex_buffer_size,sizeof(float),EBufferUsageFlags::VERTEX_BUFFER);
     RHIBufferRef vertex_buffer = g_rhi->RHICreateBuffer(vertex_buffer_create_info);
     RHIBufferCreateInfo staging_vertex_buffer_create_info(vertex_buffer_size,sizeof(float),EBufferUsageFlags::TRANSFER_SRC | EBufferUsageFlags::CPU_VISIBLE);
     RHIBufferRef staging_vertex_buffer = g_rhi->RHICreateBuffer(staging_vertex_buffer_create_info);

     auto  staging_vertex_buffer_mapped_ptr = static_cast<float * >(g_rhi->RHIMapBuffer(staging_vertex_buffer,0,vertex_buffer_size));
     memcpy(staging_vertex_buffer_mapped_ptr,m_vertex_data->data(),vertex_buffer_size);
     g_rhi->RHIUnmapBuffer(staging_vertex_buffer);


     Array vertex_buffer_region_array({RHIBufferRegion{.src_offset = 0 ,.dst_offset = 0,.size =  vertex_buffer_size}});
     RHICopyBufferInfo vertex_copy_buffer_info{};
     vertex_copy_buffer_info.regions = vertex_buffer_region_array;
     cmd_list->CopyBuffer(vertex_copy_buffer_info,staging_vertex_buffer,vertex_buffer);

     RHIBufferCreateInfo  index_buffer_create_info( index_buffer_size,sizeof(uint32_t),EBufferUsageFlags::INDEX_BUFFER);
     RHIBufferRef  index_buffer = g_rhi->RHICreateBuffer( index_buffer_create_info);
     RHIBufferCreateInfo staging_index_buffer_create_info(index_buffer_size,sizeof(uint32_t),EBufferUsageFlags::TRANSFER_SRC | EBufferUsageFlags::CPU_VISIBLE);
     RHIBufferRef staging_index_buffer = g_rhi->RHICreateBuffer(staging_index_buffer_create_info);


     auto staging_index_buffer_mapped_ptr = static_cast<uint32_t * >(g_rhi->RHIMapBuffer(staging_index_buffer,0, index_buffer_size));
     memcpy(staging_index_buffer_mapped_ptr,m_index_data->data(), index_buffer_size);
     g_rhi->RHIUnmapBuffer(staging_index_buffer);

     Array index_buffer_region_array({RHIBufferRegion{.src_offset = 0 ,.dst_offset = 0,.size =  index_buffer_size}});
     RHICopyBufferInfo index_copy_buffer_info{};
     index_copy_buffer_info.regions = index_buffer_region_array;
     cmd_list->CopyBuffer(index_copy_buffer_info,staging_index_buffer,index_buffer);

     cmd_list->EndRecording();

     RHIFenceRef fence = g_rhi->RHICreateFence({.usage = EFenceUsageFlags::BINARY});
     RHISubmitInfo submit_info;
     submit_info.Signal(fence,1);
     copy_queue->SubmitCommands(1,cmd_list,&submit_info);
     copy_queue->WaitForQueueComplete();

     RHIRenderPrimitiveRef primitive = new RHIRenderPrimitive(vertex_buffer,index_buffer,EPrimitiveType::TRIANGLES,0,m_index_data->size());
     return primitive;
}

bool GpuPrimitiveBuilder::Impl::Validate() const {
    return m_vertex_data != nullptr && m_index_data != nullptr;
}



void GpuPrimitiveBuilder::Impl::InitBuild() {
    copy_cmd_list = g_rhi->RHICreateCopyCommandList(g_rhi->RHIGetCurrentCommandAllocator());
    copy_queue    = g_rhi->RHICreateCommandQueue(ECommandQueueType::COPY);
}

void GpuPrimitiveBuilder::Impl::EndBuild() {
    copy_cmd_list = nullptr;
    copy_queue    = nullptr;
}


void GpuPrimitiveBuilder::InitBuild() {
    Impl::InitBuild();
}

void GpuPrimitiveBuilder::EndBuild() {
    Impl::EndBuild();
}


GpuPrimitiveBuilder&  GpuPrimitiveBuilder::Vertex(const std::vector<float>* data) {
    m_impl->Vertex(data);
    return *this;
}
 GpuPrimitiveBuilder&  GpuPrimitiveBuilder::Index(const std::vector<uint32_t>* data) {
     m_impl->Index(data);
     return  *this;
 }
 GpuPrimitiveBuilder&  GpuPrimitiveBuilder::Attribute(VertexAttributeFlags attribute) {
     m_impl->Attribute(attribute);
     return *this;
 }
RHIRenderPrimitiveRef       GpuPrimitiveBuilder::Build() {
     assert(Validate() && "GpuPrimitiveBuilder::Build() called without valid data");
     return m_impl->Build();
 }

 GpuPrimitiveBuilder::GpuPrimitiveBuilder() {
     m_impl = new Impl();
 }

GpuPrimitiveBuilder::~GpuPrimitiveBuilder() {
    delete m_impl;
}

bool GpuPrimitiveBuilder::Validate() const {
    return m_impl->Validate();
}



}
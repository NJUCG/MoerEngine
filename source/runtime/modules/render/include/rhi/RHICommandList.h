//
// Created by 17152 on 2023/9/20.
//

#ifndef RHI_COMMAND_LIST_H
#define RHI_COMMAND_LIST_H
#include "RHI.h"
class RHICommandListBase{
protected:
    RHI_API RHICommandListBase();
public:
    RHI_API ~RHICommandListBase();


    FORCEINLINE RHIBufferRef CreateBuffer(const RHIBufferCreateInfo& _create_info){
        auto buffer_ref = g_rhi->RHICreateBuffer(this, _create_info);
        return buffer_ref;
    };

    FORCEINLINE RHIShaderResourceViewRef CreateShaderResourceView(RHIBuffer* _buffer, const RHIBufferSRVCreateInfo& _info){
        return g_rhi->RHICreateShaderResourceView(*this, _buffer, _info);
    }

    FORCEINLINE RHIShaderResourceViewRef CreateShaderResourceView(RHITexture* _texture, const RHITextureSRVCreateInfo& _info){
        return g_rhi->RHICreateShaderResourceView(*this, _texture, _info);
    }

    FORCEINLINE RHIUnorderedAccessViewRef  CreateUnorderedAccessView(RHIBuffer* _buffer, const RHIBufferUAVCreateInfo& _info){
        return g_rhi->RHICreateUnorderedAccessView(*this, _buffer, _info);
    }

    FORCEINLINE RHIUnorderedAccessViewRef  CreateUnorderedAccessView(RHITexture* _texture, const RHITextureUAVCreateInfo& _info){
        return g_rhi->RHICreateUnorderedAccessView(*this, _texture, _info);
    }


};

class RHIGraphicsCommandList final :public RHICommandListBase{

};

class RHIComputeCommandList final: public RHICommandListBase{
    
};
#endif//RHI_COMMAND_LIST_H

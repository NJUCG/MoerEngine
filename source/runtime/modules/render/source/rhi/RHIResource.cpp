//
// Created by JohnW1ck on 2023/9/17.
//
#include "RHIResource.h"
RHITexture::RHITexture(const RHITextureCreateInfo& _info): RHIViewableResource(RRT_TEXTURE), texture_info(_info) {
    SetName(_info.name);
}
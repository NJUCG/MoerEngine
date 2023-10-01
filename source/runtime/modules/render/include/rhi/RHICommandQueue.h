#ifndef MOERENGINE_RHI_COMMANDQUEUE_H
#define MOERENGINE_RHI_COMMANDQUEUE_H
#include <cstdint>
#include "RHIResource.h"

//todo: need this(dx12 style) or not(vulkan style, stateless)
class RHICommandQueue{
public:
    virtual void SubmitCommands(
        uint32_t _num_command_lists,
        RHICommandListBase* const* _command_lists
        ) = 0;

    //pre-submit
    virtual void Wait(RHIFence* _p_fence, uint64_t _value) = 0;

    //post-submit
    virtual void Signal(RHIFence* _p_fence, uint64_t _value) = 0;
};
#endif//MOERENGINE_RHI_COMMANDQUEUE_H

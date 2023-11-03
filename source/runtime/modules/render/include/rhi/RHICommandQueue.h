#ifndef RHI_COMMAND_QUEUE_H
#define RHI_COMMAND_QUEUE_H
#include <cstdint>
#include <vector>
#include "RHIResource.h"
#include "rhi/RHIResource.h"

struct RHISubmitInfo;

class RHICommandQueue {
public:
    virtual ~RHICommandQueue(){};
    virtual void SubmitCommands(
        uint32_t                  _num_command_lists,
        const RHICommandListBase* _command_lists,
        const RHISubmitInfo*      _submit_info = nullptr) = 0;
};
struct RHIFenceWaitInfo {
    uint64_t  wait_value;
    RHIFence* wait_fence;
};

struct RHIFenceSignalInfo {
    uint64_t  signal_value;
    RHIFence* signal_fence;
};
struct RHISubmitInfo {

    void Wait(RHIFence* _fence, uint64_t _wait_value) {
        wait_infos.emplace_back(_wait_value, _fence);
    };

    void Signal(RHIFence* _fence, uint64_t _signal_value) {
        signal_infos.emplace_back(_signal_value, _fence);
    };

    const std::vector<RHIFenceWaitInfo>&   GetWaitInfos() const { return wait_infos; }
    const std::vector<RHIFenceSignalInfo>& GetSignalInfos() const { return signal_infos; }

private:
    std::vector<RHIFenceWaitInfo>   wait_infos;
    std::vector<RHIFenceSignalInfo> signal_infos;
};
#endif//RHI_COMMAND_QUEUE_H

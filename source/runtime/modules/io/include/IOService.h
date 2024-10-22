

#pragma once
#include "Core.h"
#include "io/IOCommon.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "rhi/RHIResource.h"
// #include "misc/utils.h"
#include <atomic>
#include <cassert>
#include <filesystem>
#include <functional>
#include <span>
#include <thread>
#include <variant>
namespace Moer {

    struct Event {
        std::atomic_int64_t timeline;
        void                Wait(uint64_t _timeline) {
            while (this->timeline.load(std::memory_order_relaxed) < _timeline) {
                std::this_thread::yield();
            }
        }
        void Signal(uint64_t _timeline) {
            if (_timeline > this->timeline.load(std::memory_order_relaxed))
                this->timeline.store(_timeline, std::memory_order_relaxed);
        }
        bool IsSignaled(uint64_t _timeline) {
            return this->timeline.load(std::memory_order_relaxed) >= _timeline;
        }
    };
    class IOInterface;
    struct IOService {
        static void Init();
        static void Dispose();

        static uint64_t     Execute(class IOCommandList& _cmd_list);
        static void         Sync(uint64_t _time_stamp);
        static IOInterface* CreateGPUService(Render::RenderDevice* _device);
        static IOInterface* CreateCPUService();

        static void Destroy(IOInterface* _service);
        struct Impl;
    };

    class IOInterface {
    public:
        virtual uint64            Execute(IOCommandList& _cmd_list) = 0;
        virtual Render::WaitEvent GetWaitEvent(uint64 _time_stamp)  = 0;
        virtual void              Sync(uint64_t _time_stamp)        = 0;
    };

}// namespace Moer
#include "VulkanFaultInjection.h"
#include "rhi/vulkan/VulkanDevice.h"
#include "log/LogSystem.h"

#include <atomic>
#include <charconv>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace Moer::Render {
namespace {
uint64 present_trigger = 0;
uint64 worker_trigger = 0;
std::atomic<uint64> present_attempts{0};
std::atomic<uint64> worker_attempts{0};

uint64_t ParseVulkanPresentSubmitFaultTrigger(int argc, const char** argv) {
    constexpr std::string_view argument_name = "--vulkan-fault-inject";
    constexpr std::string_view prefix        = "--vulkan-fault-inject=";
    constexpr std::string_view point_prefix  = "present-submit@";

    std::optional<std::string_view> specification;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        std::optional<std::string_view> candidate;
        if (argument == argument_name) {
            if (index + 1 >= argc) {
                throw std::invalid_argument("--vulkan-fault-inject requires <point>@<positive-count>");
            }
            candidate = std::string_view(argv[++index]);
        } else if (argument.starts_with(prefix)) {
            candidate = argument.substr(prefix.size());
        }

        if (!candidate.has_value()) {
            continue;
        }
        if (specification.has_value()) {
            throw std::invalid_argument("--vulkan-fault-inject may be specified only once");
        }
        specification = *candidate;
    }

    if (!specification.has_value()) {
        return 0;
    }
    if (!specification->starts_with(point_prefix)) {
        throw std::invalid_argument(
            "unsupported Vulkan fault point; expected present-submit@<positive-count>"
        );
    }

    const std::string_view count_text = specification->substr(point_prefix.size());
    uint64_t               count      = 0;
    const auto [end, error] = std::from_chars(
        count_text.data(), count_text.data() + count_text.size(), count
    );
    if (count_text.empty() || error != std::errc{} || end != count_text.data() + count_text.size() ||
        count == 0) {
        throw std::invalid_argument(
            "Vulkan fault trigger count must be a positive integer: present-submit@<positive-count>"
        );
    }
    return count;
}

uint64_t ParseParallelRecordWorkerThrowTrigger(int argc, const char** argv) {
    constexpr std::string_view argument_name = "--parallel-record-fault-inject";
    constexpr std::string_view prefix        = "--parallel-record-fault-inject=";
    constexpr std::string_view point_prefix  = "worker-throw@";

    std::optional<std::string_view> specification;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        std::optional<std::string_view> candidate;
        if (argument == argument_name) {
            if (index + 1 >= argc) {
                throw std::invalid_argument(
                    "--parallel-record-fault-inject requires worker-throw@<positive-count>"
                );
            }
            candidate = std::string_view(argv[++index]);
        } else if (argument.starts_with(prefix)) {
            candidate = argument.substr(prefix.size());
        }

        if (!candidate.has_value()) {
            continue;
        }
        if (specification.has_value()) {
            throw std::invalid_argument(
                "--parallel-record-fault-inject may be specified only once"
            );
        }
        specification = *candidate;
    }

    if (!specification.has_value()) {
        return 0;
    }
    if (!specification->starts_with(point_prefix)) {
        throw std::invalid_argument(
            "unsupported parallel-record fault point; expected worker-throw@<positive-count>"
        );
    }

    const std::string_view count_text = specification->substr(point_prefix.size());
    uint64_t               count      = 0;
    const auto [end, error] = std::from_chars(
        count_text.data(), count_text.data() + count_text.size(), count
    );
    if (count_text.empty() || error != std::errc{} || end != count_text.data() + count_text.size() ||
        count == 0) {
        throw std::invalid_argument(
            "parallel-record worker fault trigger must be a positive integer: "
            "worker-throw@<positive-count>"
        );
    }
    return count;
}

} // namespace

void VulkanFaultInjection::Configure(uint64 _present_trigger, uint64 _worker_trigger) {
    present_trigger = _present_trigger;
    worker_trigger = _worker_trigger;
    present_attempts.store(0, std::memory_order_relaxed);
    worker_attempts.store(0, std::memory_order_relaxed);
    if (present_trigger != 0) {
        LOG_INFO("[VulkanFault][Config] point=present-submit trigger={} mode=synthetic-device-lost", present_trigger);
    }
    if (worker_trigger != 0) {
        LOG_INFO("[ParallelRecord][FaultConfig] point=worker-throw trigger={}", worker_trigger);
    }
}

void VulkanFaultInjection::ValidateCommandLine(int argc, const char** argv) {
    (void)ParseVulkanPresentSubmitFaultTrigger(argc, argv);
    (void)ParseParallelRecordWorkerThrowTrigger(argc, argv);
}

void VulkanFaultInjection::ConfigureFromCommandLine(int argc, const char** argv) {
    Configure(ParseVulkanPresentSubmitFaultTrigger(argc, argv), ParseParallelRecordWorkerThrowTrigger(argc, argv));
}

uint64 VulkanFaultInjection::GetPresentTrigger() { return present_trigger; }
uint64 VulkanFaultInjection::GetWorkerTrigger() { return worker_trigger; }

bool VulkanFaultInjection::ShouldInjectPresentSubmit() {
    return present_trigger != 0 &&
           present_attempts.fetch_add(1, std::memory_order_relaxed) + 1 == present_trigger;
}

bool VulkanFaultInjection::ShouldFailWorker(EQueueType _queue) {
    return _queue == EQueueType::Graphics && worker_trigger != 0 &&
           worker_attempts.fetch_add(1, std::memory_order_acq_rel) + 1 == worker_trigger;
}

VulkanOperationResult VulkanFaultInjection::InjectPresentSubmitFault(
    VulkanDevice& _device, const VulkanOperationContext& _context
) {
    std::unique_lock<std::shared_mutex> gate(_device.native_queue_gate);
    if (_device.IsFaulted()) {
        gate.unlock();
        _device.RecordRejectedSubmit();
        return {EVulkanOperationStatus::Rejected, _device.GetFirstFaultResult()};
    }

    Array<std::unique_lock<std::mutex>> queue_locks;
    const auto                         queue_mutexes = _device.GetUniqueQueueHostMutexes();
    queue_locks.reserve(queue_mutexes.size());
    for (auto* mutex : queue_mutexes) {
        queue_locks.emplace_back(*mutex);
    }

    const VkResult predrain_result = vkDeviceWaitIdle(_device.m_device);
    const bool     predrained      = predrain_result == VK_SUCCESS;
    const VkResult result          = predrained ? VK_ERROR_DEVICE_LOST : predrain_result;
    if (_device.TryBeginFirstFault(result)) {
        _device.PublishFirstFaultLocked(_context, result, true, predrained);
    }
    if (!predrained && result != VK_ERROR_DEVICE_LOST) {
        _device.EmergencyExitWithoutVulkanCleanup(_context, result);
    }
    return {EVulkanOperationStatus::Faulted, result};
}

} // namespace Moer::Render

#include "rhi/RHICommand.h"
#include "rhi/RHIImpl.h"
#include "rhi/RHIReadback.h"
#include "rhi/RHIThreadOwnership.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

using namespace Moer::Render;
using namespace std::chrono_literals;
using Moer::byte;

class ReadbackTestBuffer final : public Buffer {
public:
    explicit ReadbackTestBuffer(bool _supports_owning_readback) :
        Buffer(BufferInfo{
            64, 1, EBufferUsageFlags::TRANSFER_SRC
        }),
        supports_owning_readback_(_supports_owning_readback) {}

    void SetName(const std::string_view) override {}

    [[nodiscard]] bool SupportsOwningReadback() const noexcept override {
        return supports_owning_readback_;
    }

private:
    bool supports_owning_readback_{false};
};

void Expect(bool _condition, std::string_view _message) {
    if (!_condition) {
        throw std::runtime_error(std::string(_message));
    }
}

template<typename TException, typename TCallback>
void ExpectThrows(TCallback&& _callback, std::string_view _message) {
    bool threw_expected = false;
    try {
        _callback();
    } catch (const TException&) {
        threw_expected = true;
    }
    Expect(threw_expected, _message);
}

struct ByteWriterContext {
    std::span<const byte> source{};
    bool                  throw_failure{false};
};

void WriteBytes(void* _context, std::span<byte> _destination) {
    auto* context = static_cast<ByteWriterContext*>(_context);
    if (context == nullptr || context->throw_failure) {
        throw std::runtime_error("injected readback materialization failure");
    }
    if (context->source.size_bytes() != _destination.size_bytes()) {
        throw std::runtime_error("readback test payload size mismatch");
    }
    std::copy(
        context->source.begin(),
        context->source.end(),
        _destination.begin()
    );
}

struct ReadbackHarness {
    CommandList         list{EQueueType::Copy};
    GpuCompletionFuture completion{};
    ReadbackToken       token{};
    ReadbackFuture      future{};

    explicit ReadbackHarness(
        std::size_t      _byte_size,
        std::string_view _name = "ReadbackHarness"
    ) {
        completion = list.TrackGpuCompletion(_name);
        token = ReadbackBackendAccess::Create(
            1, _byte_size, _name, completion
        );
        future = token.GetFuture();
    }

    CmdSubmit Submit() {
        return list.Submit();
    }
};

void PublishCompletion(
    CmdSubmit& _submit,
    bool       _ready,
    bool       _notify = true
) {
    const GpuCompletionPublishBatch batch =
        GpuCompletionBackendAccess::BeginPublishBatch();
    if (_ready) {
        GpuCompletionBackendAccess::PublishReady(
            _submit.gpu_completion_tokens, batch
        );
    } else {
        GpuCompletionBackendAccess::PublishErrorsIfPending(
            _submit.gpu_completion_tokens,
            "injected readback completion failure",
            batch
        );
    }
    if (_notify) {
        GpuCompletionBackendAccess::NotifyTerminals(
            _submit.gpu_completion_tokens, batch
        );
        _submit.gpu_completion_tokens.clear();
    } else {
        _submit.gpu_completion_publish_batch = batch;
    }
}

void NotifyPublishedCompletion(CmdSubmit& _submit) {
    const GpuCompletionPublishBatch batch =
        _submit.gpu_completion_publish_batch;
    Expect(batch.Valid(), "readback test lost its completion batch");
    GpuCompletionBackendAccess::NotifyTerminals(
        _submit.gpu_completion_tokens, batch
    );
    _submit.gpu_completion_tokens.clear();
    _submit.gpu_completion_publish_batch = {};
}

BufferView FakeBufferView(
    std::uint64_t _byte_offset = 0,
    std::uint64_t _byte_size = 16
) {
    static ReadbackTestBuffer supported_buffer{true};
    return BufferView(
        &supported_buffer,
        _byte_offset,
        _byte_size,
        1
    );
}

void InvalidFutureIsBoundedAndExplicit() {
    ReadbackFuture invalid{};
    Expect(!invalid.Valid(), "default readback future was valid");
    Expect(!invalid.IsReady(), "invalid readback future reported Ready");
    Expect(
        invalid.Status() == ReadbackStatus::Error,
        "invalid readback future did not fail closed"
    );
    invalid.Wait();
    Expect(
        !invalid.WaitFor(1ms),
        "invalid readback unexpectedly satisfied WaitFor"
    );
    const auto try_result = invalid.TryGet();
    Expect(
        try_result.has_value() &&
            try_result->status == ReadbackStatus::Error,
        "invalid readback TryGet did not return Error"
    );
    Expect(
        invalid.Get().status == ReadbackStatus::Error,
        "invalid readback Get did not return Error"
    );

    Expect(
        !ReadbackBackendAccess::Create(
             0, 4, "InvalidId", GpuCompletionFuture{}
         ).Valid(),
        "invalid readback creation unexpectedly succeeded"
    );
}

void ReadinessRequiresCompletionAndPayload() {
    constexpr std::array<std::uint32_t, 2> expected{
        0x12345678u, 0x90abcdefu
    };
    const auto bytes = std::as_bytes(std::span(expected));

    ReadbackHarness payload_first(bytes.size_bytes(), "PayloadFirst");
    ByteWriterContext writer{bytes};
    Expect(
        ReadbackBackendAccess::MaterializePayload(
            payload_first.token, &writer, WriteBytes
        ),
        "payload-first materialization failed"
    );
    Expect(
        payload_first.future.Status() == ReadbackStatus::Pending,
        "payload alone made the readback Ready"
    );
    CmdSubmit payload_submit = payload_first.Submit();
    PublishCompletion(payload_submit, true);
    Expect(
        payload_first.future.Status() == ReadbackStatus::Ready,
        "payload-first readback did not become Ready"
    );

    ReadbackHarness completion_first(
        bytes.size_bytes(), "CompletionFirst"
    );
    CmdSubmit completion_submit = completion_first.Submit();
    PublishCompletion(completion_submit, true, false);
    Expect(
        completion_first.completion.Status() ==
            GpuCompletionStatus::Ready,
        "completion publication did not wake the logical completion"
    );
    Expect(
        completion_first.future.Status() == ReadbackStatus::Pending &&
            !completion_first.future.TryGet().has_value(),
        "completion publication exposed an unmaterialized payload"
    );
    Expect(
        ReadbackBackendAccess::MaterializePayload(
            completion_first.token, &writer, WriteBytes
        ),
        "completion-first payload materialization failed"
    );
    Expect(
        completion_first.future.Status() == ReadbackStatus::Ready,
        "materialized completion-first readback stayed Pending"
    );
    NotifyPublishedCompletion(completion_submit);

    ReadbackHarness missing_payload(
        bytes.size_bytes(), "MissingPayload"
    );
    CmdSubmit missing_submit = missing_payload.Submit();
    PublishCompletion(missing_submit, true);
    Expect(
        missing_payload.future.Status() == ReadbackStatus::Error,
        "GPU Ready notification did not reject a missing payload"
    );
    Expect(
        !missing_payload.future.Get().error_reason.empty(),
        "missing payload Error lost its diagnostic"
    );
}

void PayloadOutcomeIsOneShotAndImmutable() {
    constexpr std::array<std::uint32_t, 3> expected{
        7u, 11u, 13u
    };
    const auto bytes = std::as_bytes(std::span(expected));

    ReadbackHarness harness(bytes.size_bytes(), "ImmutableSnapshot");
    ByteWriterContext writer{bytes};
    Expect(
        ReadbackBackendAccess::MaterializePayload(
            harness.token, &writer, WriteBytes
        ),
        "first payload publication failed"
    );
    Expect(
        !ReadbackBackendAccess::MaterializePayload(
            harness.token, &writer, WriteBytes
        ),
        "Ready payload admitted a second writer"
    );
    Expect(
        !ReadbackBackendAccess::PublishPayloadErrorIfPending(
            harness.token, "late payload failure"
        ),
        "late payload Error overwrote Ready"
    );

    CmdSubmit submit = harness.Submit();
    PublishCompletion(submit, true);
    ReadbackResult result = harness.future.Get();
    Expect(
        result.status == ReadbackStatus::Ready,
        "immutable snapshot did not resolve Ready"
    );
    const auto values = result.CopyAs<std::uint32_t>();
    Expect(
        values.has_value() &&
            std::equal(
                values->begin(), values->end(), expected.begin()
            ),
        "typed readback snapshot changed its payload"
    );
    const auto last = result.ReadValue<std::uint32_t>(
        sizeof(std::uint32_t) * 2
    );
    Expect(
        last.has_value() && *last == expected.back(),
        "typed readback lost the last valid value"
    );
    Expect(
        !result.ReadValue<std::uint32_t>(
             result.ByteSize() - sizeof(std::uint32_t) + 1
         ).has_value() &&
            !result.ReadValue<std::uint32_t>(
                 std::numeric_limits<std::size_t>::max()
             ).has_value(),
        "typed readback accepted an out-of-bounds offset"
    );

    harness.token = {};
    harness.future = {};
    Expect(
        result.CopyAs<std::uint32_t>().has_value(),
        "result did not own its immutable byte snapshot"
    );

    ReadbackResult error_result{};
    error_result.status = ReadbackStatus::Error;
    Expect(
        !error_result.CopyAs<std::uint32_t>().has_value(),
        "Error result masqueraded as an empty typed payload"
    );
}

void PayloadReadyErrorRaceHasOneWinner() {
    constexpr std::array<byte, 4> expected{
        byte{1}, byte{2}, byte{3}, byte{4}
    };

    for (int iteration = 0; iteration < 64; ++iteration) {
        ReadbackHarness harness(expected.size(), "PayloadRace");
        ByteWriterContext writer{std::span<const byte>(expected)};
        std::atomic_bool start{false};
        bool ready_won = false;
        bool error_won = false;

        std::thread ready_thread([&] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            ready_won = ReadbackBackendAccess::MaterializePayload(
                harness.token, &writer, WriteBytes
            );
        });
        std::thread error_thread([&] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            error_won =
                ReadbackBackendAccess::PublishPayloadErrorIfPending(
                    harness.token, "injected payload race"
                );
        });
        start.store(true, std::memory_order_release);
        ready_thread.join();
        error_thread.join();

        Expect(
            ready_won != error_won,
            "payload Ready/Error race did not have one winner"
        );
        CmdSubmit submit = harness.Submit();
        PublishCompletion(submit, true);
        Expect(
            harness.future.Get().status ==
                (ready_won ? ReadbackStatus::Ready :
                             ReadbackStatus::Error),
            "payload race result disagreed with its winner"
        );
    }
}

void MaterializationFailureIsTerminal() {
    ReadbackHarness harness(8, "MaterializationFailure");
    ByteWriterContext writer{
        .source = {},
        .throw_failure = true,
    };
    Expect(
        !ReadbackBackendAccess::MaterializePayload(
            harness.token, &writer, WriteBytes
        ),
        "throwing payload writer unexpectedly succeeded"
    );

    CmdSubmit submit = harness.Submit();
    PublishCompletion(submit, true);
    const ReadbackResult result = harness.future.Get();
    Expect(
        result.status == ReadbackStatus::Error &&
            result.Bytes().empty() &&
            !result.error_reason.empty(),
        "payload writer failure did not resolve a bounded Error"
    );
}

void AsyncSnapshotCaptureOutlivesHandlesAndResult() {
    constexpr std::array<std::uint16_t, 8> expected{
        0x3c00u, 0x4000u, 0x4200u, 0x4400u,
        0x4500u, 0x4600u, 0x4700u, 0x4800u,
    };
    const auto bytes = std::as_bytes(std::span(expected));
    ReadbackHarness harness(bytes.size_bytes(), "AsyncHdrSnapshot");
    ByteWriterContext writer{bytes};
    Expect(
        ReadbackBackendAccess::MaterializePayload(
            harness.token, &writer, WriteBytes
        ),
        "asynchronous snapshot payload materialization failed"
    );
    CmdSubmit submit = harness.Submit();
    PublishCompletion(submit, true);
    ReadbackResult result = harness.future.Get();

    std::binary_semaphore encoder_started{0};
    std::binary_semaphore release_encoder{0};
    Moer::Array<byte>     observed{};
    std::string           observed_name{};
    std::thread encoder(
        [payload = result.data,
         name = result.name,
         &encoder_started,
         &release_encoder,
         &observed,
         &observed_name] {
            encoder_started.release();
            release_encoder.acquire();
            observed.assign(payload->begin(), payload->end());
            observed_name = name;
        }
    );
    encoder_started.acquire();

    result         = {};
    harness.future = {};
    harness.token  = {};
    release_encoder.release();
    encoder.join();

    Expect(
        observed == Moer::Array<byte>(bytes.begin(), bytes.end()) &&
            observed_name == "AsyncHdrSnapshot",
        "asynchronous encoder capture lost payload or metadata after "
        "Future/Result destruction"
    );
}

void CallbacksAreExactlyOnceAndExceptionContained() {
    constexpr std::array<byte, 3> expected{
        byte{4}, byte{5}, byte{6}
    };
    ReadbackHarness harness(expected.size(), "Callbacks");
    ByteWriterContext writer{std::span<const byte>(expected)};
    Expect(
        ReadbackBackendAccess::MaterializePayload(
            harness.token, &writer, WriteBytes
        ),
        "callback payload materialization failed"
    );

    constexpr int callback_thread_count = 24;
    std::atomic<int>  callback_count{0};
    std::atomic_bool  start{false};
    std::vector<std::thread> threads{};
    threads.reserve(callback_thread_count);
    for (int index = 0; index < callback_thread_count; ++index) {
        threads.emplace_back([&, future = harness.future] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            future.Then([&](const ReadbackResult& _result) {
                Expect(
                    _result.status == ReadbackStatus::Ready,
                    "concurrent readback callback observed non-Ready"
                );
                callback_count.fetch_add(1, std::memory_order_relaxed);
            });
        });
    }
    harness.future.Then([](const ReadbackResult&) {
        throw std::runtime_error("client readback callback failure");
    });
    start.store(true, std::memory_order_release);
    for (std::thread& thread : threads) {
        thread.join();
    }

    CmdSubmit submit = harness.Submit();
    PublishCompletion(submit, true);
    Expect(
        callback_count.load(std::memory_order_relaxed) ==
            callback_thread_count,
        "pre-terminal callbacks were not invoked exactly once"
    );
    harness.future.Then([&](const ReadbackResult& _result) {
        Expect(
            _result.status == ReadbackStatus::Ready,
            "post-terminal callback observed non-Ready"
        );
        callback_count.fetch_add(1, std::memory_order_relaxed);
        throw std::runtime_error("post-terminal callback failure");
    });
    Expect(
        callback_count.load(std::memory_order_relaxed) ==
            callback_thread_count + 1,
        "post-terminal callback was not invoked exactly once"
    );
}

void OwnerThreadsCannotBlockOnPendingReadback() {
    ReadbackHarness harness(4, "OwnerWaitGuard");
    CmdSubmit submit = harness.Submit();
    {
        RHIThreadRoleScope completion_owner(
            ERHIThreadRole::Completion
        );
        ExpectThrows<std::logic_error>(
            [&] { harness.future.Wait(); },
            "Completion owner blocked on pending readback Wait"
        );
        ExpectThrows<std::logic_error>(
            [&] { (void)harness.future.WaitFor(1ms); },
            "Completion owner blocked on pending readback WaitFor"
        );
        ExpectThrows<std::logic_error>(
            [&] { (void)harness.future.Get(); },
            "Completion owner blocked on pending readback Get"
        );
    }

    constexpr std::array<byte, 4> expected{
        byte{9}, byte{8}, byte{7}, byte{6}
    };
    ByteWriterContext writer{std::span<const byte>(expected)};
    Expect(
        ReadbackBackendAccess::MaterializePayload(
            harness.token, &writer, WriteBytes
        ),
        "owner guard payload materialization failed"
    );
    PublishCompletion(submit, true);
    {
        RHIThreadRoleScope submission_owner(
            ERHIThreadRole::Submission
        );
        harness.future.Wait();
        Expect(
            harness.future.WaitFor(1ms) &&
                harness.future.Get().status ==
                    ReadbackStatus::Ready,
            "owner thread could not inspect terminal readback"
        );
    }
}

void CommandOwnershipAndDestructionAreTerminal() {
    ReadbackFuture list_destruction{};
    {
        CommandList list(EQueueType::Copy);
        list_destruction = list.Readback(
            FakeBufferView(8, 12), "ListReadbackDestruction"
        );
        Expect(
            list_destruction.Valid(),
            "CommandList readback returned invalid Future"
        );
    }
    Expect(
        list_destruction.Get().status == ReadbackStatus::Error,
        "CommandList destruction left readback Pending"
    );

    ReadbackFuture submit_destruction{};
    {
        CommandList list(EQueueType::Copy);
        submit_destruction = list.Readback(
            FakeBufferView(4, 8), "SubmitReadbackDestruction"
        );
        [[maybe_unused]] CmdSubmit abandoned = list.Submit();
    }
    Expect(
        submit_destruction.Get().status == ReadbackStatus::Error,
        "CmdSubmit destruction left readback Pending"
    );

    CommandList list(EQueueType::Copy);
    ReadbackFuture future = list.Readback(
        FakeBufferView(16, 8), "CommandIntegration"
    );
    CmdSubmit submit = list.Submit();
    Expect(
        submit.cmds.size() == 1 &&
            submit.cmds.front()->Type() ==
                Command::EType::CopyBackBuffer &&
            submit.gpu_completion_tokens.size() == 1,
        "owning readback command lost its completion identity"
    );
    const auto& command = *static_cast<const CopyBackBufferCmd*>(
        submit.cmds.front().get()
    );
    Expect(
        command.HasOwningReadback() &&
            command.ByteSize() == 8 &&
            command.Offset() == 16,
        "owning CopyBackBufferCmd lost its source range"
    );

    constexpr std::array<byte, 8> expected{
        byte{0}, byte{1}, byte{2}, byte{3},
        byte{4}, byte{5}, byte{6}, byte{7}
    };
    ByteWriterContext writer{std::span<const byte>(expected)};
    Expect(
        ReadbackBackendAccess::MaterializePayload(
            command.OwningReadback(), &writer, WriteBytes
        ),
        "command-owned payload could not materialize"
    );
    PublishCompletion(submit, true);
    Expect(
        future.Get().Bytes().size_bytes() == expected.size(),
        "command-owned readback returned the wrong byte count"
    );
}

void UnsupportedBackendFailsBeforeRecording() {
    ReadbackTestBuffer unsupported_buffer{false};
    CommandList        list(EQueueType::Copy);
    const ReadbackFuture future = list.Readback(
        BufferView(&unsupported_buffer, 0, 16, 1),
        "UnsupportedOwningReadback"
    );
    Expect(
        !future.Valid(),
        "unsupported backend returned a valid readback Future"
    );
    CmdSubmit submit = list.Submit();
    Expect(
        submit.cmds.empty() &&
            submit.gpu_completion_tokens.empty(),
        "unsupported backend recorded readback ownership"
    );
}

} // namespace

int main() {
    static_assert(std::is_copy_constructible_v<ReadbackFuture>);
    static_assert(std::is_copy_constructible_v<ReadbackToken>);
    static_assert(std::is_nothrow_copy_constructible_v<ReadbackToken>);

    try {
        InvalidFutureIsBoundedAndExplicit();
        ReadinessRequiresCompletionAndPayload();
        PayloadOutcomeIsOneShotAndImmutable();
        PayloadReadyErrorRaceHasOneWinner();
        MaterializationFailureIsTerminal();
        AsyncSnapshotCaptureOutlivesHandlesAndResult();
        CallbacksAreExactlyOnceAndExceptionContained();
        OwnerThreadsCannotBlockOnPendingReadback();
        CommandOwnershipAndDestructionAreTerminal();
        UnsupportedBackendFailsBeforeRecording();
    } catch (const std::exception& exception) {
        std::cerr
            << "RHIReadbackContract: "
            << exception.what() << '\n';
        return 1;
    }

    std::cout << "RHIReadbackContract: all checks passed\n";
    return 0;
}

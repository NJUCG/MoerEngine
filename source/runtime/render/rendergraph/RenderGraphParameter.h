#pragma once
#include "shader/ShaderCommon.h"

namespace Moer::Render::RenderGraph {
/** Adapter for existing shader parameter system. */
class FRDGParameterStruct {
public:
    FRDGParameterStruct() = default;

    explicit FRDGParameterStruct(ArrayArguments&& InArguments) : Arguments(std::move(InArguments)) {}

    template<typename PipelineType, typename... Args>
    FRDGParameterStruct(PipelineType& Pipeline, Args&&... args) {
        Arguments = PipelineType::SetArgs(std::forward<Args>(args)...);
    }

    const ArrayArguments& GetArguments() const {
        return Arguments;
    }

    template<typename FunctionType>
    void EnumerateTextures(FunctionType Function) const {
        for (const TArg& Arg : Arguments.args) {
            std::visit(
                [&](auto&& arg) {
                    using T = std::decay_t<decltype(arg)>;
                    if constexpr (std::is_same_v<T, TextureView>) {
                        Function(arg);
                    } else if constexpr (std::is_same_v<T, std::span<TextureView>>) {
                        for (const auto& tex : arg) {
                            Function(tex);
                        }
                    }
                },
                Arg
            );
        }
    }

    template<typename FunctionType>
    void EnumerateBuffers(FunctionType Function) const {
        for (const TArg& Arg : Arguments.args) {
            std::visit(
                [&](auto&& arg) {
                    using T = std::decay_t<decltype(arg)>;
                    if constexpr (std::is_same_v<T, BufferView>) {
                        Function(arg);
                    } else if constexpr (std::is_same_v<T, std::span<BufferView>>) {
                        for (const auto& buf : arg) {
                            Function(buf);
                        }
                    }
                },
                Arg
            );
        }
    }

    template<typename FunctionType>
    void Enumerate(FunctionType Function) const {
        for (const TArg& Arg : Arguments.args) {
            Function(Arg);
        }
    }

    uint32 GetTextureParameterCount() const {
        uint32 Count = 0;
        EnumerateTextures([&](const TextureView&) {
            Count++;
        });
        return Count;
    }

    uint32 GetBufferParameterCount() const {
        uint32 Count = 0;
        EnumerateBuffers([&](const BufferView&) {
            Count++;
        });
        return Count;
    }

private:
    ArrayArguments Arguments;
};
} // namespace Moer::Render::RenderGraph
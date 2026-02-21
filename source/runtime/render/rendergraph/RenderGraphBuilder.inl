// Adapted from Epic Games' Render Dependency Graph implementation.

#pragma once

// When the IDE analyzes this .inl file standalone, it doesn't know the surrounding
// namespace/include context from RenderGraphBuilder.h. This block provides that context
// for IntelliSense only; actual compilation is unaffected (#pragma once prevents circular include).
#if defined(__INTELLISENSE__) || defined(__RESHARPER__)
#include "RenderGraphBuilder.h"
namespace Moer::Render::RenderGraph {
#endif

inline FRDGTexture* FRDGBuilder::FindExternalTexture(Texture* ExternalTexture) const {
    if (FRDGTexture* const* FoundTexturePtr = ExternalTextures.Find(ExternalTexture)) {
        return *FoundTexturePtr;
    }
    return nullptr;
}

inline FRDGTexture* FRDGBuilder::FindExternalTexture(IPooledRenderTarget* ExternalTexture) const {
    if (ExternalTexture) {
        return FindExternalTexture(ExternalTexture->GetRHI());
    }
    return nullptr;
}

inline FRDGBuffer* FRDGBuilder::FindExternalBuffer(Buffer* ExternalBuffer) const {
    if (FRDGBuffer* const* FoundBufferPtr = ExternalBuffers.Find(ExternalBuffer)) {
        return *FoundBufferPtr;
    }
    return nullptr;
}

inline FRDGBuffer* FRDGBuilder::FindExternalBuffer(FRDGPooledBuffer* ExternalBuffer) const {
    if (ExternalBuffer) {
        return FindExternalBuffer(ExternalBuffer->GetRHI());
    }
    return nullptr;
}

inline FRDGTextureRef
FRDGBuilder::CreateTexture(const FRDGTextureDesc& Desc, const TCHAR* Name, ERDGTextureFlags Flags) {
    FRDGTextureDesc OverrideDesc = Desc;

    // Clamp the texture size to that which is permissible, otherwise it's a guaranteed crash.
    OverrideDesc.Extent.X = std::clamp<int32>(OverrideDesc.Extent.X, 1, GetMax2DTextureDimension());
    OverrideDesc.Extent.Y = std::clamp<int32>(OverrideDesc.Extent.Y, 1, GetMax2DTextureDimension());

    FRDGTextureRef Texture = Textures.Allocate(Allocators.Root, Name, OverrideDesc, Flags);

    return Texture;
}

inline FRDGBufferRef
FRDGBuilder::CreateBuffer(const FRDGBufferDesc& Desc, const TCHAR* Name, ERDGBufferFlags Flags) {
    FRDGBufferDesc OverrideDesc = Desc;

    // Clamp the buffer size to that which is permissible, otherwise it's a guaranteed crash.
    OverrideDesc.BytesPerElement = std::max<uint32>(1u, OverrideDesc.BytesPerElement);
    OverrideDesc.NumElements     = std::max<uint32>(1u, OverrideDesc.NumElements);

    FRDGBufferRef Buffer = Buffers.Allocate(Allocators.Root, Name, OverrideDesc, Flags);
    return Buffer;
}

inline FRDGBufferRef FRDGBuilder::CreateBuffer(
    const FRDGBufferDesc&           Desc,
    const TCHAR*                    Name,
    FRDGBufferNumElementsCallback&& InNumElementsCallback,
    ERDGBufferFlags                 Flags
) {
    // RDG no longer supports the legacy transient resource API.
    FRDGBufferDesc OverrideDesc = Desc;

    FRDGBufferNumElementsCallback* NumElementsCallback =
        InNumElementsCallback ?
            Allocators.Root.AllocNoDestruct<FRDGBufferNumElementsCallback>(std::move(InNumElementsCallback)) :
            nullptr;
    FRDGBufferRef Buffer = Buffers.Allocate(Allocators.Root, Name, OverrideDesc, Flags, NumElementsCallback);
    NumElementsCallbackBuffers.Emplace(Buffer);
    return Buffer;
}

inline FRDGTextureSRV* FRDGBuilder::CreateSRV(const FRDGTextureSRVDesc& Desc) {
    return Views.Allocate<FRDGTextureSRV>(Allocators.Root, Desc.Texture->Name, Desc);
}

inline FRDGBufferSRV* FRDGBuilder::CreateSRV(const FRDGBufferSRVDesc& Desc) {
    return Views.Allocate<FRDGBufferSRV>(Allocators.Root, Desc.Buffer->Name, Desc);
}

inline FRDGTextureUAV*
FRDGBuilder::CreateUAV(const FRDGTextureUAVDesc& Desc, ERDGUnorderedAccessViewFlags InFlags) {
    return Views.Allocate<FRDGTextureUAV>(Allocators.Root, Desc.Texture->Name, Desc, InFlags);
}

inline FRDGBufferUAV*
FRDGBuilder::CreateUAV(const FRDGBufferUAVDesc& Desc, ERDGUnorderedAccessViewFlags InFlags) {
    return Views.Allocate<FRDGBufferUAV>(Allocators.Root, Desc.Buffer->Name, Desc, InFlags);
}

inline void* FRDGBuilder::Alloc(uint64 SizeInBytes, uint32 AlignInBytes) {
    return Allocators.Root.Alloc(SizeInBytes, AlignInBytes);
}

template<typename PODType>
inline PODType* FRDGBuilder::AllocPOD() {
    return Allocators.Root.AllocUninitialized<PODType>();
}

template<typename PODType>
inline PODType* FRDGBuilder::AllocPODArray(uint32 Count) {
    return Allocators.Root.AllocUninitialized<PODType>(Count);
}

template<typename PODType>
std::span<PODType> FRDGBuilder::AllocPODArrayView(uint32 Count) {
    return std::span<PODType>(AllocPODArray<PODType>(Count), Count);
}

template<typename ObjectType, typename... TArgs>
inline ObjectType* FRDGBuilder::AllocObject(TArgs&&... Args) {
    return Allocators.Root.Alloc<ObjectType>(std::forward<TArgs>(Args)...);
}

template<typename ObjectType>
inline Array<ObjectType, SceneRenderingAllocator>& FRDGBuilder::AllocArray() {
    return *Allocators.Root.Alloc<Array<ObjectType, SceneRenderingAllocator>>();
}

template<typename ParameterStructType>
inline ParameterStructType* FRDGBuilder::AllocParameters() {
    return Allocators.Root.Alloc<ParameterStructType>();
}

template<typename ParameterStructType>
inline ParameterStructType* FRDGBuilder::AllocParameters(const ParameterStructType* StructToCopy) {
    ParameterStructType* Struct = Allocators.Root.Alloc<ParameterStructType>();
    *Struct                     = *StructToCopy;
    return Struct;
}

inline FRDGSubresourceState* FRDGBuilder::AllocSubresource(const FRDGSubresourceState& Other) {
    return Allocators.Transition.AllocNoDestruct<FRDGSubresourceState>(Other);
}

inline FRDGSubresourceState* FRDGBuilder::AllocSubresource() {
    return Allocators.Transition.AllocNoDestruct<FRDGSubresourceState>();
}

//////////////////////////////////////////////////////////////////////////////
// Pass Creation

template<typename ParameterStructType, typename ExecuteLambdaType>
FRDGPass* FRDGBuilder::AddPassInternal(
    FRDGEventName&&     Name,
    FRDGParameterStruct ParameterStruct,
    ERDGPassFlags       Flags,
    ExecuteLambdaType&& ExecuteLambda
) {
    using LambdaPassType = TRDGLambdaPass<ParameterStructType, ExecuteLambdaType>;
    FlushAccessModeQueue();
    const char* NameString = Name.GetCStr();
    FRDGPass*   Pass       = Allocators.Root.AllocNoDestruct<LambdaPassType>(
        std::forward<FRDGEventName>(Name),
        std::move(ParameterStruct),
        OverridePassFlags(NameString, Flags),
        std::forward<ExecuteLambdaType>(ExecuteLambda)
    );

    Passes.Insert(Pass);
    SetupParameterPass(Pass);
    return Pass;
}

template<typename ExecuteLambdaType>
FRDGPass* FRDGBuilder::AddPass(FRDGEventName&& Name, ERDGPassFlags Flags, ExecuteLambdaType&& ExecuteLambda) {
    using LambdaPassType = TRDGEmptyLambdaPass<ExecuteLambdaType>;
    Flags |= ERDGPassFlags::NeverCull;
    FlushAccessModeQueue();
    LambdaPassType* Pass = Passes.Allocate<LambdaPassType>(
        Allocators.Root,
        std::forward<FRDGEventName>(Name),
        Flags,
        std::forward<ExecuteLambdaType>(ExecuteLambda)
    );
    SetupEmptyPass(Pass);
    return Pass;
}

template<typename ParameterStructType, typename ExecuteLambdaType>
FRDGPass* FRDGBuilder::AddPass(
    FRDGEventName&&     Name,
    FRDGParameterStruct ParameterStruct,
    ERDGPassFlags       Flags,
    ExecuteLambdaType&& ExecuteLambda
) {
    return AddPassInternal<ParameterStructType>(
        std::forward<FRDGEventName>(Name),
        std::move(ParameterStruct),
        Flags,
        std::forward<ExecuteLambdaType>(ExecuteLambda)
    );
}

inline void FRDGBuilder::SetPassWorkload(FRDGPass* Pass, uint32 Workload) {
    Pass->Workload = Workload;
}

//////////////////////////////////////////////////////////////////////////////
// Buffer Uploads

inline void FRDGBuilder::QueueBufferUpload(
    FRDGBufferRef        Buffer,
    const void*          InitialData,
    uint64               InitialDataSize,
    ERDGInitialDataFlags InitialDataFlags
) {

    if (InitialDataSize > 0 && !EnumHasAnyFlags(InitialDataFlags, ERDGInitialDataFlags::NoCopy)) {
        void* InitialDataCopy = Alloc(InitialDataSize, 16);
        std::memcpy(InitialDataCopy, InitialData, InitialDataSize);
        InitialData = InitialDataCopy;
    }

    UploadedBuffers.Emplace(Buffer, InitialData, InitialDataSize);
    Buffer->bQueuedForUpload = 1;
}

inline void FRDGBuilder::QueueBufferUpload(
    FRDGBufferRef                       Buffer,
    const void*                         InitialData,
    uint64                              InitialDataSize,
    FRDGBufferInitialDataFreeCallback&& InitialDataFreeCallback
) {

    if (InitialDataSize == 0) {
        return;
    }

    UploadedBuffers.Emplace(Buffer, InitialData, InitialDataSize, std::move(InitialDataFreeCallback));
    Buffer->bQueuedForUpload = 1;
}

inline void FRDGBuilder::QueueBufferUpload(
    FRDGBufferRef                       Buffer,
    FRDGBufferInitialDataFillCallback&& InitialDataFillCallback
) {

    UploadedBuffers.Emplace(Buffer, std::move(InitialDataFillCallback));
    Buffer->bQueuedForUpload = 1;
}

inline void FRDGBuilder::QueueBufferUpload(
    FRDGBufferRef                       Buffer,
    FRDGBufferInitialDataCallback&&     InitialDataCallback,
    FRDGBufferInitialDataSizeCallback&& InitialDataSizeCallback
) {

    UploadedBuffers.Emplace(Buffer, std::move(InitialDataCallback), std::move(InitialDataSizeCallback));
    Buffer->bQueuedForUpload = 1;
}

inline void FRDGBuilder::QueueBufferUpload(
    FRDGBufferRef                       Buffer,
    FRDGBufferInitialDataCallback&&     InitialDataCallback,
    FRDGBufferInitialDataSizeCallback&& InitialDataSizeCallback,
    FRDGBufferInitialDataFreeCallback&& InitialDataFreeCallback
) {

    UploadedBuffers.Emplace(
        Buffer,
        std::move(InitialDataCallback),
        std::move(InitialDataSizeCallback),
        std::move(InitialDataFreeCallback)
    );
    Buffer->bQueuedForUpload = 1;
}

//////////////////////////////////////////////////////////////////////////////
// Reserved Buffer Commits

inline void FRDGBuilder::QueueCommitReservedBuffer(FRDGBufferRef Buffer, uint64 CommitSizeInBytes) {
    Buffer->PendingCommitSize = CommitSizeInBytes;
    Buffer->PooledBuffer->SetCommittedSize(CommitSizeInBytes);
}

//////////////////////////////////////////////////////////////////////////////
// Resource Extraction

inline void FRDGBuilder::QueueTextureExtraction(
    FRDGTextureRef                     Texture,
    CountableRef<IPooledRenderTarget>* OutTexturePtr,
    ERHIAccess                         AccessFinal,
    ERDGResourceExtractionFlags        Flags
) {
    QueueTextureExtraction(Texture, OutTexturePtr, Flags);
    SetTextureAccessFinal(Texture, AccessFinal);
}

inline void FRDGBuilder::QueueTextureExtraction(
    FRDGTextureRef                     Texture,
    CountableRef<IPooledRenderTarget>* OutTexturePtr,
    ERDGResourceExtractionFlags        Flags
) {

    *OutTexturePtr = nullptr;

    const bool bWasExtracted = Texture->bExtracted;

    Texture->bExtracted = true;

    // Transient extraction is disabled (no-op).

    ExtractedTextures.Emplace(Texture, OutTexturePtr);

    if (!bWasExtracted) {
        AddCullRootTexture(Texture);
    }
}

inline void
FRDGBuilder::QueueBufferExtraction(FRDGBufferRef Buffer, CountableRef<FRDGPooledBuffer>* OutBufferPtr) {

    *OutBufferPtr = nullptr;

    const bool bWasExtracted = Buffer->bExtracted;

    Buffer->bExtracted         = true;
    Buffer->bForceNonTransient = true;
    ExtractedBuffers.Emplace(Buffer, OutBufferPtr);

    if (!bWasExtracted) {
        AddCullRootBuffer(Buffer);
    }
}

inline void FRDGBuilder::QueueBufferExtraction(
    FRDGBufferRef                   Buffer,
    CountableRef<FRDGPooledBuffer>* OutBufferPtr,
    ERHIAccess                      AccessFinal
) {
    QueueBufferExtraction(Buffer, OutBufferPtr);
    SetBufferAccessFinal(Buffer, AccessFinal);
}

//////////////////////////////////////////////////////////////////////////////
// Misc

inline void FRDGBuilder::AddDispatchHint() {
    if (Passes.Num() > 0) {
        Passes[Passes.Last()]->bDispatchAfterExecute = 1;
    }
}

inline const CountableRef<IPooledRenderTarget>& FRDGBuilder::GetPooledTexture(FRDGTextureRef Texture) const {
    return Texture->Allocation;
}

inline const CountableRef<FRDGPooledBuffer>& FRDGBuilder::GetPooledBuffer(FRDGBufferRef Buffer) const {
    return Buffer->Allocation;
}

inline void FRDGBuilder::SetTextureAccessFinal(FRDGTextureRef Texture, ERHIAccess AccessFinal) {
    Texture->EpilogueAccess = AccessFinal;
}

inline void FRDGBuilder::SetBufferAccessFinal(FRDGBufferRef Buffer, ERHIAccess AccessFinal) {
    Buffer->EpilogueAccess = AccessFinal;
}

#if defined(__INTELLISENSE__) || defined(__RESHARPER__)
} // namespace Moer::Render::RenderGraph
#endif

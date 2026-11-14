#pragma once

// Read-only diagnostic for the locally installed engine's matching PDB.
// No FRDGBuilder layout assumptions, engine binary patches, or counter resets.
#include "CoreMinimal.h"
#include "Misc/Paths.h"
#include "Windows/WindowsHWrapper.h"
#include "Windows/AllowWindowsPlatformTypes.h"
#include <dbghelp.h>
#include "Windows/HideWindowsPlatformTypes.h"

class FPrivateRdgStats
{
public:
    explicit FPrivateRdgStats(bool bEnabled)
    {
        if (!bEnabled) { return; }
        check(DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(),
                              &Session, 0, false, DUPLICATE_SAME_ACCESS));
        check(SymInitialize(Session, nullptr, false));
        const FString Binary = FPaths::ConvertRelativePathToFull(FPaths::EngineDir() / TEXT("Binaries/Win64/UnrealEditor-RenderCore.dll"));
        const auto Module = GetModuleHandleW(TEXT("UnrealEditor-RenderCore.dll"));
        check(Module && SymLoadModuleExW(Session, nullptr, *Binary, nullptr, uint64(Module), 0, nullptr, 0));
        for (const char* Name : {
            "GRDGStatPassCount", "GRDGStatPassCullCount", "GRDGStatPassDependencyCount",
            "GRDGStatTextureCount", "GRDGStatTextureReferenceCount", "GRDGStatBufferCount",
            "GRDGStatBufferReferenceCount", "GRDGStatTransitionCount", "GRDGStatTransitionBatchCount",
            "GRDGStatMemoryWatermark" })
        {
            alignas(SYMBOL_INFO) char Storage[sizeof(SYMBOL_INFO) + MAX_SYM_NAME]{};
            auto* Symbol = reinterpret_cast<SYMBOL_INFO*>(Storage);
            Symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            Symbol->MaxNameLen = MAX_SYM_NAME;
            checkf(SymFromName(Session, Name, Symbol), TEXT("RDG diagnostic symbol is unavailable: %hs"), Name);
            Entries.Add({UTF8_TO_TCHAR(Name), reinterpret_cast<const int32*>(Symbol->Address)});
        }
    }
    ~FPrivateRdgStats()
    {
        if (Session) { SymCleanup(Session); CloseHandle(Session); }
    }
    FPrivateRdgStats(const FPrivateRdgStats&) = delete;
    FPrivateRdgStats& operator=(const FPrivateRdgStats&) = delete;
    TArray<int32> Read() const
    {
        TArray<int32> Values;
        for (const auto& Entry : Entries) { Values.Add(*Entry.Value); }
        return Values;
    }
    FString Json(const TArray<int32>& Before, const TArray<int32>& After) const
    {
        FString Result = TEXT("{");
        for (int32 Index = 0; Index < Entries.Num(); ++Index)
        {
            if (Index) { Result += TEXT(","); }
            Result += FString::Printf(TEXT("\"%s\":{\"before\":%d,\"after\":%d,\"delta\":%lld}"),
                *Entries[Index].Name, Before[Index], After[Index], int64(After[Index]) - Before[Index]);
        }
        return Result + TEXT("}");
    }
private:
    struct FEntry { FString Name; const int32* Value; };
    void* Session = nullptr;
    TArray<FEntry> Entries;
};

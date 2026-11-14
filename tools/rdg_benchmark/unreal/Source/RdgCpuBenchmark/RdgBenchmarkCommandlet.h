#pragma once

#include "Commandlets/Commandlet.h"
#include "RdgBenchmarkCommandlet.generated.h"

UCLASS()
class URdgBenchmarkCommandlet : public UCommandlet
{
    GENERATED_BODY()
public:
    URdgBenchmarkCommandlet();
    int32 Main(const FString& Params) override;
};

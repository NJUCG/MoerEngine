#pragma once

#include "ProfileTypes.h"
#include "misc/Types.h"

Moer::uint64 now_us();

void capture_frames_fast(
    void**        out_frames,
    int           max_frames,
    Moer::uint16& out_count,
    Moer::uint16  skip = 1
);

std::string symbolicate_address(void* addr);
void        ShutdownSymbolEngine();

bool InitSymbolEngine();
void* GetProcAddressFromPdb(const char* mangledName, const char* readableName = nullptr);
std::string get_log_path();
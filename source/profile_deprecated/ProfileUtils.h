#pragma once
#include "ProfileTypes.h"

uint64_t now_us();

void capture_frames_fast(
    void**   out_frames,
    int      max_frames,
    uint16_t& out_count,
    uint16_t  skip = 1);

std::string symbolicate_address(void* addr);
void        ShutdownSymbolEngine();

bool InitSymbolEngine();
void* GetProcAddressFromPdb(const char* mangledName, const char* readableName = nullptr);
std::string get_log_path();
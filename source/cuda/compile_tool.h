#pragma once

#if defined(MOER_CUDA_EXPORTS) // moer_cuda模块内

#if defined(_WIN32) || defined(_WIN64)
#define MOER_CUDA_API __declspec(dllexport)
#elif defined(linux) || defined(__linux) || defined(__linux__) || defined(__GNUC__)
#define MOER_CUDA_API __attribute__((visibility("default")))
#else
#define MOER_CUDA_API
#endif

#else // moer_cuda模块外

#if defined(_WIN32) || defined(_WIN64)
#define MOER_CUDA_API __declspec(dllimport)
#elif defined(linux) || defined(__linux) || defined(__linux__) || defined(__GNUC__)
#define MOER_CUDA_API
#else
#define MOER_CUDA_API
#endif

#endif
#ifndef WINDOWS_PLATFORM_H
#define WINDOWS_PLATFORM_H
#include <Windows.h>

class WindowsPlatform {
public:
	static void SetThreadAffinity(void* current_thread_handle, UINT64 mask);
	static void SetThreadGroupAffinity(void* current_thread_handle, USHORT group_mask, UINT64 affinity_mask);
	static INT32 GetProcessorWorkGroupCount();
	static INT32 GetProcessorCoreCountInGroup(INT32 groupID);
	static INT32 GetProcessorCoreCount();
	static UINT32 GetCurrentThreadID();
};
#endif // !WINDOWS_PLATFORM_H


#include "platform/PlatformWindows.h"

void WindowsPlatform::SetThreadAffinity(void* current_thread_handle, UINT64 mask) {

	::SetThreadAffinityMask(current_thread_handle, mask);
}
void WindowsPlatform::SetThreadGroupAffinity(void* current_thread_handle, USHORT group_mask, UINT64 affinity_mask) {
	GROUP_AFFINITY group_affinity{ affinity_mask, group_mask, {0,0,0} };
	::SetThreadGroupAffinity(current_thread_handle, &group_affinity, nullptr);
}

INT32 WindowsPlatform::GetProcessorWorkGroupCount()
{
	return ::GetActiveProcessorGroupCount();
	
}

INT32 WindowsPlatform::GetProcessorCoreCountInGroup(INT32 groupID)
{
	return ::GetActiveProcessorCount(groupID);
}

INT32 WindowsPlatform::GetProcessorCoreCount()
{
	SYSTEM_INFO sysInfo;
	GetSystemInfo(&sysInfo);
	return sysInfo.dwNumberOfProcessors;
}

UINT32 WindowsPlatform::GetCurrentThreadID()
{
	return ::GetCurrentThreadId();
}

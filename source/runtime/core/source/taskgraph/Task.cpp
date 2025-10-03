#include "taskgraph/Task.h"
//
//FTaskEventBaseAllocator TaskEventBaseAllocator;
//
//void TaskBase::Schedule()
//{
//	TaskTrace::Scheduled(GetTraceId());
//
//	LowLevelTasks::FScheduler::Get().TryLaunch(LowLevelTask, LowLevelTasks::EQueuePreference::GlobalQueuePreference, /*bWakeUpWorker=*/ true);
//}
//
//
//thread_local uint32 TaskRetractionRecursion = 0;
//
//bool IsThreadRetractingTask()
//{
//	return TaskRetractionRecursion != 0;
//}
//
//struct FThreadLocalRetractionScope
//{
//	FThreadLocalRetractionScope()
//	{
//		checkSlow(TaskRetractionRecursion != TNumericLimits<decltype(TaskRetractionRecursion)>::Max() - 1);
//		++TaskRetractionRecursion;
//	}
//
//	~FThreadLocalRetractionScope()
//	{
//		checkSlow(TaskRetractionRecursion != 0);
//		--TaskRetractionRecursion;
//	}
//};
//
//bool TaskBase::TryRetractAndExecute(uint32 RecursionDepth/* = 0*/)
//{
//	TRACE_CPUPROFILER_EVENT_SCOPE(TaskRetraction);
//
//	if (IsCompleted())
//	{
//		return true;
//	}
//
//	// avoid stack overflow. is not expected in a real-life cases but happens in stress tests
//	if (RecursionDepth == 200)
//	{
//		return false;
//	}
//	++RecursionDepth;
//
//	// returns false if the task has passed "pre-scheduling" state: all (if any) prerequisites are completed
//	auto IsLockedByPrerequisites = [this]
//	{
//		uint32 LocalNumLocks = NumLocks.load(std::memory_order_relaxed); // the order doesn't matter as this "happens before" task execution
//		return LocalNumLocks != 0 && LocalNumLocks < ExecutionFlag;
//	};
//
//	if (IsLockedByPrerequisites())
//	{
//		// try to unlock the task. even if (some or all) prerequisites retraction fails we still proceed to try helping with other prerequisites or this task execution
//
//		// prerequisites are "consumed" here even if their retraction fails. this means that once prerequisite retraction failed, it won't be performed again.
//		// this can be potentially improved by using a different container for prerequisites
//		while (TaskBase* Prerequisite = Prerequisites.Pop())
//		{
//			// ignore if retraction failed, as this thread still can try to help with other prerequisites instead of being blocked in waiting
//			Prerequisite->TryRetractAndExecute(RecursionDepth);
//			Prerequisite->Release();
//		}
//	}
//
//	{
//		FThreadLocalRetractionScope ThreadLocalRetractionScope;
//
//		// next we try to execute the task, despite we haven't verified that the task is unlocked. trying to obtain execution permission will fail in this case
//
//		if (ExtendedPriority == EExtendedTaskPriority::TaskEvent)
//		{
//			if (!TrySetExecutionFlag())
//			{
//				return false;
//			}
//
//			// task events have nothing to execute, and so can't have nested task, just close it
//			Close();
//			ReleaseInternalReference();
//			return true;
//		}
//
//		if (!TryExecuteTask())
//		{
//			return false; // still locked by prerequisites, or another thread managed to set execution flag first, or we're inside this task execution
//			// we could try to help with nested tasks execution (the task execution could already spawned a couple of nested tasks sitting in the queue).
//			// it's unclear how important this is, but this would definitely lead to more complicated impl. we can revisit this once we see such instances in profiler captures
//		}
//	}
//	// the task was launched so the scheduler will handle the internal reference held by low-level task
//
//	if (IsCompleted()) // still can be hold back by nested tasks, this is an optional early out for better perf
//	{
//		return true;
//	}
//
//	// retract nested tasks, if any
//	{
//		// keep trying retracting all nested tasks even if some of them fail, so the current worker can contribute instead of being blocked
//		bool bSucceeded = true;
//		// prerequisites are "consumed" here even if their retraction fails. this means that once prerequisite retraction failed, it won't be performed again.
//		// this can be potentially improved by using a different container for prerequisites
//		while (TaskBase* Prerequisite = Prerequisites.Pop())
//		{
//			if (!Prerequisite->TryRetractAndExecute(RecursionDepth))
//			{
//				bSucceeded = false;
//			}
//			Prerequisite->Release();
//		}
//
//		if (!bSucceeded)
//		{
//			return false;
//		}
//	}
//
//	// it happens that all nested tasks are completed and are in the process of completing the parent (this task) concurrently,
//	// but the flag is not set yet. wait for it to maintain postconditions
//	while (!IsCompleted())
//	{
//		FPlatformProcess::Yield();
//	}
//
//	return true;
//}
//
//void TaskBase::Wait()
//{
//	if (IsCompleted())
//	{
//		return;
//	}
//
//	if (!IsAwaitable())
//	{
//		UE_LOG(LogTemp, Fatal, TEXT("Deadlock detected! A task can't be waited here, e.g. because it's being executed by the currect thread"));
//		return;
//	}
//
//	if (TryRetractAndExecute())
//	{
//		return;
//	}
//
//	// if we are on a named thread, handle waiting in TaskGraph-specific style
//	if (TryWaitOnNamedThread(*this))
//	{
//		return;
//	}
//
//	FEventRef CompletionEvent;
//	auto WaitingTaskBody = [&CompletionEvent] { CompletionEvent->Trigger(); };
//	using FWaitingTask = TExecutableTask<decltype(WaitingTaskBody)>;
//
//	// the task is stored on the stack as we can guarantee that it's out of the system by the end of the call
//	FWaitingTask WaitingTask{ TEXT("Waiting Task"), MoveTemp(WaitingTaskBody), ETaskPriority::Default /* doesn't matter*/, EExtendedTaskPriority::Inline };
//	WaitingTask.AddPrerequisites(*this);
//
//	if (WaitingTask.TryLaunch())
//	{	// was executed inline
//		check(WaitingTask.IsCompleted());
//	}
//	else
//	{
//		CompletionEvent->Wait();
//	}
//
//	// the waiting task will be destroyed leaving this scope, wait for the internal reference to it to be released
//	while (WaitingTask.GetRefCount() != 1)
//	{
//		FPlatformProcess::Yield();
//	}
//}
//
//bool TaskBase::Wait(FTimespan InTimeout)
//{
//	TaskTrace::FWaitingScope WaitingScope(GetTraceId());
//	TRACE_CPUPROFILER_EVENT_SCOPE(Tasks::Wait);
//
//	FTimeout Timeout{ InTimeout };
//
//	if (TryRetractAndExecute())
//	{
//		return true;
//	}
//
//	if (GetCurrentTask() == this)
//	{
//		UE_LOG(LogTemp, Fatal, TEXT("A task waiting for itself detected"));
//		return true;
//	}
//
//	// the event must be alive for the task and this function lifetime, we don't know which one will be finished first as waiting can
//	// time out before the waiting task is completed
//	FSharedEventRef CompletionEvent;
//	auto WaitingTaskBody = [CompletionEvent] { CompletionEvent->Trigger(); };
//	using FWaitingTask = TExecutableTask<decltype(WaitingTaskBody)>;
//
//	TRefCountPtr<FWaitingTask> WaitingTask{ FWaitingTask::Create(TEXT("Waiting Task"), MoveTemp(WaitingTaskBody), ETaskPriority::Default /* doesn't matter*/, EExtendedTaskPriority::Inline), /*bAddRef=*/ false };
//	WaitingTask->AddPrerequisites(*this);
//
//	if (WaitingTask->TryLaunch())
//	{	// was executed inline
//		check(WaitingTask->IsCompleted());
//		return true;
//	}
//
//	return CompletionEvent->Wait((uint32)FMath::Clamp<int64>(Timeout.GetRemainingTime().GetTicks() / ETimespan::TicksPerMillisecond, 0, MAX_uint32));
//}
//
//TaskBase* TaskBase::TryPushIntoPipe()
//{
//	return GetPipe()->PushIntoPipe(*this);
//}
//
//void TaskBase::StartPipeExecution()
//{
//	GetPipe()->ExecutionStarted();
//}
//
//void TaskBase::FinishPipeExecution()
//{
//	GetPipe()->ExecutionFinished();
//}
//
//void TaskBase::ClearPipe()
//{
//	GetPipe()->ClearTask(*this);
//}
//
//static thread_local TaskBase* CurrentTask = nullptr;
//
//TaskBase* GetCurrentTask()
//{
//	return CurrentTask;
//}
//
//TaskBase* ExchangeCurrentTask(TaskBase* Task)
//{
//	TaskBase* PrevTask = CurrentTask;
//	CurrentTask = Task;
//	return PrevTask;
//}
//
//bool TryWaitOnNamedThread(TaskBase& Task)
//{
//#if TASKGRAPH_NEW_FRONTEND
//	// handle waiting only on a named thread and if not called from inside a task
//	FTaskGraphInterface& TaskGraph = FTaskGraphInterface::Get();
//	ENamedThreads::Type CurrentThread = TaskGraph.GetCurrentThreadIfKnown();
//	if (CurrentThread < ENamedThreads::ActualRenderingThread /* is a named thread? */ && !TaskGraph.IsThreadProcessingTasks(CurrentThread))
//	{
//		// execute other tasks of this named thread while waiting
//		ETaskPriority Dummy;
//		EExtendedTaskPriority ExtendedPriority;
//		FBaseGraphTask::TranslatePriority(CurrentThread, Dummy, ExtendedPriority);
//
//		auto TaskBody = [CurrentThread, &TaskGraph] { TaskGraph.RequestReturn(CurrentThread); };
//		using FReturnFromNamedThreadTask = TExecutableTask<decltype(TaskBody)>;
//		FReturnFromNamedThreadTask ReturnTask{ TEXT("ReturnFromNamedThreadTask"), MoveTemp(TaskBody), ETaskPriority::High, ExtendedPriority };
//		ReturnTask.AddPrerequisites(Task);
//		ReturnTask.TryLaunch(); // the result doesn't matter
//
//		TaskGraph.ProcessThreadUntilRequestReturn(CurrentThread);
//		return true;
//	}
//#endif
//
//	return false;
//}
//	}